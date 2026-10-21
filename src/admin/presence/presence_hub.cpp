#include "presence/presence_hub.hpp"

#include "api/http_types.hpp"
#include "auth/auth_middleware.hpp"
#include "db/repositories.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace highcube::admin {
namespace {

namespace websocket = beast::websocket;

std::string nowIso() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

void rejectSocket(tcp::socket& socket, const http::request<http::string_body>& req,
                  http::status status, const std::string& message) {
  Response res{status, req.version()};
  res.set(http::field::content_type, "application/json");
  res.set(http::field::access_control_allow_origin, "*");
  res.body() = nlohmann::json{{"error", message}}.dump();
  res.prepare_payload();
  beast::error_code ec;
  http::write(socket, res, ec);
}

// Holds a single browser presence connection open until the tab is closed.
class PresenceSession : public std::enable_shared_from_this<PresenceSession> {
public:
  PresenceSession(tcp::socket socket, std::shared_ptr<PresenceHub> hub, int64_t userId)
      : ws_(std::move(socket)), hub_(std::move(hub)), userId_(userId) {}

  void start(http::request<http::string_body> req) {
    auto self = shared_from_this();
    ws_.async_accept(req, [self](beast::error_code ec) {
      if (ec) {
        self->fail();
        return;
      }
      self->hub_->onConnect(self->userId_);
      self->connected_ = true;
      self->doRead();
    });
  }

private:
  void doRead() {
    auto self = shared_from_this();
    ws_.async_read(buffer_, [self](beast::error_code ec, std::size_t) {
      if (ec) {
        self->fail();
        return;
      }
      // Client messages (keep-alive pings) are ignored; the live connection is
      // all the presence signal we need.
      self->buffer_.consume(self->buffer_.size());
      self->doRead();
    });
  }

  void fail() {
    if (closed_) {
      return;
    }
    closed_ = true;
    if (connected_) {
      hub_->onDisconnect(userId_);
    }
  }

  websocket::stream<beast::tcp_stream> ws_;
  beast::flat_buffer buffer_;
  std::shared_ptr<PresenceHub> hub_;
  int64_t userId_;
  bool connected_ = false;
  bool closed_ = false;
};

}  // namespace

bool isPresencePath(const std::string& path) {
  return path == "/api/presence";
}

PresenceHub::PresenceHub(net::io_context& ioc, Repositories& repos, AuthMiddleware& auth)
    : ioc_(ioc), repos_(repos), auth_(auth) {}

void PresenceHub::onUpgrade(tcp::socket socket, http::request<http::string_body> req) {
  auto header = std::string(req[http::field::authorization]);
  const auto query = parseQuery(std::string(req.target()));
  if (header.empty()) {
    const auto token = query.find("token");
    if (token != query.end() && !token->second.empty()) {
      header = "Bearer " + token->second;
    }
  }
  const auto user = auth_.authenticate(header);
  if (!user) {
    rejectSocket(socket, req, http::status::unauthorized, "Unauthorized");
    return;
  }
  auto session = std::make_shared<PresenceSession>(std::move(socket), shared_from_this(), user->id);
  session->start(std::move(req));
}

void PresenceHub::onConnect(int64_t userId) {
  bool firstConnection = false;
  {
    std::lock_guard lock(mutex_);
    firstConnection = (connectionsByUser_[userId]++ == 0);
  }
  if (firstConnection) {
    // Record presence immediately so even a short visit shows up on the timeline.
    try {
      repos_.recordPresence(userId, nowIso());
    } catch (const std::exception& ex) {
      spdlog::warn("presence record failed: {}", ex.what());
    }
  }
}

void PresenceHub::onDisconnect(int64_t userId) {
  std::lock_guard lock(mutex_);
  auto it = connectionsByUser_.find(userId);
  if (it == connectionsByUser_.end()) {
    return;
  }
  if (--it->second <= 0) {
    connectionsByUser_.erase(it);
  }
}

std::unordered_set<int64_t> PresenceHub::onlineUserIds() {
  std::unordered_set<int64_t> ids;
  std::lock_guard lock(mutex_);
  for (const auto& [userId, count] : connectionsByUser_) {
    if (count > 0) {
      ids.insert(userId);
    }
  }
  return ids;
}

bool PresenceHub::isOnline(int64_t userId) {
  std::lock_guard lock(mutex_);
  auto it = connectionsByUser_.find(userId);
  return it != connectionsByUser_.end() && it->second > 0;
}

void PresenceHub::recordHeartbeats() {
  const auto ids = onlineUserIds();
  const auto stamp = nowIso();
  for (const int64_t userId : ids) {
    try {
      repos_.recordPresence(userId, stamp);
    } catch (const std::exception& ex) {
      spdlog::warn("presence heartbeat failed: {}", ex.what());
    }
  }
}

}  // namespace highcube::admin
