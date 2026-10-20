#pragma once

#include "agents/session.hpp"
#include "api/http_types.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>

namespace highcube::admin {

class AgentHub;
class AuthMiddleware;
class Repositories;

// HTTP long-polling transport for host and container terminals.
// State is only touched from the io_context thread.
class HostTerminalPolling : public std::enable_shared_from_this<HostTerminalPolling> {
public:
  using Reply = std::function<void(Response)>;

  HostTerminalPolling(net::io_context& ioc, AgentHub& hub, AuthMiddleware& auth,
                      Repositories& repos);

  void start();

  // Output polls are answered asynchronously, so they bypass the synchronous Router.
  bool tryPollOutput(const Request& request, const Reply& reply);

  Response open(int64_t serverId, const User& user, const std::string& body);
  Response openReplica(int64_t replicaId, const User& user, const std::string& body);
  Response write(int64_t serverId, const std::string& streamId, const User& user,
                 const std::string& data);
  Response writeReplica(int64_t replicaId, const std::string& streamId, const User& user,
                        const std::string& data);
  Response resize(int64_t serverId, const std::string& streamId, const User& user,
                  const std::string& body);
  Response resizeReplica(int64_t replicaId, const std::string& streamId, const User& user,
                         const std::string& body);
  Response close(int64_t serverId, const std::string& streamId, const User& user);
  Response closeReplica(int64_t replicaId, const std::string& streamId, const User& user);

private:
  struct Waiter {
    std::uint64_t id = 0;
    std::uint64_t offset = 0;
    Reply reply;
    std::shared_ptr<net::steady_timer> timer;
  };

  struct Stream {
    int64_t serverId = 0;
    int64_t replicaId = 0;  // 0 for host terminals
    int64_t userId = 0;
    std::string buffer;
    std::uint64_t startOffset = 0;
    bool closed = false;
    std::optional<Waiter> waiter;
    std::chrono::steady_clock::time_point lastSeen;
  };

  static Response outputResponse(const Stream& stream, std::uint64_t offset);
  Stream* findHost(int64_t serverId, const std::string& streamId, int64_t userId);
  Stream* findReplica(int64_t replicaId, const std::string& streamId, int64_t userId);
  void onOutput(const std::string& streamId, const std::string& data);
  void onAgentClosed(const std::string& streamId);
  void answerWaiter(Stream& stream);
  void remove(const std::string& streamId);
  void scheduleSweep();
  void sweep();
  std::string newStreamId();

  net::io_context& ioc_;
  AgentHub& hub_;
  AuthMiddleware& auth_;
  Repositories& repos_;
  net::steady_timer sweepTimer_;
  std::mt19937_64 rng_;
  std::uint64_t nextWaiterId_ = 1;
  std::unordered_map<std::string, Stream> streams_;
};

}  // namespace highcube::admin
