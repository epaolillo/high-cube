#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace highcube::admin {

struct Role {
  int64_t id = 0;
  std::string name;
  std::string code;  // "admin" | "user"
  bool isBuiltin = true;
};

struct User {
  int64_t id = 0;
  std::string email;
  std::string passwordHash;
  std::string name;
  std::string locale = "en";
  std::string avatarPath;
  int64_t roleId = 0;
  std::string role;  // "admin" | "user"
  std::vector<int64_t> serverIds;
  std::vector<int64_t> domainIds;
};

struct Server {
  int64_t id = 0;
  std::string name;
  std::string ip;
  std::string agentTokenHash;
  std::string lastSeenAt;
  std::string status = "pending";
};

struct BaseDomain {
  int64_t id = 0;
  std::string hostname;
  int64_t createdBy = 0;
};

struct ApplicationHostname {
  int64_t id = 0;
  int64_t applicationId = 0;
  std::string hostname;
  int64_t baseDomainId = 0;
  std::string tlsStatus = "pending";
  int position = 0;
};

struct ApplicationVolume {
  int64_t id = 0;
  int64_t applicationId = 0;
  std::string name;
  std::string mountPath;
};

// A server the application is allowed to run on. Lower priority is earlier in the round-robin.
struct ApplicationServerTarget {
  int64_t serverId = 0;
  int priority = 0;
};

struct PortMapping {
  int containerPort = 0;
  int hostPort = 0;  // 0 = agent picks a free host port

  bool operator==(const PortMapping& other) const {
    return containerPort == other.containerPort && hostPort == other.hostPort;
  }
};

struct GithubApp {
  int64_t id = 0;
  int64_t userId = 0;
  std::string name;
  std::optional<int64_t> appId;
  std::string slug;
  std::string clientId;
  std::string clientSecret;
  std::string webhookSecret;
  std::string privateKeyPem;
  std::string htmlUrl;
  std::string ownerLogin;
  std::optional<int64_t> installationId;
  std::string status = "pending";  // pending | registered | installed
  std::string createdAt;
  std::string ownerEmail;  // joined for admin list views
};

struct Application {
  int64_t id = 0;
  std::string name;
  std::string type = "web";
  // Catalog logo path ("/app-images/<logo>") or an uploaded square data URL.
  std::string icon;
  std::string repoUrl;
  std::string branch = "main";
  std::string baseDirectory = "/";
  std::string dockerfilePath = "Dockerfile";
  int portsExposes = 80;
  std::vector<PortMapping> portMappings;
  int desiredReplicas = 1;
  // Scale: manual | cpu | memory | load | composite
  std::string scaleMode = "manual";
  int minReplicas = 1;
  int maxReplicas = 1;
  double scaleTargetCpuPercent = 70.0;
  double scaleTargetMemoryPercent = 80.0;
  double scaleTargetLoad = 1.0;
  int scaleCooldownSeconds = 120;
  int scaleWindowSeconds = 180;
  std::string scaleLastActionAt;
  std::string envJson = "{}";
  std::string hostname;  // primary hostname (position 0), mirrored for list views
  std::string tlsStatus = "pending";  // primary hostname TLS status
  std::vector<ApplicationHostname> hostnames;
  std::string healthCheckType = "none";
  std::string healthCheckPath = "/";
  std::string healthCheckCommand;
  int maxRestartCount = 0;
  std::string deployKeyPublic;
  std::string deployKeyPrivate;
  int64_t createdBy = 0;
  std::optional<int64_t> githubAppId;
  std::string repoFullName;
  std::vector<int64_t> dependencies;
  std::vector<ApplicationVolume> volumes;
  std::vector<ApplicationServerTarget> targetServers;
  bool pendingRedeploy = false;
  std::string ownerEmail;  // joined for admin list views
};

struct Deployment {
  int64_t id = 0;
  int64_t applicationId = 0;
  std::string gitSha;
  std::string gitMessage;
  std::string status = "pending";
  std::string logText;
  std::string buildLog;
  std::string deployLog;
  bool noCache = false;
  int64_t createdBy = 0;
  std::string createdAt;
};

struct Replica {
  int64_t id = 0;
  int64_t deploymentId = 0;
  int64_t applicationId = 0;
  int64_t serverId = 0;
  std::string containerId;
  std::optional<int> hostPort;
  // IP on the highcube Docker network; nginx proxies public hostnames to it.
  std::string containerIp;
  std::string status = "pending";
  std::string desiredState = "running";
  int restartCount = 0;
  std::string statusChangedAt;
  // Last time an agent reported this container alive. Used to detect silent falls.
  std::string lastSeenAt;
  // When set, this replica is a rolling replacement for the given prior replica.
  std::optional<int64_t> replacesReplicaId;
};

// A traceable event in the life of a replica container: a Docker restart, a
// health/failure transition, a detected fall, or an orchestrator recovery.
struct ReplicaIncident {
  int64_t id = 0;
  int64_t applicationId = 0;
  std::optional<int64_t> replicaId;
  std::optional<int64_t> serverId;
  std::string kind;  // restart | unhealthy | failed | down | healed
  std::string detail;
  int restartCount = 0;
  std::string createdAt;
};

struct MetricRow {
  int64_t id = 0;
  int64_t serverId = 0;
  std::optional<int64_t> replicaId;
  std::string payloadJson;
  std::string collectedAt;
};

// A single audited action performed by a user against the control API.
struct UserActivity {
  int64_t id = 0;
  int64_t userId = 0;
  std::string action;      // semantic key, e.g. "application.deploy"
  std::string method;      // HTTP method
  std::string path;        // concrete request path
  std::string targetType;  // affected resource type when known
  std::string targetId;    // affected resource id when known
  std::string detail;      // optional JSON with extra context
  int status = 0;          // HTTP response status
  std::string ip;
  std::string createdAt;
};

struct ListQuery {
  int page = 1;
  int pageSize = 10;
  std::string sort;
  std::string order = "asc";
  std::string q;
  std::optional<int64_t> ownerId;  // when set, filter applications by created_by
  std::string typeFilter;          // when set, filter applications by category (type)
};

template <typename T>
struct Page {
  std::vector<T> rows;
  int64_t total = 0;
};

class ForbiddenError : public std::runtime_error {
public:
  explicit ForbiddenError(const std::string& message = "Forbidden")
      : std::runtime_error(message) {}
};

}  // namespace highcube::admin
