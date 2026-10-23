#pragma once

#include "agents/agent_hub.hpp"
#include "auth/auth_middleware.hpp"
#include "auth/jwt_service.hpp"
#include "certificates/certbot_service.hpp"
#include "config/admin_config.hpp"
#include "db/repositories.hpp"
#include "domain/models.hpp"
#include "presence/presence_hub.hpp"
#include "proxy/nginx_proxy.hpp"

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace highcube::admin {

class PlacementService;
class DeploymentService;
class GithubAppService;
class HostTerminalPolling;

struct AppContext {
  AdminConfig config;
  std::unique_ptr<Database> database;
  std::unique_ptr<Repositories> repos;
  std::unique_ptr<JwtService> jwt;
  std::unique_ptr<AuthMiddleware> auth;
  std::unique_ptr<NginxProxy> proxy;
  std::unique_ptr<CertbotService> certbot;
  std::shared_ptr<AgentHub> agentHub;
  std::shared_ptr<PresenceHub> presence;
  std::unique_ptr<PlacementService> placement;
  std::unique_ptr<DeploymentService> deployments;
  std::unique_ptr<GithubAppService> githubApps;
  // Owned by main after the io_context, because it holds a timer bound to it.
  HostTerminalPolling* terminalPolling = nullptr;
};

nlohmann::json userToJson(const User& user, bool includeServers = false);
nlohmann::json serverToJson(const Server& server);
nlohmann::json applicationToJson(const Application& app);
nlohmann::json deploymentToJson(const Deployment& deployment);
nlohmann::json replicaToJson(const Replica& replica);
nlohmann::json replicaIncidentToJson(const ReplicaIncident& incident);
nlohmann::json githubAppToJson(const GithubApp& app);
nlohmann::json userActivityToJson(const UserActivity& activity);

// Every volume host directory lives under this root on its server; the agents enforce it too.
inline constexpr const char* kVolumesRoot = "/var/lib/data";
// hostDirectory is the absolute host directory, null when Docker manages the data.
nlohmann::json volumeToJson(const ApplicationVolume& volume);

// Maps an HTTP method and matched route pattern to a stable, human-oriented
// action key plus the affected resource, used by the audit trail.
struct DerivedActivity {
  std::string action;
  std::string targetType;
};
DerivedActivity deriveActivity(const std::string& method, const std::string& routePattern);

class UserService {
public:
  explicit UserService(AppContext& ctx);
  nlohmann::json login(const std::string& email, const std::string& password);
  nlohmann::json list(const ListQuery& query);
  nlohmann::json get(int64_t id);
  nlohmann::json create(const nlohmann::json& body, const User& actor);
  nlohmann::json update(int64_t id, const nlohmann::json& body, const User& actor);
  void remove(int64_t id, const User& actor);
  nlohmann::json setServers(int64_t id, const nlohmann::json& body);
  nlohmann::json setDomains(int64_t id, const nlohmann::json& body);
  nlohmann::json listMyBaseDomains(const User& actor);
  nlohmann::json hostnameSuggestion();
  nlohmann::json autocomplete(const std::string& q);

private:
  AppContext& ctx_;
};

class ActivityService {
public:
  explicit ActivityService(AppContext& ctx);
  // Persist one audited action. Best-effort: never throws to the caller.
  void record(const UserActivity& activity);
  // Paginated audit trail for a user.
  nlohmann::json list(int64_t userId, const ListQuery& query);
  // Activity vs. presence timeline for the last `hours` hours, bucketed.
  nlohmann::json chart(int64_t userId, int hours);

private:
  AppContext& ctx_;
};

class SettingsService {
public:
  explicit SettingsService(AppContext& ctx);
  nlohmann::json listBaseDomains(const ListQuery& query);
  // Read-only view for the User role: only base domains used by the user's applications.
  nlohmann::json listBaseDomainsUsedByUserApplications(int64_t userId);
  nlohmann::json createBaseDomain(const nlohmann::json& body, const User& actor);
  nlohmann::json updateBaseDomain(int64_t id, const nlohmann::json& body);
  void deleteBaseDomain(int64_t id);
  nlohmann::json autocompleteBaseDomains(const std::string& q);
  nlohmann::json getMetricsSettings();
  nlohmann::json updateMetricsSettings(const nlohmann::json& body);

private:
  AppContext& ctx_;
};

class ServerService {
public:
  explicit ServerService(AppContext& ctx);
  nlohmann::json list(const ListQuery& query);
  // Read-only view for the User role: only servers hosting the user's applications, and only
  // their name, status and current metrics.
  nlohmann::json listHostingUserApplications(int64_t userId);
  nlohmann::json get(int64_t id);
  nlohmann::json create(const nlohmann::json& body);
  nlohmann::json update(int64_t id, const nlohmann::json& body);
  nlohmann::json rotateToken(int64_t id);
  // Asks the connected agent to install the hc-agent binary that sits next to hc-admin.
  nlohmann::json updateAgent(int64_t id);
  void remove(int64_t id);
  nlohmann::json autocomplete(const std::string& q);
  // Downsampled chart ({series, latest}) instead of every raw 1-second sample.
  nlohmann::json metrics(int64_t id, const std::string& from, const std::string& to);
  // Latest resource readings for one server, from the two newest samples.
  nlohmann::json snapshot(int64_t id);
  // Server record plus the last five minutes of downsampled metrics.
  nlohmann::json overview(int64_t id);

private:
  AppContext& ctx_;
};

// One poll for the whole dashboard, plus one poll per open detail drawer.
class StateService {
public:
  explicit StateService(AppContext& ctx);
  nlohmann::json dashboard(const ListQuery& servers, const ListQuery& applications,
                           const ListQuery& users, const ListQuery& domains, const User& actor);
  nlohmann::json applicationOverview(int64_t id, const User& actor);
  nlohmann::json serverOverview(int64_t id);

private:
  AppContext& ctx_;
};

// True when flavor is one of the supported MySQL or PostgreSQL versions.
bool isKnownDatabaseFlavor(const std::string& flavor);
// "mysql" or "postgres" for a known flavor, otherwise empty.
std::string databaseFlavorEngine(const std::string& flavor);
// Engine derived from application.databaseFlavor.
std::string managedDatabaseEngine(const Application& app);

class DatabaseAdminService {
public:
  explicit DatabaseAdminService(AppContext& ctx);
  // Starts an agent round-trip. The caller polls the returned request id.
  nlohmann::json start(int64_t applicationId, const nlohmann::json& body, const User& actor);
  nlohmann::json poll(int64_t applicationId, const std::string& requestId, const User& actor);

private:
  AppContext& ctx_;
};

class ApplicationService {
public:
  explicit ApplicationService(AppContext& ctx);
  nlohmann::json list(const ListQuery& query, const User& actor);
  nlohmann::json get(int64_t id, const User& actor);
  nlohmann::json create(const nlohmann::json& body, const User& actor);
  nlohmann::json update(int64_t id, const nlohmann::json& body, const User& actor);
  void remove(int64_t id, const User& actor);
  // Stop the application and keep it down (desiredReplicas = 0) so the containers
  // do not come back up. This lets the operator delete volumes and then the app.
  void stop(int64_t id, const User& actor);
  nlohmann::json scale(int64_t id, const nlohmann::json& body, const User& actor);
  nlohmann::json createDeployKey(int64_t id, const User& actor);
  nlohmann::json requestCertificate(int64_t id, const std::string& hostname, const User& actor);
  nlohmann::json certificateJob(int64_t id, const std::string& jobId, const User& actor);
  nlohmann::json autocomplete(const std::string& q, bool privateOnly, const User& actor);
  nlohmann::json listVolumes(int64_t id, const User& actor);
  nlohmann::json addVolume(int64_t id, const nlohmann::json& body, const User& actor);
  nlohmann::json updateVolume(int64_t applicationId, int64_t volumeId, const nlohmann::json& body,
                              const User& actor);
  void deleteVolume(int64_t applicationId, int64_t volumeId, const User& actor);
  nlohmann::json metrics(int64_t id, const std::string& from, const std::string& to,
                         const User& actor);
  // Crash traceability: the history of container falls and recoveries.
  nlohmann::json incidents(int64_t id, const ListQuery& query, const User& actor);

private:
  AppContext& ctx_;
  Application parseBody(const nlohmann::json& body, const Application* existing,
                        const User& actor) const;
  Application requireAccess(int64_t id, const User& actor) const;
};

class PlacementService {
public:
  explicit PlacementService(AppContext& ctx);
  int64_t chooseServer(const Application& app);
  void converge(int64_t applicationId);

private:
  AppContext& ctx_;
};

class DeploymentService {
public:
  explicit DeploymentService(AppContext& ctx);
  nlohmann::json list(int64_t applicationId, const ListQuery& query);
  nlohmann::json get(int64_t id);
  // gitSha and gitMessage record the commit that triggered the deploy. Empty keeps the
  // previous behavior: the agent checks out the tip of the configured branch.
  nlohmann::json create(int64_t applicationId, const User& actor, bool noCache = false,
                        const std::string& gitSha = {}, const std::string& gitMessage = {});
  nlohmann::json logs(int64_t id, int since, const std::string& phase = "");
  nlohmann::json listReplicas(int64_t applicationId, const ListQuery& query);
  nlohmann::json getReplica(int64_t id);
  void stopReplica(int64_t id);
  void restartReplica(int64_t id);
  // Remove container on the agent and delete the replica row. Optionally reconverge scale.
  void retireReplica(int64_t id, bool reconverge = true);
  void purgeInactiveReplicas(int64_t applicationId);
  void onContainerStatus(const highcube::ContainerStatus& status);
  void onDeploymentLog(const highcube::DeploymentLog& log);
  nlohmann::json applicationMetrics(int64_t applicationId, const std::string& from,
                                    const std::string& to);

private:
  AppContext& ctx_;
  void stopPreviousGeneration(int64_t applicationId, int64_t currentDeploymentId);
  void startReplacementReplica(const Application& app, int64_t deploymentId,
                               int64_t replacesReplicaId);
  void advanceRollout(int64_t deploymentId);
  void finalizeRollout(int64_t applicationId, int64_t deploymentId);
};

class AutoscalerService {
public:
  explicit AutoscalerService(AppContext& ctx);
  void tick();

private:
  AppContext& ctx_;
  void evaluate(Application& app);
};

// Keeps application containers alive. On every tick it detects replicas that
// stopped reporting (silent falls) on online servers, records the incident, and
// relaunches them so the desired replica count is restored.
class SupervisorService {
public:
  explicit SupervisorService(AppContext& ctx);
  void tick();

private:
  AppContext& ctx_;
  void superviseApplication(const Application& app);
};

class GithubAppService {
public:
  explicit GithubAppService(AppContext& ctx);
  nlohmann::json list(const ListQuery& query, const User& actor);
  nlohmann::json get(int64_t id, const User& actor);
  nlohmann::json createManifest(const nlohmann::json& body, const User& actor);
  nlohmann::json completeManifest(int64_t id, const nlohmann::json& body, const User& actor);
  nlohmann::json setInstallation(int64_t id, const nlohmann::json& body, const User& actor);
  nlohmann::json syncInstallation(int64_t id, const User& actor);
  nlohmann::json listRepositories(int64_t id, const User& actor, int page, int pageSize);
  nlohmann::json listBranches(int64_t id, const std::string& repo, const User& actor);
  // Reads EXPOSE instructions from a Dockerfile. The result is informational.
  nlohmann::json dockerfilePorts(const User& actor, std::optional<int64_t> githubAppId,
                                 const std::string& repo, const std::string& repoUrl,
                                 const std::string& branch, const std::string& path);
  // GitHub calls this without a user session. The signature is X-Hub-Signature-256.
  nlohmann::json handleWebhook(int64_t id, const std::string& event, const std::string& signature,
                               const std::string& body);
  // Turns on the push webhook for every registered app. Apps created earlier shipped
  // with the hook inactive, so a push never reached hc-admin.
  void ensurePushWebhooks();
  void remove(int64_t id, const User& actor);
  std::string installationAccessToken(int64_t githubAppId);

private:
  AppContext& ctx_;
  GithubApp requireAccess(int64_t id, const User& actor) const;
};

class MetricsRetentionService {
public:
  explicit MetricsRetentionService(AppContext& ctx);
  void runOnce();

private:
  AppContext& ctx_;
};

std::string hashToken(const std::string& token);
std::string randomToken(std::size_t bytes = 32);

}  // namespace highcube::admin
