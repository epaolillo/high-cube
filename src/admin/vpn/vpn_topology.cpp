#include "vpn/vpn_topology.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace highcube::admin::vpn {
namespace {

std::string trim(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::uint32_t maskOf(int prefix) {
  return prefix == 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
}

// Names end up in a comment line of the WireGuard config.
std::string commentSafe(const std::string& value) {
  std::string out;
  for (const char c : value) {
    out.push_back(c == '\n' || c == '\r' ? ' ' : c);
  }
  return out;
}

std::string joinHostPort(const std::string& host, int port) {
  if (host.find(':') != std::string::npos && host.front() != '[') {
    return "[" + host + "]:" + std::to_string(port);
  }
  return host + ":" + std::to_string(port);
}

}  // namespace

std::uint32_t Subnet::lastHost() const { return (network | ~maskOf(prefix)) - 1; }

bool Subnet::contains(std::uint32_t address) const {
  return (address & maskOf(prefix)) == network;
}

std::string Subnet::cidr() const { return formatIpv4(network) + "/" + std::to_string(prefix); }

std::optional<std::uint32_t> parseIpv4(const std::string& text) {
  std::uint32_t result = 0;
  int parts = 0;
  std::stringstream stream(trim(text));
  std::string part;
  while (std::getline(stream, part, '.')) {
    if (part.empty() || part.size() > 3 ||
        !std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c); })) {
      return std::nullopt;
    }
    const int value = std::stoi(part);
    if (value > 255) {
      return std::nullopt;
    }
    result = (result << 8) | static_cast<std::uint32_t>(value);
    ++parts;
  }
  if (parts != 4) {
    return std::nullopt;
  }
  return result;
}

std::string formatIpv4(std::uint32_t address) {
  return std::to_string((address >> 24) & 0xFF) + "." + std::to_string((address >> 16) & 0xFF) +
         "." + std::to_string((address >> 8) & 0xFF) + "." + std::to_string(address & 0xFF);
}

std::optional<Subnet> parseSubnet(const std::string& cidr) {
  const auto value = trim(cidr);
  const auto slash = value.find('/');
  if (slash == std::string::npos) {
    return std::nullopt;
  }
  const auto address = parseIpv4(value.substr(0, slash));
  const auto prefixText = value.substr(slash + 1);
  if (!address || prefixText.empty() || prefixText.size() > 2 ||
      !std::all_of(prefixText.begin(), prefixText.end(),
                   [](unsigned char c) { return std::isdigit(c); })) {
    return std::nullopt;
  }
  const int prefix = std::stoi(prefixText);
  if (prefix < 16 || prefix > 29) {
    return std::nullopt;
  }
  return Subnet{*address & maskOf(prefix), prefix};
}

std::string allocateAddress(const Subnet& subnet, const std::vector<VpnNode>& nodes) {
  std::set<std::uint32_t> used;
  for (const auto& node : nodes) {
    if (const auto address = parseIpv4(node.address)) {
      used.insert(*address);
    }
  }
  for (auto candidate = subnet.firstHost(); candidate <= subnet.lastHost(); ++candidate) {
    if (!used.count(candidate)) {
      return formatIpv4(candidate);
    }
  }
  throw std::runtime_error("The VPN subnet has no free addresses");
}

int allocateListenPort(const std::vector<VpnNode>& nodes, int64_t excludeNodeId) {
  std::set<int> used;
  for (const auto& node : nodes) {
    if (node.id != excludeNodeId && node.listenPort > 0) {
      used.insert(node.listenPort);
    }
  }
  int port = kFirstListenPort;
  while (used.count(port) && port < 65535) {
    ++port;
  }
  return port;
}

std::string normalizeObservedIp(const std::string& ip) {
  auto value = trim(ip);
  // X-Forwarded-For may carry a chain; the first entry is the client.
  if (const auto comma = value.find(','); comma != std::string::npos) {
    value = trim(value.substr(0, comma));
  }
  if (value.rfind("::ffff:", 0) == 0 && parseIpv4(value.substr(7))) {
    return value.substr(7);
  }
  return value;
}

bool isPrivateAddress(const std::string& ip) {
  const auto value = normalizeObservedIp(ip);
  if (const auto v4 = parseIpv4(value)) {
    const auto a = *v4;
    return (a >> 24) == 10 || (a >> 24) == 127 || (a >> 20) == ((172u << 4) | 1u) ||
           (a >> 16) == ((192u << 8) | 168u) || (a >> 16) == ((169u << 8) | 254u);
  }
  std::string lower;
  for (const char c : value) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return lower == "::1" || lower.rfind("fc", 0) == 0 || lower.rfind("fd", 0) == 0 ||
         lower.rfind("fe80", 0) == 0;
}

