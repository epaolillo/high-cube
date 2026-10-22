#include "services/services.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace highcube::admin {
namespace {

std::string isoFromTimePoint(std::chrono::system_clock::time_point tp) {
  const std::time_t t = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

// Parse an ISO-8601 timestamp (with or without fractional seconds) to epoch
// seconds. Returns nullopt when the value cannot be understood.
std::optional<std::time_t> parseIso(const std::string& value) {
  std::tm tm{};
  std::istringstream iss(value);
  iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (iss.fail()) {
    return std::nullopt;
  }
  return timegm(&tm);
}

}  // namespace

nlohmann::json userActivityToJson(const UserActivity& activity) {
  return {
      {"id", activity.id},
      {"userId", activity.userId},
      {"action", activity.action},
      {"method", activity.method},
      {"path", activity.path},
      {"targetType", activity.targetType},
      {"targetId", activity.targetId},
      {"detail", activity.detail},
      {"status", activity.status},
      {"ip", activity.ip},
      {"createdAt", activity.createdAt},
  };
}

DerivedActivity deriveActivity(const std::string& method, const std::string& routePattern) {
  // "METHOD pattern" -> "action|targetType". Only mutating, meaningful routes
  // are listed; anything missing but still mutating is captured generically.
  static const std::unordered_map<std::string, std::string> kMap = {
      {"PATCH /api/profile", "profile.update|profile"},
      {"POST /api/profile/avatar", "profile.avatarUpdate|profile"},
      {"POST /api/users", "user.create|user"},
      {"PATCH /api/users/:id", "user.update|user"},
      {"PUT /api/users/:id/servers", "user.setServers|user"},
      {"PUT /api/users/:id/domains", "user.setDomains|user"},
      {"DELETE /api/users/:id", "user.delete|user"},
      {"POST /api/settings/base-domains", "baseDomain.create|baseDomain"},
      {"PATCH /api/settings/base-domains/:id", "baseDomain.setAccessPoint|baseDomain"},
      {"DELETE /api/settings/base-domains/:id", "baseDomain.delete|baseDomain"},
      {"PATCH /api/settings/metrics", "settings.updateMetrics|settings"},
      {"POST /api/servers", "server.create|server"},
      {"PATCH /api/servers/:id", "server.update|server"},
      {"POST /api/servers/:id/token", "server.rotateToken|server"},
      {"DELETE /api/servers/:id", "server.delete|server"},
      {"POST /api/servers/:id/terminal/sessions", "server.terminalOpen|server"},
      {"DELETE /api/servers/:id/terminal/sessions/:streamId", "server.terminalClose|server"},
      {"POST /api/github-apps/manifest", "github.createManifest|github"},
      {"POST /api/github-apps/:id/manifest/complete", "github.completeManifest|github"},
      {"POST /api/github-apps/:id/installation", "github.setInstallation|github"},
      {"POST /api/github-apps/:id/installation/sync", "github.syncInstallation|github"},
      {"DELETE /api/github-apps/:id", "github.delete|github"},
      {"POST /api/applications", "application.create|application"},
      {"PATCH /api/applications/:id", "application.update|application"},
      {"POST /api/applications/:id/scale", "application.scale|application"},
      {"POST /api/applications/:id/stop", "application.stop|application"},
      {"POST /api/applications/:id/deploy-key", "application.createDeployKey|application"},
      {"POST /api/applications/:id/certificate", "application.requestCertificate|application"},
      {"DELETE /api/applications/:id", "application.delete|application"},
      {"POST /api/applications/:id/volumes", "application.addVolume|application"},
      {"PATCH /api/applications/:id/volumes/:volumeId", "application.updateVolume|application"},
      {"DELETE /api/applications/:id/volumes/:volumeId", "application.deleteVolume|application"},
      {"POST /api/applications/:id/deployments", "application.deploy|application"},
      {"POST /api/applications/:id/db-manage", "application.dbManage|application"},
      {"POST /api/replicas/:id/stop", "replica.stop|replica"},
      {"POST /api/replicas/:id/restart", "replica.restart|replica"},
      {"POST /api/replicas/:id/terminal/sessions", "replica.terminalOpen|replica"},
      {"DELETE /api/replicas/:id/terminal/sessions/:streamId", "replica.terminalClose|replica"},
  };

  const std::string key = method + " " + routePattern;

  // Noisy or internal routes we deliberately keep out of the audit trail.
  if (routePattern.find("/terminal/sessions/") != std::string::npos &&
      (routePattern.find("/input") != std::string::npos ||
       routePattern.find("/resize") != std::string::npos)) {
    return {"", ""};
  }
  if (key == "POST /api/applications/:id/port-check") {
    return {"", ""};
  }
  // Watching container output is read-only, like any GET.
  if (routePattern.find("/logs/sessions") != std::string::npos) {
    return {"", ""};
  }

  const auto it = kMap.find(key);
  if (it == kMap.end()) {
    // Still capture everything: unknown mutating routes get a generic action.
    return {key, ""};
  }
  const auto sep = it->second.find('|');
  return {it->second.substr(0, sep), it->second.substr(sep + 1)};
}

ActivityService::ActivityService(AppContext& ctx) : ctx_(ctx) {}

void ActivityService::record(const UserActivity& activity) {
  try {
    ctx_.repos->recordUserActivity(activity);
  } catch (const std::exception& ex) {
    // Auditing must never break the request it is observing.
    spdlog::warn("activity record failed: {}", ex.what());
  }
}

nlohmann::json ActivityService::list(int64_t userId, const ListQuery& query) {
  const auto page = ctx_.repos->listUserActivity(userId, query);
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& activity : page.rows) {
    rows.push_back(userActivityToJson(activity));
  }
  return {{"rows", rows}, {"total", page.total}};
}

