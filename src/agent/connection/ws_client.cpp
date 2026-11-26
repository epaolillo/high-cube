#include "connection/ws_client.hpp"

#include "audit/audit_engine.hpp"
#include "metrics/host_metrics.hpp"
#include "protocol/codec.hpp"
#include "runtime/db_manage.hpp"
#include "system/hostname.hpp"

#include <boost/process.hpp>
#include <openssl/err.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace bp = boost::process;

namespace highcube::agent {
namespace {

// History sent when a log stream opens; the browser keeps at most 1 MB of it.
constexpr unsigned kLogTailLines = 10000;

}  // namespace

WsClient::WsClient(net::io_context& ioc, AgentConfig config, DockerRuntime& runtime)
    : ioc_(ioc),
      config_(std::move(config)),
      runtime_(runtime),
      resolver_(ioc),
      reconnectTimer_(ioc),
      heartbeatTimer_(ioc),
      metricsTimer_(ioc),
      containerMetricsTimer_(ioc),
      processSnapshotTimer_(ioc),
      vpnStatusTimer_(ioc) {
  // admin_url like wss://host/api/agents/connect or ws://host:3000/api/agents/connect
  std::string url = config_.adminUrl;
  if (url.rfind("wss://", 0) == 0) {
    useTls_ = true;
    url = url.substr(6);
  } else if (url.rfind("ws://", 0) == 0) {
    useTls_ = false;
    url = url.substr(5);
  }
  const auto slash = url.find('/');
  const auto hostPort = slash == std::string::npos ? url : url.substr(0, slash);
  path_ = slash == std::string::npos ? "/" : url.substr(slash);
  const auto colon = hostPort.find(':');
  host_ = colon == std::string::npos ? hostPort : hostPort.substr(0, colon);
  port_ = colon == std::string::npos ? (useTls_ ? "443" : "80") : hostPort.substr(colon + 1);
}

void WsClient::start() {
  resolve();
}

void WsClient::resolve() {
  resolver_.async_resolve(host_, port_,
                          beast::bind_front_handler(&WsClient::onResolve, shared_from_this()));
}

void WsClient::onResolve(beast::error_code ec, tcp::resolver::results_type results) {
  if (ec) {
    return fail(ec, "resolve");
  }
  if (useTls_) {
    wss_ = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(ioc_, sslCtx_);
    beast::get_lowest_layer(*wss_).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(*wss_).async_connect(
        results, [self = shared_from_this()](beast::error_code ec,
                                             tcp::resolver::results_type::endpoint_type ep) {
          self->onConnect(ec, ep);
        });
  } else {
    ws_ = std::make_unique<websocket::stream<beast::tcp_stream>>(ioc_);
    beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(*ws_).async_connect(
        results, [self = shared_from_this()](beast::error_code ec,
                                             tcp::resolver::results_type::endpoint_type ep) {
          self->onConnect(ec, ep);
        });
  }
}

void WsClient::onConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
  if (ec) {
    return fail(ec, "connect");
  }
  if (useTls_) {
    if (!SSL_set_tlsext_host_name(wss_->next_layer().native_handle(), host_.c_str())) {
      return fail(beast::error_code(static_cast<int>(::ERR_get_error()),
                                    net::error::get_ssl_category()),
                  "sni");
    }
    wss_->next_layer().async_handshake(
        ssl::stream_base::client,
        beast::bind_front_handler(&WsClient::onSslHandshake, shared_from_this()));
  } else {
    beast::get_lowest_layer(*ws_).expires_never();
    ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws_->binary(true);
    ws_->async_handshake(host_, path_,
                         beast::bind_front_handler(&WsClient::onHandshake, shared_from_this()));
  }
}

void WsClient::onSslHandshake(beast::error_code ec) {
  if (ec) {
    return fail(ec, "ssl");
  }
  beast::get_lowest_layer(*wss_).expires_never();
  wss_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
  wss_->binary(true);
  wss_->async_handshake(host_, path_,
                        beast::bind_front_handler(&WsClient::onHandshake, shared_from_this()));
}

void WsClient::onHandshake(beast::error_code ec) {
  if (ec) {
    return fail(ec, "handshake");
  }
  connected_ = true;
  spdlog::info("connected to {}", config_.adminUrl);

  ::highcube::Register reg;
  reg.set_server_name(config_.serverName);
  reg.set_agent_token(config_.agentToken);
  reg.set_server_ip(config_.serverIp);
  send(makeEnvelope(::highcube::REGISTER, reg));

  doRead();
  startHeartbeat();
  startMetrics();
  startContainerMetrics();
  startProcessSnapshots();
  startVpnStatus();
}

