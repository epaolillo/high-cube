#include "services/services.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace highcube::admin {
namespace {

// Display name -> RFC 1123 hostname label: "Sigma Server_2" -> "sigma-server-2".
std::string toHostname(std::string name) {
  static const std::pair<const char*, const char*> kAccents[] = {
      {"á", "a"}, {"é", "e"}, {"í", "i"}, {"ó", "o"}, {"ú", "u"}, {"ü", "u"}, {"ñ", "n"},
      {"Á", "a"}, {"É", "e"}, {"Í", "i"}, {"Ó", "o"}, {"Ú", "u"}, {"Ü", "u"}, {"Ñ", "n"},
  };
  for (const auto& [from, to] : kAccents) {
    const std::string needle(from);
    for (auto pos = name.find(needle); pos != std::string::npos; pos = name.find(needle, pos + 1)) {
      name.replace(pos, needle.size(), to);
    }
  }
  std::string out;
  for (const unsigned char c : name) {
    if (std::isalnum(c) && c < 128) {
      out.push_back(static_cast<char>(std::tolower(c)));
    } else if (!out.empty() && out.back() != '-') {
      out.push_back('-');
    }
  }
  if (out.size() > 63) {
    out.resize(63);
  }
  while (!out.empty() && out.back() == '-') {
    out.pop_back();
  }
  return out;
}

std::string isoUtc(std::chrono::system_clock::time_point tp) {
  const auto tt = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  gmtime_r(&tt, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << ".000Z";
  return oss.str();
}

struct CpuCounters {
  double user = 0;
  double nice = 0;
  double system = 0;
  double idle = 0;
  bool ok = false;
};

struct HostSample {
  std::string at;
  CpuCounters cpu;
  double memoryTotalKb = 0;
  double memoryAvailableKb = 0;
  double diskTotalBytes = 0;
  double diskAvailableBytes = 0;
  double load1 = 0;
  double load5 = 0;
  double load15 = 0;
  bool gpuPresent = false;
  double gpuPercent = 0;
  double gpuMemoryUsedMb = 0;
  double gpuMemoryTotalMb = 0;
  int gpuCount = 0;
  std::string gpuName;
  int cpuCores = 0;
  int cpuThreads = 0;
  double uptimeSeconds = 0;
  bool networkOk = false;
  double rxBytes = 0;
  double txBytes = 0;
};

double counter(const nlohmann::json& cpu, const char* key) {
  if (!cpu.contains(key) || cpu.at(key).is_null()) {
    return 0;
  }
  if (cpu.at(key).is_number()) {
    return cpu.at(key).get<double>();
  }
  return 0;
}

HostSample parseSample(const MetricRow& row) {
  HostSample sample;
  sample.at = row.collectedAt;
  const auto payload = nlohmann::json::parse(row.payloadJson.empty() ? "{}" : row.payloadJson, nullptr, false);
  if (!payload.is_object()) {
    return sample;
  }
  if (payload.contains("cpu") && payload.at("cpu").is_object()) {
    const auto& cpu = payload.at("cpu");
    sample.cpu.user = counter(cpu, "user");
    sample.cpu.nice = counter(cpu, "nice");
    sample.cpu.system = counter(cpu, "system");
    sample.cpu.idle = counter(cpu, "idle");
    sample.cpu.ok = true;
  }
  if (payload.contains("memory") && payload.at("memory").is_object()) {
    const auto& memory = payload.at("memory");
    sample.memoryTotalKb = memory.value("totalKb", 0.0);
    sample.memoryAvailableKb = memory.value("availableKb", 0.0);
  }
  if (payload.contains("disk") && payload.at("disk").is_object()) {
    const auto& disk = payload.at("disk");
    sample.diskTotalBytes = disk.value("totalBytes", 0.0);
    sample.diskAvailableBytes = disk.value("availableBytes", 0.0);
  }
  if (payload.contains("load") && payload.at("load").is_object()) {
    const auto& load = payload.at("load");
    sample.load1 = load.value("1", 0.0);
    sample.load5 = load.value("5", 0.0);
    sample.load15 = load.value("15", 0.0);
  }
  if (payload.contains("gpu") && payload.at("gpu").is_object()) {
    const auto& gpu = payload.at("gpu");
    const auto gpus = gpu.value("gpus", nlohmann::json::array());
    if (gpu.value("present", false) && gpus.is_array() && !gpus.empty()) {
      sample.gpuPresent = true;
      sample.gpuCount = static_cast<int>(gpus.size());
      double utilization = 0;
      for (const auto& item : gpus) {
        if (!item.is_object()) {
          continue;
        }
        utilization += item.value("utilizationPercent", 0.0);
        sample.gpuMemoryUsedMb += item.value("memoryUsedMb", 0.0);
        sample.gpuMemoryTotalMb += item.value("memoryTotalMb", 0.0);
        if (sample.gpuCount == 1) {
          sample.gpuName = item.value("name", std::string{});
        }
      }
      sample.gpuPercent = utilization / static_cast<double>(sample.gpuCount);
    }
  }
  if (payload.contains("cpuInfo") && payload.at("cpuInfo").is_object()) {
    const auto& info = payload.at("cpuInfo");
    sample.cpuCores = info.value("cores", 0);
    sample.cpuThreads = info.value("threads", 0);
  }
  if (payload.contains("uptimeSeconds") && payload.at("uptimeSeconds").is_number()) {
    sample.uptimeSeconds = payload.at("uptimeSeconds").get<double>();
  }
  if (payload.contains("network") && payload.at("network").is_object()) {
    const auto& network = payload.at("network");
    sample.rxBytes = network.value("rxBytes", 0.0);
    sample.txBytes = network.value("txBytes", 0.0);
    sample.networkOk = true;
  }
  return sample;
}

struct NetworkRate {
  double rx = 0;
  double tx = 0;
};

// Bytes per second between two samples, timed by the host's uptime clock. Empty when
// the samples cannot be compared (old agent, reboot, counter reset).
std::optional<NetworkRate> networkRate(const HostSample& previous, const HostSample& current) {
  if (!previous.networkOk || !current.networkOk) {
    return std::nullopt;
  }
  const double elapsed = current.uptimeSeconds - previous.uptimeSeconds;
  if (elapsed <= 0 || current.rxBytes < previous.rxBytes || current.txBytes < previous.txBytes) {
    return std::nullopt;
  }
  return NetworkRate{(current.rxBytes - previous.rxBytes) / elapsed,
                     (current.txBytes - previous.txBytes) / elapsed};
}

double cpuPercent(const CpuCounters& previous, const CpuCounters& current) {
  const auto busy = [](const CpuCounters& sample) {
    return sample.user + sample.nice + sample.system;
  };
  const auto total = [&](const CpuCounters& sample) { return busy(sample) + sample.idle; };
  const double deltaTotal = total(current) - total(previous);
  const double deltaBusy = busy(current) - busy(previous);
  if (deltaTotal <= 0) {
    return 0;
  }
  return std::max(0.0, std::min(100.0, (deltaBusy / deltaTotal) * 100.0));
}

long long memoryUsedBytes(const HostSample& sample) {
  return static_cast<long long>(std::max(0.0, sample.memoryTotalKb - sample.memoryAvailableKb) * 1024.0);
}

long long memoryLimitBytes(const HostSample& sample) {
  return static_cast<long long>(std::max(0.0, sample.memoryTotalKb) * 1024.0);
}

double percent(double used, double total) {
  if (total <= 0) {
    return 0;
  }
  return std::max(0.0, std::min(100.0, (used / total) * 100.0));
}

nlohmann::json latestJson(const HostSample& sample, std::optional<double> cpu) {
  nlohmann::json body = {
      {"collectedAt", sample.at},
      {"memoryUsedBytes", memoryUsedBytes(sample)},
      {"memoryLimitBytes", memoryLimitBytes(sample)},
      {"diskUsedBytes", static_cast<long long>(std::max(0.0, sample.diskTotalBytes - sample.diskAvailableBytes))},
      {"diskTotalBytes", static_cast<long long>(std::max(0.0, sample.diskTotalBytes))},
      {"load", {{"1", sample.load1}, {"5", sample.load5}, {"15", sample.load15}}},
      {"gpuPresent", sample.gpuPresent},
      {"gpuPercent", sample.gpuPercent},
      {"gpuMemoryUsedBytes", static_cast<long long>(sample.gpuMemoryUsedMb * 1024.0 * 1024.0)},
      {"gpuMemoryTotalBytes", static_cast<long long>(sample.gpuMemoryTotalMb * 1024.0 * 1024.0)},
      {"gpuCount", sample.gpuCount},
  };
  if (cpu) {
    body["cpuPercent"] = *cpu;
  }
  if (!sample.gpuName.empty()) {
    body["gpuName"] = sample.gpuName;
  }
  if (sample.cpuThreads > 0) {
    body["cpuCores"] = sample.cpuCores;
    body["cpuThreads"] = sample.cpuThreads;
  }
  if (sample.uptimeSeconds > 0) {
    body["uptimeSeconds"] = static_cast<long long>(sample.uptimeSeconds);
  }
  return body;
}

nlohmann::json chartFromSamples(const std::vector<HostSample>& samples) {
  nlohmann::json latest = nullptr;
  if (!samples.empty()) {
    std::optional<double> cpu;
    if (samples.size() >= 2 && samples[samples.size() - 2].cpu.ok && samples.back().cpu.ok) {
      cpu = cpuPercent(samples[samples.size() - 2].cpu, samples.back().cpu);
    }
    latest = latestJson(samples.back(), cpu);
  }

  nlohmann::json cpuSeries = nlohmann::json::array();
  nlohmann::json memorySeries = nlohmann::json::array();
  nlohmann::json gpuSeries = nlohmann::json::array();
  nlohmann::json points = nlohmann::json::array();
  if (samples.size() >= 2) {
    const int intervals = static_cast<int>(samples.size()) - 1;
    const int buckets = std::min(480, intervals);
    // A week of one-second samples is hundreds of thousands of rows. Average them
    // into a few hundred points and keep the time of each bucket so a chart can
    // label its axes and zoom.
    struct Bucket {
      std::string at;
      double cpu = 0;
      int cpuCount = 0;
      double memory = 0;
      int memoryCount = 0;
      double disk = 0;
      int diskCount = 0;
      double load1 = 0;
      double load5 = 0;
      double load15 = 0;
      int loadCount = 0;
      double gpu = 0;
      int gpuCount = 0;
      double gpuMemory = 0;
      int gpuMemoryCount = 0;
    };
    std::vector<Bucket> aggs(static_cast<std::size_t>(buckets));
    bool anyGpu = false;
    for (int i = 1; i < static_cast<int>(samples.size()); ++i) {
      const int bucket = (i - 1) * buckets / intervals;
      auto& agg = aggs[static_cast<std::size_t>(bucket)];
      const auto& current = samples[static_cast<std::size_t>(i)];
      const auto& previous = samples[static_cast<std::size_t>(i - 1)];
      agg.at = current.at;
      if (previous.cpu.ok && current.cpu.ok) {
        agg.cpu += cpuPercent(previous.cpu, current.cpu);
        ++agg.cpuCount;
      }
      if (current.memoryTotalKb > 0) {
        agg.memory += percent(current.memoryTotalKb - current.memoryAvailableKb, current.memoryTotalKb);
        ++agg.memoryCount;
      }
      if (current.diskTotalBytes > 0) {
        agg.disk += percent(current.diskTotalBytes - current.diskAvailableBytes, current.diskTotalBytes);
        ++agg.diskCount;
      }
      agg.load1 += current.load1;
      agg.load5 += current.load5;
      agg.load15 += current.load15;
      ++agg.loadCount;
      if (current.gpuPresent) {
        anyGpu = true;
        agg.gpu += current.gpuPercent;
        ++agg.gpuCount;
        if (current.gpuMemoryTotalMb > 0) {
          agg.gpuMemory += percent(current.gpuMemoryUsedMb, current.gpuMemoryTotalMb);
          ++agg.gpuMemoryCount;
        }
      }
    }
    for (const auto& agg : aggs) {
      cpuSeries.push_back(agg.cpuCount > 0 ? agg.cpu / agg.cpuCount : 0.0);
      memorySeries.push_back(agg.memoryCount > 0 ? agg.memory / agg.memoryCount : 0.0);
      if (anyGpu) {
        gpuSeries.push_back(agg.gpuCount > 0 ? agg.gpu / agg.gpuCount : 0.0);
      }
      if (agg.at.empty()) {
        continue;
      }
      auto average = [](double sum, int count) -> nlohmann::json {
        return count > 0 ? nlohmann::json(sum / count) : nullptr;
      };
      nlohmann::json point = {{"at", agg.at},
                              {"cpu", average(agg.cpu, agg.cpuCount)},
                              {"memory", average(agg.memory, agg.memoryCount)},
                              {"disk", average(agg.disk, agg.diskCount)},
                              {"load1", average(agg.load1, agg.loadCount)},
                              {"load5", average(agg.load5, agg.loadCount)},
                              {"load15", average(agg.load15, agg.loadCount)}};
      if (anyGpu) {
        point["gpu"] = average(agg.gpu, agg.gpuCount);
        point["gpuMemory"] = average(agg.gpuMemory, agg.gpuMemoryCount);
      }
      points.push_back(std::move(point));
    }
  }
  return {{"series", {{"cpu", cpuSeries}, {"memory", memorySeries}, {"gpu", gpuSeries}}},
          {"points", points},
          {"latest", latest}};
}

}  // namespace

ServerService::ServerService(AppContext& ctx) : ctx_(ctx) {}

nlohmann::json ServerService::list(const ListQuery& query) {
  const auto page = ctx_.repos->listServers(query);
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& server : page.rows) {
    rows.push_back(serverToJson(server));
  }
  return {{"rows", rows}, {"total", page.total}};
}

