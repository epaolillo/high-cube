#include "services/services.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <iomanip>
#include <sstream>
#include <vector>

namespace highcube::admin {

nlohmann::json userToJson(const User& user, bool includeServers) {
  nlohmann::json j = {
      {"id", user.id},
      {"email", user.email},
      {"name", user.name},
      {"locale", user.locale},
      {"avatarPath", user.avatarPath},
      {"role", user.role},
      {"roleId", user.roleId},
  };
  if (includeServers) {
    j["serverIds"] = user.serverIds;
    j["domainIds"] = user.domainIds;
  }
  return j;
}

nlohmann::json serverToJson(const Server& server) {
  return {
      {"id", server.id},
      {"name", server.name},
      {"ip", server.ip},
      {"lastSeenAt", server.lastSeenAt},
      {"status", server.status},
      {"icon", server.icon},
  };
}

nlohmann::json volumeToJson(const ApplicationVolume& volume) {
  return {{"id", volume.id},
          {"name", volume.name},
          {"mountPath", volume.mountPath},
          {"hostPath", volume.hostPath},
          {"hostDirectory", volume.hostPath.empty()
                                ? nlohmann::json(nullptr)
                                : nlohmann::json(std::string(kVolumesRoot) + "/" + volume.hostPath)},
          {"dockerName", "highcube-" + std::to_string(volume.applicationId) + "-" + volume.name}};
}

nlohmann::json applicationToJson(const Application& app) {
  nlohmann::json deps = nlohmann::json::array();
  for (const auto id : app.dependencies) {
    deps.push_back(id);
  }
  nlohmann::json volumes = nlohmann::json::array();
  for (const auto& v : app.volumes) {
    volumes.push_back(volumeToJson(v));
  }
  nlohmann::json portMappings = nlohmann::json::array();
  for (const auto& mapping : app.portMappings) {
    portMappings.push_back(
        {{"containerPort", mapping.containerPort}, {"hostPort", mapping.hostPort}});
  }
  nlohmann::json targetServers = nlohmann::json::array();
  for (const auto& target : app.targetServers) {
    targetServers.push_back({{"serverId", target.serverId}, {"priority", target.priority}});
  }
  nlohmann::json hostnames = nlohmann::json::array();
  for (const auto& row : app.hostnames) {
    std::string subdomain = row.hostname;
    const auto suffix = "." + row.baseDomain;
    if (!row.baseDomain.empty() && row.hostname == row.baseDomain) {
      subdomain.clear();
    } else if (!row.baseDomain.empty() && row.hostname.size() > suffix.size() &&
               row.hostname.compare(row.hostname.size() - suffix.size(), suffix.size(),
                                    suffix) == 0) {
      subdomain = row.hostname.substr(0, row.hostname.size() - suffix.size());
    } else if (const auto dot = row.hostname.find('.'); dot != std::string::npos) {
      subdomain = row.hostname.substr(0, dot);
    }
    hostnames.push_back({{"id", row.id},
                         {"hostname", row.hostname},
                         {"subdomain", subdomain},
                         {"baseDomainId", row.baseDomainId},
                         {"tlsStatus", row.tlsStatus},
                         {"position", row.position}});
  }
  // Usernames only: the hashes never leave the server.
  nlohmann::json basicAuthUsers = nlohmann::json::array();
  {
    std::istringstream lines(app.basicAuthUsers);
    std::string line;
    while (std::getline(lines, line)) {
      const auto colon = line.find(':');
      if (colon != std::string::npos && colon > 0) {
        basicAuthUsers.push_back({{"username", line.substr(0, colon)}});
      }
    }
  }
  nlohmann::json j = {
      {"id", app.id},
      {"name", app.name},
      {"type", app.type},
      {"databaseFlavor", app.databaseFlavor},
      {"icon", app.icon},
      {"repoUrl", app.repoUrl},
      {"branch", app.branch},
      {"baseDirectory", app.baseDirectory},
      {"dockerfilePath", app.dockerfilePath},
      {"portsExposes", app.portsExposes},
      {"portMappings", portMappings},
      {"desiredReplicas", app.desiredReplicas},
      {"scaleMode", app.scaleMode},
      {"minReplicas", app.minReplicas},
      {"maxReplicas", app.maxReplicas},
      {"scaleTargetCpuPercent", app.scaleTargetCpuPercent},
      {"scaleTargetMemoryPercent", app.scaleTargetMemoryPercent},
      {"scaleTargetLoad", app.scaleTargetLoad},
      {"scaleCooldownSeconds", app.scaleCooldownSeconds},
      {"scaleWindowSeconds", app.scaleWindowSeconds},
      {"env", nlohmann::json::parse(app.envJson.empty() ? "{}" : app.envJson)},
      {"hostname", app.hostname},
      {"tlsStatus", app.tlsStatus},
      {"hostnames", hostnames},
      {"basicAuthUsers", basicAuthUsers},
      {"healthCheckType", app.healthCheckType},
      {"healthCheckPath", app.healthCheckPath},
      {"healthCheckCommand", app.healthCheckCommand},
      {"maxRestartCount", app.maxRestartCount},
      {"useGpu", app.useGpu},
      {"hasDeployKey", !app.deployKeyPublic.empty()},
      {"deployKeyPublic", app.deployKeyPublic},
      {"internalEndpoint",
       "hc-" + std::to_string(app.id) + ":" + std::to_string(app.portsExposes)},
      {"dependencies", deps},
      {"volumes", volumes},
      {"targetServers", targetServers},
      {"pendingRedeploy", app.pendingRedeploy},
      {"createdBy", app.createdBy},
      {"ownerEmail", app.ownerEmail},
      {"repoFullName", app.repoFullName},
      {"source", app.githubAppId ? "github_app" : "public"},
  };
  const auto dbEngine = managedDatabaseEngine(app);
  if (dbEngine.empty()) {
    j["dbEngine"] = nullptr;
  } else {
    j["dbEngine"] = dbEngine;
  }
  if (app.githubAppId) {
    j["githubAppId"] = *app.githubAppId;
  } else {
    j["githubAppId"] = nullptr;
  }
  return j;
}

nlohmann::json deploymentToJson(const Deployment& deployment) {
  return {
      {"id", deployment.id},
      {"applicationId", deployment.applicationId},
      {"gitSha", deployment.gitSha},
      {"gitMessage", deployment.gitMessage},
      {"status", deployment.status},
      {"noCache", deployment.noCache},
      {"deployAttempts", deployment.deployAttempts},
      {"createdBy", deployment.createdBy},
      {"createdAt", deployment.createdAt},
      {"hasBuildLog", !deployment.buildLog.empty()},
      {"hasDeployLog", !deployment.deployLog.empty() || !deployment.logText.empty()},
  };
}

nlohmann::json replicaToJson(const Replica& replica) {
  nlohmann::json j = {
      {"id", replica.id},
      {"deploymentId", replica.deploymentId},
      {"applicationId", replica.applicationId},
      {"serverId", replica.serverId},
      {"containerId", replica.containerId},
      {"status", replica.status},
      {"desiredState", replica.desiredState},
      {"restartCount", replica.restartCount},
      {"statusChangedAt", replica.statusChangedAt},
      {"lastSeenAt", replica.lastSeenAt},
  };
  if (replica.hostPort) {
    j["hostPort"] = *replica.hostPort;
  } else {
    j["hostPort"] = nullptr;
  }
  if (replica.replacesReplicaId) {
    j["replacesReplicaId"] = *replica.replacesReplicaId;
  } else {
    j["replacesReplicaId"] = nullptr;
  }
  return j;
}

nlohmann::json replicaIncidentToJson(const ReplicaIncident& incident) {
  nlohmann::json j = {
      {"id", incident.id},
      {"applicationId", incident.applicationId},
      {"kind", incident.kind},
      {"detail", incident.detail},
      {"restartCount", incident.restartCount},
      {"createdAt", incident.createdAt},
  };
  j["replicaId"] = incident.replicaId ? nlohmann::json(*incident.replicaId) : nlohmann::json(nullptr);
  j["serverId"] = incident.serverId ? nlohmann::json(*incident.serverId) : nlohmann::json(nullptr);
  return j;
}

nlohmann::json githubAppToJson(const GithubApp& app) {
  return {
      {"id", app.id},
      {"userId", app.userId},
      {"name", app.name},
      {"appId", app.appId ? nlohmann::json(*app.appId) : nullptr},
      {"slug", app.slug},
      {"htmlUrl", app.htmlUrl},
      {"ownerLogin", app.ownerLogin},
      {"installationId", app.installationId ? nlohmann::json(*app.installationId) : nullptr},
      {"status", app.status},
      {"createdAt", app.createdAt},
      {"ownerEmail", app.ownerEmail},
  };
}

std::string hashToken(const std::string& token) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  EVP_Digest(token.data(), token.size(), digest, &len, EVP_sha256(), nullptr);
  std::ostringstream oss;
  for (unsigned int i = 0; i < len; ++i) {
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
  }
  return oss.str();
}

std::string randomToken(std::size_t bytes) {
  std::vector<unsigned char> buffer(bytes);
  RAND_bytes(buffer.data(), static_cast<int>(buffer.size()));
  std::ostringstream oss;
  for (auto b : buffer) {
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
  }
  return oss.str();
}

}  // namespace highcube::admin