void WsClient::doRead() {
  auto handler = [self = shared_from_this()](beast::error_code ec, std::size_t) {
    if (ec) {
      return self->fail(ec, "read");
    }
    std::string bytes;
    bytes.assign(static_cast<const char*>(self->buffer_.cdata().data()), self->buffer_.size());
    ::highcube::Envelope envelope;
    if (parseEnvelope(bytes, envelope)) {
      self->handleEnvelope(envelope);
    }
    self->buffer_.consume(self->buffer_.size());
    self->doRead();
  };
  if (useTls_) {
    wss_->async_read(buffer_, handler);
  } else {
    ws_->async_read(buffer_, handler);
  }
}

void WsClient::handleEnvelope(const ::highcube::Envelope& envelope) {
  switch (envelope.kind()) {
    case ::highcube::DEPLOY_REPLICA: {
      ::highcube::DeployReplica order;
      if (order.ParseFromString(envelope.payload())) {
        auto self = shared_from_this();
        std::thread([self, order]() {
          auto sendStatus = [self](const ::highcube::ContainerStatus& status) {
            self->send(makeEnvelope(::highcube::CONTAINER_STATUS, status));
          };
          auto sendLog = [self, deploymentId = order.deployment_id()](const std::string& phase,
                                                                     const std::string& chunk) {
            ::highcube::DeploymentLog log;
            log.set_deployment_id(deploymentId);
            log.set_phase(phase);
            log.set_data(chunk);
            self->send(makeEnvelope(::highcube::DEPLOYMENT_LOG, log));
          };
          self->runtime_.handleDeploy(order, sendStatus, sendLog);
        }).detach();
      }
      break;
    }
    case ::highcube::STOP_REPLICA: {
      ::highcube::StopReplica order;
      if (order.ParseFromString(envelope.payload())) {
        // A database may take up to two minutes to shut down; keep the socket loop responsive.
        auto self = shared_from_this();
        std::thread([self, order]() {
          self->runtime_.handleStop(order, [self](const ::highcube::ContainerStatus& status) {
            self->send(makeEnvelope(::highcube::CONTAINER_STATUS, status));
          });
        }).detach();
      }
      break;
    }
    case ::highcube::RECONCILE_REPLICAS: {
      ::highcube::ReconcileReplicas order;
      if (order.ParseFromString(envelope.payload())) {
        runtime_.removeUnkeptReplicas(
            std::vector<int64_t>(order.replica_ids().begin(), order.replica_ids().end()));
      }
      break;
    }
    case ::highcube::RESTART_REPLICA: {
      ::highcube::RestartReplica order;
      if (order.ParseFromString(envelope.payload())) {
        // Waits for the health check after the restart; keep the socket loop responsive.
        auto self = shared_from_this();
        std::thread([self, order]() {
          self->runtime_.handleRestart(order, [self](const ::highcube::ContainerStatus& status) {
            self->send(makeEnvelope(::highcube::CONTAINER_STATUS, status));
          });
        }).detach();
      }
      break;
    }
    case ::highcube::DELETE_VOLUME: {
      ::highcube::DeleteVolume order;
      if (order.ParseFromString(envelope.payload())) {
        // Removing a large data directory can take a while; keep the socket loop responsive.
        auto self = shared_from_this();
        std::thread([self, order]() {
          self->runtime_.deleteVolume(order.volume_name(), order.host_path());
        }).detach();
      }
      break;
    }
    case ::highcube::RENAME_VOLUME: {
      ::highcube::RenameVolume order;
      if (order.ParseFromString(envelope.payload())) {
        auto self = shared_from_this();
        std::thread([self, order]() {
          try {
            self->runtime_.renameVolume(order.from_volume_name(), order.to_volume_name(),
                                        order.host_path());
          } catch (const std::exception& ex) {
            spdlog::error("rename volume failed: {}", ex.what());
          }
        }).detach();
      }
      break;
    }
    case ::highcube::OPEN_HOST_TERMINAL: {
      ::highcube::OpenHostTerminal order;
      if (!order.ParseFromString(envelope.payload())) {
        break;
      }
      const auto streamId = order.stream_id();
      const auto cols = order.cols();
      const auto rows = order.rows();
      const auto command = order.command();
      auto session = std::make_shared<TerminalSession>();
      auto previous = terminals_[streamId];
      terminals_[streamId] = session;
      if (previous) {
        previous->close();
      }
      session->openHost(
          command,
          [self = shared_from_this(), streamId](const std::string& data) {
            net::post(self->ioc_, [self, streamId, data]() {
              if (!self->connected_ || !self->terminals_.count(streamId)) {
                return;
              }
              ::highcube::TerminalOutput out;
              out.set_stream_id(streamId);
              out.set_data(data);
              self->send(makeEnvelope(::highcube::TERMINAL_OUTPUT, out));
            });
          },
          cols, rows);
      break;
    }
    case ::highcube::OPEN_CONTAINER_TERMINAL: {
      ::highcube::OpenContainerTerminal order;
      if (!order.ParseFromString(envelope.payload())) {
        break;
      }
      const auto streamId = order.stream_id();
      const auto cols = order.cols();
      const auto rows = order.rows();
      const auto containerId = order.container_id();
      const auto command = order.command().empty() ? "/bin/sh" : order.command();
      if (!runtime_.containerHasShell(containerId)) {
        ::highcube::TerminalOutput out;
        out.set_stream_id(streamId);
        out.set_data("Shell is not available in this container.\n");
        send(makeEnvelope(::highcube::TERMINAL_OUTPUT, out));
        break;
      }
      auto session = std::make_shared<TerminalSession>();
      auto previous = terminals_[streamId];
      terminals_[streamId] = session;
      if (previous) {
        previous->close();
      }
      session->openContainer(
          containerId, command,
          [self = shared_from_this(), streamId](const std::string& data) {
            net::post(self->ioc_, [self, streamId, data]() {
              if (!self->connected_ || !self->terminals_.count(streamId)) {
                return;
              }
              ::highcube::TerminalOutput out;
              out.set_stream_id(streamId);
              out.set_data(data);
              self->send(makeEnvelope(::highcube::TERMINAL_OUTPUT, out));
            });
          },
          cols, rows);
      break;
    }
    case ::highcube::SUBSCRIBE_LOGS: {
      ::highcube::SubscribeLogs order;
      if (!order.ParseFromString(envelope.payload())) {
        break;
      }
      const auto streamId = order.stream_id();
      const auto replicaId = order.replica_id();
      const auto containerId = order.container_id();
      // The id ends up in a shell command line.
      const bool validId =
          !containerId.empty() && std::all_of(containerId.begin(), containerId.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
          });
      if (!validId) {
        ::highcube::LogChunk out;
        out.set_stream_id(streamId);
        out.set_replica_id(replicaId);
        out.set_data("Invalid container id.\n");
        send(makeEnvelope(::highcube::LOG_CHUNK, out));
        break;
      }
      auto session = std::make_shared<TerminalSession>();
      auto previous = terminals_[streamId];
      terminals_[streamId] = session;
      if (previous) {
        previous->close();
      }
      session->openLogs(
          containerId, kLogTailLines,
          [self = shared_from_this(), streamId, replicaId](const std::string& data) {
            net::post(self->ioc_, [self, streamId, replicaId, data]() {
              if (!self->connected_ || !self->terminals_.count(streamId)) {
                return;
              }
              ::highcube::LogChunk out;
              out.set_stream_id(streamId);
              out.set_replica_id(replicaId);
              out.set_data(data);
              self->send(makeEnvelope(::highcube::LOG_CHUNK, out));
            });
          });
      break;
    }
    case ::highcube::TERMINAL_INPUT: {
      ::highcube::TerminalInput order;
      if (!order.ParseFromString(envelope.payload())) {
        break;
      }
      auto it = terminals_.find(order.stream_id());
      if (it != terminals_.end()) {
        it->second->write(order.data());
      }
      break;
    }
    case ::highcube::RESIZE_TERMINAL: {
      ::highcube::ResizeTerminal order;
      if (!order.ParseFromString(envelope.payload())) {
        break;
      }
      auto it = terminals_.find(order.stream_id());
      if (it != terminals_.end()) {
        it->second->resize(order.cols(), order.rows());
      }
      break;
    }
    case ::highcube::CLOSE_STREAM: {
      ::highcube::CloseStream order;
      if (!order.ParseFromString(envelope.payload())) {
        break;
      }
      auto it = terminals_.find(order.stream_id());
      if (it != terminals_.end()) {
        auto session = it->second;
        terminals_.erase(it);
        session->close();
      }
      break;
    }
    case ::highcube::CHECK_HOST_PORT: {
      ::highcube::CheckHostPort order;
      if (!order.ParseFromString(envelope.payload())) {
        break;
      }
      ::highcube::HostPortResult result;
      result.set_correlation_id(order.correlation_id());
      result.set_port(order.port());
      result.set_available(runtime_.isHostPortAvailable(order.port()));
      send(makeEnvelope(::highcube::HOST_PORT_RESULT, result));
      break;
    }
    case ::highcube::DB_MANAGE: {
      ::highcube::DbManage order;
      if (!order.ParseFromString(envelope.payload())) {
        break;
      }
      // SQL can take a few seconds; keep the socket loop responsive.
      auto self = shared_from_this();
      std::thread([self, order]() {
        const auto result = manageDatabase(order.container_id(), order.payload_json());
        ::highcube::DbManageResult msg;
        msg.set_correlation_id(order.correlation_id());
        msg.set_ok(result.ok);
        msg.set_error(result.error);
        msg.set_payload_json(result.payloadJson.empty() ? "{}" : result.payloadJson);
        net::post(self->ioc_, [self, msg]() {
          if (!self->connected_) {
            return;
          }
          self->send(makeEnvelope(::highcube::DB_MANAGE_RESULT, msg));
        });
      }).detach();
      break;
    }
    case ::highcube::QUICK_COMMAND: {
      ::highcube::QuickCommand order;
      if (order.ParseFromString(envelope.payload())) {
        runQuickCommand(order);
      }
      break;
    }
    case ::highcube::VPN_APPLY: {
      ::highcube::VpnApply order;
      if (order.ParseFromString(envelope.payload())) {
        // apt-get and wg-quick can take minutes; keep the socket loop responsive.
        auto self = shared_from_this();
        std::thread([self, configJson = order.config_json()]() {
          try {
            self->vpn_->apply(configJson);
            self->reportVpnStatus();
          } catch (const std::exception& ex) {
            spdlog::warn("vpn apply failed: {}", ex.what());
          }
        }).detach();
      }
      break;
    }
    case ::highcube::SET_HOSTNAME: {
      ::highcube::SetHostname order;
      if (order.ParseFromString(envelope.payload())) {
        auto self = shared_from_this();
        std::thread([self, order]() {
          ::highcube::HostnameResult result;
          result.set_correlation_id(order.correlation_id());
          result.set_hostname(order.hostname());
          try {
            const auto change = setHostname(order.hostname());
            result.set_ok(change.ok);
            result.set_error(change.error);
          } catch (const std::exception& ex) {
            result.set_ok(false);
            result.set_error(ex.what());
          }
          if (result.ok()) {
            spdlog::info("hostname changed to {}", order.hostname());
          } else {
            spdlog::warn("hostname change to {} failed: {}", order.hostname(), result.error());
          }
          net::post(self->ioc_, [self, result]() {
            if (self->connected_) {
              self->send(makeEnvelope(::highcube::HOSTNAME_RESULT, result));
            }
          });
        }).detach();
      }
      break;
    }
    default:
      break;
  }
}