nlohmann::json ServerService::listHostingUserApplications(int64_t userId) {
  std::vector<Server> servers;
  std::unordered_set<int64_t> seen;
  auto add = [&](const Server& server) {
    if (!seen.insert(server.id).second) {
      return;
    }
    servers.push_back(server);
  };
  for (const auto id : ctx_.repos->getUserServers(userId)) {
    if (const auto server = ctx_.repos->findServerById(id)) {
      add(*server);
    }
  }
  for (const auto& server : ctx_.repos->listServersHostingUserApplications(userId)) {
    add(server);
  }
  std::sort(servers.begin(), servers.end(),
            [](const Server& a, const Server& b) { return a.name < b.name; });
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& server : servers) {
    rows.push_back({{"id", server.id},
                    {"name", server.name},
                    {"status", server.status},
                    {"icon", server.icon},
                    {"metrics", snapshot(server.id)}});
  }
  return {{"rows", rows}, {"total", rows.size()}};
}

nlohmann::json ServerService::get(int64_t id) {
  auto server = ctx_.repos->findServerById(id);
  if (!server) {
    throw std::runtime_error("Server not found");
  }
  return serverToJson(*server);
}

nlohmann::json ServerService::create(const nlohmann::json& body) {
  Server server;
  server.name = body.at("name").get<std::string>();
  server.ip = body.at("ip").get<std::string>();
  server.status = "pending";
  const auto token = randomToken();
  server.agentTokenHash = hashToken(token);
  server.id = ctx_.repos->createServer(server);
  auto json = serverToJson(*ctx_.repos->findServerById(server.id));
  json["agentToken"] = token;
  return json;
}

