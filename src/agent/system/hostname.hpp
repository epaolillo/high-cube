#pragma once

#include <string>

namespace highcube::agent {

struct HostnameChange {
  bool ok = false;
  std::string error;
};

// Renames the host through hc-vpn-helper (sudo -n when the agent is not root).
// hostname must be a lowercase RFC 1123 label; anything else is rejected here.
HostnameChange setHostname(const std::string& hostname);

}  // namespace highcube::agent