void WsClient::send(const ::highcube::Envelope& envelope) {
  if (!connected_) {
    return;
  }
  auto data = std::make_shared<std::string>(envelope.SerializeAsString());
  net::post(ioc_, [self = shared_from_this(), data]() {
    if (!self->connected_) {
      return;
    }
    self->outgoing_.push_back(data);
    if (!self->writing_) {
      self->doWrite();
    }
  });
}

void WsClient::doWrite() {
  if (!connected_ || outgoing_.empty()) {
    writing_ = false;
    return;
  }
  writing_ = true;
  auto data = outgoing_.front();
  auto handler = [self = shared_from_this(), data](beast::error_code ec, std::size_t) {
    if (!self->outgoing_.empty() && self->outgoing_.front() == data) {
      self->outgoing_.pop_front();
    }
    if (ec) {
      self->writing_ = false;
      self->fail(ec, "write");
      return;
    }
    self->doWrite();
  };
  if (useTls_) {
    wss_->async_write(net::buffer(*data), handler);
  } else {
    ws_->async_write(net::buffer(*data), handler);
  }
}

void WsClient::closeTerminals() {
  auto sessions = std::move(terminals_);
  terminals_.clear();
  for (auto& [_, session] : sessions) {
    session->close();
  }
}

void WsClient::startHeartbeat() {
  heartbeatTimer_.expires_after(std::chrono::seconds(60));
  heartbeatTimer_.async_wait([self = shared_from_this()](beast::error_code ec) {
    if (ec || !self->connected_) {
      return;
    }
    ::highcube::Heartbeat hb;
    hb.set_unix_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count());
    self->send(makeEnvelope(::highcube::HEARTBEAT, hb));
    self->startHeartbeat();
  });
}

