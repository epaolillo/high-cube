#include "services/services.hpp"

#include "db/vpn_node_repository.hpp"
#include "vpn/device_script.hpp"
#include "vpn/vpn_topology.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace highcube::admin {
namespace {

constexpr auto kEnrollmentTtl = std::chrono::minutes(30);
// Agents report every 5 seconds and devices sync every 15 seconds.
constexpr int kServerStaleSeconds = 20;
constexpr int kDeviceStaleSeconds = 60;
// WireGuard renews the handshake every two minutes while a session is alive.
constexpr std::int64_t kHandshakeFreshSeconds = 180;
constexpr int kLastSeenPersistSeconds = 60;

std::string isoAt(std::chrono::system_clock::time_point tp) {
  const std::time_t t = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

std::string nowIso() { return isoAt(std::chrono::system_clock::now()); }

bool olderThan(const std::string& iso, int seconds) {
  if (iso.empty()) {
    return true;
  }
  std::tm tm{};
  std::istringstream iss(iso);
  iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  if (iss.fail()) {
    return true;
  }
  const auto at = std::chrono::system_clock::from_time_t(timegm(&tm));
  return std::chrono::system_clock::now() - at > std::chrono::seconds(seconds);
}

std::int64_t nowUnix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::optional<vpn::Subnet> currentSubnet(Repositories& repos) {
  const auto cidr = repos.getSetting("vpn_cidr", "");
  if (cidr.empty()) {
    return std::nullopt;
  }
  return vpn::parseSubnet(cidr);
}

// Prefers the origin the request arrived on: it is reachable from the machine that
// enrolls, while public_url may point at an address only valid on another network.
std::string baseUrl(const AppContext& ctx, const std::string& requestOrigin) {
  auto url = requestOrigin.empty() ? ctx.config.publicUrl : requestOrigin;
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url;
}

int64_t hubNodeId(Repositories& repos) {
  const auto value = repos.getSetting("vpn_hub_node_id", "");
  try {
    return value.empty() ? 0 : std::stoll(value);
  } catch (const std::exception&) {
    return 0;
  }
}

void setHubNodeId(Repositories& repos, int64_t nodeId) {
  repos.setSetting("vpn_hub_node_id", nodeId == 0 ? std::string{} : std::to_string(nodeId));
}

bool usablePublicHost(const std::string& host) {
  return !host.empty() && host != "localhost" && !vpn::isPrivateAddress(host);
}

// public_url often holds a LAN address; then the name devices reach hc-admin through
// (learned from their requests) is the public one.
std::string adminPublicHost(AppContext& ctx) {
  const auto configured = vpn::hostOfUrl(ctx.config.publicUrl);
  if (usablePublicHost(configured)) {
    return configured;
  }
  return ctx.repos->getSetting("vpn_public_host", "");
}

// IPv4 addresses of the admin's public host, cached: topology is rebuilt on every sync.
std::vector<std::string> resolvePublicHost(const std::string& host) {
  static std::mutex mutex;
  static std::string cachedHost;
  static std::vector<std::string> cachedIps;
  static std::chrono::steady_clock::time_point cachedAt;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(mutex);
  if (host == cachedHost && now - cachedAt < std::chrono::minutes(10)) {
    return cachedIps;
  }
  std::vector<std::string> ips;
  if (!host.empty()) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0) {
      for (auto* entry = result; entry; entry = entry->ai_next) {
        char text[INET_ADDRSTRLEN] = {};
        const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
        if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text))) {
          ips.emplace_back(text);
        }
      }
      freeaddrinfo(result);
    }
  }
  cachedHost = host;
  cachedIps = ips;
  cachedAt = now;
  return ips;
}

vpn::Topology makeTopology(AppContext& ctx, const vpn::Subnet& subnet,
                           std::vector<VpnNode> nodes) {
  const auto host = adminPublicHost(ctx);
  return vpn::Topology(subnet, std::move(nodes), hubNodeId(*ctx.repos), host,
                       resolvePublicHost(host));
}

