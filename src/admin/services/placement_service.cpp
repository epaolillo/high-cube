#include "services/services.hpp"

#include <algorithm>
#include <limits>

namespace highcube::admin {
namespace {

bool countsTowardScale(const Replica& replica) {
  if (replica.desiredState != "running") {
    return false;
  }
  if (replica.status == "failed" || replica.status == "restart_limit_reached" ||
      replica.status == "unhealthy" || replica.status == "stopped") {
    return false;
  }
  return true;
}

int replicasOnServer(const std::vector<Replica>& replicas, int64_t serverId) {
  int count = 0;
  for (const auto& replica : replicas) {
    if (replica.serverId == serverId && countsTowardScale(replica)) {
      ++count;
    }
  }
  return count;
}

}  // namespace

PlacementService::PlacementService(AppContext& ctx) : ctx_(ctx) {}

int64_t PlacementService::chooseServer(const Application& app) {
  if (!app.dependencies.empty()) {
    std::optional<int64_t> pinned;
    for (const auto depId : app.dependencies) {
      auto replicas = ctx_.repos->listReplicas(depId, ListQuery{1, 1000, "", "asc", ""});
      if (replicas.rows.empty()) {
        throw std::runtime_error("Dependency is not running on any server");
      }
      const auto serverId = replicas.rows.front().serverId;
      if (pinned && *pinned != serverId) {
        throw std::runtime_error("Dependencies must run on the same server");
      }
      pinned = serverId;
    }
    if (app.createdBy > 0) {
      auto owner = ctx_.repos->findUserById(app.createdBy);
      if (owner && owner->role == "user") {
        const auto assigned = ctx_.repos->getUserServers(owner->id);
        if (std::find(assigned.begin(), assigned.end(), *pinned) == assigned.end()) {
          throw std::runtime_error("Pinned dependency server is not assigned to this user");
        }
      }
    }
    return *pinned;
  }

  if (!app.volumes.empty()) {
    auto replicas = ctx_.repos->listReplicas(app.id, ListQuery{1, 1000, "", "asc", ""});
    if (!replicas.rows.empty()) {
      return replicas.rows.front().serverId;
    }
  }

  std::vector<Server> servers;
  if (app.createdBy > 0) {
    auto owner = ctx_.repos->findUserById(app.createdBy);
    if (owner && owner->role == "user") {
      const auto assigned = ctx_.repos->getUserServers(owner->id);
      if (assigned.empty()) {
        throw std::runtime_error("No servers assigned to this user");
      }
      servers = ctx_.repos->listHealthyServersForUser(owner->id);
      if (servers.empty()) {
        throw std::runtime_error("None of the assigned servers are online");
      }
    }
  }
  if (servers.empty()) {
    servers = ctx_.repos->listHealthyServers();
  }
  if (servers.empty()) {
    throw std::runtime_error("No healthy servers available");
  }

  if (!app.targetServers.empty()) {
    auto ordered = app.targetServers;
    std::sort(ordered.begin(), ordered.end(),
              [](const ApplicationServerTarget& a, const ApplicationServerTarget& b) {
                return a.priority < b.priority;
              });
    std::vector<int64_t> available;
    available.reserve(ordered.size());
    for (const auto& target : ordered) {
      const auto match = std::find_if(servers.begin(), servers.end(), [&](const Server& server) {
        return server.id == target.serverId;
      });
      if (match != servers.end()) {
        available.push_back(match->id);
      }
    }
    if (available.empty()) {
      throw std::runtime_error("None of the selected servers are online");
    }
    // Round-robin in priority order: the next replica goes to the selected server
    // that currently has the fewest replicas. Ties stay with the higher priority.
    const auto placed =
        ctx_.repos->listReplicas(app.id, ListQuery{1, 1000, "", "asc", ""}).rows;
    int64_t bestId = available.front();
    int bestCount = std::numeric_limits<int>::max();
    for (const auto serverId : available) {
      const int count = replicasOnServer(placed, serverId);
      if (count < bestCount) {
        bestCount = count;
        bestId = serverId;
      }
    }
    return bestId;
  }

  int64_t bestId = servers.front().id;
  int bestCount = std::numeric_limits<int>::max();
  for (const auto& server : servers) {
    const int count = ctx_.repos->countReplicasOnServer(server.id, app.id);
    if (count < bestCount) {
      bestCount = count;
      bestId = server.id;
    }
  }
  return bestId;
}

namespace {

std::optional<int64_t> pickDeploymentId(Repositories& repos, int64_t applicationId) {
  auto deployments = repos.listDeployments(applicationId, ListQuery{1, 50, "", "desc", ""});
  for (const auto& deployment : deployments.rows) {
    if (deployment.status == "success" || deployment.status == "deploying") {
      return deployment.id;
    }
  }
  if (!deployments.rows.empty()) {
    return deployments.rows.front().id;
  }
  return std::nullopt;
}

}  // namespace

void PlacementService::converge(int64_t applicationId) {
  auto app = ctx_.repos->findApplicationById(applicationId);
  if (!app) {
    return;
  }

  if (ctx_.deployments) {
    ctx_.deployments->purgeInactiveReplicas(applicationId);
  }

  auto replicas = ctx_.repos->listReplicas(applicationId, ListQuery{1, 1000, "", "asc", ""});
  int running = 0;
  for (const auto& replica : replicas.rows) {
    if (countsTowardScale(replica)) {
      ++running;
    }
  }

  const auto deploymentId = pickDeploymentId(*ctx_.repos, applicationId);

  while (running < app->desiredReplicas) {
    if (!deploymentId) {
      break;
    }
    Replica replica;
    replica.applicationId = applicationId;
    replica.deploymentId = *deploymentId;
    replica.serverId = chooseServer(*app);
    replica.status = "pending";
    replica.desiredState = "running";
    replica.id = ctx_.repos->createReplica(replica);
    if (ctx_.agentHub) {
      ctx_.agentHub->deployReplica(replica.id);
    }
    ++running;
  }

  while (running > app->desiredReplicas) {
    bool retired = false;
    for (auto it = replicas.rows.rbegin(); it != replicas.rows.rend(); ++it) {
      if (!countsTowardScale(*it)) {
        continue;
      }
      if (ctx_.deployments) {
        ctx_.deployments->retireReplica(it->id, false);
      } else if (ctx_.agentHub) {
        ctx_.agentHub->stopReplica(it->id);
        ctx_.repos->deleteReplica(it->id);
      }
      --running;
      retired = true;
      break;
    }
    if (!retired) {
      break;
    }
    replicas = ctx_.repos->listReplicas(applicationId, ListQuery{1, 1000, "", "asc", ""});
  }

  ctx_.proxy->renderApplicationSite(*app);
}

}  // namespace highcube::admin
