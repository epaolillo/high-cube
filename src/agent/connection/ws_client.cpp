#include "connection/ws_client.hpp"

#include "metrics/host_metrics.hpp"
#include "protocol/codec.hpp"

#include <boost/process.hpp>
#include <openssl/err.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace bp = boost::process;

namespace highcube::agent {
namespace {

// Container names are hc-<application_id>-<replica_id>.
bool parseReplicaIdFromName(const std::string& name, int64_t& replicaId) {
  if (name.rfind("hc-", 0) != 0) {
    return false;
  }
  const auto lastDash = name.rfind('-');
  if (lastDash == std::string::npos || lastDash <= 2) {
    return false;
  }
  try {
    std::size_t idx = 0;
    replicaId = std::stoll(name.substr(lastDash + 1), &idx);
    return idx == name.size() - lastDash - 1 && replicaId > 0;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace

WsClient::WsClient(net::io_context& ioc, AgentConfig config, DockerRuntime& runtime)
    : ioc_(ioc),
      config_(std::move(config)),
      runtime_(runtime),
      resolver_(ioc),
      reconnectTimer_(ioc),
      heartbeatTimer_(ioc),
      metricsTimer_(ioc),
      containerMetricsTimer_(ioc) {
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
  auto sendStatus = [this](const ::highcube::ContainerStatus& status) {
    send(makeEnvelope(::highcube::CONTAINER_STATUS, status));
  };
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
        runtime_.handleStop(order, sendStatus);
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
        runtime_.handleRestart(order, sendStatus);
      }
      break;
    }
    case ::highcube::DELETE_VOLUME: {
      ::highcube::DeleteVolume order;
      if (order.ParseFromString(envelope.payload())) {
        runtime_.deleteVolume(order.volume_name());
      }
      break;
    }
    case ::highcube::RENAME_VOLUME: {
      ::highcube::RenameVolume order;
      if (order.ParseFromString(envelope.payload())) {
        try {
          runtime_.renameVolume(order.from_volume_name(), order.to_volume_name());
        } catch (const std::exception& ex) {
          spdlog::error("rename volume failed: {}", ex.what());
        }
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

void WsClient::startContainerMetrics() {
  containerMetricsTimer_.expires_after(std::chrono::seconds(30));
  containerMetricsTimer_.async_wait([self = shared_from_this()](beast::error_code ec) {
    if (ec || !self->connected_) {
      return;
    }
    self->reportContainerMetrics();
    self->startContainerMetrics();
  });
}

void WsClient::reportContainerMetrics() {
  bp::ipstream out;
  const int code = bp::system(bp::search_path("docker"), "ps", "--format", "{{.Names}} {{.ID}}",
                              bp::std_out > out, bp::std_err > bp::null);
  if (code != 0) {
    return;
  }
  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  std::string line;
  while (std::getline(out, line)) {
    if (line.empty()) {
      continue;
    }
    const auto space = line.find(' ');
    const std::string name = space == std::string::npos ? line : line.substr(0, space);
    const std::string containerId = space == std::string::npos ? name : line.substr(space + 1);
    int64_t replicaId = 0;
    if (!parseReplicaIdFromName(name, replicaId)) {
      continue;
    }
    ::highcube::ContainerMetrics metrics;
    metrics.set_replica_id(replicaId);
    metrics.set_payload_json(runtime_.containerStatsJson(containerId));
    metrics.set_collected_at_unix_ms(nowMs);
    send(makeEnvelope(::highcube::CONTAINER_METRICS, metrics));
  }
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
