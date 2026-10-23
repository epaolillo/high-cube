#include "services/services.hpp"

#include <highcube.pb.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace highcube::admin {
namespace {

std::string nowIso() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto tt = clock::to_time_t(now);
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  std::tm tm{};
  gmtime_r(&tt, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
      << ms.count() << 'Z';
  return oss.str();
}

std::string hourBucket(const std::string& iso) {
  if (iso.size() < 13) {
    return iso;
  }
  return iso.substr(0, 13) + ":00:00.000Z";
}

bool isReadyStatus(const std::string& status) {
  return status == "running" || status == "healthy";
}

bool isFailedStatus(const std::string& status) {
  return status == "failed" || status == "restart_limit_reached" || status == "unhealthy";
}

bool isInFlightStatus(const std::string& status) {
  return status == "pending" || status == "building" || status == "starting" ||
         status == "deploying";
}

bool isInactiveReplica(const Replica& replica) {
  if (replica.desiredState == "stopped") {
    return true;
  }
  return isFailedStatus(replica.status) || replica.status == "stopped";
}

}  // namespace

DeploymentService::DeploymentService(AppContext& ctx) : ctx_(ctx) {}

void DeploymentService::retireReplica(int64_t id, bool reconverge) {
  auto replica = ctx_.repos->findReplicaById(id);
  if (!replica) {
    return;
  }
  const int64_t applicationId = replica->applicationId;
  const int64_t serverId = replica->serverId;
  const auto containerId = replica->containerId;
  // Drop the row before touching the agent socket. A fault while sending must
  // not leave the same replica in place, or the supervisor crash-loops forever.
  ctx_.repos->deleteReplica(id);
  if (ctx_.agentHub) {
    try {
      ctx_.agentHub->stopContainer(serverId, id, applicationId, containerId);
    } catch (const std::exception& ex) {
      spdlog::error("stop replica {} failed: {}", id, ex.what());
    }
  }

  auto app = ctx_.repos->findApplicationById(applicationId);
  if (app) {
    ctx_.proxy->renderApplicationSite(*app);
  }
  if (reconverge && ctx_.placement) {
    ctx_.placement->converge(applicationId);
  }
  // After the row is gone (and a replacement, if any, is already recorded), drop
  // containers on that server that no longer belong to a replica.
  if (ctx_.agentHub) {
    ctx_.agentHub->reconcileServer(serverId);
  }
}

void DeploymentService::purgeInactiveReplicas(int64_t applicationId) {
  auto replicas = ctx_.repos->listReplicas(applicationId, ListQuery{1, 1000, "", "asc", ""});
  std::vector<int64_t> toRetire;
  for (const auto& replica : replicas.rows) {
    if (isInactiveReplica(replica)) {
      toRetire.push_back(replica.id);
    }
  }
  for (const auto id : toRetire) {
    retireReplica(id, false);
  }
}

void DeploymentService::stopPreviousGeneration(int64_t applicationId, int64_t currentDeploymentId) {
  auto replicas = ctx_.repos->listReplicas(applicationId, ListQuery{1, 1000, "", "asc", ""});
  std::vector<int64_t> toRetire;
  for (const auto& replica : replicas.rows) {
    if (replica.deploymentId != currentDeploymentId) {
      toRetire.push_back(replica.id);
    }
  }
  for (const auto id : toRetire) {
    retireReplica(id, false);
  }
  auto app = ctx_.repos->findApplicationById(applicationId);
  if (app) {
    ctx_.proxy->renderApplicationSite(*app);
  }
}

void DeploymentService::startReplacementReplica(const Application& app, int64_t deploymentId,
                                                int64_t replacesReplicaId) {
  Replica replica;
  replica.applicationId = app.id;
  replica.deploymentId = deploymentId;
  replica.serverId = ctx_.placement->chooseServer(app);
  replica.status = "pending";
  replica.desiredState = "running";
  replica.statusChangedAt = nowIso();
  if (replacesReplicaId > 0) {
    replica.replacesReplicaId = replacesReplicaId;
  }
  replica.id = ctx_.repos->createReplica(replica);
  if (ctx_.agentHub) {
    ctx_.agentHub->deployReplica(replica.id);
  }
}

