#include "agents/agent_hub.hpp"

#include "agents/host_terminal.hpp"
#include "api/http_types.hpp"
#include "auth/auth_middleware.hpp"
#include "proxy/nginx_proxy.hpp"
#include "services/services.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <unordered_set>

namespace highcube::admin {
namespace {

std::string nowIso() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

::highcube::Envelope makeEnvelope(::highcube::MessageKind kind, const google::protobuf::Message& payload) {
  ::highcube::Envelope envelope;
  envelope.set_kind(kind);
  envelope.set_payload(payload.SerializeAsString());
  return envelope;
}

void rejectSocket(tcp::socket& socket, const http::request<http::string_body>& req, http::status status,
                  const std::string& message) {
  Response res{status, req.version()};
  res.set(http::field::content_type, "application/json");
  res.set(http::field::access_control_allow_origin, "*");
  res.body() = nlohmann::json{{"error", message}}.dump();
  res.prepare_payload();
  beast::error_code ec;
  http::write(socket, res, ec);
}

std::uint32_t queryUint(const std::unordered_map<std::string, std::string>& query, const std::string& key,
                        std::uint32_t fallback, std::uint32_t min, std::uint32_t max) {
  const auto it = query.find(key);
  if (it == query.end() || it->second.empty()) {
    return fallback;
  }
  try {
    const auto value = std::stoul(it->second);
    if (value < min) {
      return min;
    }
    if (value > max) {
      return max;
    }
    return static_cast<std::uint32_t>(value);
  } catch (const std::exception&) {
    return fallback;
  }
}

std::string makeStreamId() {
  std::random_device device;
  std::mt19937_64 rng(device());
  std::ostringstream oss;
  oss << std::hex << rng();
  return oss.str();
}

}  // namespace

bool parseTerminalPath(const std::string& path, const std::string& prefix, int64_t* outId,
                       const std::string& suffix = "/terminal") {
  if (path.size() <= prefix.size() + suffix.size()) {
    return false;
  }
  if (path.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }
  if (path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return false;
  }
  const auto idPart = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
  if (idPart.empty() || idPart.find('/') != std::string::npos) {
    return false;
  }
  try {
    std::size_t idx = 0;
    const auto id = std::stoll(idPart, &idx);
    if (idx != idPart.size() || id <= 0) {
      return false;
    }
    if (outId) {
      *outId = id;
    }
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool isHostTerminalPath(const std::string& path, int64_t* serverId) {
  return parseTerminalPath(path, "/api/servers/", serverId);
}

bool isReplicaTerminalPath(const std::string& path, int64_t* replicaId) {
  return parseTerminalPath(path, "/api/replicas/", replicaId);
}

bool isReplicaLogsPath(const std::string& path, int64_t* replicaId) {
  return parseTerminalPath(path, "/api/replicas/", replicaId, "/logs");
}

AgentHub::AgentHub(net::io_context& ioc, Repositories& repos) : ioc_(ioc), repos_(repos) {}

void AgentHub::setDeploymentService(DeploymentService* deployments) {
  deployments_ = deployments;
}

void AgentHub::setProxy(NginxProxy* proxy) {
  proxy_ = proxy;
}

void AgentHub::setAuth(AuthMiddleware* auth) {
  auth_ = auth;
}

void AgentHub::setGithubApps(GithubAppService* githubApps) {
  githubApps_ = githubApps;
}

void AgentHub::onUpgrade(tcp::socket socket, http::request<http::string_body> req) {
  const auto path = pathOnly(std::string(req.target()));
  int64_t serverId = 0;
  if (isHostTerminalPath(path, &serverId)) {
    onHostTerminal(std::move(socket), std::move(req), serverId);
    return;
  }
  int64_t replicaId = 0;
  if (isReplicaTerminalPath(path, &replicaId)) {
    onReplicaTerminal(std::move(socket), std::move(req), replicaId, false);
    return;
  }
  if (isReplicaLogsPath(path, &replicaId)) {
    onReplicaTerminal(std::move(socket), std::move(req), replicaId, true);
    return;
  }
  onAgentUpgrade(std::move(socket), std::move(req));
}

void AgentHub::onAgentUpgrade(tcp::socket socket, http::request<http::string_body> req) {
  auto session = std::make_shared<AgentSession>(
      std::move(socket),
      [self = shared_from_this()](AgentSession::Pointer s, const ::highcube::Envelope& env) {
        try {
          self->onMessage(s, env);
        } catch (const std::exception& ex) {
          spdlog::error("agent message failed: {}", ex.what());
        }
      });
  session->start(std::move(req), [self = shared_from_this()](AgentSession::Pointer s) {
    self->onSessionClosed(s);
  });
}

void AgentHub::onHostTerminal(tcp::socket socket, http::request<http::string_body> req, int64_t serverId) {
  if (!auth_) {
    rejectSocket(socket, req, http::status::unauthorized, "Unauthorized");
    return;
  }
  auto header = std::string(req[http::field::authorization]);
  const auto query = parseQuery(std::string(req.target()));
  if (header.empty()) {
    const auto token = query.find("token");
    if (token != query.end() && !token->second.empty()) {
      header = "Bearer " + token->second;
    }
  }
  const auto user = auth_->authenticate(header);
  if (!user) {
    rejectSocket(socket, req, http::status::unauthorized, "Unauthorized");
    return;
  }
  if (!auth_->isAdmin(*user)) {
    rejectSocket(socket, req, http::status::forbidden, "Forbidden");
    return;
  }
  if (!sessionForServer(serverId)) {
    rejectSocket(socket, req, http::status::conflict, "Agent is not connected");
    return;
  }
  const auto cols = queryUint(query, "cols", 120, 40, 300);
  const auto rows = queryUint(query, "rows", 32, 10, 80);
  const auto streamId = makeStreamId();
  auto relay = std::make_shared<HostTerminalRelay>(std::move(socket), *this, serverId, streamId, cols, rows);
  relay->start(std::move(req));
}

void AgentHub::onReplicaTerminal(tcp::socket socket, http::request<http::string_body> req,
                                 int64_t replicaId, bool logs) {
  if (!auth_) {
    rejectSocket(socket, req, http::status::unauthorized, "Unauthorized");
    return;
  }
  auto header = std::string(req[http::field::authorization]);
  const auto query = parseQuery(std::string(req.target()));
  if (header.empty()) {
    const auto token = query.find("token");
    if (token != query.end() && !token->second.empty()) {
      header = "Bearer " + token->second;
    }
  }
  const auto user = auth_->authenticate(header);
  if (!user) {
    rejectSocket(socket, req, http::status::unauthorized, "Unauthorized");
    return;
  }
  auto replica = repos_.findReplicaById(replicaId);
  auto app = replica ? repos_.findApplicationById(replica->applicationId) : std::nullopt;
  if (!replica || !app) {
    rejectSocket(socket, req, http::status::not_found, "Replica not found");
    return;
  }
  if (!auth_->canAccessApplication(*user, *app)) {
    rejectSocket(socket, req, http::status::forbidden, "Forbidden");
    return;
  }
  if (replica->containerId.empty()) {
    rejectSocket(socket, req, http::status::conflict, "Container is not ready");
    return;
  }
  if (!sessionForServer(replica->serverId)) {
    rejectSocket(socket, req, http::status::conflict, "Agent is not connected");
    return;
  }
  const auto cols = queryUint(query, "cols", 120, 40, 300);
  const auto rows = queryUint(query, "rows", 32, 10, 80);
  const auto streamId = makeStreamId();
  auto relay = std::make_shared<HostTerminalRelay>(
      std::move(socket), *this, replica->serverId, streamId, cols, rows,
      logs ? HostTerminalRelay::Kind::Logs : HostTerminalRelay::Kind::Container, replicaId);
  relay->start(std::move(req));
}

void AgentHub::onMessage(AgentSession::Pointer session, const ::highcube::Envelope& envelope) {
  switch (envelope.kind()) {
    case ::highcube::REGISTER: {
      ::highcube::Register msg;
      if (msg.ParseFromString(envelope.payload())) {
        handleRegister(session, msg);
      }
      break;
    }
    case ::highcube::HEARTBEAT:
      handleHeartbeat(session);
      break;
    case ::highcube::HOST_METRICS: {
      ::highcube::HostMetrics msg;
      if (msg.ParseFromString(envelope.payload())) {
        handleHostMetrics(session, msg);
      }
      break;
    }
    case ::highcube::CONTAINER_STATUS: {
      ::highcube::ContainerStatus msg;
      if (msg.ParseFromString(envelope.payload())) {
        handleContainerStatus(session, msg);
      }
      break;
    }
    case ::highcube::CONTAINER_METRICS: {
      ::highcube::ContainerMetrics msg;
      if (msg.ParseFromString(envelope.payload())) {
        handleContainerMetrics(session, msg);
      }
      break;
    }
    case ::highcube::HOST_PORT_RESULT: {
      ::highcube::HostPortResult msg;
      if (msg.ParseFromString(envelope.payload())) {
        handleHostPortResult(msg);
      }
      break;
    }
    case ::highcube::DB_MANAGE_RESULT: {
      ::highcube::DbManageResult msg;
      if (msg.ParseFromString(envelope.payload())) {
        handleDbManageResult(msg);
      }
      break;
    }
    case ::highcube::DEPLOYMENT_LOG: {
      ::highcube::DeploymentLog msg;
      if (msg.ParseFromString(envelope.payload())) {
        handleDeploymentLog(msg);
      }
      break;
    }
    case ::highcube::TERMINAL_OUTPUT: {
      ::highcube::TerminalOutput msg;
      if (msg.ParseFromString(envelope.payload())) {
        forwardStreamOutput(msg.stream_id(), msg.data());
      }
      break;
    }
    case ::highcube::LOG_CHUNK: {
      ::highcube::LogChunk msg;
      if (msg.ParseFromString(envelope.payload())) {
        forwardStreamOutput(msg.stream_id(), msg.data());
      }
      break;
    }
    default:
      break;
  }
}

void AgentHub::forwardStreamOutput(const std::string& streamId, const std::string& data) {
  std::function<void(const std::string&)> sink;
  {
    std::lock_guard lock(mutex_);
    auto it = terminals_.find(streamId);
    if (it != terminals_.end()) {
      sink = it->second.onData;
    }
  }
  if (sink) {
    sink(data);
  }
}

void AgentHub::handleRegister(AgentSession::Pointer session, const ::highcube::Register& msg) {
  const auto hash = hashToken(msg.agent_token());
  auto server = repos_.findServerByTokenHash(hash);
  if (!server) {
    spdlog::warn("rejecting unknown agent token for {}", msg.server_name());
    session->close();
    return;
  }
  // The address is chosen in the admin UI and must survive agent reconnects.
  server->status = "online";
  repos_.updateServer(*server);
  repos_.touchServer(server->id, "online");
  session->setServerId(server->id);
  session->setServerName(server->name);
  {
    std::lock_guard lock(mutex_);
    sessionsByServer_[server->id] = session;
    containerRounds_.erase(server->id);
  }
  spdlog::info("agent registered: {} ({})", server->name, server->id);
  reconcileServer(server->id);
}

void AgentHub::handleHeartbeat(AgentSession::Pointer session) {
  if (session->serverId() == 0) {
    return;
  }
  repos_.touchServer(session->serverId(), "online");
}

void AgentHub::handleHostMetrics(AgentSession::Pointer session, const ::highcube::HostMetrics& msg) {
  if (session->serverId() == 0) {
    return;
  }
  repos_.insertMetric(session->serverId(), std::nullopt, msg.payload_json(), nowIso());
}

void AgentHub::handleContainerStatus(AgentSession::Pointer session, const ::highcube::ContainerStatus& msg) {
  if (msg.replica_id() > 0 && !repos_.findReplicaById(msg.replica_id())) {
    // "stopped" is the ack for a removal we already requested. Stopping again would loop.
    if (msg.status() != "stopped") {
      spdlog::warn("stopping container for removed replica {}", msg.replica_id());
      ::highcube::StopReplica order;
      order.set_replica_id(msg.replica_id());
      order.set_container_id(msg.container_id());
      session->send(makeEnvelope(::highcube::STOP_REPLICA, order));
    }
    return;
  }
  if (deployments_) {
    deployments_->onContainerStatus(msg);
  }
}

void AgentHub::handleHostPortResult(const ::highcube::HostPortResult& msg) {
  std::lock_guard lock(mutex_);
  auto it = portChecks_.find(msg.correlation_id());
  if (it == portChecks_.end()) {
    return;
  }
  it->second.done = true;
  it->second.available = msg.available();
}

std::string AgentHub::startHostPortCheck(int port, const std::vector<int64_t>& serverIds) {
  // Snapshot the target sessions under the lock, then send outside of it.
  std::vector<std::pair<std::string, AgentSession::Pointer>> targets;
  const std::string batchId =
      "pc-" + std::to_string(++portCheckSeq_) + "-" + std::to_string(port);
  {
    std::lock_guard lock(mutex_);
    std::vector<std::string> corrIds;
    const auto now = std::chrono::steady_clock::now();
    auto consider = [&](int64_t serverId, const AgentSession::Pointer& session) {
      const std::string corr = batchId + "-" + std::to_string(serverId);
      portChecks_[corr] = PortCheckEntry{false, false, now};
      corrIds.push_back(corr);
      targets.emplace_back(corr, session);
    };
    if (serverIds.empty()) {
      for (const auto& [serverId, session] : sessionsByServer_) {
        consider(serverId, session);
      }
    } else {
      for (const int64_t serverId : serverIds) {
        auto it = sessionsByServer_.find(serverId);
        if (it != sessionsByServer_.end()) {
          consider(serverId, it->second);
        }
      }
    }
    portCheckBatches_[batchId] = std::move(corrIds);
  }
  for (const auto& [corr, session] : targets) {
    ::highcube::CheckHostPort order;
    order.set_correlation_id(corr);
    order.set_port(port);
    session->send(makeEnvelope(::highcube::CHECK_HOST_PORT, order));
  }
  return batchId;
}

void AgentHub::handleDbManageResult(const ::highcube::DbManageResult& msg) {
  {
    std::lock_guard lock(mutex_);
    auto it = dbManage_.find(msg.correlation_id());
    if (it == dbManage_.end()) {
      return;
    }
    it->second.done = true;
    it->second.ok = msg.ok();
    it->second.error = msg.error();
    it->second.resultJson = msg.payload_json().empty() ? "{}" : msg.payload_json();
  }
  finishDbWaiter(msg.correlation_id(), 0);
}

void AgentHub::applyDbManageEffect(const DbManageEffect& effect, const std::string& resultJson) {
  if (effect.applicationId <= 0) {
    return;
  }
  if (!effect.passwordEnvKey.empty()) {
    auto app = repos_.findApplicationById(effect.applicationId);
    if (app) {
      auto env = nlohmann::json::parse(app->envJson.empty() ? "{}" : app->envJson, nullptr, false);
      if (!env.is_object()) {
        env = nlohmann::json::object();
      }
      env[effect.passwordEnvKey] = effect.newPassword;
      repos_.updateApplicationEnv(app->id, env.dump());
    }
  }
  if (!effect.healthCommand.empty()) {
    repos_.updateHealthCheckCommand(effect.applicationId, effect.healthCommand);
    const auto result = nlohmann::json::parse(resultJson.empty() ? "{}" : resultJson, nullptr, false);
    if (result.is_object() && result.value("healthUpdated", true) == false) {
      repos_.setPendingRedeploy(effect.applicationId, true);
    }
  }
}

std::string AgentHub::startDbManage(int64_t serverId, int64_t applicationId,
                                    const std::string& containerId, const std::string& payloadJson,
                                    const DbManageEffect& effect) {
  AgentSession::Pointer session;
  std::string requestId;
  {
    std::lock_guard lock(mutex_);
    auto it = sessionsByServer_.find(serverId);
    if (it == sessionsByServer_.end()) {
      throw std::runtime_error("The server agent is offline");
    }
    session = it->second;
    const auto now = std::chrono::steady_clock::now();
    for (auto entry = dbManage_.begin(); entry != dbManage_.end();) {
      if (now - entry->second.createdAt > std::chrono::minutes(10)) {
        entry = dbManage_.erase(entry);
      } else {
        ++entry;
      }
    }
    requestId = "db-" + std::to_string(++dbManageSeq_) + "-" + std::to_string(applicationId);
    DbManageEntry row;
    row.applicationId = applicationId;
    row.effect = effect;
    row.createdAt = now;
    dbManage_[requestId] = std::move(row);
  }
  ::highcube::DbManage order;
  order.set_correlation_id(requestId);
  order.set_container_id(containerId);
  order.set_payload_json(payloadJson);
  session->send(makeEnvelope(::highcube::DB_MANAGE, order));
  return requestId;
}

AgentHub::DbManagePoll AgentHub::pollDbManage(const std::string& requestId, int64_t applicationId) {
  DbManagePoll poll;
  DbManageEffect effect;
  std::string resultJson;
  bool apply = false;
  {
    std::lock_guard lock(mutex_);
    auto it = dbManage_.find(requestId);
    if (it == dbManage_.end() || it->second.applicationId != applicationId) {
      poll.status = "unknown";
      return poll;
    }
    auto& entry = it->second;
    if (!entry.done &&
        std::chrono::steady_clock::now() - entry.createdAt > std::chrono::seconds(60)) {
      entry.done = true;
      entry.ok = false;
      entry.error = "The database server did not respond in time";
    }
    if (!entry.done) {
      poll.status = "pending";
      return poll;
    }
    if (!entry.ok) {
      poll.status = entry.error == "The database server did not respond in time" ? "timeout" : "error";
      poll.error = entry.error.empty() ? "database command failed" : entry.error;
      return poll;
    }
    poll.status = "ok";
    poll.resultJson = entry.resultJson.empty() ? "{}" : entry.resultJson;
    if (!entry.effectApplied &&
        (!entry.effect.passwordEnvKey.empty() || !entry.effect.healthCommand.empty())) {
      apply = true;
      effect = entry.effect;
      resultJson = entry.resultJson;
    }
  }
  if (apply) {
    try {
      applyDbManageEffect(effect, resultJson);
      std::lock_guard lock(mutex_);
      auto it = dbManage_.find(requestId);
      if (it != dbManage_.end()) {
        it->second.effectApplied = true;
        it->second.effect.newPassword.clear();
      }
    } catch (const std::exception& ex) {
      spdlog::error("db manage side effect failed: {}", ex.what());
    }
  }
  return poll;
}

namespace {

constexpr auto kDbPollHold = std::chrono::seconds(12);

Response dbManagePollResponse(const AgentHub::DbManagePoll& poll) {
  nlohmann::json body = {{"status", poll.status}};
  if (!poll.error.empty()) {
    body["error"] = poll.error;
  } else if (poll.status == "unknown") {
    body["error"] = "The management request was not found";
  }
  if (poll.status == "ok") {
    auto result = nlohmann::json::parse(poll.resultJson.empty() ? "{}" : poll.resultJson, nullptr, false);
    body["result"] = result.is_discarded() ? nlohmann::json::object() : result;
  }
  return jsonResponse(http::status::ok, body);
}

bool parseDbManagePollPath(const std::string& path, int64_t& applicationId, std::string& requestId) {
  const std::string prefix = "/api/applications/";
  if (path.rfind(prefix, 0) != 0) {
    return false;
  }
  const auto rest = path.substr(prefix.size());
  const auto slash = rest.find('/');
  if (slash == std::string::npos) {
    return false;
  }
  const std::string tail = rest.substr(slash + 1);
  const std::string marker = "db-manage/";
  if (tail.rfind(marker, 0) != 0) {
    return false;
  }
  requestId = tail.substr(marker.size());
  if (requestId.empty() || requestId.find('/') != std::string::npos) {
    return false;
  }
  try {
    applicationId = std::stoll(rest.substr(0, slash));
  } catch (const std::exception&) {
    return false;
  }
  return applicationId > 0;
}

}  // namespace

void AgentHub::finishDbWaiter(const std::string& requestId, std::uint64_t waiterId) {
  std::function<void(Response)> reply;
  int64_t applicationId = 0;
  {
    std::lock_guard lock(mutex_);
    auto it = dbWaiters_.find(requestId);
    if (it == dbWaiters_.end()) {
      return;
    }
    if (waiterId != 0 && it->second.id != waiterId) {
      return;
    }
    reply = std::move(it->second.reply);
    applicationId = it->second.applicationId;
    if (it->second.timer) {
      it->second.timer->cancel();
    }
    dbWaiters_.erase(it);
  }
  if (reply) {
    reply(dbManagePollResponse(pollDbManage(requestId, applicationId)));
  }
}

bool AgentHub::tryWaitDbManage(const http::request<http::string_body>& request,
                               const std::function<void(Response)>& reply) {
  if (request.method() != http::verb::get) {
    return false;
  }
  int64_t applicationId = 0;
  std::string requestId;
  if (!parseDbManagePollPath(pathOnly(std::string(request.target())), applicationId, requestId)) {
    return false;
  }
  if (!auth_) {
    reply(errorResponse(http::status::internal_server_error, "Auth is not configured"));
    return true;
  }
  const auto user = auth_->authenticate(std::string(request[http::field::authorization]));
  if (!user) {
    reply(errorResponse(http::status::unauthorized, "Unauthorized"));
    return true;
  }
  const auto app = repos_.findApplicationById(applicationId);
  if (!app) {
    reply(errorResponse(http::status::conflict, "Application not found"));
    return true;
  }
  if (!auth_->canAccessApplication(*user, *app)) {
    reply(errorResponse(http::status::forbidden, "Forbidden"));
    return true;
  }

  const auto current = pollDbManage(requestId, applicationId);
  if (current.status != "pending") {
    reply(dbManagePollResponse(current));
    return true;
  }

  // A newer poll replaces the previous hold so that request can finish.
  finishDbWaiter(requestId, 0);

  DbWaiter waiter;
  waiter.id = ++dbWaiterSeq_;
  waiter.applicationId = applicationId;
  waiter.reply = reply;
  waiter.timer = std::make_shared<net::steady_timer>(ioc_, kDbPollHold);
  const auto waiterId = waiter.id;
  waiter.timer->async_wait([weak = weak_from_this(), requestId, waiterId](const boost::system::error_code& ec) {
    if (ec) {
      return;
    }
    if (auto self = weak.lock()) {
      self->finishDbWaiter(requestId, waiterId);
    }
  });
  {
    std::lock_guard lock(mutex_);
    dbWaiters_[requestId] = std::move(waiter);
  }
  // The agent may have answered while the waiter was being armed.
  if (pollDbManage(requestId, applicationId).status != "pending") {
    finishDbWaiter(requestId, waiterId);
  }
  return true;
}

AgentHub::PortCheckState AgentHub::pollHostPortCheck(const std::string& batchId) {
  std::lock_guard lock(mutex_);
  auto batchIt = portCheckBatches_.find(batchId);
  if (batchIt == portCheckBatches_.end()) {
    return PortCheckState::Unknown;
  }
  const auto& corrIds = batchIt->second;
  if (corrIds.empty()) {
    // No online agent could verify the port; let the caller decide (soft allow).
    return PortCheckState::Unknown;
  }
  const auto now = std::chrono::steady_clock::now();
  bool anyPending = false;
  bool allAvailable = true;
  bool timedOut = false;
  for (const auto& corr : corrIds) {
    auto it = portChecks_.find(corr);
    if (it == portChecks_.end()) {
      continue;
    }
    if (!it->second.done) {
      if (now - it->second.createdAt > std::chrono::seconds(6)) {
        timedOut = true;
      } else {
        anyPending = true;
      }
      continue;
    }
    if (!it->second.available) {
      allAvailable = false;
    }
  }
  if (anyPending) {
    return PortCheckState::Pending;
  }
  if (timedOut) {
    // Drop the batch so the registry does not grow unbounded.
    for (const auto& corr : corrIds) {
      portChecks_.erase(corr);
    }
    portCheckBatches_.erase(batchIt);
    return PortCheckState::Unknown;
  }
  for (const auto& corr : corrIds) {
    portChecks_.erase(corr);
  }
  portCheckBatches_.erase(batchIt);
  return allAvailable ? PortCheckState::Available : PortCheckState::Unavailable;
}

void AgentHub::handleContainerMetrics(AgentSession::Pointer session,
                                      const ::highcube::ContainerMetrics& msg) {
  if (session->serverId() == 0) {
    return;
  }
  if (msg.replica_id() <= 0) {
    // End of a complete round: every running container was reported just before this.
    std::lock_guard lock(mutex_);
    auto& rounds = containerRounds_[session->serverId()];
    rounds.push_back(std::chrono::system_clock::now());
    while (rounds.size() > 5) {
      rounds.pop_front();
    }
    return;
  }
  // A removed replica must not keep serving. The row is already gone, so tell this agent
  // to delete the container instead of storing metrics for an id that no longer exists.
  auto replica = repos_.findReplicaById(msg.replica_id());
  if (!replica) {
    static std::unordered_set<int64_t> reported;
    if (reported.insert(msg.replica_id()).second) {
      spdlog::warn("removing stale container for unknown replica {}", msg.replica_id());
    }
    ::highcube::StopReplica order;
    order.set_replica_id(msg.replica_id());
    session->send(makeEnvelope(::highcube::STOP_REPLICA, order));
    return;
  }

  const auto now = nowIso();
  int restartCount = replica->restartCount;
  const auto payload = nlohmann::json::parse(msg.payload_json(), nullptr, false);
  if (payload.is_object() && payload.contains("restartCount")) {
    restartCount = payload.value("restartCount", replica->restartCount);
  }
  // Trace Docker-level restarts: the container fell and was brought back up by the
  // "unless-stopped" policy. We only record the transition, not every heartbeat.
  if (restartCount > replica->restartCount) {
    ReplicaIncident incident;
    incident.applicationId = replica->applicationId;
    incident.replicaId = replica->id;
    incident.serverId = replica->serverId;
    incident.kind = "restart";
    incident.detail = "Container auto-restarted by Docker (restarts " +
                      std::to_string(replica->restartCount) + " -> " +
                      std::to_string(restartCount) + ")";
    incident.restartCount = restartCount;
    repos_.recordReplicaIncident(incident);
    spdlog::warn("replica {} restarted by Docker (count {})", replica->id, restartCount);
  }
  // Refresh liveness so the supervisor can tell this container is still alive.
  repos_.touchReplicaSeen(replica->id, restartCount, now);
  // An empty payload only proves the container runs; storing it would chart zeros.
  if (payload.is_object() && !payload.empty()) {
    repos_.insertMetric(session->serverId(), msg.replica_id(), msg.payload_json(), now);
  }
}

void AgentHub::handleDeploymentLog(const ::highcube::DeploymentLog& msg) {
  if (deployments_) {
    deployments_->onDeploymentLog(msg);
  }
}

void AgentHub::onSessionClosed(AgentSession::Pointer session) {
  std::vector<std::function<void()>> closers;
  bool wasActive = false;
  {
    std::lock_guard lock(mutex_);
    if (session->serverId() != 0) {
      auto it = sessionsByServer_.find(session->serverId());
      // A session already replaced by a reconnect must not take the server offline.
      if (it != sessionsByServer_.end() && it->second == session) {
        sessionsByServer_.erase(it);
        containerRounds_.erase(session->serverId());
        wasActive = true;
      }
    }
    for (auto it = terminals_.begin(); it != terminals_.end();) {
      if (it->second.serverId == session->serverId()) {
        if (it->second.onClose) {
          closers.push_back(it->second.onClose);
        }
        it = terminals_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& close : closers) {
    close();
  }
  if (wasActive) {
    spdlog::warn("agent disconnected: {} ({})", session->serverName(), session->serverId());
    try {
      repos_.setServerStatus(session->serverId(), "offline");
    } catch (const std::exception& ex) {
      spdlog::error("failed to mark server {} offline: {}", session->serverId(), ex.what());
    }
  }
}

void AgentHub::registerHostTerminal(const std::string& streamId, int64_t serverId,
                                    std::function<void(const std::string&)> onData,
                                    std::function<void()> onClose) {
  std::lock_guard lock(mutex_);
  terminals_[streamId] = TerminalLink{serverId, std::move(onData), std::move(onClose)};
}

void AgentHub::openHostTerminal(int64_t serverId, const std::string& streamId, std::uint32_t cols,
                                std::uint32_t rows) {
  auto session = sessionForServer(serverId);
  if (!session) {
    return;
  }
  ::highcube::OpenHostTerminal order;
  order.set_stream_id(streamId);
  order.set_command("/bin/bash");
  order.set_cols(cols);
  order.set_rows(rows);
  session->send(makeEnvelope(::highcube::OPEN_HOST_TERMINAL, order));
}

void AgentHub::openContainerTerminal(int64_t replicaId, const std::string& streamId,
                                     std::uint32_t cols, std::uint32_t rows) {
  auto replica = repos_.findReplicaById(replicaId);
  auto session = replica ? sessionForServer(replica->serverId) : nullptr;
  if (!replica || !session || replica->containerId.empty()) {
    return;
  }
  ::highcube::OpenContainerTerminal order;
  order.set_stream_id(streamId);
  order.set_replica_id(replica->id);
  order.set_container_id(replica->containerId);
  order.set_command("/bin/sh");
  order.set_cols(cols);
  order.set_rows(rows);
  session->send(makeEnvelope(::highcube::OPEN_CONTAINER_TERMINAL, order));
}

void AgentHub::openReplicaLogs(int64_t replicaId, const std::string& streamId) {
  auto replica = repos_.findReplicaById(replicaId);
  auto session = replica ? sessionForServer(replica->serverId) : nullptr;
  if (!replica || !session || replica->containerId.empty()) {
    return;
  }
  ::highcube::SubscribeLogs order;
  order.set_stream_id(streamId);
  order.set_replica_id(replica->id);
  order.set_container_id(replica->containerId);
  session->send(makeEnvelope(::highcube::SUBSCRIBE_LOGS, order));
}

void AgentHub::writeHostTerminal(int64_t serverId, const std::string& streamId, const std::string& data) {
  auto session = sessionForServer(serverId);
  if (!session) {
    return;
  }
  ::highcube::TerminalInput order;
  order.set_stream_id(streamId);
  order.set_data(data);
  session->send(makeEnvelope(::highcube::TERMINAL_INPUT, order));
}

void AgentHub::resizeHostTerminal(int64_t serverId, const std::string& streamId, std::uint32_t cols,
                                  std::uint32_t rows) {
  auto session = sessionForServer(serverId);
  if (!session || cols == 0 || rows == 0) {
    return;
  }
  ::highcube::ResizeTerminal order;
  order.set_stream_id(streamId);
  order.set_cols(cols);
  order.set_rows(rows);
  session->send(makeEnvelope(::highcube::RESIZE_TERMINAL, order));
}

void AgentHub::closeHostTerminal(int64_t serverId, const std::string& streamId) {
  AgentSession::Pointer session;
  {
    std::lock_guard lock(mutex_);
    terminals_.erase(streamId);
    session = [&]() -> AgentSession::Pointer {
      auto it = sessionsByServer_.find(serverId);
      if (it == sessionsByServer_.end()) {
        return nullptr;
      }
      return it->second;
    }();
  }
  if (!session) {
    return;
  }
  ::highcube::CloseStream order;
  order.set_stream_id(streamId);
  session->send(makeEnvelope(::highcube::CLOSE_STREAM, order));
}

bool AgentHub::isConnected(int64_t serverId) {
  return sessionForServer(serverId) != nullptr;
}

AgentSession::Pointer AgentHub::sessionForServer(int64_t serverId) {
  std::lock_guard lock(mutex_);
  auto it = sessionsByServer_.find(serverId);
  if (it == sessionsByServer_.end()) {
    return nullptr;
  }
  return it->second;
}

void AgentHub::deployReplica(int64_t replicaId) {
  auto replica = repos_.findReplicaById(replicaId);
  auto deployment = replica ? repos_.findDeploymentById(replica->deploymentId) : std::nullopt;
  auto app = replica ? repos_.findApplicationById(replica->applicationId) : std::nullopt;
  auto session = replica ? sessionForServer(replica->serverId) : nullptr;
  if (!replica || !deployment || !app || !session) {
    spdlog::warn("cannot deploy replica {}", replicaId);
    return;
  }

  ::highcube::DeployReplica order;
  order.set_replica_id(replica->id);
  order.set_application_id(app->id);
  order.set_deployment_id(deployment->id);
  order.set_application_type(app->type);
  order.set_network_alias("hc-" + std::to_string(app->id));
  order.set_repo_url(app->repoUrl);
  order.set_branch(app->branch);
  order.set_commit_sha(deployment->gitSha);
  order.set_base_directory(app->baseDirectory);
  order.set_dockerfile_path(app->dockerfilePath);
  order.set_deploy_key_private(app->deployKeyPrivate);
  order.set_ports_exposes(app->portsExposes);
  order.set_health_check_type(app->healthCheckType);
  order.set_health_check_path(app->healthCheckPath);
  order.set_health_check_command(app->healthCheckCommand);
  order.set_max_restart_count(app->maxRestartCount);
  order.set_no_cache(deployment->noCache);
  order.set_use_gpu(app->useGpu);
  for (const auto& mapping : app->portMappings) {
    auto* port = order.add_port_mappings();
    port->set_container_port(mapping.containerPort);
    port->set_host_port(mapping.hostPort);
  }

  if (app->githubAppId && githubApps_) {
    try {
      order.set_git_access_token(githubApps_->installationAccessToken(*app->githubAppId));
    } catch (const std::exception& ex) {
      spdlog::error("failed to mint GitHub token for app {}: {}", app->id, ex.what());
      repos_.appendDeploymentLog(deployment->id,
                                 std::string("GitHub access token failed: ") + ex.what() + "\n");
      return;
    }
  }

  auto env = nlohmann::json::parse(app->envJson.empty() ? "{}" : app->envJson);
  for (auto it = env.begin(); it != env.end(); ++it) {
    (*order.mutable_env())[it.key()] = it.value().is_string() ? it.value().get<std::string>()
                                                              : it.value().dump();
  }
  for (const auto& volume : app->volumes) {
    auto* v = order.add_volumes();
    v->set_name(volume.name);
    v->set_mount_path(volume.mountPath);
    v->set_host_path(volume.hostPath);
  }

  session->send(makeEnvelope(::highcube::DEPLOY_REPLICA, order));
}

void AgentHub::stopReplica(int64_t replicaId) {
  auto replica = repos_.findReplicaById(replicaId);
  if (!replica) {
    return;
  }
  stopContainer(replica->serverId, replica->id, replica->applicationId, replica->containerId);
}

void AgentHub::stopContainer(int64_t serverId, int64_t replicaId, int64_t applicationId,
                             const std::string& containerId) {
  auto session = sessionForServer(serverId);
  if (!session) {
    spdlog::warn("agent offline for server {}; replica {} will be removed on reconnect",
                 serverId, replicaId);
    return;
  }
  ::highcube::StopReplica order;
  order.set_replica_id(replicaId);
  order.set_application_id(applicationId);
  order.set_container_id(containerId);
  session->send(makeEnvelope(::highcube::STOP_REPLICA, order));
}

void AgentHub::reconcileServer(int64_t serverId) {
  auto session = sessionForServer(serverId);
  if (!session) {
    return;
  }
  ::highcube::ReconcileReplicas order;
  for (const auto& replica : repos_.listReplicasForServer(serverId)) {
    order.add_replica_ids(replica.id);
  }
  session->send(makeEnvelope(::highcube::RECONCILE_REPLICAS, order));
}

void AgentHub::restartReplica(int64_t replicaId) {
  auto replica = repos_.findReplicaById(replicaId);
  auto session = replica ? sessionForServer(replica->serverId) : nullptr;
  if (!replica || !session) {
    return;
  }
  ::highcube::RestartReplica order;
  order.set_replica_id(replica->id);
  order.set_container_id(replica->containerId);
  session->send(makeEnvelope(::highcube::RESTART_REPLICA, order));
}

void AgentHub::setAgentBinaryPath(std::string path) { agentBinaryPath_ = std::move(path); }

void AgentHub::updateAgent(int64_t serverId) {
  auto session = sessionForServer(serverId);
  if (!session) {
    throw std::runtime_error("Agent is not connected");
  }
  if (agentBinaryPath_.empty() || !std::filesystem::is_regular_file(agentBinaryPath_)) {
    throw std::runtime_error("hc-agent binary was not found next to hc-admin");
  }
  std::ifstream in(agentBinaryPath_, std::ios::binary);
  if (!in) {
    throw std::runtime_error("hc-agent binary was not found next to hc-admin");
  }
  auto bytes = std::make_shared<std::string>((std::istreambuf_iterator<char>(in)),
                                             std::istreambuf_iterator<char>());
  if (bytes->empty()) {
    throw std::runtime_error("hc-agent binary is empty");
  }
  const auto sha = hashToken(*bytes);
  const auto token = randomToken(16);
  {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = agentBinaryTickets_.begin(); it != agentBinaryTickets_.end();) {
      if (now > it->second.expires) {
        it = agentBinaryTickets_.erase(it);
      } else {
        ++it;
      }
    }
    agentBinaryTickets_[token] = AgentBinaryTicket{sha, bytes, now + std::chrono::minutes(2)};
  }

  // The agent already running understands host-terminal commands, so the update does not
  // need a new wire message. The script reads admin_url from its own config, checks the
  // checksum, replaces the binary, and restarts the service.
  // PPID of this bash is hc-agent. Restart=always brings the new binary back after SIGTERM.
  // A systemctl restart from inside the service would deadlock on the parent process.
  const std::string command =
      "export HC_AGENT_PID=\"$PPID\"; "
      "export HC_AGENT_EXE=\"$(readlink -f /proc/$PPID/exe)\"; "
      "export HC_AGENT_UPDATE_TOKEN='" +
      token +
      "'; "
      "export HC_AGENT_UPDATE_SHA='" +
      sha +
      "'; "
      "python3 -c '"
      "import hashlib, os, pathlib, signal, ssl, urllib.request\n"
      "exe = os.environ.get(\"HC_AGENT_EXE\", \"\")\n"
      "token = os.environ[\"HC_AGENT_UPDATE_TOKEN\"]\n"
      "expect = os.environ[\"HC_AGENT_UPDATE_SHA\"]\n"
      "log = pathlib.Path(exe).resolve().with_name(\"hc-agent-update.log\") if exe else pathlib.Path(\"hc-agent-update.log\")\n"
      "dest = exe + \".new\"\n"
      "try:\n"
      "    conf = pathlib.Path(exe).resolve().parent / \"hc-agent.conf\"\n"
      "    raw = \"\"\n"
      "    for line in conf.read_text().splitlines():\n"
      "        if line.startswith(\"admin_url=\"):\n"
      "            raw = line.split(\"=\", 1)[1].strip()\n"
      "    if raw.startswith(\"wss://\"):\n"
      "        base = \"https://\" + raw[6:]\n"
      "    elif raw.startswith(\"ws://\"):\n"
      "        base = \"http://\" + raw[5:]\n"
      "    else:\n"
      "        raise SystemExit(\"admin_url is not ws or wss\")\n"
      "    cut = base.find(\"/api/agents/connect\")\n"
      "    if cut >= 0:\n"
      "        base = base[:cut]\n"
      "    url = base.rstrip(\"/\") + \"/api/agents/binary?token=\" + token\n"
      "    ctx = ssl._create_unverified_context()\n"
      "    with urllib.request.urlopen(url, context=ctx, timeout=120) as resp:\n"
      "        data = resp.read()\n"
      "    pathlib.Path(dest).write_bytes(data)\n"
      "    digest = hashlib.sha256(data).hexdigest()\n"
      "    if digest != expect:\n"
      "        raise SystemExit(\"checksum mismatch\")\n"
      "    os.chmod(dest, 0o755)\n"
      "    os.replace(dest, exe)\n"
      "    log.write_text(\"replaced\\n\")\n"
      "    os.kill(int(os.environ[\"HC_AGENT_PID\"]), signal.SIGTERM)\n"
      "except BaseException as exc:\n"
      "    try:\n"
      "        os.remove(dest)\n"
      "    except OSError:\n"
      "        pass\n"
      "    log.write_text(str(exc) + \"\\n\")\n"
      "    raise\n"
      "'";

  ::highcube::OpenHostTerminal order;
  order.set_stream_id("agent-update-" + token.substr(0, 12));
  order.set_command(command);
  order.set_cols(80);
  order.set_rows(24);
  session->send(makeEnvelope(::highcube::OPEN_HOST_TERMINAL, order));
  spdlog::info("agent update started for server {}", serverId);
}

std::optional<http::response<http::string_body>> AgentHub::serveAgentBinary(
    const http::request<http::string_body>& request) {
  if (pathOnly(std::string(request.target())) != "/api/agents/binary") {
    return std::nullopt;
  }
  if (request.method() != http::verb::get) {
    return errorResponse(http::status::method_not_allowed, "Method not allowed");
  }
  const auto query = parseQuery(std::string(request.target()));
  const auto itToken = query.find("token");
  const auto token = itToken == query.end() ? std::string{} : itToken->second;
  std::shared_ptr<std::string> bytes;
  {
    std::lock_guard lock(mutex_);
    auto it = agentBinaryTickets_.find(token);
    if (it == agentBinaryTickets_.end() || std::chrono::steady_clock::now() > it->second.expires) {
      return errorResponse(http::status::unauthorized, "Invalid or expired update token");
    }
    bytes = it->second.bytes;
    agentBinaryTickets_.erase(it);
  }
  Response res{http::status::ok, request.version()};
  res.set(http::field::content_type, "application/octet-stream");
  res.set(http::field::cache_control, "no-store");
  res.body() = *bytes;
  res.prepare_payload();
  return res;
}

void AgentHub::disconnectServer(int64_t serverId) {
  AgentSession::Pointer session;
  {
    std::lock_guard lock(mutex_);
    auto it = sessionsByServer_.find(serverId);
    if (it == sessionsByServer_.end()) {
      return;
    }
    session = it->second;
  }
  session->close();
}

void AgentHub::deleteVolumeOnServers(const std::string& volumeName, const std::string& hostPath) {
  ::highcube::DeleteVolume order;
  order.set_volume_name(volumeName);
  order.set_host_path(hostPath);
  const auto envelope = makeEnvelope(::highcube::DELETE_VOLUME, order);
  std::lock_guard lock(mutex_);
  for (auto& [_, session] : sessionsByServer_) {
    session->send(envelope);
  }
}

void AgentHub::renameVolumeOnServers(const std::string& fromName, const std::string& toName,
                                     const std::string& hostPath) {
  ::highcube::RenameVolume order;
  order.set_from_volume_name(fromName);
  order.set_to_volume_name(toName);
  order.set_host_path(hostPath);
  const auto envelope = makeEnvelope(::highcube::RENAME_VOLUME, order);
  std::lock_guard lock(mutex_);
  for (auto& [_, session] : sessionsByServer_) {
    session->send(envelope);
  }
}

void AgentHub::checkOfflineServers() {
  auto page = repos_.listServers(ListQuery{1, 1000, "", "asc", ""});
  const auto now = std::chrono::system_clock::now();
  for (auto& server : page.rows) {
    if (server.lastSeenAt.empty()) {
      continue;
    }
    std::tm tm{};
    std::istringstream iss(server.lastSeenAt);
    iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (iss.fail()) {
      continue;
    }
    const auto last = std::chrono::system_clock::from_time_t(timegm(&tm));
    if (now - last > std::chrono::seconds{180}) {
      if (server.status != "offline") {
        spdlog::warn("server {} went offline", server.name);
        repos_.setServerStatus(server.id, "offline");
        // Volume-backed apps stay put; non-volume replicas are reconverged by PlacementService callers.
      }
    }
  }
}

int AgentHub::containerRoundsSince(int64_t serverId, std::chrono::system_clock::time_point since) {
  std::lock_guard lock(mutex_);
  auto it = containerRounds_.find(serverId);
  if (it == containerRounds_.end()) {
    return 0;
  }
  int count = 0;
  for (const auto& at : it->second) {
    if (at > since) {
      ++count;
    }
  }
  return count;
}

}  // namespace highcube::admin