// A WireGuard public key is 32 bytes in base64: 43 characters plus one '='.
bool validPublicKey(const std::string& key) {
  if (key.size() != 44 || key.back() != '=') {
    return false;
  }
  return std::all_of(key.begin(), key.end() - 1, [](unsigned char c) {
    return std::isalnum(c) || c == '+' || c == '/';
  });
}

std::string sanitizeName(const std::string& raw) {
  std::string out;
  for (const char c : raw) {
    const auto u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == '.' || c == '_' || c == '-' || c == ' ') {
      out.push_back(c);
    }
    if (out.size() >= 63) {
      break;
    }
  }
  return out.empty() ? std::string("device") : out;
}

std::string validLanIp(const std::string& raw) {
  return vpn::parseIpv4(raw) ? raw : std::string{};
}

std::string validateEndpoint(const std::string& raw) {
  std::string value;
  for (const char c : raw) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      value.push_back(c);
    }
  }
  if (value.size() > 255 || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']';
      })) {
    throw std::invalid_argument("The endpoint must be a host name or IP, optionally with :port");
  }
  return value;
}

int readInt(const nlohmann::json& body, const char* key, int fallback) {
  if (!body.contains(key) || body[key].is_null()) {
    return fallback;
  }
  if (body[key].is_number_integer()) {
    return body[key].get<int>();
  }
  if (body[key].is_string() && !body[key].get<std::string>().empty()) {
    return std::stoi(body[key].get<std::string>());
  }
  return fallback;
}

int validatePort(int port, const std::vector<VpnNode>& nodes, int64_t excludeNodeId) {
  if (port < 1024 || port > 65535) {
    throw std::invalid_argument("The listen port must be between 1024 and 65535");
  }
  for (const auto& node : nodes) {
    if (node.id != excludeNodeId && node.listenPort == port) {
      throw std::runtime_error("UDP port " + std::to_string(port) + " is already used by " +
                               node.name);
    }
  }
  return port;
}

nlohmann::json applyJson(const VpnNode& node, const vpn::Topology& topology) {
  return {{"enabled", true},
          {"interface", vpn::kInterfaceName},
          {"listenPort", node.listenPort},
          {"forward", topology.isHub(node)},
          {"conf", topology.renderConfig(node)}};
}

nlohmann::json disableJson() {
  return {{"enabled", false}, {"interface", vpn::kInterfaceName}};
}

nlohmann::json nodeToJson(const VpnNode& node) {
  return {{"id", node.id},
          {"kind", node.kind},
          {"serverId", node.serverId ? nlohmann::json(*node.serverId) : nlohmann::json(nullptr)},
          {"name", node.name},
          {"address", node.address},
          {"publicKey", node.publicKey},
          {"listenPort", node.listenPort},
          {"endpoint", node.endpoint},
          {"lanIp", node.lanIp},
          {"observedIp", node.observedIp},
          {"site", vpn::Topology::siteOf(node)},
          {"status", node.status},
          {"lastSeenAt", node.lastSeenAt},
          {"createdAt", node.createdAt}};
}

const vpn::PeerSample* findPeer(const std::optional<vpn::NodeSample>& sample,
                                const std::string& publicKey) {
  if (!sample || publicKey.empty()) {
    return nullptr;
  }
  for (const auto& peer : sample->peers) {
    if (peer.publicKey == publicKey) {
      return &peer;
    }
  }
  return nullptr;
}

}  // namespace

VpnService::VpnService(AppContext& ctx) : ctx_(ctx) {}

void VpnService::pushAll() {
  if (!ctx_.agentHub) {
    return;
  }
  const auto subnet = currentSubnet(*ctx_.repos);
  if (!subnet) {
    return;
  }
  const auto nodes = VpnNodeRepository(*ctx_.database).list();
  const auto topology = makeTopology(ctx_, *subnet, nodes);
  for (const auto& node : nodes) {
    if (node.kind == "server" && node.serverId && !node.address.empty()) {
      ctx_.agentHub->sendVpnApply(*node.serverId, applyJson(node, topology).dump());
    }
  }
}