void DeploymentService::finalizeRollout(int64_t applicationId, int64_t deploymentId) {
  stopPreviousGeneration(applicationId, deploymentId);
  purgeInactiveReplicas(applicationId);
  auto app = ctx_.repos->findApplicationById(applicationId);
  if (app && app->pendingRedeploy) {
    app->pendingRedeploy = false;
    ctx_.repos->updateApplication(*app);
  }
  auto deployment = ctx_.repos->findDeploymentById(deploymentId);
  if (deployment) {
    deployment->status = "success";
    // A clean rollout clears the failure budget so future relaunches get a fresh cap.
    deployment->deployAttempts = 0;
    ctx_.repos->updateDeployment(*deployment);
  }
  if (app) {
    ctx_.proxy->renderApplicationSite(*app);
  }
  if (ctx_.placement) {
    ctx_.placement->converge(applicationId);
  }
}

void DeploymentService::advanceRollout(int64_t deploymentId) {
  auto deployment = ctx_.repos->findDeploymentById(deploymentId);
  if (!deployment) {
    return;
  }
  auto app = ctx_.repos->findApplicationById(deployment->applicationId);
  if (!app) {
    return;
  }
  purgeInactiveReplicas(app->id);

  const int desired = std::max(1, app->desiredReplicas);
  auto replicas = ctx_.repos->listReplicas(app->id, ListQuery{1, 1000, "", "asc", ""});

  int readyCurrent = 0;
  int inFlightCurrent = 0;
  std::vector<Replica> oldStillRunning;
  for (const auto& replica : replicas.rows) {
    if (replica.deploymentId == deploymentId) {
      if (replica.desiredState != "running") {
        continue;
      }
      if (isReadyStatus(replica.status)) {
        ++readyCurrent;
      } else if (isInFlightStatus(replica.status) || replica.status.empty()) {
        ++inFlightCurrent;
      }
    } else if (replica.desiredState == "running" &&
               (isReadyStatus(replica.status) || isInFlightStatus(replica.status))) {
      oldStillRunning.push_back(replica);
    }
  }
  std::sort(oldStillRunning.begin(), oldStillRunning.end(),
            [](const Replica& a, const Replica& b) { return a.id < b.id; });

  if (inFlightCurrent > 0) {
    return;
  }

  if (!oldStillRunning.empty() && readyCurrent < desired) {
    startReplacementReplica(*app, deploymentId, oldStillRunning.front().id);
    return;
  }

  if (!oldStillRunning.empty()) {
    stopPreviousGeneration(app->id, deploymentId);
  }

  if (readyCurrent < desired) {
    if (ctx_.placement) {
      ctx_.placement->converge(app->id);
    }
    return;
  }

  finalizeRollout(app->id, deploymentId);
}

nlohmann::json DeploymentService::list(int64_t applicationId, const ListQuery& query) {
  const auto page = ctx_.repos->listDeployments(applicationId, query);
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& deployment : page.rows) {
    rows.push_back(deploymentToJson(deployment));
  }
  return {{"rows", rows}, {"total", page.total}};
}

nlohmann::json DeploymentService::get(int64_t id) {
  auto deployment = ctx_.repos->findDeploymentById(id);
  if (!deployment) {
    throw std::runtime_error("Deployment not found");
  }
  return deploymentToJson(*deployment);
}