nlohmann::json ServerService::update(int64_t id, const nlohmann::json& body) {
  auto server = ctx_.repos->findServerById(id);
  if (!server) {
    throw std::runtime_error("Server not found");
  }
  const auto previousIp = server->ip;
  const auto previousName = server->name;
  if (body.contains("name")) {
    const auto name = body.at("name").get<std::string>();
    if (name.empty()) {
      throw std::runtime_error("Name is required");
    }
    server->name = name;
  }
  if (body.contains("ip")) {
    const auto ip = body.at("ip").get<std::string>();
    if (ip.empty()) {
      throw std::runtime_error("IP is required");
    }
    server->ip = ip;
  }
  if (body.contains("icon") && !body.at("icon").is_null()) {
    const auto icon = body.at("icon").get<std::string>();
    // Same cap as application icons: a catalog path or a square data URL.
    if (icon.size() > 500000) {
      throw std::invalid_argument("icon is too large");
    }
    server->icon = icon;
  }
  ctx_.repos->updateServer(*server);

  if (server->ip != previousIp && ctx_.proxy) {
    std::vector<int64_t> applicationIds;
    for (const auto& replica : ctx_.repos->listReplicasForServer(id)) {
      applicationIds.push_back(replica.applicationId);
    }
    std::sort(applicationIds.begin(), applicationIds.end());
    applicationIds.erase(std::unique(applicationIds.begin(), applicationIds.end()),
                         applicationIds.end());
    for (const auto applicationId : applicationIds) {
      if (auto app = ctx_.repos->findApplicationById(applicationId)) {
        ctx_.proxy->renderApplicationSite(*app);
      }
    }
  }
  auto reply = serverToJson(*ctx_.repos->findServerById(id));
  if (server->name != previousName) {
    const auto hostname = toHostname(server->name);
    nlohmann::json change = {{"hostname", hostname}};
    if (hostname.empty()) {
      change["status"] = "invalid";
    } else {
      const auto requestId =
          ctx_.agentHub ? ctx_.agentHub->startHostnameChange(id, hostname) : std::string{};
      change["status"] = requestId.empty() ? "offline" : "sent";
      if (!requestId.empty()) {
        change["requestId"] = requestId;
      }
    }
    reply["hostnameChange"] = change;
  }
  return reply;
}