void WsClient::startMetrics() {
  metricsTimer_.expires_after(std::chrono::seconds(1));
  metricsTimer_.async_wait([self = shared_from_this()](beast::error_code ec) {
    if (ec || !self->connected_) {
      return;
    }
    ::highcube::HostMetrics metrics;
    metrics.set_payload_json(collectHostMetricsJson());
    metrics.set_collected_at_unix_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
    self->send(makeEnvelope(::highcube::HOST_METRICS, metrics));
    self->startMetrics();
  });
}

void WsClient::startProcessSnapshots() {
  processSnapshotTimer_.expires_after(std::chrono::seconds(60));
  processSnapshotTimer_.async_wait([self = shared_from_this()](beast::error_code ec) {
    if (ec || !self->connected_) {
      return;
    }
    // Walking /proc and resolving container names stays off the socket thread.
    if (!self->processSnapshotRunning_.exchange(true)) {
      std::thread([self]() {
        try {
          auto processes = self->processSampler_.sample();
          const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
          ::highcube::ProcessSnapshot snapshot;
          snapshot.set_payload_json(collectProcessSnapshotJson(self->processSampler_, processes));
          snapshot.set_collected_at_unix_ms(nowMs);
          ::highcube::CpuAttribution attribution;
          attribution.set_payload_json(
              collectCpuAttributionJson(self->processSampler_, processes, self->previousCgroupUsec_));
          attribution.set_collected_at_unix_ms(nowMs);
          net::post(self->ioc_, [self, snapshot, attribution]() {
            if (self->connected_) {
              self->send(makeEnvelope(::highcube::PROCESS_SNAPSHOT, snapshot));
              self->send(makeEnvelope(::highcube::CPU_ATTRIBUTION, attribution));
            }
          });
        } catch (const std::exception& ex) {
          spdlog::warn("process snapshot failed: {}", ex.what());
        }
        self->processSnapshotRunning_ = false;
      }).detach();
    }
    self->startProcessSnapshots();
  });
}

