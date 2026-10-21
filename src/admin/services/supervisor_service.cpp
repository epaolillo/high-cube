#include "services/services.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace highcube::admin {
namespace {

// A container must miss several heartbeats (reported every ~30s) before we treat
// it as fallen, to avoid reacting to a single late or dropped metrics message.
constexpr int kDownThresholdSeconds = 120;
// Silence alone is not proof: a frozen or reconnecting agent, a restarted admin or a slow
// Docker daemon silence every replica of a server at once. The agent must also have
// completed this many full container rounds that no longer listed the replica.
constexpr int kMissedRoundsBeforeDown = 2;

std::string nowIso() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

std::chrono::system_clock::time_point parseIso(const std::string& iso) {
  if (iso.size() < 19) {
    return std::chrono::system_clock::time_point{};
  }
  std::tm tm{};
  std::istringstream iss(iso.substr(0, 19));
  iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (iss.fail()) {
    return std::chrono::system_clock::time_point{};
  }
  return std::chrono::system_clock::from_time_t(timegm(&tm));
}

// A replica that is supposed to be up and is not already known to be failing.
bool shouldBeAlive(const Replica& replica) {
  if (replica.desiredState != "running") {
    return false;
  }
  return replica.status != "failed" && replica.status != "restart_limit_reached" &&
         replica.status != "unhealthy" && replica.status != "stopped";
}

// Replicas that still count toward the running target after a healing pass.
bool countsAsActive(const Replica& replica) {
  if (replica.desiredState != "running") {
    return false;
  }
  return replica.status != "failed" && replica.status != "restart_limit_reached" &&
         replica.status != "unhealthy" && replica.status != "stopped";
}

}  // namespace

SupervisorService::SupervisorService(AppContext& ctx) : ctx_(ctx) {}

void SupervisorService::tick() {
  const auto page = ctx_.repos->listApplications(ListQuery{1, 1000, "", "asc", ""});
  for (const auto& app : page.rows) {
    try {
      superviseApplication(app);
    } catch (const std::exception& ex) {
      spdlog::error("supervisor: application {} failed: {}", app.id, ex.what());
    } catch (...) {
      // Keep supervising the remaining applications even if one throws.
    }
  }
}

void SupervisorService::superviseApplication(const Application& app) {
  // Applications intentionally stopped (desiredReplicas == 0) must stay down.
  if (app.desiredReplicas < 1) {
    return;
  }

  auto replicas = ctx_.repos->listReplicas(app.id, ListQuery{1, 1000, "", "asc", ""});
  const auto now = std::chrono::system_clock::now();

  int fallen = 0;
  for (const auto& replica : replicas.rows) {
    if (!shouldBeAlive(replica)) {
      continue;
    }
    // A container that has never reported yet is still being rolled out; leave it
    // to the deployment flow instead of tearing it down prematurely.
    if (replica.lastSeenAt.empty()) {
      continue;
    }
    // Only act while the hosting server is online. An offline server is handled by
    // the offline-detection path and must not cause us to destroy its replicas.
    const auto server = ctx_.repos->findServerById(replica.serverId);
    if (!server || server->status != "online") {
      continue;
    }
    const auto last = parseIso(replica.lastSeenAt);
    if (last.time_since_epoch().count() == 0) {
      continue;
    }
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - last).count();
    if (age <= kDownThresholdSeconds) {
      continue;
    }
    // lastSeenAt has second precision, so the round that last listed the replica can
    // still count here; one extra round covers it.
    const int rounds =
        ctx_.agentHub ? ctx_.agentHub->containerRoundsSince(replica.serverId, last) : 0;
    if (rounds <= kMissedRoundsBeforeDown) {
      continue;
    }

    // The agent keeps reporting its containers and this one is no longer among them.
    ReplicaIncident incident;
    incident.applicationId = app.id;
    incident.replicaId = replica.id;
    incident.serverId = replica.serverId;
    incident.kind = "down";
    incident.detail = "Missing from " + std::to_string(rounds) + " agent reports over " +
                      std::to_string(age) + "s (last status '" + replica.status +
                      "'); relaunching";
    incident.restartCount = replica.restartCount;
    ctx_.repos->recordReplicaIncident(incident);
    spdlog::warn("supervisor: replica {} of app {} fell silent for {}s; relaunching",
                 replica.id, app.id, age);

    if (ctx_.deployments) {
      ctx_.deployments->retireReplica(replica.id, false);
    }
    ++fallen;
  }

  // Recompute how many replicas are still alive and relaunch any deficit.
  auto current = ctx_.repos->listReplicas(app.id, ListQuery{1, 1000, "", "asc", ""});
  int active = 0;
  for (const auto& replica : current.rows) {
    if (countsAsActive(replica)) {
      ++active;
    }
  }

  if (active < app.desiredReplicas && ctx_.placement) {
    ctx_.placement->converge(app.id);

    auto after = ctx_.repos->listReplicas(app.id, ListQuery{1, 1000, "", "asc", ""});
    int restored = 0;
    for (const auto& replica : after.rows) {
      if (countsAsActive(replica)) {
        ++restored;
      }
    }
    if (restored > active) {
      ReplicaIncident healed;
      healed.applicationId = app.id;
      healed.kind = "healed";
      healed.detail = "Orchestrator relaunched " + std::to_string(restored - active) +
                      " replica(s) to restore the desired count (" +
                      std::to_string(app.desiredReplicas) + ")";
      ctx_.repos->recordReplicaIncident(healed);
      spdlog::info("supervisor: app {} healed, {} -> {} active replicas", app.id, active,
                   restored);
    }
  } else if (fallen > 0) {
    spdlog::info("supervisor: app {} had {} fallen replica(s) already at desired count",
                 app.id, fallen);
  }
}

}  // namespace highcube::admin