nlohmann::json ServerService::hostnameChange(int64_t id, const std::string& requestId) {
  if (!ctx_.agentHub) {
    return {{"status", "unknown"}};
  }
  const auto poll = ctx_.agentHub->pollHostnameChange(requestId, id);
  return {{"status", poll.status}, {"hostname", poll.hostname}, {"error", poll.error}};
}

nlohmann::json ServerService::updateAgent(int64_t id) {
  if (!ctx_.repos->findServerById(id)) {
    throw std::runtime_error("Server not found");
  }
  if (!ctx_.agentHub) {
    throw std::runtime_error("Agent hub is not available");
  }
  ctx_.agentHub->updateAgent(id);
  return {{"status", "started"}};
}

nlohmann::json ServerService::rotateToken(int64_t id) {
  auto server = ctx_.repos->findServerById(id);
  if (!server) {
    throw std::runtime_error("Server not found");
  }
  const auto token = randomToken();
  server->agentTokenHash = hashToken(token);
  ctx_.repos->updateServer(*server);
  auto json = serverToJson(*server);
  json["agentToken"] = token;
  return json;
}

void ServerService::remove(int64_t id) {
  std::vector<int64_t> applicationIds;
  for (const auto& replica : ctx_.repos->listReplicasForServer(id)) {
    applicationIds.push_back(replica.applicationId);
  }
  // Before the disconnect, so the agent still receives the order to drop the interface.
  VpnService(ctx_).onServerRemoved(id);
  if (ctx_.agentHub) {
    ctx_.agentHub->disconnectServer(id);
  }
  ctx_.repos->deleteServer(id);

  std::sort(applicationIds.begin(), applicationIds.end());
  applicationIds.erase(std::unique(applicationIds.begin(), applicationIds.end()),
                       applicationIds.end());
  if (!ctx_.proxy) {
    return;
  }
  for (const auto applicationId : applicationIds) {
    if (auto app = ctx_.repos->findApplicationById(applicationId)) {
      ctx_.proxy->renderApplicationSite(*app);
    }
  }
}

