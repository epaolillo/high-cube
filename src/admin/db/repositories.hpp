#pragma once

#include "db/database.hpp"
#include "domain/models.hpp"

#include <optional>
#include <string>
#include <vector>

namespace highcube::admin {

class Repositories {
public:
  explicit Repositories(Database& database);

  // Users / roles
  int64_t userCount();
  int64_t adminCount();
  std::optional<User> findUserByEmail(const std::string& email);
  std::optional<User> findUserById(int64_t id);
  Page<User> listUsers(const ListQuery& query);
  int64_t createUser(const User& user);
  void updateUser(const User& user);
  void deleteUser(int64_t id);
  std::vector<User> autocompleteUsers(const std::string& q);
  int countApplicationsByUser(int64_t userId);
  int countGithubAppsByUser(int64_t userId);

  std::optional<Role> findRoleByCode(const std::string& code);
  std::optional<Role> findRoleById(int64_t id);
  std::vector<int64_t> getUserServers(int64_t userId);
  void setUserServers(int64_t userId, const std::vector<int64_t>& serverIds);
  std::vector<Server> listHealthyServersForUser(int64_t userId);
  std::vector<int64_t> getUserBaseDomains(int64_t userId);
  void setUserBaseDomains(int64_t userId, const std::vector<int64_t>& domainIds);
  std::vector<BaseDomain> listBaseDomainsForUser(int64_t userId);
  bool userHasBaseDomain(int64_t userId, int64_t domainId);

  // Servers
  Page<Server> listServers(const ListQuery& query);
  std::optional<Server> findServerById(int64_t id);
  std::optional<Server> findServerByTokenHash(const std::string& hash);
  std::optional<Server> findServerByName(const std::string& name);
  int64_t createServer(const Server& server);
  void updateServer(const Server& server);
  void touchServer(int64_t id, const std::string& status);
  void deleteServer(int64_t id);
  std::vector<Server> autocompleteServers(const std::string& q);
  std::vector<Server> listHealthyServers();

  // Settings / base domains
  std::string getSetting(const std::string& key, const std::string& fallback = "");
  void setSetting(const std::string& key, const std::string& value);
  Page<BaseDomain> listBaseDomains(const ListQuery& query);
  int64_t createBaseDomain(const BaseDomain& domain);
  void deleteBaseDomain(int64_t id);
  std::optional<BaseDomain> findBaseDomainById(int64_t id);
  bool hostnameMatchesBaseDomain(const std::string& hostname);
  int applicationsUsingBaseDomain(int64_t id);
  std::vector<BaseDomain> autocompleteBaseDomains(const std::string& q);
  std::vector<ApplicationHostname> getApplicationHostnames(int64_t applicationId);
  void setApplicationHostnames(int64_t applicationId,
                               const std::vector<ApplicationHostname>& hostnames);
  bool hostnameExists(const std::string& hostname, std::optional<int64_t> excludeApplicationId = std::nullopt);
  bool subdomainLabelExists(const std::string& subdomain);
  void setHostnameTlsStatus(int64_t applicationId, const std::string& hostname,
                            const std::string& status);
  void syncPrimaryHostname(int64_t applicationId);

  // GitHub Apps
  Page<GithubApp> listGithubApps(const ListQuery& query);
  std::optional<GithubApp> findGithubAppById(int64_t id);
  int64_t createGithubApp(const GithubApp& app);
  void updateGithubApp(const GithubApp& app);
  void deleteGithubApp(int64_t id);
  int countApplicationsUsingGithubApp(int64_t githubAppId);

  // Applications
  Page<Application> listApplications(const ListQuery& query);
  std::optional<Application> findApplicationById(int64_t id);
  int64_t createApplication(const Application& app);
  void updateApplication(const Application& app);
  void deleteApplication(int64_t id);
  void setTlsStatus(int64_t id, const std::string& status);
  void setDeployKey(int64_t id, const std::string& pub, const std::string& priv);
  std::vector<Application> autocompleteApplications(const std::string& q, bool privateOnly = false,
                                                    std::optional<int64_t> ownerId = std::nullopt);
  std::vector<int64_t> getDependencies(int64_t applicationId);
  void setDependencies(int64_t applicationId, const std::vector<int64_t>& deps);
  std::vector<ApplicationServerTarget> getTargetServers(int64_t applicationId);
  void setTargetServers(int64_t applicationId, const std::vector<ApplicationServerTarget>& targets);
  std::vector<ApplicationVolume> getVolumes(int64_t applicationId);
  int64_t addVolume(int64_t applicationId, const std::string& name, const std::string& mountPath);
  void updateVolume(int64_t volumeId, const std::string& name, const std::string& mountPath);
  void deleteVolume(int64_t volumeId);
  std::optional<ApplicationVolume> findVolumeById(int64_t volumeId);

  // Deployments / replicas
  Page<Deployment> listDeployments(int64_t applicationId, const ListQuery& query);
  std::optional<Deployment> findDeploymentById(int64_t id);
  int64_t createDeployment(const Deployment& deployment);
  void updateDeployment(const Deployment& deployment);
  void appendDeploymentLog(int64_t id, const std::string& chunk);
  void appendDeploymentPhaseLog(int64_t id, const std::string& phase, const std::string& chunk);

  Page<Replica> listReplicas(int64_t applicationId, const ListQuery& query);
  std::optional<Replica> findReplicaById(int64_t id);
  int64_t createReplica(const Replica& replica);
  void updateReplica(const Replica& replica);
  void deleteReplica(int64_t id);
  // Record that an agent reported this container alive, refreshing its restart count.
  void touchReplicaSeen(int64_t id, int restartCount, const std::string& seenAt);
  int countReplicasForApplication(int64_t applicationId);
  int countReplicasOnServer(int64_t serverId, int64_t applicationId);
  std::vector<Replica> listReplicasForServer(int64_t serverId);

  // Replica incidents (crash traceability)
  int64_t recordReplicaIncident(const ReplicaIncident& incident);
  Page<ReplicaIncident> listReplicaIncidents(int64_t applicationId, const ListQuery& query);
  int64_t deleteIncidentsOlderThan(const std::string& cutoffIso);
  std::vector<MetricRow> listApplicationMetrics(int64_t applicationId, const std::string& from,
                                                const std::string& to);

  // Metrics
  void insertMetric(int64_t serverId, std::optional<int64_t> replicaId,
                    const std::string& payloadJson, const std::string& collectedAt);
  std::vector<MetricRow> listServerMetrics(int64_t serverId, const std::string& from,
                                           const std::string& to);
  std::vector<MetricRow> listReplicaMetrics(int64_t replicaId, const std::string& from,
                                            const std::string& to);
  int64_t deleteMetricsOlderThan(const std::string& cutoffIso);

private:
  Database& database_;
  User hydrateUser(SQLite::Statement& st);
  Application hydrateApplication(SQLite::Statement& st);
  GithubApp hydrateGithubApp(SQLite::Statement& st);
  void attachUserRelations(User& user);
  void attachApplicationRelations(Application& app);
};

}  // namespace highcube::admin