nlohmann::json ActivityService::chart(int64_t userId, int hours) {
  if (hours < 1) {
    hours = 1;
  }
  if (hours > 72) {
    hours = 72;
  }
  const auto now = std::chrono::system_clock::now();
  const auto from = now - std::chrono::hours{hours};
  const std::string fromIso = isoFromTimePoint(from);
  const std::string toIso = isoFromTimePoint(now);

  // Fixed 15-minute buckets covering the window.
  constexpr int kBucketMinutes = 15;
  const int bucketCount = hours * (60 / kBucketMinutes);
  const std::time_t fromEpoch = std::chrono::system_clock::to_time_t(from);
  const std::time_t bucketSeconds = kBucketMinutes * 60;

  std::vector<int> actionCounts(bucketCount, 0);
  std::vector<bool> active(bucketCount, false);

  const auto bucketIndexFor = [&](const std::string& iso) -> int {
    const auto epoch = parseIso(iso);
    if (!epoch) {
      return -1;
    }
    const long long delta = static_cast<long long>(*epoch) - static_cast<long long>(fromEpoch);
    if (delta < 0) {
      return -1;
    }
    const int index = static_cast<int>(delta / bucketSeconds);
    return (index >= 0 && index < bucketCount) ? index : -1;
  };

  for (const auto& iso : ctx_.repos->listUserActivityTimestamps(userId, fromIso, toIso)) {
    const int index = bucketIndexFor(iso);
    if (index >= 0) {
      actionCounts[index] += 1;
    }
  }
  for (const auto& iso : ctx_.repos->listPresenceTimestamps(userId, fromIso, toIso)) {
    const int index = bucketIndexFor(iso);
    if (index >= 0) {
      active[index] = true;
    }
  }

  nlohmann::json buckets = nlohmann::json::array();
  for (int i = 0; i < bucketCount; ++i) {
    const auto bucketTime = from + std::chrono::minutes{kBucketMinutes * i};
    buckets.push_back({
        {"at", isoFromTimePoint(bucketTime)},
        {"count", actionCounts[i]},
        {"active", active[i]},
    });
  }

  return {
      {"from", fromIso},
      {"to", toIso},
      {"bucketMinutes", kBucketMinutes},
      {"online", ctx_.presence ? ctx_.presence->isOnline(userId) : false},
      {"buckets", buckets},
  };
}

}  // namespace highcube::admin