nlohmann::json ServerService::autocomplete(const std::string& q) {
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& server : ctx_.repos->autocompleteServers(q)) {
    rows.push_back({{"id", server.id}, {"label", server.name}});
  }
  return {{"rows", rows}};
}

nlohmann::json ServerService::metrics(int64_t id, const std::string& from, const std::string& to) {
  std::vector<HostSample> samples;
  for (const auto& row : ctx_.repos->listServerMetrics(id, from, to)) {
    samples.push_back(parseSample(row));
  }
  return chartFromSamples(samples);
}

nlohmann::json ServerService::snapshot(int64_t id) {
  // One sample per second: enough rows for a short network sparkline.
  constexpr int kTrafficPoints = 20;
  const auto rows = ctx_.repos->listRecentServerMetrics(id, kTrafficPoints + 1);
  if (rows.empty()) {
    return nlohmann::json::object();
  }
  // listRecentServerMetrics returns newest first.
  std::vector<HostSample> samples;
  samples.reserve(rows.size());
  for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
    samples.push_back(parseSample(*it));
  }
  const auto& current = samples.back();
  std::optional<double> cpu;
  if (samples.size() >= 2) {
    const auto& previous = samples[samples.size() - 2];
    if (previous.cpu.ok && current.cpu.ok) {
      cpu = cpuPercent(previous.cpu, current.cpu);
    }
  }
  auto body = latestJson(current, cpu);

  nlohmann::json rxHistory = nlohmann::json::array();
  nlohmann::json txHistory = nlohmann::json::array();
  for (std::size_t i = 1; i < samples.size(); ++i) {
    if (const auto rate = networkRate(samples[i - 1], samples[i])) {
      rxHistory.push_back(rate->rx);
      txHistory.push_back(rate->tx);
    }
  }
  if (!rxHistory.empty()) {
    body["rxBytesPerSec"] = rxHistory.back();
    body["txBytesPerSec"] = txHistory.back();
    body["rxHistory"] = rxHistory;
    body["txHistory"] = txHistory;
  }
  return body;
}

