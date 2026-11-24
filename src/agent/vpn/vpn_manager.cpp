#include "vpn/vpn_manager.hpp"

#include "audit/command_runner.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace highcube::agent {
namespace {

using audit::runCommand;
using audit::splitLines;
using audit::trim;

constexpr const char* kHelperPath = "/usr/local/sbin/hc-vpn-helper";

// Interface names end up in command lines and file paths.
bool validInterface(const std::string& name) {
  return !name.empty() && name.size() <= 15 &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) {
           return std::islower(c) || std::isdigit(c) || c == '-';
         });
}

std::string base64Encode(const std::string& input) {
  static const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((input.size() + 2) / 3) * 4);
  for (std::size_t i = 0; i < input.size(); i += 3) {
    const unsigned int n =
        (static_cast<unsigned char>(input[i]) << 16) |
        ((i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0) << 8) |
        (i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0);
    out.push_back(table[(n >> 18) & 63]);
    out.push_back(table[(n >> 12) & 63]);
    out.push_back(i + 1 < input.size() ? table[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < input.size() ? table[n & 63] : '=');
  }
  return out;
}

std::string lanIp() {
  const auto out = runCommand({"ip", "-4", "route", "get", "1.1.1.1"}, 5, false).out;
  const auto pos = out.find(" src ");
  if (pos == std::string::npos) {
    return {};
  }
  const auto rest = out.substr(pos + 5);
  return rest.substr(0, rest.find_first_of(" \n"));
}

// The helper prints "ERROR: <reason>" on failure; sudo -n prints nothing on stdout when
// the helper or its sudoers rule is missing.
std::string helperError(const audit::CommandOutput& result) {
  const auto out = trim(result.out);
  const auto pos = out.rfind("ERROR: ");
  if (pos != std::string::npos) {
    return out.substr(pos + 7);
  }
  return "hc-vpn-helper exited with code " + std::to_string(result.code);
}

}  // namespace

std::vector<std::string> VpnManager::helperCommand(const std::vector<std::string>& args) const {
  std::vector<std::string> argv;
  if (geteuid() != 0) {
    argv = {"sudo", "-n"};
  }
  argv.emplace_back(kHelperPath);
  argv.insert(argv.end(), args.begin(), args.end());
  return argv;
}

void VpnManager::apply(const std::string& configJson) {
  std::lock_guard lock(mutex_);
  applyLocked(configJson);
}

bool VpnManager::retryPending() {
  std::lock_guard lock(mutex_);
  if (pendingConfig_.empty() || lastError_ != kSetupRequired) {
    return false;
  }
  const auto check = runCommand(helperCommand({"check"}), 10, false);
  if (trim(check.out) != "ok") {
    return false;
  }
  spdlog::info("vpn: hc-vpn-helper is available, applying the pending configuration");
  applyLocked(pendingConfig_);
  return true;
}

void VpnManager::applyLocked(const std::string& configJson) {
  const auto config = nlohmann::json::parse(configJson, nullptr, false);
  if (!config.is_object()) {
    return;
  }
  const auto iface = config.value("interface", std::string("hc-vpn"));
  if (!validInterface(iface)) {
    lastError_ = "Invalid interface name";
    return;
  }
  interface_ = iface;
  const auto check = runCommand(helperCommand({"check"}), 10, false);
  const bool helperReady = trim(check.out) == "ok";

  if (!config.value("enabled", false)) {
    pendingConfig_.clear();
    lastError_.clear();
    // Without the helper this host never joined, so there is nothing to tear down.
    if (helperReady) {
      runCommand(helperCommand({"down", interface_}), 60, false);
      spdlog::info("vpn: left the VPN");
    }
    return;
  }

  pendingConfig_ = configJson;
  if (!helperReady) {
    if (lastError_ != kSetupRequired) {
      spdlog::warn("vpn: {} is missing or not allowed through sudo; run the setup command "
                   "shown in hc-admin",
                   kHelperPath);
    }
    lastError_ = kSetupRequired;
    return;
  }
  const auto port = config.value("listenPort", 0);
  const auto result = runCommand(
      helperCommand({"apply", interface_, std::to_string(port),
                     config.value("forward", false) ? "1" : "0",
                     base64Encode(config.value("conf", std::string{}))}),
      900, false);
  if (result.code != 0 || trim(result.out) != "ok") {
    lastError_ = helperError(result);
    spdlog::warn("vpn apply failed: {}", lastError_);
    return;
  }
  lastError_.clear();
  listenPort_ = port;
  forward_ = config.value("forward", false);
  spdlog::info("vpn: {} is configured", interface_);
}

void VpnManager::heal() {
  std::lock_guard lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  if (pendingConfig_.empty() || !lastError_.empty() || now - lastHeal_ < std::chrono::seconds(30)) {
    return;
  }
  lastHeal_ = now;
  const auto result = runCommand(
      helperCommand({"heal", interface_, std::to_string(listenPort_), forward_ ? "1" : "0"}), 60,
      false);
  if (result.out.find("unknown action") != std::string::npos) {
    if (!healUnsupportedLogged_) {
      healUnsupportedLogged_ = true;
      spdlog::warn("vpn: hc-vpn-helper predates self-healing; run the setup command again");
    }
    return;
  }
  for (const auto& line : splitLines(result.out)) {
    if (line.rfind("HEALED ", 0) == 0) {
      spdlog::info("vpn: {}", line.substr(7));
    } else if (line.rfind("ERROR: ", 0) == 0) {
      spdlog::warn("vpn heal failed: {}", line.substr(7));
    }
  }
}

std::string VpnManager::statusJson() {
  std::lock_guard lock(mutex_);
  nlohmann::json status = {{"up", false},
                           {"lanIp", lanIp()},
                           {"error", lastError_},
                           {"publicKey", ""},
                           {"dump", ""}};
  if (lastError_ == kSetupRequired) {
    return status.dump();
  }
  // The helper and the sudo rule may be absent on hosts that never joined.
  std::error_code ec;
  if (!std::filesystem::exists(kHelperPath, ec)) {
    return lastError_.empty() ? std::string{} : status.dump();
  }
  const auto result = runCommand(helperCommand({"status", interface_}), 15, false);
  bool member = false;
  std::string dump;
  for (const auto& line : splitLines(result.out)) {
    if (line == "MEMBER 1") {
      member = true;
    } else if (line == "UP 1") {
      status["up"] = true;
    } else if (line.rfind("PUBKEY ", 0) == 0) {
      status["publicKey"] = trim(line.substr(7));
    } else if (line.rfind("PEER ", 0) == 0) {
      dump += line.substr(5) + "\n";
    }
  }
  status["dump"] = dump;
  if (!member && lastError_.empty() && pendingConfig_.empty()) {
    return {};
  }
  return status.dump();
}

}  // namespace highcube::agent
