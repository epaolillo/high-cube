#include "services/services.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <vector>

namespace highcube::admin {
namespace {

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
  return sample;
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
  if (samples.size() >= 2) {
    const int intervals = static_cast<int>(samples.size()) - 1;
    const int buckets = std::min(60, intervals);
    struct Bucket {
      double cpu = 0;
      int cpuCount = 0;
      double memory = 0;
      int memoryCount = 0;
      double gpu = 0;
      int gpuCount = 0;
    };
    std::vector<Bucket> aggs(static_cast<std::size_t>(buckets));
    bool anyGpu = false;
    for (int i = 1; i < static_cast<int>(samples.size()); ++i) {
      const int bucket = (i - 1) * buckets / intervals;
      auto& agg = aggs[static_cast<std::size_t>(bucket)];
      const auto& current = samples[static_cast<std::size_t>(i)];
      const auto& previous = samples[static_cast<std::size_t>(i - 1)];
      if (previous.cpu.ok && current.cpu.ok) {
        agg.cpu += cpuPercent(previous.cpu, current.cpu);
        ++agg.cpuCount;
      }
      if (current.memoryTotalKb > 0) {
        agg.memory += percent(current.memoryTotalKb - current.memoryAvailableKb, current.memoryTotalKb);
        ++agg.memoryCount;
      }
      if (current.gpuPresent) {
        anyGpu = true;
        agg.gpu += current.gpuPercent;
        ++agg.gpuCount;
      }
    }
    for (const auto& agg : aggs) {
      cpuSeries.push_back(agg.cpuCount > 0 ? agg.cpu / agg.cpuCount : 0.0);
      memorySeries.push_back(agg.memoryCount > 0 ? agg.memory / agg.memoryCount : 0.0);
      if (anyGpu) {
        gpuSeries.push_back(agg.gpuCount > 0 ? agg.gpu / agg.gpuCount : 0.0);
      }
    }
  }
  return {{"series", {{"cpu", cpuSeries}, {"memory", memorySeries}, {"gpu", gpuSeries}}},
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
  const auto servers = ctx_.repos->listServersHostingUserApplications(userId);
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& server : servers) {
    rows.push_back({{"id", server.id},
                    {"name", server.name},
                    {"status", server.status},
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
  return serverToJson(*ctx_.repos->findServerById(id));
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
  const auto rows = ctx_.repos->listRecentServerMetrics(id, 2);
  if (rows.empty()) {
    return nlohmann::json::object();
  }
  // listRecentServerMetrics returns newest first.
  const auto current = parseSample(rows.front());
  std::optional<double> cpu;
  if (rows.size() >= 2) {
    const auto previous = parseSample(rows[1]);
    if (previous.cpu.ok && current.cpu.ok) {
      cpu = cpuPercent(previous.cpu, current.cpu);
    }
  }
  return latestJson(current, cpu);
}

nlohmann::json ServerService::overview(int64_t id) {
  const auto to = std::chrono::system_clock::now();
  const auto from = to - std::chrono::minutes(5);
  return {{"server", get(id)}, {"metrics", metrics(id, isoUtc(from), isoUtc(to))}};
}

}  // namespace highcube::admin
