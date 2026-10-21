#pragma once

#include "agents/session.hpp"
#include "db/repositories.hpp"

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace http = boost::beast::http;

namespace highcube::admin {

class DeploymentService;
class NginxProxy;
class AuthMiddleware;
class GithubAppService;

bool isHostTerminalPath(const std::string& path, int64_t* serverId = nullptr);
bool isReplicaTerminalPath(const std::string& path, int64_t* replicaId = nullptr);

class AgentHub : public std::enable_shared_from_this<AgentHub> {
public:
  AgentHub(net::io_context& ioc, Repositories& repos);

  void setDeploymentService(DeploymentService* deployments);
  void setProxy(NginxProxy* proxy);
  void setAuth(AuthMiddleware* auth);
  void setGithubApps(GithubAppService* githubApps);

  void onUpgrade(tcp::socket socket, http::request<http::string_body> req);
  bool isConnected(int64_t serverId);
  void registerHostTerminal(const std::string& streamId, int64_t serverId,
                            std::function<void(const std::string&)> onData,
                            std::function<void()> onClose);
  void openHostTerminal(int64_t serverId, const std::string& streamId, std::uint32_t cols,
                        std::uint32_t rows);
  void openContainerTerminal(int64_t replicaId, const std::string& streamId, std::uint32_t cols,
                             std::uint32_t rows);
  void closeHostTerminal(int64_t serverId, const std::string& streamId);
  void writeHostTerminal(int64_t serverId, const std::string& streamId, const std::string& data);
  void resizeHostTerminal(int64_t serverId, const std::string& streamId, std::uint32_t cols,
                          std::uint32_t rows);
  void deployReplica(int64_t replicaId);
  void stopReplica(int64_t replicaId);
  // Stops a container without reading the replica row. Used after the row was deleted.
  void stopContainer(int64_t serverId, int64_t replicaId, int64_t applicationId,
                     const std::string& containerId);
  void reconcileServer(int64_t serverId);
  void restartReplica(int64_t replicaId);
  void deleteVolumeOnServers(const std::string& volumeName);
  void renameVolumeOnServers(const std::string& fromName, const std::string& toName);
  void disconnectServer(int64_t serverId);
  void checkOfflineServers();

  // Host port availability check via an async admin<->agent round-trip.
  enum class PortCheckState { Pending, Available, Unavailable, Unknown };
  // Starts a check across the given online servers (all online when empty).
  // Returns a batch id used to poll the outcome.
  std::string startHostPortCheck(int port, const std::vector<int64_t>& serverIds);
  PortCheckState pollHostPortCheck(const std::string& batchId);

  // Side effects applied on the admin once the agent reports success.
  // newPassword is kept only until the stored environment is updated.
  struct DbManageEffect {
    int64_t applicationId = 0;
    std::string passwordEnvKey;
    std::string newPassword;
    std::string healthCommand;
  };
  struct DbManagePoll {
    std::string status;  // pending | ok | error | timeout | unknown
    std::string error;
    std::string resultJson;
  };
  std::string startDbManage(int64_t serverId, int64_t applicationId, const std::string& containerId,
                            const std::string& payloadJson, const DbManageEffect& effect);
  DbManagePoll pollDbManage(const std::string& requestId, int64_t applicationId);

private:
  void onMessage(AgentSession::Pointer session, const ::highcube::Envelope& envelope);
  void handleRegister(AgentSession::Pointer session, const ::highcube::Register& msg);
  void handleHeartbeat(AgentSession::Pointer session);
  void handleHostMetrics(AgentSession::Pointer session, const ::highcube::HostMetrics& msg);
  void handleContainerStatus(AgentSession::Pointer session, const ::highcube::ContainerStatus& msg);
  void handleContainerMetrics(AgentSession::Pointer session, const ::highcube::ContainerMetrics& msg);
  void handleHostPortResult(const ::highcube::HostPortResult& msg);
  void handleDbManageResult(const ::highcube::DbManageResult& msg);
  void applyDbManageEffect(const DbManageEffect& effect, const std::string& resultJson);
  void handleDeploymentLog(const ::highcube::DeploymentLog& msg);
  void onAgentUpgrade(tcp::socket socket, http::request<http::string_body> req);
  void onHostTerminal(tcp::socket socket, http::request<http::string_body> req, int64_t serverId);
  void onReplicaTerminal(tcp::socket socket, http::request<http::string_body> req, int64_t replicaId);
  void onSessionClosed(AgentSession::Pointer session);
  AgentSession::Pointer sessionForServer(int64_t serverId);

  struct TerminalLink {
    int64_t serverId = 0;
    std::function<void(const std::string&)> onData;
    std::function<void()> onClose;
  };

  net::io_context& ioc_;
  Repositories& repos_;
  DeploymentService* deployments_ = nullptr;
  NginxProxy* proxy_ = nullptr;
  AuthMiddleware* auth_ = nullptr;
  GithubAppService* githubApps_ = nullptr;
  std::mutex mutex_;
  std::unordered_map<int64_t, AgentSession::Pointer> sessionsByServer_;
  std::unordered_map<std::string, TerminalLink> terminals_;

  struct PortCheckEntry {
    bool done = false;
    bool available = false;
    std::chrono::steady_clock::time_point createdAt;
  };
  // correlationId -> single-server result; batchId -> its correlation ids.
  std::unordered_map<std::string, PortCheckEntry> portChecks_;
  std::unordered_map<std::string, std::vector<std::string>> portCheckBatches_;
  std::atomic<std::uint64_t> portCheckSeq_{0};

  struct DbManageEntry {
    int64_t applicationId = 0;
    bool done = false;
    bool ok = false;
    std::string error;
    std::string resultJson;
    DbManageEffect effect;
    bool effectApplied = false;
    std::chrono::steady_clock::time_point createdAt;
  };
  std::unordered_map<std::string, DbManageEntry> dbManage_;
  std::atomic<std::uint64_t> dbManageSeq_{0};
};

}  // namespace highcube::admin