nlohmann::json ServerService::overview(int64_t id) {
  const auto to = std::chrono::system_clock::now();
  const auto from = to - std::chrono::minutes(5);
  return {{"server", get(id)}, {"metrics", metrics(id, isoUtc(from), isoUtc(to))}};
}

namespace {

long long parseIsoMs(const std::string& value) {
  std::tm tm{};
  std::istringstream in(value);
  in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (in.fail()) {
    return 0;
  }
  return static_cast<long long>(timegm(&tm)) * 1000;
}

std::string isoUtcMs(long long ms) {
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << ".000Z";
  return oss.str();
}

int bucketSecForWindow(long long fromMs, long long toMs) {
  const long long spanSec = std::max(1LL, (toMs - fromMs) / 1000);
  if (spanSec <= 15 * 60) {
    return 60;
  }
  if (spanSec <= 60 * 60) {
    return 300;
  }
  if (spanSec <= 6 * 60 * 60) {
    return 900;
  }
  if (spanSec <= 24 * 60 * 60) {
    return 3600;
  }
  return 21600;
}

std::string chartGroupKey(const nlohmann::json& group) {
  const auto kind = group.value("kind", "");
  if (kind == "replica") {
    return "replica:" + std::to_string(group.value("replicaId", 0LL));
  }
  if (kind == "docker") {
    return "docker";
  }
  return "host";
}

std::string detailGroupKey(const nlohmann::json& group) {
  const auto kind = group.value("kind", "");
  if (kind == "replica") {
    return "replica:" + std::to_string(group.value("replicaId", 0LL));
  }
  if (kind == "docker") {
    return "docker:" + group.value("container", std::string("unknown"));
  }
  return "host";
}

double round2(double value) {
  return std::round(value * 100.0) / 100.0;
}

}  // namespace