nlohmann::json VpnService::state() {
  VpnNodeRepository repo(*ctx_.database);
  const auto subnet = currentSubnet(*ctx_.repos);
  std::vector<VpnNode> nodes;
  for (auto& node : repo.list()) {
    if (node.status != "enrolling") {
      nodes.push_back(std::move(node));
    }
  }
  nlohmann::json body;
  body["now"] = nowUnix() * 1000;
  body["suggestedPort"] = vpn::allocateListenPort(nodes);
  body["network"] = nullptr;
  const auto hubId = hubNodeId(*ctx_.repos);
  if (subnet) {
    body["network"] = {{"name", vpn::kNetworkName},
                       {"cidr", subnet->cidr()},
                       {"interface", vpn::kInterfaceName},
                       {"hubNodeId", hubId == 0 ? nlohmann::json(nullptr) : nlohmann::json(hubId)},
                       {"createdAt", ctx_.repos->getSetting("vpn_created_at", "")}};
  }

  const auto now = nowUnix();
  std::vector<std::optional<vpn::NodeSample>> samples;
  for (const auto& node : nodes) {
    auto sample = ctx_.vpnLive ? ctx_.vpnLive->get(node.id) : std::nullopt;
    const int staleAfter = node.kind == "server" ? kServerStaleSeconds : kDeviceStaleSeconds;
    if (sample && std::chrono::steady_clock::now() - sample->receivedAt >
                      std::chrono::seconds(staleAfter)) {
      sample.reset();
    }
    samples.push_back(std::move(sample));
  }

  // One link between the hub and every other active member.
  nlohmann::json links = nlohmann::json::array();
  std::vector<int> connectedLinks(nodes.size(), 0);
  std::vector<int> totalLinks(nodes.size(), 0);
  int linksUp = 0;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    for (std::size_t j = 0; j < nodes.size(); ++j) {
      const auto& a = nodes[i];
      const auto& b = nodes[j];
      if (a.id != hubId || i == j || a.status != "active" || b.status != "active") {
        continue;
      }
      const auto* aViewOfB = findPeer(samples[i], b.publicKey);
      const auto* bViewOfA = findPeer(samples[j], a.publicKey);
      const std::int64_t handshake =
          std::max(aViewOfB ? aViewOfB->latestHandshake : 0, bViewOfA ? bViewOfA->latestHandshake : 0);
      const bool connected = handshake > 0 && now - handshake < kHandshakeFreshSeconds;
      const double aToB = aViewOfB ? aViewOfB->txRate : (bViewOfA ? bViewOfA->rxRate : 0);
      const double bToA = aViewOfB ? aViewOfB->rxRate : (bViewOfA ? bViewOfA->txRate : 0);
      ++totalLinks[i];
      ++totalLinks[j];
      if (connected) {
        ++connectedLinks[i];
        ++connectedLinks[j];
        ++linksUp;
      }
      links.push_back({{"source", a.id},
                       {"target", b.id},
                       {"connected", connected},
                       {"latestHandshake", handshake},
                       {"sourceToTargetRate", aToB},
                       {"targetToSourceRate", bToA},
                       {"sourceEndpoint", aViewOfB ? aViewOfB->endpoint : std::string{}},
                       {"targetEndpoint", bViewOfA ? bViewOfA->endpoint : std::string{}}});
    }
  }

  nlohmann::json rows = nlohmann::json::array();
  double totalRx = 0;
  double totalTx = 0;
  int online = 0;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const auto& node = nodes[i];
    auto row = nodeToJson(node);
    if (node.serverId) {
      if (const auto server = ctx_.repos->findServerById(*node.serverId)) {
        row["name"] = server->name;
        row["icon"] = server->icon;
        row["serverStatus"] = server->status;
      }
    }
    const auto& sample = samples[i];
    double rxRate = 0;
    double txRate = 0;
    std::uint64_t rxBytes = 0;
    std::uint64_t txBytes = 0;
    if (sample) {
      for (const auto& peer : sample->peers) {
        rxRate += peer.rxRate;
        txRate += peer.txRate;
        rxBytes += peer.rxBytes;
        txBytes += peer.txBytes;
      }
    }
    const bool up = sample && sample->interfaceUp;
    const bool isHub = node.id == hubId;
    // A hub alone in the VPN is as connected as it can be. A member needs its hub link.
    const bool connected =
        up && (isHub ? (totalLinks[i] == 0 || connectedLinks[i] > 0) : connectedLinks[i] > 0);
    row["isHub"] = isHub;
    if (connected) {
      ++online;
    }
    totalRx += rxRate;
    totalTx += txRate;
    row["reporting"] = sample.has_value();
    row["interfaceUp"] = up;
    row["connected"] = connected;
    row["error"] = sample ? sample->error : std::string{};
    row["reportedAt"] = sample ? sample->reportedAtUnixMs : 0;
    row["linksUp"] = connectedLinks[i];
    row["linksTotal"] = totalLinks[i];
    row["rxRate"] = rxRate;
    row["txRate"] = txRate;
    row["rxBytes"] = rxBytes;
    row["txBytes"] = txBytes;
    row["agentConnected"] = node.serverId && ctx_.agentHub && ctx_.agentHub->isConnected(*node.serverId);
    rows.push_back(std::move(row));
  }
  body["totals"] = {{"nodes", nodes.size()},
                    {"online", online},
                    {"links", links.size()},
                    {"linksUp", linksUp},
                    {"rxRate", totalRx},
                    {"txRate", totalTx}};
  body["nodes"] = std::move(rows);
  body["links"] = std::move(links);
  return body;
}

