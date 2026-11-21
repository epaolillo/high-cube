#include "audit/audit_engine.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <stdexcept>

namespace highcube::agent::audit {

json runAudit(const std::string& name, const json& params, const ProgressSink& sink) {
  std::vector<CheckSpec> checks;
  if (name == "disk") {
    checks = diskChecks();
  } else if (name == "cpu") {
    checks = cpuChecks();
  } else if (name == "ram") {
    checks = ramChecks();
  } else {
    throw std::invalid_argument("Unknown audit");
  }
  const auto only = params.value("only", std::string{});
  int ran = 0;
  for (const auto& check : checks) {
    if (!only.empty() && check.id != only) {
      continue;
    }
    sink({{"id", check.id}, {"status", "running"}});
    const auto started = std::chrono::steady_clock::now();
    json result;
    try {
      result = check.run(params);
      if (!result.is_object()) {
        result = json::object();
      }
      if (!result.contains("status")) {
        result["status"] = "done";
      }
    } catch (const std::exception& ex) {
      spdlog::warn("audit check {} failed: {}", check.id, ex.what());
      result = {{"status", "error"},
                {"severity", "info"},
                {"conclusion", message("check_error", {{"message", ex.what()}})}};
    }
    result["id"] = check.id;
    result["durationMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    if (!result.contains("severity")) {
      result["severity"] = "info";
    }
    sink(result);
    ++ran;
  }
  return {{"checks", ran}};
}

}  // namespace highcube::agent::audit
