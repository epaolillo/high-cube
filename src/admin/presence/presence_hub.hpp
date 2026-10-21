#pragma once

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace highcube::admin {

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class AuthMiddleware;
class Repositories;

bool isPresencePath(const std::string& path);

// Tracks which users currently have the dashboard open through a persistent
// WebSocket, and periodically records presence heartbeats so the activity
// timeline can tell, historically, whether a user was active.
class PresenceHub : public std::enable_shared_from_this<PresenceHub> {
public:
  PresenceHub(net::io_context& ioc, Repositories& repos, AuthMiddleware& auth);

  // Accept a browser WebSocket upgrade on /api/presence. Authenticates via the
  // bearer token carried in the query string and starts tracking the user.
  void onUpgrade(tcp::socket socket, http::request<http::string_body> req);

  // User ids with at least one open dashboard connection right now.
  std::unordered_set<int64_t> onlineUserIds();
  bool isOnline(int64_t userId);

  // Called from a timer: write a presence heartbeat for every online user.
  void recordHeartbeats();

  // Connection bookkeeping used by the internal session objects.
  void onConnect(int64_t userId);
  void onDisconnect(int64_t userId);

private:
  net::io_context& ioc_;
  Repositories& repos_;
  AuthMiddleware& auth_;
  std::mutex mutex_;
  std::unordered_map<int64_t, int> connectionsByUser_;
};

}  // namespace highcube::admin