nlohmann::json ServerService::cpuAttribution(int64_t id, const std::string& from, const std::string& to) {
  (void)get(id);
  const long long fromMs = parseIsoMs(from);
  const long long toMs = parseIsoMs(to);
  if (fromMs <= 0 || toMs <= 0 || toMs <= fromMs) {
    throw std::invalid_argument("Invalid from/to range");
  }
  const int bucketSec = bucketSecForWindow(fromMs, toMs);
  const long long bucketMs = static_cast<long long>(bucketSec) * 1000;

  // Resolve application names for replicas that currently live on this server.
  std::unordered_map<int64_t, int64_t> replicaToApp;
  std::unordered_map<int64_t, std::string> appNames;
  std::unordered_map<int64_t, int> appReplicaCount;
  for (const auto& replica : ctx_.repos->listReplicasForServer(id)) {
    replicaToApp[replica.id] = replica.applicationId;
    appReplicaCount[replica.applicationId] += 1;
    if (!appNames.count(replica.applicationId)) {
      if (auto app = ctx_.repos->findApplicationById(replica.applicationId)) {
        appNames[replica.applicationId] = app->name;
      }
    }
  }

  struct Acc {
    double sum = 0;
  };
  struct BucketAcc {
    int samples = 0;
    std::unordered_map<std::string, Acc> values;
  };
  std::map<long long, BucketAcc> buckets;
  std::unordered_map<std::string, nlohmann::json> groupMeta;

  for (const auto& row : ctx_.repos->listCpuAttribution(id, from, to)) {
    const long long at = parseIsoMs(row.collectedAt);
    if (at <= 0) {
      continue;
    }
    const long long bucketStart = fromMs + ((at - fromMs) / bucketMs) * bucketMs;
    if (bucketStart < fromMs || bucketStart >= toMs) {
      continue;
    }
    const auto payload =
        nlohmann::json::parse(row.payloadJson.empty() ? "{}" : row.payloadJson, nullptr, false);
    if (!payload.is_object() || !payload.contains("groups") || !payload.at("groups").is_array()) {
      continue;
    }
    auto& bucket = buckets[bucketStart];
    bucket.samples += 1;
    for (const auto& group : payload.at("groups")) {
      if (!group.is_object()) {
        continue;
      }
      const auto key = chartGroupKey(group);
      bucket.values[key].sum += group.value("cpu", 0.0);
      if (!groupMeta.count(key)) {
        nlohmann::json meta = {{"key", key}, {"kind", group.value("kind", "host")}};
        if (key.rfind("replica:", 0) == 0) {
          const auto replicaId = group.value("replicaId", 0LL);
          const auto applicationId = group.value("applicationId", 0LL);
          meta["replicaId"] = replicaId;
          meta["applicationId"] = applicationId;
          std::string label = group.value("container", std::string("replica"));
          const auto appId = applicationId > 0
                                 ? applicationId
                                 : (replicaToApp.count(replicaId) ? replicaToApp[replicaId] : 0);
          if (appId > 0 && appNames.count(appId)) {
            label = appNames[appId];
            if (appReplicaCount[appId] > 1) {
              label += " #" + std::to_string(replicaId);
            }
          } else if (appId > 0 && !replicaToApp.count(replicaId)) {
            // Historical replica: still try the application id from the payload.
            if (auto app = ctx_.repos->findApplicationById(appId)) {
              label = app->name;
              appNames[appId] = label;
            }
          }
          meta["label"] = label;
        } else if (key == "docker") {
          meta["label"] = "Other Docker containers";
        } else {
          meta["label"] = "Host (outside containers)";
        }
        groupMeta[key] = std::move(meta);
      }
    }
  }

  nlohmann::json groups = nlohmann::json::array();
  for (auto& [_, meta] : groupMeta) {
    groups.push_back(std::move(meta));
  }
  std::sort(groups.begin(), groups.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    return a.value("key", "") < b.value("key", "");
  });

  nlohmann::json outBuckets = nlohmann::json::array();
  for (auto& [start, acc] : buckets) {
    if (acc.samples <= 0) {
      continue;
    }
    nlohmann::json values = nlohmann::json::object();
    double total = 0;
    for (const auto& [key, value] : acc.values) {
      const double avg = value.sum / static_cast<double>(acc.samples);
      values[key] = round2(avg);
      total += avg;
    }
    outBuckets.push_back({{"from", isoUtcMs(start)},
                          {"to", isoUtcMs(std::min(start + bucketMs, toMs))},
                          {"total", round2(total)},
                          {"values", std::move(values)}});
  }

  return {{"bucketSec", bucketSec}, {"groups", std::move(groups)}, {"buckets", std::move(outBuckets)}};
}

