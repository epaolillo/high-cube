#include "services/services.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace highcube::admin {
namespace {

std::string nowIso() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto tt = clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&tt, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << ".000Z";
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
  const auto tt = timegm(&tm);
  return std::chrono::system_clock::from_time_t(tt);
}

std::string isoUtc(std::chrono::system_clock::time_point tp) {
  const auto tt = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  gmtime_r(&tt, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << ".000Z";
  return oss.str();
}

}  // namespace

AutoscalerService::AutoscalerService(AppContext& ctx) : ctx_(ctx) {}

void AutoscalerService::tick() {
  const auto page = ctx_.repos->listApplications(ListQuery{1, 1000, "", "asc", ""});
  for (auto app : page.rows) {
    if (app.scaleMode == "manual" || app.scaleMode.empty()) {
      continue;
    }
    if (!app.volumes.empty()) {
      continue;
    }
    try {
      evaluate(app);
    } catch (...) {
      // Keep ticking other apps when one evaluation fails.
    }
  }
}

void AutoscalerService::evaluate(Application& app) {
  if (app.minReplicas < 1) {
    app.minReplicas = 1;
  }
  if (app.maxReplicas < app.minReplicas) {
    app.maxReplicas = app.minReplicas;
  }

  const auto last = parseIso(app.scaleLastActionAt);
  if (last.time_since_epoch().count() > 0) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now() - last)
                             .count();
    if (elapsed < app.scaleCooldownSeconds) {
      return;
    }
  }

  const int windowSec = std::max(30, app.scaleWindowSeconds);
  const auto to = std::chrono::system_clock::now();
  const auto from = to - std::chrono::seconds(windowSec);
  const auto fromIso = isoUtc(from);
  const auto toIso = isoUtc(to);

  const auto metrics = ctx_.repos->listApplicationMetrics(app.id, fromIso, toIso);
  double cpuSum = 0;
  int cpuSamples = 0;
  double memPctSum = 0;
  int memSamples = 0;
  for (const auto& row : metrics) {
    const auto payload = nlohmann::json::parse(row.payloadJson, nullptr, false);
    if (!payload.is_object()) {
      continue;
    }
    if (payload.contains("cpuPercent")) {
      cpuSum += payload.value("cpuPercent", 0.0);
      ++cpuSamples;
    }
    const auto used = payload.value("memoryUsedBytes", 0.0);
    const auto limit = payload.value("memoryLimitBytes", 0.0);
    if (limit > 0) {
      memPctSum += (used / limit) * 100.0;
      ++memSamples;
    }
  }

  double loadAvg = 0;
  int loadSamples = 0;
  auto replicas = ctx_.repos->listReplicas(app.id, ListQuery{1, 1000, "", "asc", ""});
  std::unordered_set<int64_t> serverIds;
  int running = 0;
  for (const auto& replica : replicas.rows) {
    if (replica.desiredState == "running") {
      ++running;
      serverIds.insert(replica.serverId);
    }
  }
  for (const auto serverId : serverIds) {
    const auto hostRows = ctx_.repos->listServerMetrics(serverId, fromIso, toIso);
    for (const auto& row : hostRows) {
      if (row.replicaId) {
        continue;
      }
      const auto payload = nlohmann::json::parse(row.payloadJson, nullptr, false);
      if (!payload.is_object() || !payload.contains("load")) {
        continue;
      }
      const auto& load = payload.at("load");
      if (load.is_object() && load.contains("1")) {
        loadAvg += load.value("1", 0.0);
        ++loadSamples;
      }
    }
  }

  const double avgCpu = cpuSamples > 0 ? cpuSum / cpuSamples : -1;
  const double avgMem = memSamples > 0 ? memPctSum / memSamples : -1;
  const double avgLoadPerReplica =
      (loadSamples > 0 && running > 0) ? (loadAvg / loadSamples) / running : -1;

  bool scaleUp = false;
  bool scaleDown = false;
  const double cpuTarget = app.scaleTargetCpuPercent;
  const double memTarget = app.scaleTargetMemoryPercent;
  const double loadTarget = app.scaleTargetLoad;
  const double downFactor = 0.7;

  auto consider = [&](bool up, bool down) {
    scaleUp = scaleUp || up;
    scaleDown = scaleDown || down;
  };

  if (app.scaleMode == "cpu" || app.scaleMode == "composite") {
    if (avgCpu >= 0) {
      consider(avgCpu > cpuTarget, avgCpu < cpuTarget * downFactor);
    }
  }
  if (app.scaleMode == "memory" || app.scaleMode == "composite") {
    if (avgMem >= 0) {
      consider(avgMem > memTarget, avgMem < memTarget * downFactor);
    }
  }
  if (app.scaleMode == "load" || app.scaleMode == "composite") {
    if (avgLoadPerReplica >= 0) {
      consider(avgLoadPerReplica > loadTarget, avgLoadPerReplica < loadTarget * downFactor);
    }
  }

  if (app.scaleMode == "composite") {
    // Scale down only when every available signal is below hysteresis.
    bool allDown = true;
    bool anySignal = false;
    if (avgCpu >= 0) {
      anySignal = true;
      allDown = allDown && avgCpu < cpuTarget * downFactor;
    }
    if (avgMem >= 0) {
      anySignal = true;
      allDown = allDown && avgMem < memTarget * downFactor;
    }
    if (avgLoadPerReplica >= 0) {
      anySignal = true;
      allDown = allDown && avgLoadPerReplica < loadTarget * downFactor;
    }
    scaleDown = anySignal && allDown;
  }

  if (scaleUp && scaleDown) {
    scaleDown = false;
  }
  if (!scaleUp && !scaleDown) {
    return;
  }

  int next = app.desiredReplicas;
  if (scaleUp) {
    next = std::min(app.maxReplicas, app.desiredReplicas + 1);
  } else if (scaleDown) {
    next = std::max(app.minReplicas, app.desiredReplicas - 1);
  }
  if (next == app.desiredReplicas) {
    return;
  }

  app.desiredReplicas = next;
  app.scaleLastActionAt = nowIso();
  ctx_.repos->updateApplication(app);
  if (ctx_.placement) {
    ctx_.placement->converge(app.id);
  }
}

}  // namespace highcube::admin
