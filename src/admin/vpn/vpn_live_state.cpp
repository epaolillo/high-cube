#include "vpn/vpn_live_state.hpp"

#include <sstream>

namespace highcube::admin::vpn {
namespace {

std::vector<std::string> splitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::stringstream stream(line);
  std::string field;
  while (std::getline(stream, field, '\t')) {
    fields.push_back(field);
  }
  return fields;
}

template <typename T>
T parseNumber(const std::string& text) {
  try {
    return static_cast<T>(std::stoull(text));
  } catch (const std::exception&) {
    return 0;
  }
}

}  // namespace

std::vector<PeerSample> parseWgDump(const std::string& dump) {
  std::vector<PeerSample> peers;
  std::stringstream stream(dump);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    // Peer: public-key preshared-key endpoint allowed-ips latest-handshake rx tx keepalive.
    const auto fields = splitTabs(line);
    if (fields.size() != 8) {
      continue;
    }
    PeerSample peer;
    peer.publicKey = fields[0];
    peer.endpoint = fields[2] == "(none)" ? std::string{} : fields[2];
    peer.latestHandshake = parseNumber<std::int64_t>(fields[4]);
    peer.rxBytes = parseNumber<std::uint64_t>(fields[5]);
    peer.txBytes = parseNumber<std::uint64_t>(fields[6]);
    peers.push_back(std::move(peer));
  }
  return peers;
}

void LiveState::record(std::int64_t nodeId, bool interfaceUp, const std::string& error,
                       std::vector<PeerSample> peers) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(mutex_);
  auto& sample = samples_[nodeId];
  const double seconds =
      std::chrono::duration<double>(now - sample.receivedAt).count();
  if (!sample.peers.empty() && seconds > 0.5 && seconds < 120) {
    for (auto& peer : peers) {
      for (const auto& previous : sample.peers) {
        if (previous.publicKey != peer.publicKey) {
          continue;
        }
        // Counters restart when the interface is recreated.
        if (peer.rxBytes >= previous.rxBytes) {
          peer.rxRate = static_cast<double>(peer.rxBytes - previous.rxBytes) / seconds;
        }
        if (peer.txBytes >= previous.txBytes) {
          peer.txRate = static_cast<double>(peer.txBytes - previous.txBytes) / seconds;
        }
        break;
      }
    }
  }
  sample.receivedAt = now;
  sample.reportedAtUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
  sample.interfaceUp = interfaceUp;
  sample.error = error;
  sample.peers = std::move(peers);
}

std::optional<NodeSample> LiveState::get(std::int64_t nodeId) const {
  std::lock_guard lock(mutex_);
  const auto it = samples_.find(nodeId);
  if (it == samples_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void LiveState::forget(std::int64_t nodeId) {
  std::lock_guard lock(mutex_);
  samples_.erase(nodeId);
}

void LiveState::clear() {
  std::lock_guard lock(mutex_);
  samples_.clear();
}

std::optional<bool> LiveState::exchangeRoutable(std::int64_t serverId, bool routable) {
  std::lock_guard lock(mutex_);
  const auto it = routable_.find(serverId);
  std::optional<bool> previous;
  if (it != routable_.end()) {
    previous = it->second;
  }
  routable_[serverId] = routable;
  return previous;
}

}  // namespace highcube::admin::vpn
