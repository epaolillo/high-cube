#include "agents/terminal_polling.hpp"

#include "agents/agent_hub.hpp"
#include "auth/auth_middleware.hpp"
#include "db/repositories.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <vector>

namespace highcube::admin {
namespace {

constexpr std::size_t kMaxBufferedOutput = 1024 * 1024;
constexpr auto kPollWait = std::chrono::seconds(25);
constexpr auto kIdleTimeout = std::chrono::seconds(60);
constexpr auto kSweepInterval = std::chrono::seconds(15);

std::uint32_t sizeField(const nlohmann::json& body, const char* key, std::uint32_t fallback,
                        std::uint32_t min, std::uint32_t max) {
  const auto it = body.find(key);
  if (it == body.end() || !it->is_number()) {
    return fallback;
  }
  const auto value = std::clamp(it->get<double>(), static_cast<double>(min), static_cast<double>(max));
  return static_cast<std::uint32_t>(value);
}

bool parseHostOutputPath(const std::string& path, int64_t& serverId, std::string& streamId) {
  std::vector<std::string> parts;
  std::stringstream ss(path);
  std::string part;
  while (std::getline(ss, part, '/')) {
    parts.push_back(part);
  }
  if (parts.size() != 8 || !parts[0].empty() || parts[1] != "api" || parts[2] != "servers" ||
      parts[4] != "terminal" || parts[5] != "sessions" || parts[6].empty() || parts[7] != "output") {
    return false;
  }
  try {
    std::size_t idx = 0;
    serverId = std::stoll(parts[3], &idx);
    if (idx != parts[3].size() || serverId <= 0) {
      return false;
    }
  } catch (const std::exception&) {
    return false;
  }
  streamId = parts[6];
  return true;
}

bool parseReplicaOutputPath(const std::string& path, int64_t& replicaId, std::string& streamId) {
  std::vector<std::string> parts;
  std::stringstream ss(path);
  std::string part;
  while (std::getline(ss, part, '/')) {
    parts.push_back(part);
  }
  if (parts.size() != 8 || !parts[0].empty() || parts[1] != "api" || parts[2] != "replicas" ||
      (parts[4] != "terminal" && parts[4] != "logs") || parts[5] != "sessions" ||
      parts[6].empty() || parts[7] != "output") {
    return false;
  }
  try {
    std::size_t idx = 0;
    replicaId = std::stoll(parts[3], &idx);
    if (idx != parts[3].size() || replicaId <= 0) {
      return false;
    }
  } catch (const std::exception&) {
    return false;
  }
  streamId = parts[6];
  return true;
}

std::uint64_t queryOffset(const std::unordered_map<std::string, std::string>& query) {
  const auto it = query.find("offset");
  if (it == query.end() || it->second.empty()) {
    return 0;
  }
  try {
    return std::stoull(it->second);
  } catch (const std::exception&) {
    return 0;
  }
}

Response notFound() {
  return errorResponse(http::status::not_found, "Terminal session not found");
}

}  // namespace

HostTerminalPolling::HostTerminalPolling(net::io_context& ioc, AgentHub& hub, AuthMiddleware& auth,
                                         Repositories& repos)
    : ioc_(ioc),
      hub_(hub),
      auth_(auth),
      repos_(repos),
      sweepTimer_(ioc),
      rng_(std::random_device{}()) {}

void HostTerminalPolling::start() {
  scheduleSweep();
}

Response HostTerminalPolling::open(int64_t serverId, const User& user, const std::string& body) {
  if (!hub_.isConnected(serverId)) {
    return errorResponse(http::status::conflict, "Agent is not connected");
  }
  const auto json = nlohmann::json::parse(body, nullptr, false);
  const auto cols = sizeField(json, "cols", 120, 40, 300);
  const auto rows = sizeField(json, "rows", 32, 10, 80);
  const auto streamId = newStreamId();

  Stream stream;
  stream.serverId = serverId;
  stream.userId = user.id;
  stream.lastSeen = std::chrono::steady_clock::now();
  streams_.emplace(streamId, std::move(stream));

  std::weak_ptr<HostTerminalPolling> weak = shared_from_this();
  hub_.registerHostTerminal(
      streamId, serverId,
      [weak, streamId](const std::string& data) {
        if (auto self = weak.lock()) {
          net::post(self->ioc_, [self, streamId, data]() { self->onOutput(streamId, data); });
        }
      },
      [weak, streamId]() {
        if (auto self = weak.lock()) {
          net::post(self->ioc_, [self, streamId]() { self->onAgentClosed(streamId); });
        }
      });
  hub_.openHostTerminal(serverId, streamId, cols, rows);
  return jsonResponse(http::status::created, {{"streamId", streamId}});
}

Response HostTerminalPolling::openReplica(int64_t replicaId, const User& user,
                                          const std::string& body, bool logs) {
  auto replica = repos_.findReplicaById(replicaId);
  auto app = replica ? repos_.findApplicationById(replica->applicationId) : std::nullopt;
  if (!replica || !app) {
    return errorResponse(http::status::not_found, "Replica not found");
  }
  if (!auth_.canAccessApplication(user, *app)) {
    return errorResponse(http::status::forbidden, "Forbidden");
  }
  if (replica->containerId.empty()) {
    return errorResponse(http::status::conflict, "Container is not ready");
  }
  if (!hub_.isConnected(replica->serverId)) {
    return errorResponse(http::status::conflict, "Agent is not connected");
  }
  const auto json = nlohmann::json::parse(body, nullptr, false);
  const auto cols = sizeField(json, "cols", 120, 40, 300);
  const auto rows = sizeField(json, "rows", 32, 10, 80);
  const auto streamId = newStreamId();

  Stream stream;
  stream.serverId = replica->serverId;
  stream.replicaId = replicaId;
  stream.userId = user.id;
  stream.lastSeen = std::chrono::steady_clock::now();
  streams_.emplace(streamId, std::move(stream));

  std::weak_ptr<HostTerminalPolling> weak = shared_from_this();
  hub_.registerHostTerminal(
      streamId, replica->serverId,
      [weak, streamId](const std::string& data) {
        if (auto self = weak.lock()) {
          net::post(self->ioc_, [self, streamId, data]() { self->onOutput(streamId, data); });
        }
      },
      [weak, streamId]() {
        if (auto self = weak.lock()) {
          net::post(self->ioc_, [self, streamId]() { self->onAgentClosed(streamId); });
        }
      });
  if (logs) {
    hub_.openReplicaLogs(replicaId, streamId);
  } else {
    hub_.openContainerTerminal(replicaId, streamId, cols, rows);
  }
  return jsonResponse(http::status::created, {{"streamId", streamId}});
}

bool HostTerminalPolling::tryPollOutput(const Request& request, const Reply& reply) {
  if (request.method() != http::verb::get) {
    return false;
  }
  const auto target = std::string(request.target());
  const auto path = pathOnly(target);
  int64_t serverId = 0;
  int64_t replicaId = 0;
  std::string streamId;
  const bool hostPath = parseHostOutputPath(path, serverId, streamId);
  const bool replicaPath = !hostPath && parseReplicaOutputPath(path, replicaId, streamId);
  if (!hostPath && !replicaPath) {
    return false;
  }

  const auto user = auth_.authenticate(std::string(request[http::field::authorization]));
  if (!user) {
    reply(errorResponse(http::status::unauthorized, "Unauthorized"));
    return true;
  }
  if (hostPath && !auth_.isAdmin(*user)) {
    reply(errorResponse(http::status::forbidden, "Forbidden"));
    return true;
  }
  Stream* stream =
      hostPath ? findHost(serverId, streamId, user->id) : findReplica(replicaId, streamId, user->id);
  if (!stream) {
    reply(notFound());
    return true;
  }
  stream->lastSeen = std::chrono::steady_clock::now();
  const auto offset = queryOffset(parseQuery(target));

  answerWaiter(*stream);

  const std::uint64_t end = stream->startOffset + stream->buffer.size();
  if (offset > stream->startOffset) {
    const auto acknowledged = std::min(offset, end) - stream->startOffset;
    stream->buffer.erase(0, static_cast<std::size_t>(acknowledged));
    stream->startOffset += acknowledged;
  }

  if (offset < end || stream->closed) {
    reply(outputResponse(*stream, offset));
    return true;
  }

  Waiter waiter;
  waiter.id = nextWaiterId_++;
  waiter.offset = offset;
  waiter.reply = reply;
  waiter.timer = std::make_shared<net::steady_timer>(ioc_, kPollWait);
  waiter.timer->async_wait(
      [weak = weak_from_this(), streamId, id = waiter.id, timer = waiter.timer](beast::error_code ec) {
        auto self = weak.lock();
        if (ec || !self) {
          return;
        }
        auto it = self->streams_.find(streamId);
        if (it != self->streams_.end() && it->second.waiter && it->second.waiter->id == id) {
          self->answerWaiter(it->second);
        }
      });
  stream->waiter = std::move(waiter);
  return true;
}

Response HostTerminalPolling::write(int64_t serverId, const std::string& streamId, const User& user,
                                    const std::string& data) {
  auto* stream = findHost(serverId, streamId, user.id);
  if (!stream) {
    return notFound();
  }
  stream->lastSeen = std::chrono::steady_clock::now();
  if (stream->closed) {
    return errorResponse(http::status::gone, "Terminal closed");
  }
  if (!data.empty()) {
    hub_.writeHostTerminal(serverId, streamId, data);
  }
  return emptyResponse(http::status::no_content);
}

Response HostTerminalPolling::writeReplica(int64_t replicaId, const std::string& streamId,
                                           const User& user, const std::string& data) {
  auto* stream = findReplica(replicaId, streamId, user.id);
  if (!stream) {
    return notFound();
  }
  stream->lastSeen = std::chrono::steady_clock::now();
  if (stream->closed) {
    return errorResponse(http::status::gone, "Terminal closed");
  }
  if (!data.empty()) {
    hub_.writeHostTerminal(stream->serverId, streamId, data);
  }
  return emptyResponse(http::status::no_content);
}

Response HostTerminalPolling::resize(int64_t serverId, const std::string& streamId, const User& user,
                                     const std::string& body) {
  auto* stream = findHost(serverId, streamId, user.id);
  if (!stream) {
    return notFound();
  }
  stream->lastSeen = std::chrono::steady_clock::now();
  if (stream->closed) {
    return errorResponse(http::status::gone, "Terminal closed");
  }
  const auto json = nlohmann::json::parse(body, nullptr, false);
  hub_.resizeHostTerminal(serverId, streamId, sizeField(json, "cols", 120, 40, 300),
                          sizeField(json, "rows", 32, 10, 80));
  return emptyResponse(http::status::no_content);
}

Response HostTerminalPolling::resizeReplica(int64_t replicaId, const std::string& streamId,
                                            const User& user, const std::string& body) {
  auto* stream = findReplica(replicaId, streamId, user.id);
  if (!stream) {
    return notFound();
  }
  stream->lastSeen = std::chrono::steady_clock::now();
  if (stream->closed) {
    return errorResponse(http::status::gone, "Terminal closed");
  }
  const auto json = nlohmann::json::parse(body, nullptr, false);
  hub_.resizeHostTerminal(stream->serverId, streamId, sizeField(json, "cols", 120, 40, 300),
                          sizeField(json, "rows", 32, 10, 80));
  return emptyResponse(http::status::no_content);
}

Response HostTerminalPolling::close(int64_t serverId, const std::string& streamId, const User& user) {
  if (!findHost(serverId, streamId, user.id)) {
    return notFound();
  }
  remove(streamId);
  return emptyResponse(http::status::no_content);
}

Response HostTerminalPolling::closeReplica(int64_t replicaId, const std::string& streamId,
                                           const User& user) {
  if (!findReplica(replicaId, streamId, user.id)) {
    return notFound();
  }
  remove(streamId);
  return emptyResponse(http::status::no_content);
}

Response HostTerminalPolling::outputResponse(const Stream& stream, std::uint64_t offset) {
  const std::uint64_t end = stream.startOffset + stream.buffer.size();
  const auto from = std::clamp(offset, stream.startOffset, end);
  Response res{http::status::ok, 11};
  res.set(http::field::content_type, "application/octet-stream");
  res.set(http::field::cache_control, "no-store");
  res.set(http::field::access_control_allow_origin, "*");
  res.set(http::field::access_control_expose_headers, "X-Next-Offset, X-Terminal-Closed");
  res.set("X-Next-Offset", std::to_string(end));
  if (stream.closed) {
    res.set("X-Terminal-Closed", "1");
  }
  res.keep_alive(false);
  res.body() = stream.buffer.substr(static_cast<std::size_t>(from - stream.startOffset));
  res.prepare_payload();
  return res;
}

HostTerminalPolling::Stream* HostTerminalPolling::findHost(int64_t serverId,
                                                          const std::string& streamId,
                                                          int64_t userId) {
  auto it = streams_.find(streamId);
  if (it == streams_.end() || it->second.serverId != serverId || it->second.userId != userId ||
      it->second.replicaId != 0) {
    return nullptr;
  }
  return &it->second;
}

HostTerminalPolling::Stream* HostTerminalPolling::findReplica(int64_t replicaId,
                                                             const std::string& streamId,
                                                             int64_t userId) {
  auto it = streams_.find(streamId);
  if (it == streams_.end() || it->second.replicaId != replicaId || it->second.userId != userId) {
    return nullptr;
  }
  return &it->second;
}

void HostTerminalPolling::onOutput(const std::string& streamId, const std::string& data) {
  auto it = streams_.find(streamId);
  if (it == streams_.end()) {
    return;
  }
  auto& stream = it->second;
  stream.buffer += data;
  if (stream.buffer.size() > kMaxBufferedOutput) {
    const auto dropped = stream.buffer.size() - kMaxBufferedOutput;
    stream.buffer.erase(0, dropped);
    stream.startOffset += dropped;
  }
  answerWaiter(stream);
}

void HostTerminalPolling::onAgentClosed(const std::string& streamId) {
  auto it = streams_.find(streamId);
  if (it == streams_.end()) {
    return;
  }
  it->second.closed = true;
  answerWaiter(it->second);
}

void HostTerminalPolling::answerWaiter(Stream& stream) {
  if (!stream.waiter) {
    return;
  }
  auto waiter = std::move(*stream.waiter);
  stream.waiter.reset();
  waiter.timer->cancel();
  stream.lastSeen = std::chrono::steady_clock::now();
  waiter.reply(outputResponse(stream, waiter.offset));
}

void HostTerminalPolling::remove(const std::string& streamId) {
  auto it = streams_.find(streamId);
  if (it == streams_.end()) {
    return;
  }
  auto& stream = it->second;
  const bool agentOpen = !stream.closed;
  const auto serverId = stream.serverId;
  stream.closed = true;
  answerWaiter(stream);
  streams_.erase(it);
  if (agentOpen) {
    hub_.closeHostTerminal(serverId, streamId);
  }
}

void HostTerminalPolling::scheduleSweep() {
  sweepTimer_.expires_after(kSweepInterval);
  sweepTimer_.async_wait([weak = weak_from_this()](beast::error_code ec) {
    auto self = weak.lock();
    if (ec || !self) {
      return;
    }
    self->sweep();
    self->scheduleSweep();
  });
}

void HostTerminalPolling::sweep() {
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::string> idle;
  for (const auto& [id, stream] : streams_) {
    if (!stream.waiter && now - stream.lastSeen > kIdleTimeout) {
      idle.push_back(id);
    }
  }
  for (const auto& id : idle) {
    remove(id);
  }
}

std::string HostTerminalPolling::newStreamId() {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0') << std::setw(16) << rng_() << std::setw(16) << rng_();
  return oss.str();
}

}  // namespace highcube::admin
