#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace highcube::agent {

// Drives the host WireGuard tools for the High Cube VPN. hc-admin decides addresses and
// peers. hc-agent usually runs as a regular user, so every root step (apt-get, the private
// key, /etc/wireguard, wg-quick) goes through /usr/local/sbin/hc-vpn-helper via sudo -n.
// The helper is installed once per server by the setup command hc-admin shows.
class VpnManager {
public:
  // Reported as the status error until the helper is installed.
  static constexpr const char* kSetupRequired = "setup-required";

  // config_json of VpnApply. Blocking: the helper may run apt-get and wg-quick.
  void apply(const std::string& configJson);
  // Re-applies the last order once the helper shows up after a setup-required failure.
  // Returns true when it ran. Blocking like apply().
  bool retryPending();
  // Every 30 seconds at most: asks the helper to bring the interface back, restore the
  // hub forwarding rule, reopen the port in ufw, and re-resolve named endpoints.
  void heal();
  // VpnStatus payload, or empty when this server is not a VPN member.
  std::string statusJson();

private:
  void applyLocked(const std::string& configJson);
  std::vector<std::string> helperCommand(const std::vector<std::string>& args) const;

  std::mutex mutex_;
  std::string interface_ = "hc-vpn";
  std::string lastError_;
  // Last enabled order, kept so it can be retried after the one-time setup.
  std::string pendingConfig_;
  // From the last successful apply; heal() needs them.
  int listenPort_ = 0;
  bool forward_ = false;
  std::chrono::steady_clock::time_point lastHeal_{};
  bool healUnsupportedLogged_ = false;
};

}  // namespace highcube::agent
