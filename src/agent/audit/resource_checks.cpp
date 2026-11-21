#include "audit/audit_engine.hpp"
#include "audit/command_runner.hpp"
#include "metrics/process_snapshot.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <map>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

namespace highcube::agent::audit {
namespace {

struct Window {
  long long fromMs = 0;
  long long toMs = 0;
};

Window windowFrom(const json& params) {
  Window window;
  window.fromMs = params.value("from", 0LL);
  window.toMs = params.value("to", 0LL);
  if (window.toMs <= window.fromMs) {
    throw std::invalid_argument("Invalid time window");
  }
  return window;
}

long long toLong(const std::string& value) {
  try {
    return std::stoll(value);
  } catch (const std::exception&) {
    return 0;
  }
}

double toDouble(const std::string& value) {
  try {
    return std::stod(value);
  } catch (const std::exception&) {
    return 0;
  }
}

std::string localStamp(long long ms, const char* format) {
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  localtime_r(&t, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, format);
  return oss.str();
}

// ---------------------------------------------------------------- atop
json checkAtopWindow(const json& params, bool memory) {
  const auto window = windowFrom(params);
  if (!commandExists("atop")) {
    return skipped("res_atop_unavailable");
  }
  const int cores = std::max(1L, ::sysconf(_SC_NPROCESSORS_ONLN));
  struct Agg {
    double sum = 0;
    double peak = 0;
    int samples = 0;
    int pid = 0;
  };
  std::unordered_map<std::string, Agg> byName;
  std::map<long long, bool> epochs;
  bool anyFile = false;
  // One atop log per local day; walk the days the window touches.
  std::time_t dayStart = static_cast<std::time_t>(window.fromMs / 1000);
  {
    std::tm tm{};
    localtime_r(&dayStart, &tm);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    dayStart = std::mktime(&tm);
  }
  for (int guard = 0; guard < 8 && static_cast<long long>(dayStart) * 1000 <= window.toMs; ++guard) {
    std::tm next{};
    localtime_r(&dayStart, &next);
    next.tm_mday += 1;
    next.tm_isdst = -1;
    const std::time_t nextStart = std::mktime(&next);
    const long long dayMs = static_cast<long long>(dayStart) * 1000;
    const auto file = "/var/log/atop/atop_" + localStamp(dayMs, "%Y%m%d");
    const long long beginMs = std::max(window.fromMs, dayMs);
    const long long endMs = std::min(window.toMs, static_cast<long long>(nextStart) * 1000 - 1000);
    dayStart = nextStart;
    std::error_code ec;
    if (!fs::exists(file, ec)) {
      continue;
    }
    anyFile = true;
    const auto out = runCommand({"atop", "-r", file, "-b", localStamp(beginMs, "%H:%M"), "-e",
                                 localStamp(endMs + 60000, "%H:%M"), "-P", memory ? "PRM" : "PRC"},
                                120, true, 32 * 1024 * 1024);
    for (const auto& line : splitLines(out.out)) {
      if (line.rfind(memory ? "PRM " : "PRC ", 0) != 0) continue;
      const auto open = line.find('(');
      const auto close = line.rfind(')');
      if (open == std::string::npos || close == std::string::npos || close < open) continue;
      std::istringstream head(line.substr(0, open));
      std::string label, host, epoch, date, time, interval, pid;
      head >> label >> host >> epoch >> date >> time >> interval >> pid;
      const auto name = line.substr(open + 1, close - open - 1);
      std::istringstream tail(line.substr(close + 1));
      std::vector<std::string> fields;
      std::string field;
      while (tail >> field) fields.push_back(field);
      const long long epochSec = toLong(epoch);
      if (epochSec * 1000 < window.fromMs || epochSec * 1000 > window.toMs) continue;
      double value = 0;
      if (memory) {
        // PRM: state pagesize vsize(KB) rsize(KB) ...
        if (fields.size() < 4) continue;
        value = toDouble(fields[3]);
      } else {
        // PRC: state ticks/sec utime stime ...
        if (fields.size() < 4) continue;
        const double ticks = std::max(1.0, toDouble(fields[1]));
        const double seconds = std::max(1.0, toDouble(interval));
        value = 100.0 * (toDouble(fields[2]) + toDouble(fields[3])) / (ticks * seconds * cores);
      }
      epochs[epochSec] = true;
      auto& agg = byName[name];
      agg.sum += value;
      agg.peak = std::max(agg.peak, value);
      ++agg.samples;
      agg.pid = static_cast<int>(toLong(pid));
    }
  }
  if (!anyFile) {
    return skipped("res_atop_nologs");
  }
  if (byName.empty()) {
    return skipped("res_atop_empty");
  }
  const double sampleCount = std::max<std::size_t>(1, epochs.size());
  std::vector<std::pair<std::string, Agg>> sorted(byName.begin(), byName.end());
  std::sort(sorted.begin(), sorted.end(), [&](const auto& a, const auto& b) {
    return memory ? a.second.peak > b.second.peak : a.second.sum > b.second.sum;
  });
  json rows = json::array();
  json raw = json::array();
  for (const auto& [name, agg] : sorted) {
    if (rows.size() >= 15) break;
    if (memory) {
      const auto avgBytes = static_cast<long long>(agg.sum / agg.samples * 1024);
      const auto peakBytes = static_cast<long long>(agg.peak * 1024);
      rows.push_back({{"process", name}, {"avgRss", avgBytes}, {"peakRss", peakBytes}, {"samples", agg.samples}});
      raw.push_back({{"name", name}, {"avg", avgBytes}, {"peak", peakBytes}});
    } else {
      const double avg = agg.sum / sampleCount;
      rows.push_back({{"process", name}, {"avgCpu", avg}, {"peakCpu", agg.peak}, {"samples", agg.samples}});
      raw.push_back({{"name", name}, {"avg", avg}, {"peak", agg.peak}});
    }
  }
  const auto& top = raw.front();
  return {{"severity", "info"},
          {"conclusion", memory ? message("res_atop_top_ram", {{"name", top["name"]},
                                                                {"avgBytes", top["avg"]},
                                                                {"peakBytes", top["peak"]},
                                                                {"samples", static_cast<int>(sampleCount)}})
                                : message("res_atop_top_cpu", {{"name", top["name"]},
                                                                {"avg", top["avg"]},
                                                                {"peak", top["peak"]},
                                                                {"samples", static_cast<int>(sampleCount)}})},
          {"blocks", {memory ? tableBlock("atopProcesses",
                                          {{"process", "text"}, {"avgRss", "bytes"}, {"peakRss", "bytes"}, {"samples", "number"}},
                                          rows)
                             : tableBlock("atopProcesses",
                                          {{"process", "text"}, {"avgCpu", "percent"}, {"peakCpu", "percent"}, {"samples", "number"}},
                                          rows)}},
          {"raw", raw}};
}

// ---------------------------------------------------------------- journal events
json checkSystemEvents(const json& params) {
  const auto window = windowFrom(params);
  if (!commandExists("journalctl")) {
    return skipped("res_journal_unavailable");
  }
  const auto since = "@" + std::to_string(window.fromMs / 1000);
  const auto until = "@" + std::to_string(window.toMs / 1000);
  const auto out = runCommand({"journalctl", "--since", since, "--until", until, "-o", "short-unix", "-q",
                               "--no-pager", "-n", "20000"},
                              90, true, 16 * 1024 * 1024);
  struct Event {
    std::string source;
    std::string event;
    int count = 0;
    long long first = 0;
  };
  std::map<std::string, Event> events;
  int cron = 0, units = 0, dockerLines = 0, kernelLines = 0, total = 0;
  static const std::regex cronCmd(R"(CMD \((.*)\))");
  for (const auto& line : splitLines(out.out)) {
    std::istringstream fields(line);
    std::string stamp, host, ident;
    if (!(fields >> stamp >> host >> ident)) continue;
    std::string text;
    std::getline(fields, text);
    text = trim(text);
    ++total;
    const long long at = static_cast<long long>(toDouble(stamp) * 1000.0);
    const auto bracket = ident.find('[');
    std::string program = ident.substr(0, bracket == std::string::npos ? ident.size() : bracket);
    if (!program.empty() && program.back() == ':') program.pop_back();
    std::string key;
    Event event;
    std::smatch match;
    if ((program == "CRON" || program == "cron" || program == "crond") && std::regex_search(text, match, cronCmd)) {
      ++cron;
      event = {"cron", match[1].str().substr(0, 160)};
    } else if (program == "systemd" && (text.rfind("Started ", 0) == 0 || text.rfind("Starting ", 0) == 0)) {
      if (text.rfind("Starting ", 0) == 0) continue;
      ++units;
      auto unit = text.substr(8);
      if (!unit.empty() && unit.back() == '.') unit.pop_back();
      event = {"systemd", unit.substr(0, 160)};
    } else if (program == "dockerd" || program == "containerd") {
      ++dockerLines;
      continue;
    } else if (program == "kernel") {
      ++kernelLines;
      event = {"kernel", text.substr(0, 160)};
    } else {
      continue;
    }
    key = event.source + "|" + event.event;
    auto& slot = events[key];
    if (slot.count == 0) {
      slot = event;
      slot.first = at;
    }
    ++slot.count;
  }
  int warnings = 0;
  std::map<std::string, int> warningSources;
  const auto warnOut = runCommand({"journalctl", "--since", since, "--until", until, "-p", "0..4", "-o", "short-unix",
                                   "-q", "--no-pager", "-n", "5000"},
                                  60, true, 8 * 1024 * 1024);
  for (const auto& line : splitLines(warnOut.out)) {
    std::istringstream fields(line);
    std::string stamp, host, ident;
    if (!(fields >> stamp >> host >> ident)) continue;
    ++warnings;
    const auto bracket = ident.find('[');
    auto program = ident.substr(0, bracket == std::string::npos ? ident.size() : bracket);
    if (!program.empty() && program.back() == ':') program.pop_back();
    ++warningSources[program];
  }
  std::vector<Event> sorted;
  for (const auto& [key, event] : events) sorted.push_back(event);
  std::sort(sorted.begin(), sorted.end(), [](const Event& a, const Event& b) { return a.count > b.count; });
  json rows = json::array();
  for (const auto& event : sorted) {
    if (rows.size() >= 30) break;
    rows.push_back({{"source", event.source}, {"event", event.event}, {"count", event.count}, {"firstAt", event.first}});
  }
  json bars = json::array();
  std::vector<std::pair<std::string, int>> sources(warningSources.begin(), warningSources.end());
  std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  for (const auto& [program, count] : sources) {
    if (bars.size() >= 8) break;
    bars.push_back({{"label", program}, {"value", count}});
  }
  json findings = json::array();
  if (!sorted.empty() && sorted.front().source == "cron") {
    findings.push_back(message("res_events_top_cron", {{"command", sorted.front().event}, {"count", sorted.front().count}}));
  }
  if (dockerLines > 0) findings.push_back(message("res_events_docker", {{"count", dockerLines}}));
  if (!sources.empty()) {
    findings.push_back(message("res_events_noisy", {{"source", sources.front().first}, {"count", sources.front().second}}));
  }
  json blocks = json::array({tableBlock("windowEvents",
                                        {{"source", "text"}, {"event", "text"}, {"count", "number"}, {"firstAt", "date"}},
                                        rows)});
  if (!bars.empty()) blocks.push_back(barsBlock("warningsBySource", bars, "number"));
  return {{"severity", warnings >= 200 ? "warning" : "info"},
          {"conclusion", message("res_events", {{"cron", cron}, {"units", units}, {"warnings", warnings}, {"lines", total}})},
          {"findings", findings},
          {"blocks", blocks}};
}

// ---------------------------------------------------------------- OOM killer
json checkOomEvents(const json& params) {
  const auto window = windowFrom(params);
  if (!commandExists("journalctl")) {
    return skipped("res_journal_unavailable");
  }
  const auto out = runCommand({"journalctl", "-k", "--since", "@" + std::to_string(window.fromMs / 1000), "--until",
                               "@" + std::to_string(window.toMs / 1000), "-o", "short-unix", "-q", "--no-pager"},
                              60, true, 16 * 1024 * 1024);
  static const std::regex killed(R"(Killed process (\d+) \(([^)]+)\).*?anon-rss:(\d+)kB)");
  static const std::regex killedShort(R"(Killed process (\d+) \(([^)]+)\))");
  static const std::regex invoked(R"((\S+) invoked oom-killer)");
  json rows = json::array();
  int kills = 0;
  std::map<std::string, int> victims;
  std::string trigger;
  for (const auto& line : splitLines(out.out)) {
    std::smatch match;
    const auto space = line.find(' ');
    const long long at = static_cast<long long>(toDouble(line.substr(0, space)) * 1000.0);
    if (std::regex_search(line, match, invoked) && trigger.empty()) {
      trigger = match[1].str();
    }
    if (std::regex_search(line, match, killed) || std::regex_search(line, match, killedShort)) {
      ++kills;
      const auto name = match[2].str();
      ++victims[name];
      const long long rss = match.size() > 3 && match[3].matched ? toLong(match[3].str()) * 1024 : 0;
      rows.push_back({{"at", at}, {"process", name}, {"pid", toLong(match[1].str())}, {"rss", rss}});
    }
  }
  if (kills == 0) {
    return {{"severity", "ok"}, {"conclusion", message("res_oom_none")}};
  }
  std::string topVictim;
  int topCount = 0;
  for (const auto& [name, count] : victims) {
    if (count > topCount) {
      topVictim = name;
      topCount = count;
    }
  }
  json findings = json::array();
  if (!trigger.empty()) findings.push_back(message("res_oom_trigger", {{"process", trigger}}));
  return {{"severity", "critical"},
          {"conclusion", message("res_oom_kills", {{"count", kills}, {"process", topVictim}})},
          {"findings", findings},
          {"blocks", {tableBlock("oomKills", {{"at", "date"}, {"process", "text"}, {"pid", "number"}, {"rss", "bytes"}}, rows)}}};
}

// ---------------------------------------------------------------- container cgroups
std::vector<std::string> runningContainerIds() {
  std::vector<std::string> ids;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/var/lib/docker/containers", ec)) {
    const auto id = entry.path().filename().string();
    if (id.size() == 64) ids.push_back(id);
  }
  return ids;
}

std::string cgroupDir(const std::string& id, const std::string& v1Controller) {
  const std::vector<std::string> candidates = {
      "/sys/fs/cgroup/system.slice/docker-" + id + ".scope",
      "/sys/fs/cgroup/docker/" + id,
      "/sys/fs/cgroup/" + v1Controller + "/docker/" + id,
      "/sys/fs/cgroup/" + v1Controller + "/system.slice/docker-" + id + ".scope",
  };
  for (const auto& dir : candidates) {
    std::error_code ec;
    if (fs::exists(dir, ec)) return dir;
  }
  return "";
}

std::map<std::string, long long> keyValues(const std::string& text) {
  std::map<std::string, long long> values;
  for (const auto& line : splitLines(text)) {
    std::istringstream fields(line);
    std::string key;
    long long value = 0;
    if (fields >> key >> value) values[key] = value;
  }
  return values;
}

json checkCgroupThrottling(const json&) {
  json rows = json::array();
  std::string worstName;
  double worstPct = 0;
  for (const auto& id : runningContainerIds()) {
    const auto dir = cgroupDir(id, "cpu,cpuacct");
    if (dir.empty()) continue;
    const auto stat = keyValues(readFile(dir + "/cpu.stat"));
    const auto periods = stat.count("nr_periods") ? stat.at("nr_periods") : 0;
    const auto throttled = stat.count("nr_throttled") ? stat.at("nr_throttled") : 0;
    double limit = 0;
    const auto cpuMax = trim(readFile(dir + "/cpu.max"));
    if (!cpuMax.empty()) {
      std::istringstream fields(cpuMax);
      std::string quota;
      long long period = 0;
      fields >> quota >> period;
      if (quota != "max" && period > 0) limit = static_cast<double>(toLong(quota)) / period;
    } else {
      const auto quota = toLong(trim(readFile(dir + "/cpu.cfs_quota_us")));
      const auto period = toLong(trim(readFile(dir + "/cpu.cfs_period_us")));
      if (quota > 0 && period > 0) limit = static_cast<double>(quota) / period;
    }
    const double pct = periods > 0 ? 100.0 * throttled / periods : 0;
    const auto name = containerNameForId(id);
    rows.push_back({{"container", name},
                    {"limitCores", limit > 0 ? json(limit) : json("-")},
                    {"periods", periods},
                    {"throttled", throttled},
                    {"throttledPct", pct}});
    if (periods >= 100 && pct > worstPct) {
      worstPct = pct;
      worstName = name;
    }
  }
  if (rows.empty()) {
    return skipped("res_throttle_nocontainers");
  }
  const bool throttling = worstPct >= 20;
  return {{"severity", throttling ? "warning" : "ok"},
          {"conclusion", throttling ? message("res_throttle_found", {{"container", worstName}, {"pct", worstPct}})
                                    : message("res_throttle_none", {{"count", rows.size()}})},
          {"blocks", {tableBlock("cpuThrottling",
                                 {{"container", "text"},
                                  {"limitCores", "number"},
                                  {"periods", "number"},
                                  {"throttled", "number"},
                                  {"throttledPct", "percent"}},
                                 rows)}}};
}

json checkMemoryLimits(const json&) {
  json rows = json::array();
  json findings = json::array();
  std::string severity = "ok";
  std::string worstName;
  double worstPct = 0;
  int oomTotal = 0;
  constexpr long long kUnlimited = 1LL << 60;
  for (const auto& id : runningContainerIds()) {
    const auto dir = cgroupDir(id, "memory");
    if (dir.empty()) continue;
    long long usage = 0, limit = 0;
    int oom = 0;
    const auto current = trim(readFile(dir + "/memory.current"));
    if (!current.empty()) {
      usage = toLong(current);
      const auto max = trim(readFile(dir + "/memory.max"));
      limit = max == "max" || max.empty() ? 0 : toLong(max);
      const auto events = keyValues(readFile(dir + "/memory.events"));
      oom = events.count("oom_kill") ? static_cast<int>(events.at("oom_kill")) : 0;
    } else {
      usage = toLong(trim(readFile(dir + "/memory.usage_in_bytes")));
      limit = toLong(trim(readFile(dir + "/memory.limit_in_bytes")));
      if (limit >= kUnlimited) limit = 0;
      oom = static_cast<int>(toLong(trim(readFile(dir + "/memory.failcnt"))) > 0 ? 1 : 0);
    }
    const double pct = limit > 0 ? 100.0 * usage / limit : 0;
    const auto name = containerNameForId(id);
    rows.push_back({{"container", name},
                    {"usage", usage},
                    {"limit", limit > 0 ? json(limit) : json("-")},
                    {"usagePct", limit > 0 ? json(pct) : json("-")},
                    {"oomKills", oom}});
    oomTotal += oom;
    if (oom > 0) {
      findings.push_back(message("res_mem_container_oom", {{"container", name}, {"count", oom}}));
      severity = "warning";
    }
    if (pct > worstPct) {
      worstPct = pct;
      worstName = name;
    }
  }
  if (rows.empty()) {
    return skipped("res_throttle_nocontainers");
  }
  std::sort(rows.begin(), rows.end(), [](const json& a, const json& b) {
    return a["usage"].get<long long>() > b["usage"].get<long long>();
  });
  if (worstPct >= 90) severity = "warning";
  json conclusion = worstPct >= 90 ? message("res_mem_limit_near", {{"container", worstName}, {"pct", worstPct}})
                                   : message("res_mem_limits_ok", {{"count", rows.size()}, {"oomKills", oomTotal}});
  return {{"severity", severity},
          {"conclusion", conclusion},
          {"findings", findings},
          {"blocks", {tableBlock("containerMemory",
                                 {{"container", "text"}, {"usage", "bytes"}, {"limit", "bytes"}, {"usagePct", "percent"},
                                  {"oomKills", "number"}},
                                 rows)}}};
}

// ---------------------------------------------------------------- pressure stall information
struct Psi {
  bool ok = false;
  double someAvg60 = 0;
  double fullAvg60 = 0;
  double someAvg10 = 0;
};

Psi readPsi(const std::string& resource) {
  Psi psi;
  const auto text = readFile("/proc/pressure/" + resource);
  if (text.empty()) return psi;
  psi.ok = true;
  for (const auto& line : splitLines(text)) {
    const bool some = line.rfind("some", 0) == 0;
    const auto pos60 = line.find("avg60=");
    const auto pos10 = line.find("avg10=");
    const double avg60 = pos60 == std::string::npos ? 0 : toDouble(line.substr(pos60 + 6));
    const double avg10 = pos10 == std::string::npos ? 0 : toDouble(line.substr(pos10 + 6));
    if (some) {
      psi.someAvg60 = avg60;
      psi.someAvg10 = avg10;
    } else {
      psi.fullAvg60 = avg60;
    }
  }
  return psi;
}

json checkCpuPressure(const json&) {
  const auto cpu = readPsi("cpu");
  const auto io = readPsi("io");
  if (!cpu.ok && !io.ok) {
    return skipped("res_psi_unavailable");
  }
  std::string severity = "ok";
  if (cpu.someAvg60 >= 50 || io.fullAvg60 >= 20) severity = "critical";
  else if (cpu.someAvg60 >= 20 || io.fullAvg60 >= 5) severity = "warning";
  json findings = json::array();
  if (io.fullAvg60 >= 5) findings.push_back(message("res_psi_io", {{"pct", io.fullAvg60}}));
  return {{"severity", severity},
          {"conclusion", message("res_psi_cpu", {{"cpu", cpu.someAvg60}, {"io", io.someAvg60}})},
          {"findings", findings},
          {"blocks", {kvBlock("pressureNow", {kv("cpuSome10", cpu.someAvg10, "percent"),
                                              kv("cpuSome60", cpu.someAvg60, "percent"),
                                              kv("ioSome60", io.someAvg60, "percent"),
                                              kv("ioFull60", io.fullAvg60, "percent")})}}};
}

json checkSwapPressure(const json&) {
  std::map<std::string, long long> mem;
  for (const auto& line : splitLines(readFile("/proc/meminfo"))) {
    std::istringstream fields(line);
    std::string key;
    long long value = 0;
    if (fields >> key >> value) mem[key] = value * 1024;
  }
  const auto swapTotal = mem["SwapTotal:"];
  const auto swapUsed = swapTotal - mem["SwapFree:"];
  const auto swappiness = toLong(trim(readFile("/proc/sys/vm/swappiness")));
  const auto psi = readPsi("memory");
  struct Swapper {
    int pid;
    std::string name;
    long long bytes;
  };
  std::vector<Swapper> swappers;
  std::error_code ec;
  for (const auto& proc : fs::directory_iterator("/proc", ec)) {
    const auto pidText = proc.path().filename().string();
    if (pidText.empty() || !std::all_of(pidText.begin(), pidText.end(), ::isdigit)) continue;
    const auto status = readFile(proc.path().string() + "/status");
    const auto pos = status.find("VmSwap:");
    if (pos == std::string::npos) continue;
    const auto kb = toLong(trim(status.substr(pos + 7, status.find('\n', pos) - pos - 7)));
    if (kb <= 0) continue;
    const auto namePos = status.find("Name:");
    const auto name = namePos == std::string::npos ? std::string{} : trim(status.substr(namePos + 5, status.find('\n', namePos) - namePos - 5));
    swappers.push_back({std::stoi(pidText), name, kb * 1024});
  }
  std::sort(swappers.begin(), swappers.end(), [](const Swapper& a, const Swapper& b) { return a.bytes > b.bytes; });
  json rows = json::array();
  for (const auto& swapper : swappers) {
    if (rows.size() >= 10) break;
    rows.push_back({{"pid", swapper.pid}, {"process", swapper.name}, {"swap", swapper.bytes}});
  }
  const double swapPct = swapTotal > 0 ? 100.0 * swapUsed / swapTotal : 0;
  std::string severity = "ok";
  if (psi.fullAvg60 >= 5) severity = "critical";
  else if (psi.someAvg60 >= 10 || swapPct >= 50) severity = "warning";
  json findings = json::array();
  if (swapTotal == 0) findings.push_back(message("res_swap_none"));
  if (!swappers.empty()) {
    findings.push_back(message("res_swap_top", {{"process", swappers.front().name}, {"bytes", swappers.front().bytes}}));
  }
  json blocks = json::array({kvBlock("memoryPressure", {kv("swapTotal", swapTotal, "bytes"),
                                                        kv("swapUsed", swapUsed, "bytes"),
                                                        kv("swappiness", swappiness, "number"),
                                                        kv("memSome60", psi.someAvg60, "percent"),
                                                        kv("memFull60", psi.fullAvg60, "percent"),
                                                        kv("cached", mem["Cached:"], "bytes"),
                                                        kv("available", mem["MemAvailable:"], "bytes")})});
  if (!rows.empty()) blocks.push_back(tableBlock("swapByProcess", {{"pid", "number"}, {"process", "text"}, {"swap", "bytes"}}, rows));
  return {{"severity", severity},
          {"conclusion", message("res_swap", {{"usedBytes", swapUsed}, {"pct", swapPct}, {"psi", psi.someAvg60}})},
          {"findings", findings},
          {"blocks", blocks}};
}

// ---------------------------------------------------------------- live state
json checkLiveState(const json&, bool memory) {
  ProcessSampler sampler;
  sampler.sample();
  std::this_thread::sleep_for(std::chrono::seconds(2));
  auto processes = sampler.sample();
  std::sort(processes.begin(), processes.end(), [&](const ProcessInfo& a, const ProcessInfo& b) {
    return memory ? a.rssKb > b.rssKb : a.cpuPercent > b.cpuPercent;
  });
  long long memTotalKb = 0, memAvailableKb = 0, cachedKb = 0;
  for (const auto& line : splitLines(readFile("/proc/meminfo"))) {
    std::istringstream fields(line);
    std::string key;
    long long value = 0;
    fields >> key >> value;
    if (key == "MemTotal:") memTotalKb = value;
    if (key == "MemAvailable:") memAvailableKb = value;
    if (key == "Cached:") cachedKb = value;
  }
  json rows = json::array();
  json raw = json::array();
  const int self = ::getpid();
  const int parent = ::getppid();
  for (std::size_t i = 0; i < std::min<std::size_t>(10, processes.size()); ++i) {
    auto& process = processes[i];
    enrichProcess(process);
    rows.push_back({{"pid", process.pid},
                    {"process", process.name},
                    {"user", process.user},
                    {"container", process.container.empty() ? "-" : process.container},
                    {"cpu", process.cpuPercent},
                    {"rss", process.rssKb * 1024},
                    {"cmd", process.cmd}});
    raw.push_back(processToJson(process));
  }
  if (processes.empty()) {
    return skipped("res_live_unavailable");
  }
  const auto& top = processes.front();
  json actions = json::array();
  const bool heavy = memory ? (memTotalKb > 0 && top.rssKb * 10 >= memTotalKb) : top.cpuPercent >= 25;
  if (heavy) {
    if (!top.container.empty()) {
      actions.push_back(suggestAction("restart_container", {{"name", top.container}}, memory ? top.rssKb * 1024 : 0, "medium"));
    } else if (top.pid > 1 && top.pid != self && top.pid != parent && top.cmd.rfind('[', 0) != 0) {
      actions.push_back(suggestAction("terminate_process",
                                      {{"pid", top.pid}, {"start", top.startTicks}, {"name", top.name}},
                                      memory ? top.rssKb * 1024 : 0, "high"));
    }
  }
  if (memory && memTotalKb > 0 && cachedKb * 100 >= memTotalKb * 30 && memAvailableKb * 100 <= memTotalKb * 15) {
    actions.push_back(suggestAction("drop_page_cache", nullptr, cachedKb * 1024, "low"));
  }
  json conclusion = memory ? message("res_live_top_ram", {{"name", top.name},
                                                          {"bytes", top.rssKb * 1024},
                                                          {"pct", memTotalKb > 0 ? 100.0 * top.rssKb / memTotalKb : 0.0}})
                           : message("res_live_top_cpu", {{"name", top.name}, {"pct", top.cpuPercent}});
  std::string severity = "info";
  if (memory && memTotalKb > 0 && memAvailableKb * 100 <= memTotalKb * 10) severity = "warning";
  if (!memory && top.cpuPercent >= 80) severity = "warning";
  return {{"severity", severity},
          {"conclusion", conclusion},
          {"blocks", {tableBlock("liveProcesses",
                                 {{"pid", "number"},
                                  {"process", "text"},
                                  {"user", "text"},
                                  {"container", "text"},
                                  {"cpu", "percent"},
                                  {"rss", "bytes"},
                                  {"cmd", "text"}},
                                 rows)}},
          {"actions", actions},
          {"raw", raw}};
}

}  // namespace

std::vector<CheckSpec> cpuChecks() {
  return {{"atop_window", [](const json& p) { return checkAtopWindow(p, false); }},
          {"system_events", checkSystemEvents},
          {"cgroup_throttling", checkCgroupThrottling},
          {"pressure", checkCpuPressure},
          {"live_state", [](const json& p) { return checkLiveState(p, false); }}};
}

std::vector<CheckSpec> ramChecks() {
  return {{"atop_window", [](const json& p) { return checkAtopWindow(p, true); }},
          {"oom_events", checkOomEvents},
          {"memory_limits", checkMemoryLimits},
          {"swap_pressure", checkSwapPressure},
          {"system_events", checkSystemEvents},
          {"live_state", [](const json& p) { return checkLiveState(p, true); }}};
}

}  // namespace highcube::agent::audit
