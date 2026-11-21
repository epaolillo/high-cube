#include "metrics/process_snapshot.hpp"

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

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

}  // namespace

unsigned long long processStartTicks(int pid) {
  const auto fields = readStat(pid);
  return fields.ok ? fields.start : 0;
}

std::string containerIdForPid(int pid) {
  const auto text = readAll("/proc/" + std::to_string(pid) + "/cgroup");
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

std::string containerNameForId(const std::string& id) {
  if (id.empty()) {
    return "";
  }
  static std::mutex mutex;
  static std::unordered_map<std::string, std::string> cache;
  {
    std::lock_guard lock(mutex);
    const auto it = cache.find(id);
    if (it != cache.end()) {
      return it->second;
    }
  }
  std::string name = id.substr(0, 12);
  const auto config = nlohmann::json::parse(
      readAll("/var/lib/docker/containers/" + id + "/config.v2.json"), nullptr, false);
  if (config.is_object() && config.contains("Name") && config.at("Name").is_string()) {
    name = config.at("Name").get<std::string>();
    if (!name.empty() && name.front() == '/') {
      name.erase(name.begin());
    }
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

}  // namespace highcube::agent
