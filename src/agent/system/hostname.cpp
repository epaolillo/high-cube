#include "system/hostname.hpp"

#include "audit/command_runner.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>

namespace highcube::agent {
namespace {

constexpr const char* kHelperPath = "/usr/local/sbin/hc-vpn-helper";

bool validHostname(const std::string& name) {
  if (name.empty() || name.size() > 63 || name.front() == '-' || name.back() == '-') {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return std::islower(c) || std::isdigit(c) || c == '-';
  });
}

}  // namespace

HostnameChange setHostname(const std::string& hostname) {
  if (!validHostname(hostname)) {
    return {false, "Invalid hostname: " + hostname};
  }
  const bool root = geteuid() == 0;
  const bool helperPresent = std::filesystem::exists(kHelperPath);

  // Root without the helper (never joined the VPN) can still rename the host directly.
  if (root && !helperPresent) {
    const auto result = audit::runCommand({"hostnamectl", "set-hostname", hostname}, 30, false);
    if (result.code != 0) {
      return {false, "hostnamectl set-hostname failed"};
    }
    return {true, {}};
  }

  std::vector<std::string> argv;
  if (!root) {
    argv = {"sudo", "-n"};
  }
  argv.insert(argv.end(), {kHelperPath, "hostname", hostname});
  const auto result = audit::runCommand(argv, 30, false);
  const auto out = audit::trim(result.out);
  if (result.code == 0 && out.rfind("ok", 0) == 0) {
    return {true, {}};
  }
  if (out.find("unknown action") != std::string::npos) {
    return {false, "hc-vpn-helper on this server is outdated. Run the server setup command again "
                   "(VPN modal) and retry."};
  }
  const auto pos = out.rfind("ERROR: ");
  if (pos != std::string::npos) {
    return {false, out.substr(pos + 7)};
  }
  if (!helperPresent || out.empty()) {
    return {false, "hc-agent cannot run hc-vpn-helper as root. Run the server setup command "
                   "(VPN modal) once and retry."};
  }
  return {false, "hc-vpn-helper exited with code " + std::to_string(result.code)};
}

}  // namespace highcube::agent