nlohmann::json DeploymentService::create(int64_t applicationId, const User& actor, bool noCache,
                                             const std::string& gitSha, const std::string& gitMessage) {
  auto app = ctx_.repos->findApplicationById(applicationId);
  if (!app) {
    throw std::runtime_error("Application not found");
  }
  for (const auto depId : app->dependencies) {
    auto depReplicas = ctx_.repos->listReplicas(depId, ListQuery{1, 100, "", "asc", ""});
    bool ready = false;
    for (const auto& replica : depReplicas.rows) {
      if (isReadyStatus(replica.status)) {
        ready = true;
        break;
      }
    }
    if (!ready) {
      throw std::runtime_error("Dependencies must be ready before deploying");
    }
  }

  Deployment deployment;
  deployment.applicationId = applicationId;
  deployment.status = "pending";
  deployment.createdBy = actor.id;
  deployment.noCache = noCache;
  deployment.gitSha = gitSha;
  deployment.gitMessage = gitMessage;
  deployment.id = ctx_.repos->createDeployment(deployment);

  app->desiredReplicas = std::max(1, app->desiredReplicas);
  ctx_.repos->updateApplication(*app);

  // Drop already-dead rows so the roll starts from a clean active set.
  purgeInactiveReplicas(applicationId);

  auto existing = ctx_.repos->listReplicas(applicationId, ListQuery{1, 1000, "", "asc", ""});
  std::vector<Replica> oldRunning;
  for (const auto& replica : existing.rows) {
    if (replica.desiredState == "running" &&
        (isReadyStatus(replica.status) || isInFlightStatus(replica.status) ||
         replica.status == "pending" || replica.status.empty())) {
      oldRunning.push_back(replica);
    }
  }
  std::sort(oldRunning.begin(), oldRunning.end(),
            [](const Replica& a, const Replica& b) { return a.id < b.id; });

  if (oldRunning.empty()) {
    for (int i = 0; i < app->desiredReplicas; ++i) {
      startReplacementReplica(*app, deployment.id, 0);
    }
  } else if (!app->volumes.empty()) {
    stopPreviousGeneration(applicationId, deployment.id);
    startReplacementReplica(*app, deployment.id, 0);
  } else {
    startReplacementReplica(*app, deployment.id, oldRunning.front().id);
  }

  deployment.status = "deploying";
  ctx_.repos->updateDeployment(deployment);
  return deploymentToJson(*ctx_.repos->findDeploymentById(deployment.id));
}

nlohmann::json DeploymentService::logs(int64_t id, int since, const std::string& phase) {
  auto deployment = ctx_.repos->findDeploymentById(id);
  if (!deployment) {
    throw std::runtime_error("Deployment not found");
  }
  if (since < 0) {
    since = 0;
  }
  const std::string* source = &deployment->deployLog;
  if (phase == "build") {
    source = &deployment->buildLog;
  } else if (phase == "legacy" || (phase.empty() && deployment->deployLog.empty())) {
    source = &deployment->logText;
  } else if (phase.empty()) {
    source = &deployment->deployLog;
  }
  if (static_cast<size_t>(since) > source->size()) {
    since = static_cast<int>(source->size());
  }
  const std::string chunk = source->substr(static_cast<size_t>(since));
  return {{"offset", since + static_cast<int>(chunk.size())},
          {"chunk", chunk},
          {"phase", phase.empty() ? "deploy" : phase},
          {"status", deployment->status}};
}

nlohmann::json DeploymentService::listReplicas(int64_t applicationId, const ListQuery& query) {
  purgeInactiveReplicas(applicationId);
  const auto page = ctx_.repos->listReplicas(applicationId, query);
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& replica : page.rows) {
    auto json = replicaToJson(replica);
    if (auto server = ctx_.repos->findServerById(replica.serverId)) {
      json["serverName"] = server->name;
      json["serverStatus"] = server->status;
    } else {
      json["serverName"] = "";
      json["serverStatus"] = "removed";
    }
    rows.push_back(json);
  }
  return {{"rows", rows}, {"total", page.total}};
}

nlohmann::json DeploymentService::getReplica(int64_t id) {
  auto replica = ctx_.repos->findReplicaById(id);
  if (!replica) {
    throw std::runtime_error("Replica not found");
  }
  auto json = replicaToJson(*replica);
  if (auto server = ctx_.repos->findServerById(replica->serverId)) {
    json["serverName"] = server->name;
    json["serverStatus"] = server->status;
  }
  json["shellAvailable"] =
      !replica->containerId.empty() &&
      (replica->status == "running" || replica->status == "healthy") &&
      ctx_.agentHub && ctx_.agentHub->isConnected(replica->serverId);
  return json;
}

