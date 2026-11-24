#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace highcube::admin::vpn {

// One peer line of `wg show <iface> dump`, seen from the reporting node.
struct PeerSample {
  std::string publicKey;
  std::string endpoint;
  std::int64_t latestHandshake = 0;  // unix seconds, 0 = never
  std::uint64_t rxBytes = 0;
  std::uint64_t txBytes = 0;
  double rxRate = 0;  // bytes per second since the previous report
  double txRate = 0;
};

struct NodeSample {
  std::chrono::steady_clock::time_point receivedAt;
  std::int64_t reportedAtUnixMs = 0;
  bool interfaceUp = false;
  std::string error;
  std::vector<PeerSample> peers;
};

// Parses peer lines of `wg show <iface> dump`. The interface line is ignored so a
// private key is never kept even if a node sends it by mistake.
std::vector<PeerSample> parseWgDump(const std::string& dump);

// Latest WireGuard counters of every node, kept in memory only. Rates come from the
// difference between two consecutive reports of the same node.
class LiveState {
public:
  void record(std::int64_t nodeId, bool interfaceUp, const std::string& error,
              std::vector<PeerSample> peers);
  std::optional<NodeSample> get(std::int64_t nodeId) const;
  void forget(std::int64_t nodeId);
  void clear();
  // Stores whether nginx should reach this server through the VPN and returns the
  // previous value, empty the first time. Keyed by server id.
  std::optional<bool> exchangeRoutable(std::int64_t serverId, bool routable);

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::int64_t, NodeSample> samples_;
  std::unordered_map<std::int64_t, bool> routable_;
};

}  // namespace highcube::admin::vpn