nlohmann::json VpnService::serverIndicators() {
  nlohmann::json out = nlohmann::json::object();
  if (!currentSubnet(*ctx_.repos)) {
    return out;
  }
  const auto snapshot = state();
  for (const auto& node : snapshot["nodes"]) {
    if (node["serverId"].is_null()) {
      continue;
    }
    const auto serverId = node["serverId"].get<int64_t>();
    out[std::to_string(serverId)] = {
        {"member", true},
        {"connected", node["connected"]},
        {"address", node["address"]},
        {"isHub", node["isHub"]},
        {"status", node["status"]},
        {"peersUp", node["linksUp"]},
        {"peersTotal", node["linksTotal"]},
        // What nginx dials right now; empty means it falls back to the configured IP.
        {"routedAddress", routingAddress(serverId)}};
  }
  return out;
}

nlohmann::json VpnService::createNetwork(const nlohmann::json& body) {
  if (currentSubnet(*ctx_.repos)) {
    throw std::runtime_error("The VPN already exists");
  }
  const auto cidr = body.is_object() ? body.value("cidr", std::string(vpn::kDefaultCidr))
                                     : std::string(vpn::kDefaultCidr);
  const auto subnet = vpn::parseSubnet(cidr.empty() ? vpn::kDefaultCidr : cidr);
  if (!subnet) {
    throw std::invalid_argument("Use an IPv4 subnet between /16 and /29, such as 10.88.0.0/24");
  }
  ctx_.repos->setSetting("vpn_cidr", subnet->cidr());
  ctx_.repos->setSetting("vpn_created_at", nowIso());
  spdlog::info("vpn created on {}", subnet->cidr());
  return state()["network"];
}

void VpnService::deleteNetwork() {
  VpnNodeRepository repo(*ctx_.database);
  std::vector<int64_t> serverIds;
  for (const auto& node : repo.list()) {
    if (node.serverId) {
      serverIds.push_back(*node.serverId);
      if (ctx_.agentHub) {
        ctx_.agentHub->sendVpnApply(*node.serverId, disableJson().dump());
      }
    }
  }
  repo.removeAll();
  ctx_.repos->setSetting("vpn_cidr", "");
  ctx_.repos->setSetting("vpn_created_at", "");
  setHubNodeId(*ctx_.repos, 0);
  if (ctx_.vpnLive) {
    ctx_.vpnLive->clear();
  }
  spdlog::info("vpn deleted");
  // Back to the configured IPs before the tunnels go away.
  for (const auto serverId : serverIds) {
    refreshRouting(serverId);
  }
}