void DeploymentService::stopReplica(int64_t id) {
  auto replica = ctx_.repos->findReplicaById(id);
  if (!replica) {
    throw std::runtime_error("Replica not found");
  }
  // Stop retires the instance; converge restores the configured scale.
  retireReplica(id, true);
}

void DeploymentService::restartReplica(int64_t id) {
  auto replica = ctx_.repos->findReplicaById(id);
  if (!replica) {
    throw std::runtime_error("Replica not found");
  }
  if (ctx_.agentHub) {
    ctx_.agentHub->restartReplica(id);
  }
}

void DeploymentService::onContainerStatus(const ::highcube::ContainerStatus& status) {
  auto replica = ctx_.repos->findReplicaById(status.replica_id());
  if (!replica) {
    return;
  }
  const int64_t applicationId = replica->applicationId;
  const int64_t deploymentId = replica->deploymentId;
  const auto replacesId = replica->replacesReplicaId;

  auto deployment = ctx_.repos->findDeploymentById(deploymentId);
  if (deployment && !status.message().empty()) {
    ctx_.repos->appendDeploymentLog(deployment->id, status.message() + "\n");
    ctx_.repos->appendDeploymentPhaseLog(deployment->id, "deploy", status.message() + "\n");
  }
  if (deployment) {
    if (!status.git_sha().empty()) {
      deployment->gitSha = status.git_sha();
    }
    if (!status.git_message().empty()) {
      deployment->gitMessage = status.git_message();
    }
  }

  // Failed or stopped containers are removed immediately. Prior healthy generation stays.
  if (isFailedStatus(status.status()) || status.status() == "stopped") {
    if (deployment) {
      if (isFailedStatus(status.status())) {
        deployment->status = "failed";
        // Count the failed attempt so the orchestrator can stop relaunching a
        // deployment that never manages to build or start.
        deployment->deployAttempts += 1;
      }
      ctx_.repos->updateDeployment(*deployment);
    }
    // Trace the fall before the replica row is removed, so operators keep history.
    if (isFailedStatus(status.status())) {
      ReplicaIncident incident;
      incident.applicationId = applicationId;
      incident.replicaId = replica->id;
      incident.serverId = replica->serverId;
      incident.kind = status.status() == "unhealthy" ? "unhealthy" : "failed";
      incident.detail =
          status.message().empty() ? ("Container reported " + status.status()) : status.message();
      incident.restartCount = replica->restartCount;
      ctx_.repos->recordReplicaIncident(incident);
    }
    retireReplica(replica->id, false);
    auto app = ctx_.repos->findApplicationById(applicationId);
    if (app) {
      ctx_.proxy->renderApplicationSite(*app);
    }
    // Restore scale using a successful generation when the failed one was a replacement.
    // Do not keep creating replicas from a failed deployment.
    if (ctx_.placement && app) {
      int active = 0;
      auto remaining = ctx_.repos->listReplicas(applicationId, ListQuery{1, 1000, "", "asc", ""});
      for (const auto& row : remaining.rows) {
        if (row.desiredState == "running" &&
            (isReadyStatus(row.status) || isInFlightStatus(row.status) || row.status.empty() ||
             row.status == "pending")) {
          ++active;
        }
      }
      if (active < std::max(1, app->desiredReplicas)) {
        // Prefer last successful deployment for replacements after a failure.
        auto deps = ctx_.repos->listDeployments(applicationId, ListQuery{1, 20, "", "desc", ""});
        bool hasSuccess = false;
        for (const auto& dep : deps.rows) {
          if (dep.status == "success") {
            hasSuccess = true;
            break;
          }
        }
        if (hasSuccess || !isFailedStatus(status.status())) {
          ctx_.placement->converge(applicationId);
        }
      }
    }
    return;
  }

  const bool statusChanged = replica->status != status.status();
  replica->containerId = status.container_id();
  if (status.host_port() > 0) {
    replica->hostPort = status.host_port();
  }
  if (!status.container_ip().empty()) {
    replica->containerIp = status.container_ip();
  }
  replica->status = status.status();
  replica->restartCount = status.restart_count();
  if (statusChanged) {
    replica->statusChangedAt = nowIso();
  }
  ctx_.repos->updateReplica(*replica);

  if (deployment) {
    if (isReadyStatus(status.status())) {
      if (deployment->status != "success") {
        deployment->status = "deploying";
      }
    } else if (status.status() == "building" || status.status() == "starting") {
      deployment->status = "deploying";
    }
    ctx_.repos->updateDeployment(*deployment);
  }

  auto app = ctx_.repos->findApplicationById(applicationId);
  if (app) {
    ctx_.proxy->renderApplicationSite(*app);
  }

  if (deployment && isReadyStatus(status.status()) && replica->desiredState == "running") {
    if (replacesId) {
      auto old = ctx_.repos->findReplicaById(*replacesId);
      if (old) {
        retireReplica(old->id, false);
        if (app) {
          ctx_.proxy->renderApplicationSite(*app);
        }
      }
    }
    advanceRollout(deployment->id);
  }
}

