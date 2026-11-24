#include "metrics/process_snapshot.hpp"

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace highcube::agent {
namespace {

std::string readAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return "";
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool isHexId(const std::string& value, std::size_t minLen = 12) {
  if (value.size() < minLen) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isxdigit(ch) && !std::isupper(ch);
  });
}

// Full and short container id -> name from `docker ps`. Refreshed at most once a minute.
std::unordered_map<std::string, std::string> dockerNameMap() {
  static std::mutex mutex;
  static std::unordered_map<std::string, std::string> cache;
  static std::chrono::steady_clock::time_point loadedAt{};
  std::lock_guard lock(mutex);
  const auto now = std::chrono::steady_clock::now();
  if (!cache.empty() && now - loadedAt < std::chrono::seconds(45)) {
    return cache;
  }
  cache.clear();
  FILE* pipe = ::popen("docker ps --no-trunc --format '{{.ID}}|{{.Names}}' 2>/dev/null", "r");
  if (!pipe) {
    return cache;
  }
  char buffer[512];
  std::string out;
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    out += buffer;
  }
  ::pclose(pipe);
  std::istringstream in(out);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    const auto sep = line.find('|');
    if (sep == std::string::npos || sep == 0) {
      continue;
    }
    const auto id = line.substr(0, sep);
    auto name = line.substr(sep + 1);
    if (!name.empty() && name.front() == '/') {
      name.erase(name.begin());
    }
    if (id.empty() || name.empty()) {
      continue;
    }
    cache[id] = name;
    if (id.size() >= 12) {
      cache[id.substr(0, 12)] = name;
    }
  }
  loadedAt = now;
  return cache;
}

std::string cgroupDirForId(const std::string& id) {
  const std::vector<std::string> candidates = {
      "/sys/fs/cgroup/system.slice/docker-" + id + ".scope",
      "/sys/fs/cgroup/docker/" + id,
      "/sys/fs/cgroup/cpu,cpuacct/docker/" + id,
      "/sys/fs/cgroup/cpuacct/docker/" + id,
      "/sys/fs/cgroup/cpu,cpuacct/system.slice/docker-" + id + ".scope",
      "/sys/fs/cgroup/cpuacct/system.slice/docker-" + id + ".scope",
  };
  for (const auto& dir : candidates) {
    std::error_code ec;
    if (fs::exists(dir, ec)) {
      return dir;
    }
  }
  return "";
}

struct StatFields {
  bool ok = false;
  std::string name;
  unsigned long long ticks = 0;
  unsigned long long start = 0;
  long long rssPages = 0;
};

StatFields readStat(int pid) {
  StatFields fields;
  const auto text = readAll("/proc/" + std::to_string(pid) + "/stat");
  const auto open = text.find('(');
  const auto close = text.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close < open) {
    return fields;
  }
  fields.name = text.substr(open + 1, close - open - 1);
  std::istringstream rest(text.substr(close + 1));
  std::vector<std::string> tokens;
  std::string token;
  while (rest >> token) {
    tokens.push_back(token);
  }
  // tokens[0] is field 3 (state): field N lives at tokens[N - 3].
  if (tokens.size() < 22) {
    return fields;
  }
  try {
    fields.ticks = std::stoull(tokens[11]) + std::stoull(tokens[12]);
    fields.start = std::stoull(tokens[19]);
    fields.rssPages = std::stoll(tokens[21]);
    fields.ok = true;
  } catch (const std::exception&) {
    fields.ok = false;
  }
  return fields;
}

std::string userName(uid_t uid) {
  static std::mutex mutex;
  static std::unordered_map<uid_t, std::string> cache;
  std::lock_guard lock(mutex);
  const auto it = cache.find(uid);
  if (it != cache.end()) {
    return it->second;
  }
  passwd pwd{};
  passwd* found = nullptr;
  char buffer[4096];
  std::string name = std::to_string(uid);
  if (::getpwuid_r(uid, &pwd, buffer, sizeof(buffer), &found) == 0 && found) {
    name = found->pw_name;
  }
  cache[uid] = name;
  return name;
}