nlohmann::json VpnService::joinServer(int64_t serverId, const nlohmann::json& body) {
  const auto server = ctx_.repos->findServerById(serverId);
  if (!server) {
    throw std::runtime_error("Server not found");
  }
  if (!currentSubnet(*ctx_.repos)) {
    createNetwork(nlohmann::json::object());
  }
  const auto subnet = currentSubnet(*ctx_.repos);
  VpnNodeRepository repo(*ctx_.database);
  if (repo.findByServerId(serverId)) {
    throw std::runtime_error("This server is already in the VPN");
  }
  const auto nodes = repo.list();
  VpnNode node;
  node.kind = "server";
  node.serverId = serverId;
  node.name = server->name;
  node.address = vpn::allocateAddress(*subnet, nodes);
  const int requestedPort = body.is_object() ? readInt(body, "listenPort", 0) : 0;
  node.listenPort = requestedPort > 0 ? validatePort(requestedPort, nodes, 0)
                                      : vpn::allocateListenPort(nodes);
  node.endpoint = body.is_object() ? validateEndpoint(body.value("endpoint", std::string{})) : "";
  node.status = "pending";
  node.createdAt = nowIso();
  node.id = repo.create(node);
  // The first server becomes the hub; later ones only when asked.
  const bool asHub = hubNodeId(*ctx_.repos) == 0 || (body.is_object() && body.value("hub", false));
  if (asHub) {
    setHubNodeId(*ctx_.repos, node.id);
  }
  spdlog::info("vpn: server {} joins as {}{}", server->name, node.address, asHub ? " (hub)" : "");
  pushAll();
  auto out = nodeToJson(node);
  out["agentConnected"] = ctx_.agentHub && ctx_.agentHub->isConnected(serverId);
  return out;
}

nlohmann::json VpnService::updateNode(int64_t nodeId, const nlohmann::json& body) {
  VpnNodeRepository repo(*ctx_.database);
  auto node = repo.findById(nodeId);
  if (!node || node->status == "enrolling") {
    throw std::runtime_error("VPN node not found");
  }
  if (body.contains("name") && body["name"].is_string() && node->kind == "device") {
    node->name = sanitizeName(body["name"].get<std::string>());
  }
  if (body.contains("listenPort")) {
    node->listenPort = validatePort(readInt(body, "listenPort", 0), repo.list(), node->id);
  }
  if (body.contains("endpoint") && body["endpoint"].is_string()) {
    node->endpoint = validateEndpoint(body["endpoint"].get<std::string>());
  }
  repo.update(*node);
  pushAll();
  return nodeToJson(*node);
}

void VpnService::removeNode(int64_t nodeId) {
  VpnNodeRepository repo(*ctx_.database);
  const auto node = repo.findById(nodeId);
  if (!node) {
    throw std::runtime_error("VPN node not found");
  }
  if (node->id == hubNodeId(*ctx_.repos)) {
    for (const auto& other : repo.list()) {
      if (other.id != node->id && other.status != "enrolling") {
        throw std::runtime_error(
            "This is the central node. Make another server the central node first, or delete the VPN");
      }
    }
    setHubNodeId(*ctx_.repos, 0);
  }
  if (node->serverId && ctx_.agentHub) {
    ctx_.agentHub->sendVpnApply(*node->serverId, disableJson().dump());
  }
  repo.remove(node->id);
  if (ctx_.vpnLive) {
    ctx_.vpnLive->forget(node->id);
  }
  spdlog::info("vpn: {} left", node->name);
  pushAll();
  if (node->serverId) {
    refreshRouting(*node->serverId);
  }
}

nlohmann::json VpnService::setHub(int64_t nodeId) {
  VpnNodeRepository repo(*ctx_.database);
  const auto node = repo.findById(nodeId);
  if (!node || node->status == "enrolling") {
    throw std::runtime_error("VPN node not found");
  }
  if (node->kind != "server") {
    throw std::runtime_error("Only a server with hc-agent can be the central node");
  }
  setHubNodeId(*ctx_.repos, node->id);
  spdlog::info("vpn: {} is the central node", node->name);
  pushAll();
  return nodeToJson(*node);
}

