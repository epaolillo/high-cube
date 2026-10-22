#include "services/services.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace highcube::admin {
namespace {

nlohmann::json baseDomainToJson(Repositories& repos, const BaseDomain& domain) {
  nlohmann::json json = {
      {"id", domain.id},
      {"hostname", domain.hostname},
      {"createdBy", domain.createdBy},
      {"accessServerId", nullptr},
      {"accessServerName", nullptr},
      {"accessServerIp", nullptr},
  };
  if (!domain.accessServerId) {
    return json;
  }
  json["accessServerId"] = *domain.accessServerId;
  if (const auto server = repos.findServerById(*domain.accessServerId)) {
    json["accessServerName"] = server->name;
    json["accessServerIp"] = server->ip;
  }
  return json;
}

std::optional<int64_t> parseAccessServerId(Repositories& repos, const nlohmann::json& body) {
  if (!body.contains("accessServerId") || body.at("accessServerId").is_null()) {
    return std::nullopt;
  }
  const auto serverId = body.at("accessServerId").get<int64_t>();
  if (serverId <= 0 || !repos.findServerById(serverId)) {
    throw std::invalid_argument("Unknown server id");
  }
  return serverId;
}

}  // namespace

SettingsService::SettingsService(AppContext& ctx) : ctx_(ctx) {}

nlohmann::json SettingsService::listBaseDomains(const ListQuery& query) {
  const auto page = ctx_.repos->listBaseDomains(query);
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& d : page.rows) {
    rows.push_back(baseDomainToJson(*ctx_.repos, d));
  }
  return {{"rows", rows}, {"total", page.total}};
}

nlohmann::json SettingsService::listBaseDomainsUsedByUserApplications(int64_t userId) {
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& d : ctx_.repos->listBaseDomainsUsedByUserApplications(userId)) {
    rows.push_back(baseDomainToJson(*ctx_.repos, d));
  }
  return {{"rows", rows}, {"total", rows.size()}};
}

nlohmann::json SettingsService::createBaseDomain(const nlohmann::json& body, const User& actor) {
  BaseDomain domain;
  domain.hostname = body.at("hostname").get<std::string>();
  domain.createdBy = actor.id;
  domain.accessServerId = parseAccessServerId(*ctx_.repos, body);
  domain.id = ctx_.repos->createBaseDomain(domain);
  return baseDomainToJson(*ctx_.repos, *ctx_.repos->findBaseDomainById(domain.id));
}

nlohmann::json SettingsService::updateBaseDomain(int64_t id, const nlohmann::json& body) {
  if (!ctx_.repos->findBaseDomainById(id)) {
    throw std::runtime_error("Base domain not found");
  }
  if (!body.contains("accessServerId")) {
    throw std::invalid_argument("accessServerId is required");
  }
  ctx_.repos->setBaseDomainAccessServer(id, parseAccessServerId(*ctx_.repos, body));
  // Replicas already publishing a host port can be reached on the next nginx reload,
  // without waiting for another deploy.
  if (ctx_.proxy) {
    for (const auto appId : ctx_.repos->applicationIdsUsingBaseDomain(id)) {
      if (const auto app = ctx_.repos->findApplicationById(appId)) {
        ctx_.proxy->renderApplicationSite(*app);
      }
    }
  }
  return baseDomainToJson(*ctx_.repos, *ctx_.repos->findBaseDomainById(id));
}

void SettingsService::deleteBaseDomain(int64_t id) {
  if (ctx_.repos->applicationsUsingBaseDomain(id) > 0) {
    throw std::runtime_error("Base domain still used by applications");
  }
  ctx_.repos->deleteBaseDomain(id);
}

nlohmann::json SettingsService::autocompleteBaseDomains(const std::string& q) {
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& d : ctx_.repos->autocompleteBaseDomains(q)) {
    rows.push_back({{"id", d.id}, {"label", d.hostname}});
  }
  return {{"rows", rows}};
}

nlohmann::json SettingsService::getMetricsSettings() {
  return {{"metricsTtlDays", std::stoi(ctx_.repos->getSetting("metrics_ttl_days", "7"))}};
}

nlohmann::json SettingsService::updateMetricsSettings(const nlohmann::json& body) {
  const int days = body.at("metricsTtlDays").get<int>();
  if (days <= 0) {
    throw std::invalid_argument("metricsTtlDays must be a positive integer");
  }
  ctx_.repos->setSetting("metrics_ttl_days", std::to_string(days));
  MetricsRetentionService retention(ctx_);
  retention.runOnce();
  return getMetricsSettings();
}

MetricsRetentionService::MetricsRetentionService(AppContext& ctx) : ctx_(ctx) {}

void MetricsRetentionService::runOnce() {
  const int days = std::stoi(ctx_.repos->getSetting("metrics_ttl_days", "7"));
  const auto cutoff = std::chrono::system_clock::now() - std::chrono::hours{24 * days};
  const std::time_t t = std::chrono::system_clock::to_time_t(cutoff);
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  ctx_.repos->deleteMetricsOlderThan(oss.str());
  // Keep crash traceability under the same retention window as metrics.
  ctx_.repos->deleteIncidentsOlderThan(oss.str());
  // The audit trail and presence history share the same retention window.
  ctx_.repos->deleteActivityOlderThan(oss.str());
  ctx_.repos->deletePresenceOlderThan(oss.str());
}

}  // namespace highcube::admin