void WsClient::startVpnStatus() {
  vpnStatusTimer_.expires_after(std::chrono::seconds(5));
  vpnStatusTimer_.async_wait([self = shared_from_this()](beast::error_code ec) {
    if (ec || !self->connected_) {
      return;
    }
    if (!self->vpnStatusRunning_.exchange(true)) {
      std::thread([self]() {
        try {
          // Picks up the one-time helper setup without waiting for a new order.
          self->vpn_->retryPending();
          self->vpn_->heal();
          self->reportVpnStatus();
        } catch (const std::exception& ex) {
          spdlog::warn("vpn status failed: {}", ex.what());
        }
        self->vpnStatusRunning_ = false;
      }).detach();
    }
    self->startVpnStatus();
  });
}

// Runs on a worker thread: reading the status shells out to wg.
void WsClient::reportVpnStatus() {
  const auto payload = vpn_->statusJson();
  if (payload.empty()) {
    return;
  }
  ::highcube::VpnStatus status;
  status.set_payload_json(payload);
  net::post(ioc_, [self = shared_from_this(), status]() {
    if (self->connected_) {
      self->send(makeEnvelope(::highcube::VPN_STATUS, status));
    }
  });
}

void WsClient::runQuickCommand(const ::highcube::QuickCommand& order) {
  auto self = shared_from_this();
  const auto reply = [self](const ::highcube::QuickCommandResult& msg) {
    net::post(self->ioc_, [self, msg]() {
      if (self->connected_) {
        self->send(makeEnvelope(::highcube::QUICK_COMMAND_RESULT, msg));
      }
    });
  };
  // Audits walk the whole disk; a couple at a time is plenty for one host.
  if (quickCommandsRunning_.fetch_add(1) >= 3) {
    --quickCommandsRunning_;
    ::highcube::QuickCommandResult msg;
    msg.set_correlation_id(order.correlation_id());
    msg.set_ok(false);
    msg.set_error("Another analysis is already running on this server");
    reply(msg);
    return;
  }
  std::thread([self, order, reply]() {
    ::highcube::QuickCommandResult msg;
    msg.set_correlation_id(order.correlation_id());
    try {
      auto params = nlohmann::json::parse(order.payload_json().empty() ? "{}" : order.payload_json(), nullptr, false);
      if (!params.is_object()) {
        params = nlohmann::json::object();
      }
      nlohmann::json result;
      if (order.kind() == "audit") {
        const auto correlationId = order.correlation_id();
        result = audit::runAudit(order.name(), params, [self, correlationId](const nlohmann::json& check) {
          ::highcube::QuickCommandProgress progress;
          progress.set_correlation_id(correlationId);
          progress.set_check_json(check.dump());
          net::post(self->ioc_, [self, progress]() {
            if (self->connected_) {
              self->send(makeEnvelope(::highcube::QUICK_COMMAND_PROGRESS, progress));
            }
          });
        });
      } else if (order.kind() == "action") {
        spdlog::info("running quick action {}", order.name());
        result = audit::runAction(order.name(), params);
      } else {
        throw std::invalid_argument("Unknown quick command kind");
      }
      msg.set_ok(true);
      msg.set_payload_json(result.dump());
    } catch (const std::exception& ex) {
      msg.set_ok(false);
      msg.set_error(ex.what());
    }
    --self->quickCommandsRunning_;
    reply(msg);
  }).detach();
}