nlohmann::json VpnService::createEnrollment(const User& actor, const std::string& requestOrigin) {
  if (!currentSubnet(*ctx_.repos) || hubNodeId(*ctx_.repos) == 0) {
    throw std::runtime_error("Add a server to the VPN first: it becomes the central node");
  }
  VpnNodeRepository repo(*ctx_.database);
  repo.removeExpiredEnrollments(nowIso());
  const auto token = randomToken(24);
  const auto expires = std::chrono::system_clock::now() + kEnrollmentTtl;
  VpnNode node;
  node.kind = "device";
  node.userId = actor.id;
  node.name = "Pending device";
  node.listenPort = 0;
  node.status = "enrolling";
  node.tokenHash = hashToken(token);
  node.tokenExpiresAt = isoAt(expires);
  node.createdAt = nowIso();
  node.id = repo.create(node);
  return {{"id", node.id},
          {"command", "curl -fsSL '" + baseUrl(ctx_, requestOrigin) + "/api/vpn/enroll/" + token +
                          "' | sudo bash"},
          {"expiresAt", node.tokenExpiresAt}};
}

nlohmann::json VpnService::enrollment(int64_t nodeId) {
  VpnNodeRepository repo(*ctx_.database);
  const auto node = repo.findById(nodeId);
  if (!node) {
    return {{"status", "expired"}};
  }
  if (node->status == "enrolling") {
    const bool expired = node->tokenExpiresAt < nowIso();
    return {{"status", expired ? "expired" : "enrolling"}, {"expiresAt", node->tokenExpiresAt}};
  }
  auto out = nodeToJson(*node);
  out["status"] = "active";
  return out;
}

std::string VpnService::enrollScript(const std::string& token, const std::string& requestOrigin) {
  const auto node = VpnNodeRepository(*ctx_.database).findByTokenHash(hashToken(token));
  if (!node || node->status != "enrolling" || node->tokenExpiresAt < nowIso()) {
    return vpn::renderExpiredScript();
  }
  return vpn::renderEnrollScript(baseUrl(ctx_, requestOrigin), token);
}

std::string VpnService::enroll(const std::string& token, const nlohmann::json& body,
                               const std::string& observedIp) {
  VpnNodeRepository repo(*ctx_.database);
  auto node = repo.findByTokenHash(hashToken(token));
  if (!node || node->status != "enrolling" || node->tokenExpiresAt < nowIso()) {
    throw ForbiddenError("This enrollment link is invalid or expired");
  }
  const auto subnet = currentSubnet(*ctx_.repos);
  if (!subnet) {
    throw std::runtime_error("The VPN does not exist");
  }
  const auto publicKey = body.value("publicKey", std::string{});
  if (!validPublicKey(publicKey)) {
    throw std::invalid_argument("Invalid WireGuard public key");
  }
  // Running the command again on the same machine keeps its address and port.
  if (const auto existing = repo.findByPublicKey(publicKey); existing && existing->id != node->id) {
    if (existing->kind != "device") {
      throw std::runtime_error("This key already belongs to a server");
    }
    node->address = existing->address;
    node->listenPort = existing->listenPort;
    node->endpoint = existing->endpoint;
    repo.remove(existing->id);
    if (ctx_.vpnLive) {
      ctx_.vpnLive->forget(existing->id);
    }
  }
  const auto nodes = repo.list();
  if (node->address.empty()) {
    node->address = vpn::allocateAddress(*subnet, nodes);
  }
  if (node->listenPort <= 0) {
    node->listenPort = vpn::allocateListenPort(nodes, node->id);
  }
  const auto deviceToken = randomToken(32);
  node->name = sanitizeName(body.value("hostname", std::string{}));
  node->publicKey = publicKey;
  node->lanIp = validLanIp(body.value("lanIp", std::string{}));
  node->observedIp = vpn::normalizeObservedIp(observedIp);
  node->status = "active";
  node->tokenHash = hashToken(deviceToken);
  node->tokenExpiresAt.clear();
  node->lastSeenAt = nowIso();
  repo.update(*node);
  spdlog::info("vpn: device {} enrolled as {}", node->name, node->address);
  pushAll();
  return deviceToken;
}