std::string hostOfUrl(const std::string& url) {
  auto rest = url;
  if (const auto scheme = rest.find("://"); scheme != std::string::npos) {
    rest = rest.substr(scheme + 3);
  }
  rest = rest.substr(0, rest.find_first_of("/?#"));
  if (const auto at = rest.rfind('@'); at != std::string::npos) {
    rest = rest.substr(at + 1);
  }
  if (!rest.empty() && rest.front() == '[') {
    const auto close = rest.find(']');
    return close == std::string::npos ? std::string{} : rest.substr(1, close - 1);
  }
  return rest.substr(0, rest.find(':'));
}

Topology::Topology(Subnet subnet, std::vector<VpnNode> nodes, int64_t hubNodeId,
                   std::string adminPublicHost, std::vector<std::string> adminPublicIps)
    : subnet_(subnet),
      nodes_(std::move(nodes)),
      hubNodeId_(hubNodeId),
      adminPublicHost_(std::move(adminPublicHost)),
      adminPublicIps_(std::move(adminPublicIps)) {
  if (adminPublicHost_ == "localhost" || isPrivateAddress(adminPublicHost_)) {
    adminPublicHost_.clear();
  }
}

std::string Topology::siteOf(const VpnNode& node) {
  if (node.observedIp.empty()) {
    return {};
  }
  return isPrivateAddress(node.observedIp) ? "local" : normalizeObservedIp(node.observedIp);
}

std::string Topology::site(const VpnNode& node) const {
  const auto value = siteOf(node);
  if (!value.empty() && value != "local" &&
      std::find(adminPublicIps_.begin(), adminPublicIps_.end(), value) != adminPublicIps_.end()) {
    return "local";
  }
  return value;
}

std::string Topology::endpointFor(const VpnNode& viewer, const VpnNode& peer) const {
  const int port = peer.listenPort > 0 ? peer.listenPort : kFirstListenPort;
  const auto viewerSite = site(viewer);
  const auto peerSite = site(peer);
  // Same router: talk over the LAN, never hairpin through the public address.
  if (!viewerSite.empty() && viewerSite == peerSite && !peer.lanIp.empty()) {
    return joinHostPort(peer.lanIp, port);
  }
  if (!peer.endpoint.empty()) {
    const auto& value = peer.endpoint;
    const auto colons = std::count(value.begin(), value.end(), ':');
    const bool hasPort = (colons == 1) || value.find("]:") != std::string::npos;
    return hasPort ? value : joinHostPort(value, port);
  }
  if (!peerSite.empty() && peerSite != "local") {
    return joinHostPort(peerSite, port);
  }
  if (peerSite == "local" && !adminPublicHost_.empty()) {
    return joinHostPort(adminPublicHost_, port);
  }
  return {};
}

std::vector<VpnNode> Topology::peersOf(const VpnNode& self) const {
  std::vector<VpnNode> peers;
  for (const auto& node : nodes_) {
    if (node.id == self.id || node.status != "active" || node.publicKey.empty() ||
        node.address.empty()) {
      continue;
    }
    if (isHub(self) || isHub(node)) {
      peers.push_back(node);
    }
  }
  return peers;
}

std::string Topology::renderConfig(const VpnNode& self) const {
  std::ostringstream oss;
  oss << "Address = " << self.address << "/" << subnet_.prefix << "\n"
      << "ListenPort = " << (self.listenPort > 0 ? self.listenPort : kFirstListenPort) << "\n";
  // No hook lines: the node's helper adds the hub forwarding rules itself and drops any
  // PostUp/PostDown it receives, since they would run as root.
  for (const auto& peer : peersOf(self)) {
    oss << "\n[Peer]\n"
        << "# " << commentSafe(peer.name) << "\n"
        << "PublicKey = " << peer.publicKey << "\n";
    if (isHub(self)) {
      // Members may sit behind NAT: they dial the hub and keep the mapping open.
      oss << "AllowedIPs = " << peer.address << "/32\n";
      const auto endpoint = endpointFor(self, peer);
      if (!endpoint.empty() && !site(self).empty() && site(self) == site(peer)) {
        oss << "Endpoint = " << endpoint << "\n";
      }
    } else {
      oss << "AllowedIPs = " << subnet_.cidr() << "\n";
      const auto endpoint = endpointFor(self, peer);
      if (!endpoint.empty()) {
        oss << "Endpoint = " << endpoint << "\n";
      }
      oss << "PersistentKeepalive = " << kPersistentKeepalive << "\n";
    }
  }
  return oss.str();
}

}  // namespace highcube::admin::vpn
