#include "services/services.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace highcube::admin {

SettingsService::SettingsService(AppContext& ctx) : ctx_(ctx) {}

nlohmann::json SettingsService::listBaseDomains(const ListQuery& query) {
  const auto page = ctx_.repos->listBaseDomains(query);
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& d : page.rows) {
    rows.push_back({{"id", d.id}, {"hostname", d.hostname}, {"createdBy", d.createdBy}});
  }
  return {{"rows", rows}, {"total", page.total}};
}

nlohmann::json SettingsService::createBaseDomain(const nlohmann::json& body, const User& actor) {
  BaseDomain domain;
  domain.hostname = body.at("hostname").get<std::string>();
  domain.createdBy = actor.id;
  domain.id = ctx_.repos->createBaseDomain(domain);
  return {{"id", domain.id}, {"hostname", domain.hostname}, {"createdBy", domain.createdBy}};
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
