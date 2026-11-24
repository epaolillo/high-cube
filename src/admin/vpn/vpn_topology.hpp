#pragma once

#include "domain/models.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace highcube::admin::vpn {

inline constexpr const char* kNetworkName = "VPN";
inline constexpr const char* kInterfaceName = "hc-vpn";
inline constexpr const char* kDefaultCidr = "10.88.0.0/24";
inline constexpr int kFirstListenPort = 51821;
inline constexpr int kPersistentKeepalive = 25;

struct Subnet {
  std::uint32_t network = 0;
  int prefix = 24;

  std::uint32_t firstHost() const { return network + 1; }
  std::uint32_t lastHost() const;
  bool contains(std::uint32_t address) const;
  std::string cidr() const;
};

// Accepts an IPv4 CIDR between /16 and /29. Host bits are cleared.
std::optional<Subnet> parseSubnet(const std::string& cidr);
std::optional<std::uint32_t> parseIpv4(const std::string& text);
std::string formatIpv4(std::uint32_t address);

// Lowest free host address of the subnet. Throws when the subnet is full.
std::string allocateAddress(const Subnet& subnet, const std::vector<VpnNode>& nodes);
// Lowest port from kFirstListenPort not used by another node, so every node can be
// forwarded separately on a shared router.
int allocateListenPort(const std::vector<VpnNode>& nodes, int64_t excludeNodeId = 0);

// Loopback and RFC 1918 / ULA addresses: the node reached hc-admin from its LAN.
bool isPrivateAddress(const std::string& ip);
// "::ffff:1.2.3.4" -> "1.2.3.4"; anything else is returned trimmed.
std::string normalizeObservedIp(const std::string& ip);

// Hub-and-spoke: the hub peers with every member and forwards traffic between them.
// Every other member peers only with the hub, so only the hub must be reachable.
class Topology {
public:
  // adminPublicHost is the public name or address of hc-admin. A hub that reaches
  // hc-admin from its own LAN is reachable from outside through that address and its own
  // port. adminPublicIps are what that host resolves to: a node seen with one of them is
  // on the admin LAN too, it only came back in through the router (hairpin NAT).
  Topology(Subnet subnet, std::vector<VpnNode> nodes, int64_t hubNodeId,
           std::string adminPublicHost, std::vector<std::string> adminPublicIps = {});

  // Endpoint `viewer` uses to reach `peer`, empty when the peer cannot be dialed and
  // must start the session itself.
  std::string endpointFor(const VpnNode& viewer, const VpnNode& peer) const;
  // wg-quick body for one node, without the [Interface] line and PrivateKey: the node
  // adds both locally so its private key never leaves the machine.
  std::string renderConfig(const VpnNode& self) const;
  // Members `self` must list as WireGuard peers: every member for the hub, the hub
  // for everyone else. Only members with address and public key are listed.
  std::vector<VpnNode> peersOf(const VpnNode& self) const;
  bool isHub(const VpnNode& node) const { return hubNodeId_ != 0 && node.id == hubNodeId_; }
  // "local" for nodes on the admin LAN, otherwise their public source address.
  static std::string siteOf(const VpnNode& node);

private:
  // siteOf, with the admin's own public addresses folded into "local".
  std::string site(const VpnNode& node) const;

  Subnet subnet_;
  std::vector<VpnNode> nodes_;
  int64_t hubNodeId_ = 0;
  std::string adminPublicHost_;
  std::vector<std::string> adminPublicIps_;
};

// Host of a URL such as https://admin.example.com:8443/path -> admin.example.com.
std::string hostOfUrl(const std::string& url);

}  // namespace highcube::admin::vpn