// Extract a Docker/containerd/podman id from a cgroup file body.
std::string extractContainerId(const std::string& text) {
  // Prefer well-known prefixes so we do not grab unrelated 64-hex hashes.
  const char* markers[] = {"docker-", "cri-containerd-", "libpod-", "crio-", "/docker/", "/containers/"};
  for (const char* marker : markers) {
    std::size_t pos = 0;
    const std::string needle = marker;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
      const std::size_t start = pos + needle.size();
      std::size_t end = start;
      while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end])) &&
             !std::isupper(static_cast<unsigned char>(text[end]))) {
        ++end;
      }
      const auto len = end - start;
      if (len == 64 || (len >= 12 && len <= 64 && (end >= text.size() || text[end] == '.' ||
                                                  text[end] == '/' || text[end] == '\n'))) {
        return text.substr(start, len == 64 ? 64 : len);
      }
      pos = start;
    }
  }
  // Fallback: any standalone 64-hex run.
  int run = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (std::isxdigit(static_cast<unsigned char>(ch)) && !std::isupper(static_cast<unsigned char>(ch))) {
      ++run;
      if (run == 64) {
        const bool endOfRun = i + 1 >= text.size() || !std::isxdigit(static_cast<unsigned char>(text[i + 1]));
        if (endOfRun) {
          return text.substr(i - 63, 64);
        }
      }
    } else {
      run = 0;
    }
  }
  return "";
}

// Current CPU usage in microseconds for a container id. 0 when unavailable.
unsigned long long cgroupUsageUsecForId(const std::string& id) {
  if (id.empty()) {
    return 0;
  }
  const auto dir = cgroupDirForId(id);
  if (dir.empty()) {
    return 0;
  }
  {
    const auto text = readAll(dir + "/cpu.stat");
    std::istringstream in(text);
    std::string key;
    unsigned long long value = 0;
    while (in >> key >> value) {
      if (key == "usage_usec") {
        return value;
      }
    }
  }
  {
    const auto text = readAll(dir + "/cpuacct.usage");
    if (!text.empty()) {
      try {
        return std::stoull(text) / 1000ULL;
      } catch (const std::exception&) {
        return 0;
      }
    }
  }
  return 0;
}

double round2(double value) {
  return std::round(value * 100.0) / 100.0;
}

struct ProcAgg {
  double cpu = 0;
  int pids = 0;
  std::string cmd;
  int samplePid = 0;
};

struct GroupAgg {
  std::string kind;  // replica | docker | host
  std::string containerId;
  std::string container;
  int64_t applicationId = 0;
  int64_t replicaId = 0;
  double cpu = 0;       // share of the machine (from cgroup when possible)
  double procCpu = 0;   // sum of observed processes
  bool fromCgroup = false;
  std::unordered_map<std::string, ProcAgg> byName;
};

nlohmann::json procsToJson(std::unordered_map<std::string, ProcAgg>& byName, double otherCpu) {
  std::vector<std::pair<std::string, ProcAgg>> ranked;
  ranked.reserve(byName.size());
  for (auto& [name, agg] : byName) {
    ranked.emplace_back(name, std::move(agg));
  }
  std::sort(ranked.begin(), ranked.end(),
            [](const auto& a, const auto& b) { return a.second.cpu > b.second.cpu; });

  nlohmann::json procs = nlohmann::json::array();
  double restCpu = 0;
  int restPids = 0;
  constexpr std::size_t kTop = 10;
  for (std::size_t i = 0; i < ranked.size(); ++i) {
    if (i < kTop) {
      auto& agg = ranked[i].second;
      if (!agg.cmd.empty()) {
        // already filled
      } else if (agg.samplePid > 0) {
        ProcessInfo tmp;
        tmp.pid = agg.samplePid;
        tmp.name = ranked[i].first;
        enrichProcess(tmp);
        agg.cmd = tmp.cmd;
      }
      if (agg.cmd.size() > 120) {
        agg.cmd.resize(120);
      }
      procs.push_back({{"name", ranked[i].first},
                       {"cpu", round2(agg.cpu)},
                       {"pids", agg.pids},
                       {"cmd", agg.cmd}});
    } else {
      restCpu += ranked[i].second.cpu;
      restPids += ranked[i].second.pids;
    }
  }
  restCpu += otherCpu;
  if (restCpu > 0.005 || restPids > 0) {
    procs.push_back({{"name", "other"},
                     {"cpu", round2(restCpu)},
                     {"pids", restPids},
                     {"cmd", ""}});
  }
  return procs;
}

}  // namespace