std::optional<std::string> VpnService::deviceSync(const std::string& deviceToken,
                                                  const std::string& dump, bool interfaceUp,
                                                  const std::string& lanIp,
                                                  const std::string& observedIp) {
  if (deviceToken.empty()) {
    return std::nullopt;
  }
  VpnNodeRepository repo(*ctx_.database);
  auto node = repo.findByTokenHash(hashToken(deviceToken));
  const auto subnet = currentSubnet(*ctx_.repos);
  if (!node || node->kind != "device" || node->status != "active" || !subnet) {
    return std::nullopt;
  }
  const auto lan = validLanIp(lanIp);
  const auto observed = vpn::normalizeObservedIp(observedIp);
  const bool moved = lan != node->lanIp || (!observed.empty() && observed != node->observedIp);
  if (moved || olderThan(node->lastSeenAt, kLastSeenPersistSeconds)) {
    node->lanIp = lan;
    if (!observed.empty()) {
      node->observedIp = observed;
    }
    node->lastSeenAt = nowIso();
    repo.update(*node);
  }
  if (ctx_.vpnLive) {
    ctx_.vpnLive->record(node->id, interfaceUp, "", vpn::parseWgDump(dump));
  }
  if (moved) {
    // Endpoints other members use for this device depend on where it is.
    pushAll();
  }
  return makeTopology(ctx_, *subnet, repo.list()).renderConfig(*node);
}

void VpnService::onAgentRegistered(int64_t serverId, const std::string& remoteIp) {
  VpnNodeRepository repo(*ctx_.database);
  auto node = repo.findByServerId(serverId);
  const auto subnet = currentSubnet(*ctx_.repos);
  if (!node || !subnet) {
    return;
  }
  const auto observed = vpn::normalizeObservedIp(remoteIp);
  if (!observed.empty() && observed != node->observedIp) {
    node->observedIp = observed;
    repo.update(*node);
    pushAll();
    return;
  }
  const auto topology = makeTopology(ctx_, *subnet, repo.list());
  ctx_.agentHub->sendVpnApply(serverId, applyJson(*node, topology).dump());
}

void VpnService::onAgentStatus(int64_t serverId, const std::string& payloadJson,
                               const std::string& remoteIp) {
  const auto payload = nlohmann::json::parse(payloadJson, nullptr, false);
  if (!payload.is_object()) {
    return;
  }
  VpnNodeRepository repo(*ctx_.database);
  auto node = repo.findByServerId(serverId);
  if (!node || !currentSubnet(*ctx_.repos)) {
    // Removed while the agent was offline: tell it to drop the interface.
    if (ctx_.agentHub) {
      ctx_.agentHub->sendVpnApply(serverId, disableJson().dump());
    }
    return;
  }
  bool topologyChanged = false;
  const auto publicKey = payload.value("publicKey", std::string{});
  if (validPublicKey(publicKey) && publicKey != node->publicKey) {
    const auto owner = repo.findByPublicKey(publicKey);
    if (owner && owner->id != node->id) {
      spdlog::warn("vpn: server {} reported a key owned by {}", serverId, owner->name);
      return;
    }
    node->publicKey = publicKey;
    topologyChanged = true;
  }
  if (node->status != "active" && !node->publicKey.empty()) {
    node->status = "active";
    topologyChanged = true;
  }
  const auto lan = validLanIp(payload.value("lanIp", std::string{}));
  if (!lan.empty() && lan != node->lanIp) {
    node->lanIp = lan;
    topologyChanged = true;
  }
  const auto observed = vpn::normalizeObservedIp(remoteIp);
  if (!observed.empty() && observed != node->observedIp) {
    node->observedIp = observed;
    topologyChanged = true;
  }
  if (topologyChanged || olderThan(node->lastSeenAt, kLastSeenPersistSeconds)) {
    node->lastSeenAt = nowIso();
    repo.update(*node);
  }
  if (ctx_.vpnLive) {
    ctx_.vpnLive->record(node->id, payload.value("up", false), payload.value("error", std::string{}),
                         vpn::parseWgDump(payload.value("dump", std::string{})));
  }
  refreshRouting(serverId);
  if (topologyChanged) {
    pushAll();
  }
}

