#pragma once

#include <string>

namespace highcube::admin::vpn {

// Bash run as `curl -fsSL <url> | sudo bash` on an Ubuntu machine. It installs
// wireguard-tools, generates the key pair locally, registers the public key with the
// one-time token, and installs a systemd timer that keeps the peer list in sync.
std::string renderEnrollScript(const std::string& baseUrl, const std::string& token);

// One-time `curl -fsSL <url> | sudo bash` for a server: installs hc-vpn-helper, the root
// helper hc-agent calls through sudo to drive wg-quick, and the sudoers rule for it.
std::string renderAgentSetupScript();

// Printed instead of the enrollment when the link is unknown, used, or expired.
std::string renderExpiredScript();

}  // namespace highcube::admin::vpn