bool parseReplicaIdFromName(const std::string& name, int64_t& replicaId) {
  int64_t applicationId = 0;
  return parseHcContainerName(name, applicationId, replicaId);
}

bool parseHcContainerName(const std::string& name, int64_t& applicationId, int64_t& replicaId) {
  std::string bare = name;
  if (!bare.empty() && bare.front() == '/') {
    bare.erase(bare.begin());
  }
  if (bare.rfind("hc-", 0) != 0) {
    return false;
  }
  const auto lastDash = bare.rfind('-');
  if (lastDash == std::string::npos || lastDash <= 2) {
    return false;
  }
  const auto midDash = bare.rfind('-', lastDash - 1);
  if (midDash == std::string::npos || midDash < 2) {
    return false;
  }
  try {
    std::size_t appIdx = 0;
    std::size_t repIdx = 0;
    applicationId = std::stoll(bare.substr(midDash + 1, lastDash - midDash - 1), &appIdx);
    replicaId = std::stoll(bare.substr(lastDash + 1), &repIdx);
    return appIdx == lastDash - midDash - 1 && repIdx == bare.size() - lastDash - 1 &&
           applicationId > 0 && replicaId > 0;
  } catch (const std::exception&) {
    return false;
  }
}

unsigned long long processStartTicks(int pid) {
  const auto fields = readStat(pid);
  return fields.ok ? fields.start : 0;
}

std::string containerIdForPid(int pid) {
  return extractContainerId(readAll("/proc/" + std::to_string(pid) + "/cgroup"));
}

std::string containerNameForId(const std::string& id) {
  if (id.empty()) {
    return "";
  }
  static std::mutex mutex;
  static std::unordered_map<std::string, std::string> cache;
  {
    std::lock_guard lock(mutex);
    const auto it = cache.find(id);
    if (it != cache.end() && !isHexId(it->second)) {
      return it->second;
    }
  }
  std::string name;
  // Prefer docker ps: config.v2.json is missing on some containerd-backed installs.
  const auto names = dockerNameMap();
  auto it = names.find(id);
  if (it == names.end() && id.size() >= 12) {
    it = names.find(id.substr(0, 12));
  }
  if (it != names.end()) {
    name = it->second;
  }
  if (name.empty() || isHexId(name)) {
    std::string configPath = "/var/lib/docker/containers/" + id + "/config.v2.json";
    if (id.size() != 64) {
      // Short ids: scan the containers dir for a matching prefix.
      std::error_code ec;
      for (const auto& entry : fs::directory_iterator("/var/lib/docker/containers", ec)) {
        const auto full = entry.path().filename().string();
        if (full.rfind(id, 0) == 0 && full.size() == 64) {
          configPath = entry.path().string() + "/config.v2.json";
          break;
        }
      }
    }
    const auto config = nlohmann::json::parse(readAll(configPath), nullptr, false);
    if (config.is_object() && config.contains("Name") && config.at("Name").is_string()) {
      name = config.at("Name").get<std::string>();
      if (!name.empty() && name.front() == '/') {
        name.erase(name.begin());
      }
    }
  }
  if (name.empty()) {
    name = id.substr(0, std::min<std::size_t>(12, id.size()));
  }
  std::lock_guard lock(mutex);
  if (cache.size() > 2000) {
    cache.clear();
  }
  cache[id] = name;
  return name;
}

