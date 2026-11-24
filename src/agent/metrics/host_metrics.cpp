#include "metrics/host_metrics.hpp"

#include <nlohmann/json.hpp>

#include <sys/statvfs.h>
#include <sys/wait.h>

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace highcube::agent {

namespace {

// Runs a shell command and returns its stdout. exitCode is set to the process exit
// status (0 on success) or -1 when the process could not be started.
std::string runCapture(const char* command, int& exitCode) {
  std::string output;
  FILE* pipe = ::popen(command, "r");
  if (pipe == nullptr) {
    exitCode = -1;
    return output;
  }
  char buffer[512];
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
  const int status = ::pclose(pipe);
  exitCode = status == -1 ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  return output;
}

std::string trim(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

// Queries nvidia-smi for per-GPU utilization and memory. Returns a "gpu" JSON object
// with present=false when no NVIDIA GPU/driver is available.
nlohmann::json collectGpu() {
  int exitCode = -1;
  const std::string out = runCapture(
      "nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total,name "
      "--format=csv,noheader,nounits 2>/dev/null",
      exitCode);
  nlohmann::json gpus = nlohmann::json::array();
  if (exitCode == 0 && !out.empty()) {
    std::istringstream stream(out);
    std::string line;
    while (std::getline(stream, line)) {
      if (trim(line).empty()) {
        continue;
      }
      std::vector<std::string> fields;
      std::stringstream lineStream(line);
      std::string field;
      while (std::getline(lineStream, field, ',')) {
        fields.push_back(trim(field));
      }
      if (fields.size() < 3) {
        continue;
      }
      // Fields 4+ belong to the name, which may itself contain commas.
      std::string name;
      for (std::size_t i = 3; i < fields.size(); ++i) {
        if (!name.empty()) {
          name += ", ";
        }
        name += fields[i];
      }
      try {
        gpus.push_back({
            {"name", name},
            {"utilizationPercent", std::stod(fields[0])},
            {"memoryUsedMb", std::stod(fields[1])},
            {"memoryTotalMb", std::stod(fields[2])},
        });
      } catch (const std::exception&) {
        // Skip a malformed row (for example "[N/A]" values) instead of failing.
      }
    }
  }
  if (gpus.empty()) {
    return {{"present", false}};
  }
  return {{"present", true}, {"gpus", gpus}};
}

// Physical cores and hardware threads. They do not change while the agent runs.
nlohmann::json cpuInfo() {
  static const nlohmann::json info = [] {
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    std::string physicalId;
    int threads = 0;
    std::set<std::pair<std::string, std::string>> cores;
    while (std::getline(in, line)) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      const auto key = trim(line.substr(0, colon));
      const auto value = trim(line.substr(colon + 1));
      if (key == "processor") {
        ++threads;
        physicalId.clear();
      } else if (key == "physical id") {
        physicalId = value;
      } else if (key == "core id") {
        cores.insert({physicalId, value});
      }
    }
    // Some VMs and ARM boards omit core ids: count every thread as a core.
    const int coreCount = cores.empty() ? threads : static_cast<int>(cores.size());
    return nlohmann::json{{"cores", coreCount}, {"threads", threads}};
  }();
  return info;
}

// Byte counters of the host's real interfaces. Loopback, Docker bridges, veth pairs
// and tunnels are skipped: their traffic also crosses a physical interface.
nlohmann::json networkCounters() {
  std::ifstream in("/proc/net/dev");
  std::string line;
  unsigned long long rx = 0;
  unsigned long long tx = 0;
  while (std::getline(in, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const auto name = trim(line.substr(0, colon));
    const auto startsWith = [&name](const char* prefix) { return name.rfind(prefix, 0) == 0; };
    if (name == "lo" || startsWith("veth") || startsWith("docker") || startsWith("br-") ||
        startsWith("virbr") || startsWith("wg") || startsWith("hc-vpn") || startsWith("tun") ||
        startsWith("tailscale")) {
      continue;
    }
    std::istringstream fields(line.substr(colon + 1));
    unsigned long long values[9] = {};
    for (auto& value : values) {
      fields >> value;
    }
    rx += values[0];
    tx += values[8];
  }
  return {{"rxBytes", rx}, {"txBytes", tx}};
}

}  // namespace

std::string collectHostMetricsJson() {
  nlohmann::json j;
  {
    std::ifstream in("/proc/loadavg");
    double one = 0, five = 0, fifteen = 0;
    in >> one >> five >> fifteen;
    j["load"] = {{"1", one}, {"5", five}, {"15", fifteen}};
  }
  {
    std::ifstream in("/proc/meminfo");
    std::string key;
    long value = 0;
    std::string unit;
    long memTotal = 0, memAvailable = 0;
    while (in >> key >> value >> unit) {
      if (key == "MemTotal:") memTotal = value;
      if (key == "MemAvailable:") memAvailable = value;
    }
    j["memory"] = {{"totalKb", memTotal}, {"availableKb", memAvailable}};
  }
  {
    std::ifstream in("/proc/stat");
    std::string cpu;
    long user = 0, nice = 0, system = 0, idle = 0;
    in >> cpu >> user >> nice >> system >> idle;
    j["cpu"] = {{"user", user}, {"nice", nice}, {"system", system}, {"idle", idle}};
  }
  {
    struct statvfs fs {};
    if (statvfs("/", &fs) == 0) {
      const auto block = static_cast<unsigned long long>(fs.f_frsize);
      j["disk"] = {{"totalBytes", static_cast<unsigned long long>(fs.f_blocks) * block},
                   {"availableBytes", static_cast<unsigned long long>(fs.f_bavail) * block}};
    } else {
      j["disk"] = {{"present", false}};
    }
  }
  j["gpu"] = collectGpu();
  {
    // Also the clock for network rates: precise to 10 ms, unlike the storage timestamp.
    std::ifstream in("/proc/uptime");
    double uptime = 0;
    in >> uptime;
    j["uptimeSeconds"] = uptime;
  }
  j["cpuInfo"] = cpuInfo();
  j["network"] = networkCounters();
  return j.dump();
}

}  // namespace highcube::agent
