#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace highcube::agent::audit {

using json = nlohmann::json;
using ProgressSink = std::function<void(const json&)>;

// A check returns {severity, conclusion, findings?, blocks?, actions?, status?}.
// The engine adds id, status and durationMs.
struct CheckSpec {
  std::string id;
  std::function<json(const json& params)> run;
};

std::vector<CheckSpec> diskChecks();
std::vector<CheckSpec> cpuChecks();
std::vector<CheckSpec> ramChecks();

// Runs the audit checks in order (or only params.only) and streams each result
// through sink: once as "running" and once finished. One failing check never stops the audit.
json runAudit(const std::string& name, const json& params, const ProgressSink& sink);

// Allowlisted remediation. Throws std::invalid_argument for unknown ids or bad targets.
json runAction(const std::string& actionId, const json& params);

// ---- Result builders shared by every check ----
inline json message(const std::string& code, json params = json::object()) {
  return {{"code", code}, {"params", std::move(params)}};
}

struct Column {
  std::string key;
  std::string type;  // text | bytes | percent | number | date | duration
};

inline json tableBlock(const std::string& title, const std::vector<Column>& columns, json rows) {
  json cols = json::array();
  for (const auto& column : columns) {
    cols.push_back({{"key", column.key}, {"type", column.type}});
  }
  return {{"kind", "table"}, {"title", title}, {"columns", cols}, {"rows", std::move(rows)}};
}

// items: [{label, value}]
inline json barsBlock(const std::string& title, json items, const std::string& unit) {
  return {{"kind", "bars"}, {"title", title}, {"items", std::move(items)}, {"unit", unit}};
}

// items: [{key, value, type}]
inline json kvBlock(const std::string& title, json items) {
  return {{"kind", "kv"}, {"title", title}, {"items", std::move(items)}};
}

inline json kv(const std::string& key, json value, const std::string& type = "text") {
  return {{"key", key}, {"value", std::move(value)}, {"type", type}};
}

inline json suggestAction(const std::string& id, json target, long long estimateBytes,
                          const std::string& risk) {
  return {{"id", id}, {"target", std::move(target)}, {"estimateBytes", estimateBytes}, {"risk", risk}};
}

inline json skipped(const std::string& code, json params = json::object()) {
  return {{"status", "skipped"}, {"severity", "info"}, {"conclusion", message(code, std::move(params))}};
}

}  // namespace highcube::agent::audit