void enrichProcess(ProcessInfo& process) {
  const auto base = "/proc/" + std::to_string(process.pid);
  struct stat st {};
  if (::stat(base.c_str(), &st) == 0) {
    process.user = userName(st.st_uid);
  }
  auto cmd = readAll(base + "/cmdline");
  std::replace(cmd.begin(), cmd.end(), '\0', ' ');
  while (!cmd.empty() && cmd.back() == ' ') {
    cmd.pop_back();
  }
  if (cmd.size() > 200) {
    cmd.resize(200);
  }
  process.cmd = cmd.empty() ? "[" + process.name + "]" : cmd;
  std::istringstream status(readAll(base + "/status"));
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmSwap:", 0) == 0) {
      try {
        process.swapKb = std::stoll(line.substr(7));
      } catch (const std::exception&) {
      }
    }
  }
  process.container = containerNameForId(containerIdForPid(process.pid));
}

nlohmann::json processToJson(const ProcessInfo& process) {
  return {{"pid", process.pid},
          {"start", process.startTicks},
          {"user", process.user},
          {"name", process.name},
          {"cmd", process.cmd},
          {"container", process.container},
          {"cpu", std::round(process.cpuPercent * 100.0) / 100.0},
          {"rssKb", process.rssKb},
          {"swapKb", process.swapKb}};
}

std::vector<ProcessInfo> ProcessSampler::sample() {
  // Aggregate jiffies: user nice system idle iowait irq softirq steal.
  unsigned long long parts[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  int cores = 0;
  {
    std::istringstream stat(readAll("/proc/stat"));
    std::string line;
    while (std::getline(stat, line)) {
      if (line.rfind("cpu ", 0) == 0) {
        std::istringstream fields(line.substr(4));
        for (auto& part : parts) {
          fields >> part;
        }
      } else if (line.rfind("cpu", 0) == 0) {
        ++cores;
      }
    }
  }
  cores_ = std::max(1, cores);
  unsigned long long total = 0;
  for (const auto part : parts) {
    total += part;
  }
  const unsigned long long grouped[5] = {parts[0] + parts[1], parts[2] + parts[5] + parts[6], parts[4],
                                         parts[7], parts[3]};
  const auto deltaTotal = total > previousTotal_ ? total - previousTotal_ : 0;
  lastDeltaTotal_ = deltaTotal;
  if (previousTotal_ > 0 && deltaTotal > 0) {
    auto share = [&](int index) {
      const auto delta = grouped[index] >= previousParts_[index] ? grouped[index] - previousParts_[index] : 0;
      return 100.0 * static_cast<double>(delta) / static_cast<double>(deltaTotal);
    };
    breakdown_ = {share(0), share(1), share(2), share(3), share(4)};
  }

  static const long pageKb = std::max(1L, ::sysconf(_SC_PAGESIZE) / 1024);
  std::vector<ProcessInfo> processes;
  std::unordered_map<int, Previous> next;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/proc", ec)) {
    const auto name = entry.path().filename().string();
    if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) {
      continue;
    }
    const int pid = std::stoi(name);
    const auto fields = readStat(pid);
    if (!fields.ok) {
      continue;
    }
    ProcessInfo process;
    process.pid = pid;
    process.startTicks = fields.start;
    process.name = fields.name;
    process.rssKb = fields.rssPages * pageKb;
    const auto prev = previous_.find(pid);
    if (prev != previous_.end() && prev->second.start == fields.start && previousTotal_ > 0 &&
        deltaTotal > 0 && fields.ticks >= prev->second.ticks) {
      process.cpuPercent =
          100.0 * static_cast<double>(fields.ticks - prev->second.ticks) / static_cast<double>(deltaTotal);
    }
    next[pid] = {fields.start, fields.ticks};
    processes.push_back(std::move(process));
  }
  previous_ = std::move(next);
  previousTotal_ = total;
  for (int i = 0; i < 5; ++i) {
    previousParts_[i] = grouped[i];
  }
  return processes;
}

std::string collectProcessSnapshotJson(ProcessSampler& sampler) {
  auto processes = sampler.sample();
  return collectProcessSnapshotJson(sampler, processes);
}