std::string VpnService::routingAddress(int64_t serverId) {
  // nginx runs next to hc-admin: it can only dial VPN addresses if this host is a member.
  std::error_code ec;
  if (!ctx_.vpnLive || !std::filesystem::exists(std::string("/sys/class/net/") + vpn::kInterfaceName, ec)) {
    return {};
  }
  VpnNodeRepository repo(*ctx_.database);
  const auto node = repo.findByServerId(serverId);
  if (!node || node->status != "active" || node->address.empty()) {
    return {};
  }
  const auto sample = ctx_.vpnLive->get(node->id);
  if (!sample || !sample->interfaceUp ||
      std::chrono::steady_clock::now() - sample->receivedAt >
          std::chrono::seconds(kServerStaleSeconds)) {
    return {};
  }
  const auto hubId = hubNodeId(*ctx_.repos);
  if (node->id == hubId) {
    return node->address;
  }
  // A member is reachable while its session with the hub is alive.
  const auto hub = hubId ? repo.findById(hubId) : std::nullopt;
  const auto* peer = hub ? findPeer(sample, hub->publicKey) : nullptr;
  if (!peer || peer->latestHandshake <= 0 || nowUnix() - peer->latestHandshake > kHandshakeFreshSeconds) {
    return {};
  }
  return node->address;
}

void VpnService::refreshRouting(int64_t serverId) {
  if (!ctx_.vpnLive || !ctx_.proxy) {
    return;
  }
  const bool routable = !routingAddress(serverId).empty();
  const auto previous = ctx_.vpnLive->exchangeRoutable(serverId, routable);
  if (previous && *previous == routable) {
    return;
  }
  if (previous || routable) {
    spdlog::info("vpn: nginx reaches server {} through {}", serverId,
                 routable ? "the VPN" : "its configured IP");
  }
  std::vector<int64_t> applicationIds;
  for (const auto& replica : ctx_.repos->listReplicasForServer(serverId)) {
    applicationIds.push_back(replica.applicationId);
  }
  std::sort(applicationIds.begin(), applicationIds.end());
  applicationIds.erase(std::unique(applicationIds.begin(), applicationIds.end()), applicationIds.end());
  for (const auto applicationId : applicationIds) {
    if (auto app = ctx_.repos->findApplicationById(applicationId)) {
      ctx_.proxy->renderApplicationSite(*app);
    }
  }
}

void VpnService::learnPublicHost(const std::string& requestOrigin) {
  const auto host = vpn::hostOfUrl(requestOrigin);
  if (!usablePublicHost(host) || host.size() > 253) {
    return;
  }
  if (ctx_.repos->getSetting("vpn_public_host", "") == host) {
    return;
  }
  ctx_.repos->setSetting("vpn_public_host", host);
  spdlog::info("vpn: hc-admin is publicly reachable as {}", host);
  pushAll();
}

void VpnService::onServerRemoved(int64_t serverId) {
  VpnNodeRepository repo(*ctx_.database);
  const auto node = repo.findByServerId(serverId);
  if (!node) {
    return;
  }
  // Deleting the server must not be blocked: another server takes over as hub.
  if (node->id == hubNodeId(*ctx_.repos)) {
    int64_t next = 0;
    for (const auto& other : repo.list()) {
      if (other.id != node->id && other.kind == "server") {
        next = other.id;
        break;
      }
    }
    setHubNodeId(*ctx_.repos, next);
  }
  removeNode(node->id);
}

}  // namespace highcube::admin
