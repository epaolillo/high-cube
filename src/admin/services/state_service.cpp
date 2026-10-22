#include "services/services.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace highcube::admin {
namespace {

std::string isoUtc(std::chrono::system_clock::time_point tp) {
  const auto tt = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  gmtime_r(&tt, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << ".000Z";
  return oss.str();
}

}  // namespace

StateService::StateService(AppContext& ctx) : ctx_(ctx) {}

nlohmann::json StateService::dashboard(const ListQuery& servers, const ListQuery& applications,
                                       const ListQuery& users, const ListQuery& domains,
                                       const User& actor) {
  nlohmann::json body;
  body["applications"] = ApplicationService(ctx_).list(applications, actor);
  if (!ctx_.auth->isAdmin(actor)) {
    // Read-only slices scoped to the caller; metrics retention and other users stay hidden.
    body["servers"] = ServerService(ctx_).listHostingUserApplications(actor.id);
    body["baseDomains"] = SettingsService(ctx_).listBaseDomainsUsedByUserApplications(actor.id);
    body["users"] = {{"rows", nlohmann::json::array({userToJson(actor)})}, {"total", 1}};
    nlohmann::json self = nlohmann::json::array();
    if (ctx_.presence && ctx_.presence->isOnline(actor.id)) {
      self.push_back(actor.id);
    }
    body["onlineUserIds"] = std::move(self);
    return body;
  }

  auto serverPage = ServerService(ctx_).list(servers);
  if (serverPage.contains("rows") && serverPage["rows"].is_array()) {
    ServerService snapshots(ctx_);
    for (auto& row : serverPage["rows"]) {
      if (row.contains("id") && row["id"].is_number_integer()) {
        row["metrics"] = snapshots.snapshot(row["id"].get<int64_t>());
      }
    }
  }
  body["servers"] = std::move(serverPage);
  body["users"] = UserService(ctx_).list(users);
  body["baseDomains"] = SettingsService(ctx_).listBaseDomains(domains);
  body["metricsTtlDays"] = SettingsService(ctx_).getMetricsSettings().value("metricsTtlDays", 7);

  nlohmann::json online = nlohmann::json::array();
  if (ctx_.presence) {
    for (const int64_t userId : ctx_.presence->onlineUserIds()) {
      online.push_back(userId);
    }
  }
  body["onlineUserIds"] = std::move(online);
  return body;
}

nlohmann::json StateService::applicationOverview(int64_t id, const User& actor) {
  // Authorize first so a missing or forbidden application fails before the rest.
  auto application = ApplicationService(ctx_).get(id, actor);
  const auto to = std::chrono::system_clock::now();
  const auto from = to - std::chrono::hours(24 * 7);
  ListQuery deployments;
  deployments.page = 1;
  deployments.pageSize = 10;
  ListQuery replicas;
  replicas.page = 1;
  replicas.pageSize = 100;

  nlohmann::json incidents = {{"rows", nlohmann::json::array()}, {"total", 0}};
  try {
    ListQuery incidentQuery;
    incidentQuery.page = 1;
    incidentQuery.pageSize = 100;
    incidents = ApplicationService(ctx_).incidents(id, incidentQuery, actor);
  } catch (const ForbiddenError&) {
    throw;
  } catch (const std::exception&) {
    // Incident history is best-effort so an older database still opens the drawer.
  }

  return {{"application", std::move(application)},
          {"deployments", ctx_.deployments->list(id, deployments)},
          {"replicas", ctx_.deployments->listReplicas(id, replicas)},
          {"volumes", ApplicationService(ctx_).listVolumes(id, actor)},
          {"incidents", incidents},
          {"metrics", ApplicationService(ctx_).metrics(id, isoUtc(from), isoUtc(to), actor)}};
}

nlohmann::json StateService::serverOverview(int64_t id) { return ServerService(ctx_).overview(id); }

}  // namespace highcube::admin