std::string collectProcessSnapshotJson(ProcessSampler& sampler, std::vector<ProcessInfo>& processes) {
  std::vector<std::size_t> byCpu(processes.size());
  for (std::size_t i = 0; i < processes.size(); ++i) {
    byCpu[i] = i;
  }
  auto byRss = byCpu;
  std::sort(byCpu.begin(), byCpu.end(),
            [&](std::size_t a, std::size_t b) { return processes[a].cpuPercent > processes[b].cpuPercent; });
  std::sort(byRss.begin(), byRss.end(),
            [&](std::size_t a, std::size_t b) { return processes[a].rssKb > processes[b].rssKb; });
  std::unordered_set<std::size_t> chosen;
  for (std::size_t i = 0; i < std::min<std::size_t>(15, byCpu.size()); ++i) {
    if (processes[byCpu[i]].cpuPercent > 0) {
      chosen.insert(byCpu[i]);
    }
  }
  for (std::size_t i = 0; i < std::min<std::size_t>(15, byRss.size()); ++i) {
    chosen.insert(byRss[i]);
  }
  nlohmann::json procs = nlohmann::json::array();
  for (const auto index : chosen) {
    auto& process = processes[index];
    enrichProcess(process);
    procs.push_back(processToJson(process));
  }

  nlohmann::json j;
  const auto cpu = sampler.breakdown();
  j["cores"] = sampler.cores();
  j["cpu"] = {{"user", cpu.user}, {"system", cpu.system}, {"iowait", cpu.iowait},
              {"steal", cpu.steal}, {"idle", cpu.idle}};
  {
    std::istringstream in(readAll("/proc/loadavg"));
    double one = 0, five = 0, fifteen = 0;
    in >> one >> five >> fifteen;
    j["load"] = {one, five, fifteen};
  }
  {
    std::istringstream in(readAll("/proc/meminfo"));
    std::string key;
    long long value = 0;
    nlohmann::json mem = nlohmann::json::object();
    std::string line;
    while (std::getline(in, line)) {
      std::istringstream fields(line);
      fields >> key >> value;
      if (key == "MemTotal:") mem["totalKb"] = value;
      if (key == "MemAvailable:") mem["availableKb"] = value;
      if (key == "Cached:") mem["cachedKb"] = value;
      if (key == "SwapTotal:") mem["swapTotalKb"] = value;
      if (key == "SwapFree:") mem["swapFreeKb"] = value;
    }
    j["mem"] = mem;
  }
  j["procs"] = procs;
  return j.dump();
}