nlohmann::json ServerService::cpuAttributionProcesses(int64_t id, const std::string& from,
                                                      const std::string& to) {
  (void)get(id);
  const long long fromMs = parseIsoMs(from);
  const long long toMs = parseIsoMs(to);
  if (fromMs <= 0 || toMs <= 0 || toMs <= fromMs) {
    throw std::invalid_argument("Invalid from/to range");
  }

  std::unordered_map<int64_t, int64_t> replicaToApp;
  std::unordered_map<int64_t, std::string> appNames;
  std::unordered_map<int64_t, int> appReplicaCount;
  for (const auto& replica : ctx_.repos->listReplicasForServer(id)) {
    replicaToApp[replica.id] = replica.applicationId;
    appReplicaCount[replica.applicationId] += 1;
    if (!appNames.count(replica.applicationId)) {
      if (auto app = ctx_.repos->findApplicationById(replica.applicationId)) {
        appNames[replica.applicationId] = app->name;
      }
    }
  }

  struct ProcAcc {
    double cpu = 0;
    double pids = 0;
    std::string cmd;
  };
  struct GroupAcc {
    std::string kind;
    std::string label;
    int64_t applicationId = 0;
    int64_t replicaId = 0;
    double cpu = 0;
    double otherCpu = 0;
    int samples = 0;
    std::unordered_map<std::string, ProcAcc> procs;
  };
  std::unordered_map<std::string, GroupAcc> groups;
  int totalSamples = 0;

  for (const auto& row : ctx_.repos->listCpuAttribution(id, from, to)) {
    const auto payload =
        nlohmann::json::parse(row.payloadJson.empty() ? "{}" : row.payloadJson, nullptr, false);
    if (!payload.is_object() || !payload.contains("groups") || !payload.at("groups").is_array()) {
      continue;
    }
    totalSamples += 1;
    std::unordered_set<std::string> seen;
    for (const auto& group : payload.at("groups")) {
      if (!group.is_object()) {
        continue;
      }
      const auto key = detailGroupKey(group);
      auto& acc = groups[key];
      if (acc.kind.empty()) {
        acc.kind = group.value("kind", "host");
        acc.replicaId = group.value("replicaId", 0LL);
        acc.applicationId = group.value("applicationId", 0LL);
        if (acc.kind == "replica") {
          const auto appId = acc.applicationId > 0
                                 ? acc.applicationId
                                 : (replicaToApp.count(acc.replicaId) ? replicaToApp[acc.replicaId] : 0);
          if (appId > 0 && appNames.count(appId)) {
            acc.label = appNames[appId];
            if (appReplicaCount[appId] > 1) {
              acc.label += " #" + std::to_string(acc.replicaId);
            }
          } else if (appId > 0) {
            if (auto app = ctx_.repos->findApplicationById(appId)) {
              acc.label = app->name;
            } else {
              acc.label = group.value("container", std::string("replica"));
            }
          } else {
            acc.label = group.value("container", std::string("replica"));
          }
        } else if (acc.kind == "docker") {
          acc.label = group.value("container", std::string("docker"));
        } else {
          acc.label = "Host (outside containers)";
        }
      }
      acc.cpu += group.value("cpu", 0.0);
      acc.otherCpu += group.value("otherCpu", 0.0);
      seen.insert(key);
      if (group.contains("procs") && group.at("procs").is_array()) {
        for (const auto& proc : group.at("procs")) {
          if (!proc.is_object()) {
            continue;
          }
          const auto name = proc.value("name", std::string("unknown"));
          auto& p = acc.procs[name];
          p.cpu += proc.value("cpu", 0.0);
          p.pids += proc.value("pids", 0);
          if (p.cmd.empty()) {
            p.cmd = proc.value("cmd", "");
          }
        }
      }
    }
    for (const auto& key : seen) {
      groups[key].samples += 1;
    }
  }

  nlohmann::json out = nlohmann::json::array();
  const double denom = totalSamples > 0 ? static_cast<double>(totalSamples) : 1.0;
  for (auto& [key, acc] : groups) {
    nlohmann::json procs = nlohmann::json::array();
    for (auto& [name, proc] : acc.procs) {
      procs.push_back({{"name", name},
                       {"cpu", round2(proc.cpu / denom)},
                       {"pids", static_cast<int>(std::lround(proc.pids / denom))},
                       {"cmd", proc.cmd}});
    }
    std::sort(procs.begin(), procs.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
      return a.value("cpu", 0.0) > b.value("cpu", 0.0);
    });
    out.push_back({{"key", key},
                   {"kind", acc.kind},
                   {"label", acc.label},
                   {"applicationId", acc.applicationId},
                   {"replicaId", acc.replicaId},
                   {"cpu", round2(acc.cpu / denom)},
                   {"otherCpu", round2(acc.otherCpu / denom)},
                   {"procs", std::move(procs)}});
  }
  std::sort(out.begin(), out.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    return a.value("cpu", 0.0) > b.value("cpu", 0.0);
  });
  return {{"groups", std::move(out)}};
}

}  // namespace highcube::admin