void WsClient::startContainerMetrics() {
  containerMetricsTimer_.expires_after(std::chrono::seconds(30));
  containerMetricsTimer_.async_wait([self = shared_from_this()](beast::error_code ec) {
    if (ec || !self->connected_) {
      return;
    }
    // A round shells out to Docker and can take many seconds; on this thread it would stall
    // host metrics, heartbeats and every order from the admin. Skip a tick while one runs.
    if (!self->containerReportRunning_.exchange(true)) {
      std::thread([self]() {
        try {
          self->reportContainerMetrics();
        } catch (const std::exception& ex) {
          spdlog::warn("container metrics failed: {}", ex.what());
        }
        self->containerReportRunning_ = false;
      }).detach();
    }
    self->startContainerMetrics();
  });
}

void WsClient::reportContainerMetrics() {
  bp::ipstream out;
  const int code = bp::system(bp::search_path("docker"), "ps", "--format", "{{.Names}}",
                              bp::std_out > out, bp::std_err > bp::null);
  if (code != 0) {
    return;
  }
  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  std::vector<std::pair<std::string, int64_t>> replicas;
  std::vector<std::string> names;
  std::string line;
  while (std::getline(out, line)) {
    int64_t replicaId = 0;
    if (!parseReplicaIdFromName(line, replicaId)) {
      continue;
    }
    replicas.emplace_back(line, replicaId);
    names.push_back(line);
  }
  const auto stats = runtime_.containerStatsByName(names);
  for (const auto& [name, replicaId] : replicas) {
    // Listed by docker ps means running, so it is reported even without a stats sample.
    const auto it = stats.find(name);
    ::highcube::ContainerMetrics metrics;
    metrics.set_replica_id(replicaId);
    metrics.set_payload_json(it == stats.end() ? "{}" : it->second);
    metrics.set_collected_at_unix_ms(nowMs);
    send(makeEnvelope(::highcube::CONTAINER_METRICS, metrics));
  }
  // replica_id 0 closes a complete round. The admin only treats a replica as gone when
  // complete rounds keep arriving without it.
  ::highcube::ContainerMetrics roundDone;
  roundDone.set_collected_at_unix_ms(nowMs);
  send(makeEnvelope(::highcube::CONTAINER_METRICS, roundDone));
}

void WsClient::scheduleReconnect() {
  if (reconnectPending_) {
    return;
  }
  reconnectPending_ = true;
  connected_ = false;
  reconnectTimer_.expires_after(std::chrono::seconds(5));
  reconnectTimer_.async_wait([self = shared_from_this()](beast::error_code ec) {
    self->reconnectPending_ = false;
    if (ec) {
      return;
    }
    spdlog::info("reconnecting to admin...");
    self->resolve();
  });
}

void WsClient::fail(beast::error_code ec, const char* what) {
  if (ec == net::error::operation_aborted) {
    return;
  }
  spdlog::warn("ws {}: {}", what, ec.message());
  connected_ = false;
  writing_ = false;
  outgoing_.clear();
  closeTerminals();
  scheduleReconnect();
}

}  // namespace highcube::agent