std::string collectCpuAttributionJson(
    ProcessSampler& sampler, std::vector<ProcessInfo>& processes,
    std::unordered_map<std::string, unsigned long long>& previousCgroupUsec) {
  const auto deltaTotal = sampler.lastDeltaTotal();
  const long clkTck = std::max(1L, ::sysconf(_SC_CLK_TCK));
  // Sum the non-idle shares. Using 100-idle turns the default (all-zero) breakdown into a
  // fake 100% host bar on the first sample before deltas exist.
  const auto parts = sampler.breakdown();
  const double busy = std::max(0.0, parts.user + parts.system + parts.iowait + parts.steal);
  if (deltaTotal == 0) {
    nlohmann::json j;
    j["cores"] = sampler.cores();
    j["intervalSec"] = 60;
    j["busy"] = 0;
    j["groups"] = nlohmann::json::array();
    return j.dump();
  }

  // Warm the docker name cache once per sample so hc-* names resolve even without config.v2.json.
  const auto names = dockerNameMap();

  auto canonicalId = [&](const std::string& id) -> std::string {
    if (id.size() == 64) {
      return id;
    }
    if (id.size() >= 12) {
      for (const auto& [full, _] : names) {
        if (full.size() == 64 && full.rfind(id, 0) == 0) {
          return full;
        }
      }
    }
    return id;
  };

  // Keyed by container id, or "" for host processes.
  std::unordered_map<std::string, GroupAgg> groups;
  std::unordered_map<std::string, unsigned long long> nextCgroup;

  for (auto& process : processes) {
    if (process.cpuPercent <= 0) {
      continue;
    }
    const auto containerId = canonicalId(containerIdForPid(process.pid));
    auto& group = groups[containerId];
    if (group.containerId.empty() && !containerId.empty()) {
      group.containerId = containerId;
      group.container = containerNameForId(containerId);
      int64_t applicationId = 0;
      int64_t replicaId = 0;
      if (parseHcContainerName(group.container, applicationId, replicaId)) {
        group.kind = "replica";
        group.applicationId = applicationId;
        group.replicaId = replicaId;
      } else {
        group.kind = "docker";
      }
    } else if (containerId.empty() && group.kind.empty()) {
      group.kind = "host";
    }
    group.procCpu += process.cpuPercent;
    auto& agg = group.byName[process.name];
    agg.cpu += process.cpuPercent;
    agg.pids += 1;
    if (agg.samplePid == 0) {
      agg.samplePid = process.pid;
    }
  }

  // Prefer cgroup totals so short-lived processes inside a container still count.
  for (auto& [containerId, group] : groups) {
    if (containerId.empty()) {
      continue;
    }
    const auto usage = cgroupUsageUsecForId(containerId);
    if (usage == 0) {
      continue;
    }
    nextCgroup[containerId] = usage;
    const auto prev = previousCgroupUsec.find(containerId);
    if (prev == previousCgroupUsec.end() || usage < prev->second) {
      continue;
    }
    const auto deltaUsec = usage - prev->second;
    const double windowUsec =
        static_cast<double>(deltaTotal) * 1'000'000.0 / static_cast<double>(clkTck);
    if (windowUsec <= 0) {
      continue;
    }
    group.cpu = 100.0 * static_cast<double>(deltaUsec) / windowUsec;
    group.fromCgroup = true;
  }
  previousCgroupUsec = std::move(nextCgroup);

  double containerSum = 0;
  for (auto& [containerId, group] : groups) {
    if (containerId.empty()) {
      continue;
    }
    if (!group.fromCgroup) {
      group.cpu = group.procCpu;
    }
    containerSum += group.cpu;
  }

  auto& host = groups[""];
  host.kind = "host";
  host.cpu = std::max(0.0, busy - containerSum);
  // Keep observed host process sum for the pie chart; otherCpu covers the gap.
  const double hostProcCpu = host.procCpu;

  nlohmann::json outGroups = nlohmann::json::array();
  for (auto& [containerId, group] : groups) {
    if (group.kind.empty()) {
      continue;
    }
    // Skip empty host and empty docker leftovers.
    if (group.cpu < 0.005 && group.procCpu < 0.005 && group.byName.empty()) {
      continue;
    }
    const double otherCpu =
        containerId.empty() ? std::max(0.0, group.cpu - hostProcCpu)
                            : std::max(0.0, group.cpu - group.procCpu);
    nlohmann::json item = {{"kind", group.kind},
                           {"cpu", round2(group.cpu)},
                           {"otherCpu", round2(otherCpu)},
                           {"procs", procsToJson(group.byName, 0)}};
    // Fold short-lived remainder into the "other" process slice when present.
    if (otherCpu > 0.005) {
      auto& procs = item["procs"];
      bool found = false;
      for (auto& proc : procs) {
        if (proc.value("name", "") == "other") {
          proc["cpu"] = round2(proc.value("cpu", 0.0) + otherCpu);
          found = true;
          break;
        }
      }
      if (!found) {
        procs.push_back({{"name", "other"}, {"cpu", round2(otherCpu)}, {"pids", 0}, {"cmd", ""}});
      }
      // Keep otherCpu as an explicit field for the UI label.
    }
    if (!containerId.empty()) {
      item["container"] = group.container;
    }
    if (group.kind == "replica") {
      item["replicaId"] = group.replicaId;
      item["applicationId"] = group.applicationId;
    }
    outGroups.push_back(std::move(item));
  }

  std::sort(outGroups.begin(), outGroups.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    return a.value("cpu", 0.0) > b.value("cpu", 0.0);
  });

  nlohmann::json j;
  j["cores"] = sampler.cores();
  j["intervalSec"] = 60;
  j["busy"] = round2(busy);
  j["groups"] = std::move(outGroups);
  return j.dump();
}

}  // namespace highcube::agent