void DeploymentService::onDeploymentLog(const ::highcube::DeploymentLog& log) {
  if (log.deployment_id() <= 0 || log.data().empty()) {
    return;
  }
  const std::string phase = log.phase().empty() ? "deploy" : log.phase();
  ctx_.repos->appendDeploymentPhaseLog(log.deployment_id(), phase, log.data());
  if (phase != "build") {
    ctx_.repos->appendDeploymentLog(log.deployment_id(), log.data());
  }
}

nlohmann::json DeploymentService::applicationMetrics(int64_t applicationId, const std::string& from,
                                                     const std::string& to) {
  const auto rows = ctx_.repos->listApplicationMetrics(applicationId, from, to);
  struct Bucket {
    double cpuSum = 0;
    double memSum = 0;
    double diskSum = 0;
    int samples = 0;
    int restartMax = 0;
    int restartMin = -1;
  };
  std::unordered_map<std::string, Bucket> buckets;
  nlohmann::json latest = nullptr;
  nlohmann::json latestVolumes = nlohmann::json::array();
  for (const auto& row : rows) {
    const auto payload = nlohmann::json::parse(row.payloadJson, nullptr, false);
    if (!payload.is_object()) {
      continue;
    }
    const auto key = hourBucket(row.collectedAt);
    auto& bucket = buckets[key];
    bucket.cpuSum += payload.value("cpuPercent", 0.0);
    bucket.memSum += static_cast<double>(payload.value("memoryUsedBytes", 0LL));
    bucket.diskSum += static_cast<double>(payload.value("diskUsedBytes", 0LL));
    bucket.samples += 1;
    const int restartCount = payload.value("restartCount", 0);
    bucket.restartMax = std::max(bucket.restartMax, restartCount);
    if (bucket.restartMin < 0 || restartCount < bucket.restartMin) {
      bucket.restartMin = restartCount;
    }
    latest = payload;
    latest["collectedAt"] = row.collectedAt;
    latest["replicaId"] = row.replicaId ? *row.replicaId : 0;
    if (payload.contains("volumes") && payload["volumes"].is_array()) {
      latestVolumes = payload["volumes"];
    }
  }

  std::vector<std::string> keys;
  keys.reserve(buckets.size());
  for (const auto& [key, _] : buckets) {
    keys.push_back(key);
  }
  std::sort(keys.begin(), keys.end());

  nlohmann::json series = nlohmann::json::array();
  int previousRestart = -1;
  for (const auto& key : keys) {
    const auto& bucket = buckets[key];
    const double samples = std::max(1, bucket.samples);
    int restarts = 0;
    if (bucket.restartMin >= 0) {
      if (previousRestart >= 0) {
        restarts = std::max(0, bucket.restartMax - previousRestart);
      }
      previousRestart = bucket.restartMax;
    }
    series.push_back({{"at", key},
                      {"cpuPercent", bucket.cpuSum / samples},
                      {"memoryUsedBytes", bucket.memSum / samples},
                      {"diskUsedBytes", bucket.diskSum / samples},
                      {"restarts", restarts}});
  }

  return {{"series", series}, {"latest", latest}, {"volumes", latestVolumes}};
}

}  // namespace highcube::admin
