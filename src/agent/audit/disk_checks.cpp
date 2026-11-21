#include "audit/audit_engine.hpp"
#include "audit/command_runner.hpp"
#include "audit/disk_support.hpp"
#include "metrics/process_snapshot.hpp"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace highcube::agent::audit {

std::vector<Mount> listMounts() {
  std::vector<Mount> mounts;
  const auto out = runCommand({"df", "-PB1", "-x", "tmpfs", "-x", "devtmpfs", "-x", "squashfs", "-x",
                               "overlay", "-x", "efivarfs", "-x", "nsfs"},
                              30, false);
  std::set<std::string> seen;
  const auto lines = splitLines(out.out);
  for (std::size_t i = 1; i < lines.size(); ++i) {
    std::istringstream fields(lines[i]);
    Mount mount;
    std::string capacity;
    if (!(fields >> mount.filesystem >> mount.size >> mount.used >> mount.available >> capacity)) {
      continue;
    }
    std::getline(fields, mount.path);
    mount.path = trim(mount.path);
    if (mount.path.empty() || mount.size <= 0 || !seen.insert(mount.path).second) {
      continue;
    }
    mounts.push_back(mount);
  }
  return mounts;
}

long long availableBytes(const std::string& path) {
  struct statvfs st {};
  if (::statvfs(path.c_str(), &st) != 0) {
    return 0;
  }
  return static_cast<long long>(st.f_bavail) * static_cast<long long>(st.f_frsize);
}

long long duBytes(const std::string& path, int timeoutSeconds) {
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return 0;
  }
  const auto out = runCommand({"du", "-sxB1", path}, timeoutSeconds);
  std::istringstream fields(out.out);
  long long bytes = 0;
  fields >> bytes;
  return bytes;
}

std::vector<std::string> homeDirectories() {
  std::vector<std::string> homes = {"/root"};
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/home", ec)) {
    if (entry.is_directory(ec)) {
      homes.push_back(entry.path().string());
    }
  }
  return homes;
}

const std::vector<std::string>& userCacheDirectories() {
  static const std::vector<std::string> dirs = {".cache/pip", ".npm/_cacache", ".cache/yarn",
                                                ".cache/composer"};
  return dirs;
}

std::vector<std::string> rotatedLogsPredicate() {
  return {"(",      "-name", "*.gz",  "-o", "-name",       "*.xz",    "-o",
          "-name",  "*.bz2", "-o",    "-name", "*.old", "-o", "-regex", ".*\\.[0-9]+$", ")"};
}

namespace {

constexpr long long kMb = 1024LL * 1024LL;
constexpr long long kGb = 1024LL * kMb;

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::vector<std::string> mountPaths() {
  std::vector<std::string> paths;
  for (const auto& mount : listMounts()) {
    if (mount.path.rfind("/boot", 0) == 0 || mount.path.rfind("/snap", 0) == 0) {
      continue;
    }
    paths.push_back(mount.path);
  }
  if (paths.empty()) {
    paths.push_back("/");
  }
  return paths;
}

// find <mounts> -xdev (skip system and docker trees) -prune -o <predicate> -printf <format>
std::vector<std::string> findAcrossMounts(const std::vector<std::string>& predicate,
                                          const std::string& format) {
  std::vector<std::string> args = {"find"};
  for (const auto& path : mountPaths()) {
    args.push_back(path);
  }
  args.insert(args.end(), {"-xdev", "(", "-path", "/proc", "-o", "-path", "/sys", "-o", "-path",
                           "/var/lib/docker", "-o", "-path", "/snap", ")", "-prune", "-o"});
  args.insert(args.end(), predicate.begin(), predicate.end());
  args.insert(args.end(), {"-printf", format});
  return args;
}

long long toLong(const std::string& value) {
  try {
    return std::stoll(value);
  } catch (const std::exception&) {
    return 0;
  }
}

long long epochMs(const std::string& value) {
  try {
    return static_cast<long long>(std::stod(value) * 1000.0);
  } catch (const std::exception&) {
    return 0;
  }
}

std::string severityForPercent(double pct, double warning, double critical) {
  if (pct >= critical) return "critical";
  if (pct >= warning) return "warning";
  return "ok";
}

int severityRank(const std::string& severity) {
  if (severity == "critical") return 3;
  if (severity == "warning") return 2;
  if (severity == "info") return 1;
  return 0;
}

// ---------------------------------------------------------------- filesystems
json checkFilesystems(const json&) {
  const auto mounts = listMounts();
  std::map<std::string, double> inodes;
  const auto inodeOut = runCommand({"df", "-Pi", "-x", "tmpfs", "-x", "devtmpfs", "-x", "squashfs", "-x",
                                    "overlay", "-x", "efivarfs", "-x", "nsfs"},
                                   30, false);
  const auto inodeLines = splitLines(inodeOut.out);
  for (std::size_t i = 1; i < inodeLines.size(); ++i) {
    std::istringstream fields(inodeLines[i]);
    std::string fsName, total, used, freeCount, pct, path;
    if (!(fields >> fsName >> total >> used >> freeCount >> pct)) {
      continue;
    }
    std::getline(fields, path);
    pct.erase(std::remove(pct.begin(), pct.end(), '%'), pct.end());
    if (pct != "-") {
      inodes[trim(path)] = static_cast<double>(toLong(pct));
    }
  }

  json rows = json::array();
  json findings = json::array();
  std::string worst = "ok";
  std::string worstMount = "/";
  double worstPct = 0;
  double rootPct = 0;
  for (const auto& mount : mounts) {
    const double pct = 100.0 * static_cast<double>(mount.used) /
                       static_cast<double>(std::max(1LL, mount.used + mount.available));
    const double inodePct = inodes.count(mount.path) ? inodes[mount.path] : 0;
    rows.push_back({{"mount", mount.path},
                    {"filesystem", mount.filesystem},
                    {"size", mount.size},
                    {"used", mount.used},
                    {"available", mount.available},
                    {"usedPct", pct},
                    {"inodesPct", inodePct}});
    if (mount.path == "/") {
      rootPct = pct;
    }
    const auto severity = severityForPercent(pct, 80, 90);
    if (severityRank(severity) > severityRank(worst) ||
        (severity == worst && pct > worstPct)) {
      worst = severity;
      worstMount = mount.path;
      worstPct = pct;
    }
    if (inodePct >= 80) {
      findings.push_back(message("disk_inodes_high", {{"mount", mount.path}, {"pct", inodePct}}));
      if (inodePct >= 90) {
        worst = "critical";
      } else if (severityRank(worst) < 2) {
        worst = "warning";
      }
    }
    if (mount.path == "/boot" && pct >= 80) {
      findings.push_back(message("disk_boot_full", {{"pct", pct}}));
    }
  }
  json conclusion;
  if (worst == "ok") {
    conclusion = message("disk_fs_ok", {{"count", mounts.size()}, {"rootPct", rootPct}});
  } else {
    conclusion = message(worst == "critical" ? "disk_fs_critical" : "disk_fs_warning",
                         {{"mount", worstMount}, {"pct", worstPct}});
  }
  return {{"severity", worst},
          {"conclusion", conclusion},
          {"findings", findings},
          {"blocks",
           {tableBlock("filesystems",
                       {{"mount", "text"},
                        {"filesystem", "text"},
                        {"size", "bytes"},
                        {"used", "bytes"},
                        {"available", "bytes"},
                        {"usedPct", "percent"},
                        {"inodesPct", "percent"}},
                       rows)}}};
}

// ---------------------------------------------------------------- top directories
json checkTopDirectories(const json&) {
  const auto out = runCommand({"du", "-xB1", "-d", "3", "/"}, 150);
  struct Entry {
    std::string path;
    long long bytes = 0;
    int depth = 0;
  };
  std::vector<Entry> entries;
  long long rootBytes = 0;
  for (const auto& line : splitLines(out.out)) {
    const auto fields = splitTabs(line);
    if (fields.size() < 2) {
      continue;
    }
    Entry entry{fields[1], toLong(fields[0]), 0};
    if (entry.path == "/") {
      rootBytes = entry.bytes;
      continue;
    }
    entry.depth = static_cast<int>(std::count(entry.path.begin(), entry.path.end(), '/'));
    entries.push_back(entry);
  }
  if (entries.empty()) {
    return skipped("disk_dirs_unavailable");
  }
  if (rootBytes <= 0) {
    for (const auto& entry : entries) {
      if (entry.depth == 1) rootBytes += entry.bytes;
    }
  }
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.bytes > b.bytes; });
  json bars = json::array();
  for (const auto& entry : entries) {
    if (entry.depth == 1 && bars.size() < 12) {
      bars.push_back({{"label", entry.path}, {"value", entry.bytes}});
    }
  }
  // Deepest heavy directory: a depth-3 entry is a more actionable answer than "/var".
  json rows = json::array();
  for (const auto& entry : entries) {
    if (rows.size() >= 25) break;
    rows.push_back({{"path", entry.path},
                    {"size", entry.bytes},
                    {"depth", entry.depth},
                    {"sharePct", rootBytes > 0 ? 100.0 * entry.bytes / rootBytes : 0.0}});
  }
  const Entry* deepest = nullptr;
  for (const auto& entry : entries) {
    if (entry.depth == 3) {
      deepest = &entry;
      break;
    }
  }
  const auto& top = entries.front();
  json findings = json::array();
  if (out.timedOut) {
    findings.push_back(message("disk_dirs_partial"));
  }
  if (deepest) {
    findings.push_back(message("disk_dirs_deepest", {{"path", deepest->path}, {"bytes", deepest->bytes}}));
  }
  return {{"severity", "info"},
          {"conclusion",
           message("disk_dirs_top", {{"path", top.path},
                                     {"bytes", top.bytes},
                                     {"pct", rootBytes > 0 ? 100.0 * top.bytes / rootBytes : 0.0}})},
          {"findings", findings},
          {"blocks",
           {barsBlock("topLevelDirectories", bars, "bytes"),
            tableBlock("heaviestDirectories",
                       {{"path", "text"}, {"size", "bytes"}, {"depth", "number"}, {"sharePct", "percent"}},
                       rows)}}};
}

// ---------------------------------------------------------------- large files
json checkLargeFiles(const json&) {
  const auto out = runCommand(findAcrossMounts({"-type", "f", "-size", "+500M"}, "%s\t%A@\t%T@\t%u\t%p\n"), 150);
  struct File {
    std::string path;
    std::string owner;
    long long bytes = 0;
    long long accessed = 0;
    long long modified = 0;
  };
  std::vector<File> files;
  for (const auto& line : splitLines(out.out)) {
    const auto fields = splitTabs(line);
    if (fields.size() < 5) continue;
    files.push_back({fields[4], fields[3], toLong(fields[0]), epochMs(fields[1]), epochMs(fields[2])});
  }
  if (files.empty()) {
    return {{"severity", "ok"}, {"conclusion", message("disk_large_none")}};
  }
  std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.bytes > b.bytes; });
  const long long staleCutoff = nowMs() - 90LL * 24 * 3600 * 1000;
  long long total = 0, staleBytes = 0;
  int staleCount = 0;
  json rows = json::array();
  for (const auto& file : files) {
    const bool stale = file.accessed < staleCutoff && file.modified < staleCutoff;
    total += file.bytes;
    if (stale) {
      staleBytes += file.bytes;
      ++staleCount;
    }
    if (rows.size() < 30) {
      rows.push_back({{"path", file.path},
                      {"size", file.bytes},
                      {"owner", file.owner},
                      {"modified", file.modified},
                      {"accessed", file.accessed},
                      {"stale", stale ? "yes" : "no"}});
    }
  }
  json findings = json::array();
  if (staleCount > 0) {
    findings.push_back(message("disk_large_stale", {{"count", staleCount}, {"bytes", staleBytes}}));
  }
  findings.push_back(message("disk_large_biggest", {{"path", files.front().path}, {"bytes", files.front().bytes}}));
  if (out.timedOut) {
    findings.push_back(message("disk_scan_partial"));
  }
  return {{"severity", staleBytes >= 5 * kGb || total >= 50 * kGb ? "warning" : "info"},
          {"conclusion", message("disk_large_found", {{"count", files.size()},
                                                     {"bytes", total},
                                                     {"staleCount", staleCount},
                                                     {"staleBytes", staleBytes}})},
          {"findings", findings},
          {"blocks", {tableBlock("largeFiles",
                                 {{"path", "text"},
                                  {"size", "bytes"},
                                  {"owner", "text"},
                                  {"modified", "date"},
                                  {"accessed", "date"},
                                  {"stale", "text"}},
                                 rows)}}};
}

// ---------------------------------------------------------------- docker artifacts
json checkDocker(const json&) {
  if (!commandExists("docker")) {
    return skipped("disk_docker_missing");
  }
  const auto df = runCommand(
      {"docker", "system", "df", "--format", "{{.Type}}\t{{.TotalCount}}\t{{.Active}}\t{{.Size}}\t{{.Reclaimable}}"},
      90);
  if (df.code != 0) {
    return skipped("disk_docker_unavailable");
  }
  std::map<std::string, long long> reclaimable;
  json rows = json::array();
  long long totalReclaim = 0;
  for (const auto& line : splitLines(df.out)) {
    const auto fields = splitTabs(line);
    if (fields.size() < 5) continue;
    const auto reclaim = parseHumanSize(fields[4], true);
    reclaimable[fields[0]] = reclaim;
    totalReclaim += reclaim;
    rows.push_back({{"type", fields[0]},
                    {"total", toLong(fields[1])},
                    {"active", toLong(fields[2])},
                    {"size", parseHumanSize(fields[3], true)},
                    {"reclaimable", reclaim}});
  }
  const auto dangling = splitLines(runCommand({"docker", "images", "-f", "dangling=true", "-q"}, 60).out);
  const auto orphanVolumes = splitLines(runCommand({"docker", "volume", "ls", "-qf", "dangling=true"}, 60).out);
  const auto stoppedOut = runCommand({"docker", "ps", "-a", "-f", "status=exited", "-f", "status=created", "-f",
                                      "status=dead", "--format", "{{.Names}}\t{{.Image}}\t{{.Status}}"},
                                     60);
  json stopped = json::array();
  for (const auto& line : splitLines(stoppedOut.out)) {
    const auto fields = splitTabs(line);
    if (fields.size() < 3) continue;
    stopped.push_back({{"name", fields[0]}, {"image", fields[1]}, {"status", fields[2]}});
  }

  json actions = json::array();
  json findings = json::array();
  const auto images = reclaimable["Images"];
  const auto build = reclaimable["Build Cache"];
  const auto containers = reclaimable["Containers"];
  const auto volumes = reclaimable["Local Volumes"];
  if (images >= 100 * kMb) {
    actions.push_back(suggestAction("docker_images_prune", nullptr, images, "medium"));
  }
  if (build >= 100 * kMb) {
    actions.push_back(suggestAction("docker_builder_prune", nullptr, build, "low"));
  }
  if (!stopped.empty() || containers > 0) {
    actions.push_back(suggestAction("docker_containers_prune", nullptr, containers, "medium"));
  }
  const auto danglingVolumes = std::count_if(orphanVolumes.begin(), orphanVolumes.end(),
                                             [](const std::string& line) { return !trim(line).empty(); });
  const auto danglingImages = std::count_if(dangling.begin(), dangling.end(),
                                            [](const std::string& line) { return !trim(line).empty(); });
  if (danglingVolumes > 0) {
    findings.push_back(message("disk_docker_orphan_volumes", {{"count", danglingVolumes}, {"bytes", volumes}}));
    actions.push_back(suggestAction("docker_volumes_prune", nullptr, volumes, "high"));
  }
  if (danglingImages > 0) {
    findings.push_back(message("disk_docker_dangling", {{"count", danglingImages}}));
  }
  if (build >= kGb) {
    findings.push_back(message("disk_docker_build_cache", {{"bytes", build}}));
  }
  std::string severity = "ok";
  if (totalReclaim >= 10 * kGb) severity = "warning";
  else if (totalReclaim >= kGb) severity = "info";
  json blocks = json::array({tableBlock("dockerUsage",
                                        {{"type", "text"},
                                         {"total", "number"},
                                         {"active", "number"},
                                         {"size", "bytes"},
                                         {"reclaimable", "bytes"}},
                                        rows)});
  if (!stopped.empty()) {
    blocks.push_back(tableBlock("stoppedContainers", {{"name", "text"}, {"image", "text"}, {"status", "text"}}, stopped));
  }
  return {{"severity", severity},
          {"conclusion", totalReclaim > 0 ? message("disk_docker_reclaimable",
                                                    {{"bytes", totalReclaim}, {"stopped", stopped.size()}})
                                          : message("disk_docker_clean")},
          {"findings", findings},
          {"blocks", blocks},
          {"actions", actions}};
}

// ---------------------------------------------------------------- container logs
json checkContainerLogs(const json&) {
  const std::string root = "/var/lib/docker/containers";
  std::error_code ec;
  if (!fs::exists(root, ec)) {
    return skipped("disk_container_logs_none");
  }
  struct Log {
    std::string id;
    std::string name;
    std::string driver;
    std::string maxSize;
    long long bytes = 0;
  };
  std::vector<Log> logs;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (!entry.is_directory(ec)) continue;
    Log log;
    log.id = entry.path().filename().string();
    if (log.id.size() != 64) continue;
    const auto file = entry.path() / (log.id + "-json.log");
    log.bytes = fs::exists(file, ec) ? static_cast<long long>(fs::file_size(file, ec)) : 0;
    if (ec) {
      log.bytes = 0;
      ec.clear();
    }
    const auto host = json::parse(readFile((entry.path() / "hostconfig.json").string()), nullptr, false);
    if (host.is_object() && host.contains("LogConfig") && host["LogConfig"].is_object()) {
      log.driver = host["LogConfig"].value("Type", std::string{});
      const auto config = host["LogConfig"].value("Config", json::object());
      if (config.is_object()) log.maxSize = config.value("max-size", std::string{});
    }
    log.name = containerNameForId(log.id);
    logs.push_back(log);
  }
  if (logs.empty()) {
    return skipped("disk_container_logs_none");
  }
  std::sort(logs.begin(), logs.end(), [](const Log& a, const Log& b) { return a.bytes > b.bytes; });
  long long total = 0;
  int unbounded = 0;
  json rows = json::array();
  json actions = json::array();
  for (const auto& log : logs) {
    total += log.bytes;
    if (log.driver == "json-file" && log.maxSize.empty()) ++unbounded;
    if (rows.size() < 25) {
      rows.push_back({{"container", log.name},
                      {"size", log.bytes},
                      {"driver", log.driver},
                      {"maxSize", log.maxSize.empty() ? "-" : log.maxSize}});
    }
    if (log.bytes >= 100 * kMb && actions.size() < 5) {
      actions.push_back(suggestAction("container_log_truncate", {{"id", log.id}, {"name", log.name}}, log.bytes, "low"));
    }
  }
  json findings = json::array();
  if (unbounded > 0) {
    findings.push_back(message("disk_container_logs_unbounded", {{"count", unbounded}}));
  }
  std::string severity = "ok";
  if (total >= 5 * kGb || logs.front().bytes >= 2 * kGb) severity = "warning";
  else if (total >= 500 * kMb) severity = "info";
  return {{"severity", severity},
          {"conclusion", message("disk_container_logs", {{"bytes", total},
                                                         {"count", logs.size()},
                                                         {"largest", logs.front().name},
                                                         {"largestBytes", logs.front().bytes}})},
          {"findings", findings},
          {"blocks", {tableBlock("containerLogs",
                                 {{"container", "text"}, {"size", "bytes"}, {"driver", "text"}, {"maxSize", "text"}},
                                 rows)}},
          {"actions", actions}};
}

// ---------------------------------------------------------------- system logs
json checkSystemLogs(const json&) {
  std::error_code ec;
  if (!fs::exists("/var/log", ec)) {
    return skipped("disk_logs_missing");
  }
  const auto du = runCommand({"du", "-xB1", "-d", "1", "/var/log"}, 90);
  long long total = 0;
  json bars = json::array();
  std::vector<std::pair<std::string, long long>> dirs;
  for (const auto& line : splitLines(du.out)) {
    const auto fields = splitTabs(line);
    if (fields.size() < 2) continue;
    if (fields[1] == "/var/log") {
      total = toLong(fields[0]);
      continue;
    }
    dirs.emplace_back(fields[1], toLong(fields[0]));
  }
  std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  for (const auto& [path, bytes] : dirs) {
    if (bars.size() >= 10) break;
    bars.push_back({{"label", path}, {"value", bytes}});
  }

  long long journal = 0;
  if (commandExists("journalctl")) {
    const auto out = runCommand({"journalctl", "--disk-usage"}, 30, false);
    const auto marker = out.out.find("take up ");
    if (marker != std::string::npos) {
      journal = parseHumanSize(out.out.substr(marker + 8), false);
    }
  }

  auto rotatedArgs = std::vector<std::string>{"find", "/var/log", "-xdev", "-type", "f"};
  const auto predicate = rotatedLogsPredicate();
  rotatedArgs.insert(rotatedArgs.end(), predicate.begin(), predicate.end());
  rotatedArgs.insert(rotatedArgs.end(), {"-printf", "%s\n"});
  long long rotated = 0;
  int rotatedCount = 0;
  for (const auto& line : splitLines(runCommand(rotatedArgs, 60).out)) {
    if (line.empty()) continue;
    rotated += toLong(line);
    ++rotatedCount;
  }

  json findings = json::array();
  const auto big = runCommand({"find", "/var/log", "-xdev", "-type", "f", "-size", "+1G", "!", "-name", "*.gz",
                               "-printf", "%s\t%p\n"},
                              60);
  int bigCount = 0;
  for (const auto& line : splitLines(big.out)) {
    const auto fields = splitTabs(line);
    if (fields.size() < 2 || bigCount >= 5) continue;
    findings.push_back(message("disk_logs_active_big", {{"path", fields[1]}, {"bytes", toLong(fields[0])}}));
    ++bigCount;
  }
  if (journal >= 1 * kGb) {
    findings.push_back(message("disk_logs_journal_big", {{"bytes", journal}}));
  }
  json actions = json::array();
  if (journal >= 500 * kMb) {
    actions.push_back(suggestAction("journal_vacuum", nullptr, journal - 200 * kMb, "low"));
  }
  if (rotated >= 50 * kMb) {
    actions.push_back(suggestAction("rotated_logs_clean", nullptr, rotated, "low"));
  }
  std::string severity = "ok";
  if (total >= 5 * kGb || bigCount > 0) severity = "warning";
  else if (total >= kGb) severity = "info";
  return {{"severity", severity},
          {"conclusion", message("disk_logs_total", {{"bytes", total},
                                                     {"journalBytes", journal},
                                                     {"rotatedBytes", rotated},
                                                     {"rotatedCount", rotatedCount}})},
          {"findings", findings},
          {"blocks", {barsBlock("logDirectories", bars, "bytes"),
                      kvBlock("logSummary", {kv("total", total, "bytes"), kv("journal", journal, "bytes"),
                                             kv("rotated", rotated, "bytes"), kv("rotatedCount", rotatedCount, "number")})}},
          {"actions", actions}};
}

// ---------------------------------------------------------------- temp files and caches
json checkTempAndCaches(const json&) {
  json rows = json::array();
  long long tmpOld = 0, pkgCache = 0, userCaches = 0, totalReclaim = 0;
  for (const std::string path : {"/tmp", "/var/tmp"}) {
    std::error_code ec;
    if (!fs::exists(path, ec)) continue;
    const auto size = duBytes(path, 60);
    long long old = 0;
    for (const auto& line : splitLines(runCommand({"find", path, "-xdev", "-mindepth", "1", "-type", "f", "-mtime",
                                                   "+7", "-printf", "%s\n"},
                                                  60)
                                           .out)) {
      old += toLong(line);
    }
    tmpOld += old;
    rows.push_back({{"location", path}, {"category", "temp"}, {"size", size}, {"reclaimable", old}});
  }
  for (const std::string path : {"/var/cache/apt/archives", "/var/cache/dnf", "/var/cache/yum"}) {
    std::error_code ec;
    if (!fs::exists(path, ec)) continue;
    const auto size = duBytes(path, 60);
    pkgCache += size;
    rows.push_back({{"location", path}, {"category", "packages"}, {"size", size}, {"reclaimable", size}});
  }
  for (const auto& home : homeDirectories()) {
    for (const auto& relative : userCacheDirectories()) {
      const auto path = home + "/" + relative;
      std::error_code ec;
      if (!fs::exists(path, ec)) continue;
      const auto size = duBytes(path, 60);
      if (size <= 0) continue;
      userCaches += size;
      rows.push_back({{"location", path}, {"category", "devCache"}, {"size", size}, {"reclaimable", size}});
    }
    const auto cache = home + "/.cache";
    std::error_code ec;
    if (fs::exists(cache, ec)) {
      const auto size = duBytes(cache, 60);
      if (size >= 100 * kMb) {
        rows.push_back({{"location", cache}, {"category", "userCache"}, {"size", size}, {"reclaimable", 0}});
      }
    }
  }
  json findings = json::array();
  if (commandExists("snap")) {
    const auto out = runCommand({"snap", "list", "--all"}, 30, false);
    long long disabledBytes = 0;
    int disabledCount = 0;
    for (const auto& line : splitLines(out.out)) {
      if (line.find("disabled") == std::string::npos) continue;
      std::istringstream fields(line);
      std::string name, version, rev;
      fields >> name >> version >> rev;
      std::error_code ec;
      const auto file = "/var/lib/snapd/snaps/" + name + "_" + rev + ".snap";
      if (fs::exists(file, ec)) {
        disabledBytes += static_cast<long long>(fs::file_size(file, ec));
      }
      ++disabledCount;
    }
    if (disabledCount > 0) {
      findings.push_back(message("disk_snap_disabled", {{"count", disabledCount}, {"bytes", disabledBytes}}));
    }
  }
  json actions = json::array();
  if (tmpOld >= 50 * kMb) actions.push_back(suggestAction("tmp_clean", nullptr, tmpOld, "low"));
  if (pkgCache >= 50 * kMb) actions.push_back(suggestAction("pkg_cache_clean", nullptr, pkgCache, "low"));
  if (userCaches >= 100 * kMb) actions.push_back(suggestAction("user_caches_clean", nullptr, userCaches, "low"));
  totalReclaim = tmpOld + pkgCache + userCaches;
  std::string severity = "ok";
  if (totalReclaim >= 5 * kGb) severity = "warning";
  else if (totalReclaim >= 500 * kMb) severity = "info";
  return {{"severity", severity},
          {"conclusion", message("disk_temp_caches", {{"bytes", totalReclaim},
                                                      {"tmpBytes", tmpOld},
                                                      {"pkgBytes", pkgCache},
                                                      {"devBytes", userCaches}})},
          {"findings", findings},
          {"blocks", {tableBlock("tempAndCaches",
                                 {{"location", "text"}, {"category", "text"}, {"size", "bytes"}, {"reclaimable", "bytes"}},
                                 rows)}},
          {"actions", actions}};
}

// ---------------------------------------------------------------- archives, dumps and media
std::string mediaCategory(const std::string& path) {
  std::string lower = path;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  auto ends = [&](const std::string& suffix) {
    return lower.size() >= suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  for (const auto* ext : {".sql", ".sql.gz", ".dump", ".bak", ".backup"}) {
    if (ends(ext)) return "dump";
  }
  for (const auto* ext : {".iso", ".img", ".qcow2", ".vmdk", ".vdi"}) {
    if (ends(ext)) return "diskImage";
  }
  for (const auto* ext : {".mp4", ".mkv", ".avi", ".mov", ".webm", ".m4v"}) {
    if (ends(ext)) return "video";
  }
  for (const auto* ext : {".mp3", ".wav", ".flac"}) {
    if (ends(ext)) return "audio";
  }
  return "archive";
}

json checkArchivesAndMedia(const json&) {
  const std::vector<std::string> predicate = {
      "-type", "f", "-size", "+100M", "(", "-iname", "*.sql", "-o", "-iname", "*.sql.gz", "-o", "-iname", "*.dump",
      "-o", "-iname", "*.bak", "-o", "-iname", "*.backup", "-o", "-iname", "*.tar", "-o", "-iname", "*.tar.*", "-o",
      "-iname", "*.tgz", "-o", "-iname", "*.zip", "-o", "-iname", "*.7z", "-o", "-iname", "*.rar", "-o", "-iname",
      "*.iso", "-o", "-iname", "*.img", "-o", "-iname", "*.qcow2", "-o", "-iname", "*.vmdk", "-o", "-iname", "*.mp4",
      "-o", "-iname", "*.mkv", "-o", "-iname", "*.avi", "-o", "-iname", "*.mov", "-o", "-iname", "*.webm", "-o",
      "-iname", "*.mp3", "-o", "-iname", "*.wav", "-o", "-iname", "*.flac", ")"};
  const auto out = runCommand(findAcrossMounts(predicate, "%s\t%T@\t%p\n"), 150);
  struct File {
    std::string path;
    std::string category;
    long long bytes = 0;
    long long modified = 0;
  };
  std::vector<File> files;
  std::map<std::string, long long> byCategory;
  long long total = 0;
  for (const auto& line : splitLines(out.out)) {
    const auto fields = splitTabs(line);
    if (fields.size() < 3) continue;
    File file{fields[2], mediaCategory(fields[2]), toLong(fields[0]), epochMs(fields[1])};
    byCategory[file.category] += file.bytes;
    total += file.bytes;
    files.push_back(file);
  }
  if (files.empty()) {
    return {{"severity", "ok"}, {"conclusion", message("disk_archives_none")}};
  }
  std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.bytes > b.bytes; });
  json rows = json::array();
  for (const auto& file : files) {
    if (rows.size() >= 30) break;
    rows.push_back({{"path", file.path}, {"category", file.category}, {"size", file.bytes}, {"modified", file.modified}});
  }
  json bars = json::array();
  for (const auto& [category, bytes] : byCategory) {
    bars.push_back({{"label", category}, {"value", bytes}});
  }
  json findings = json::array();
  if (byCategory["dump"] >= kGb) {
    findings.push_back(message("disk_archives_dumps", {{"bytes", byCategory["dump"]}}));
  }
  if (out.timedOut) findings.push_back(message("disk_scan_partial"));
  return {{"severity", total >= 20 * kGb ? "warning" : "info"},
          {"conclusion", message("disk_archives", {{"bytes", total}, {"count", files.size()}})},
          {"findings", findings},
          {"blocks", {barsBlock("archivesByCategory", bars, "bytes"),
                      tableBlock("archivesAndMedia",
                                 {{"path", "text"}, {"category", "text"}, {"size", "bytes"}, {"modified", "date"}},
                                 rows)}}};
}

// ---------------------------------------------------------------- deleted but open files
json checkDeletedOpenFiles(const json&) {
  struct Holder {
    int pid = 0;
    std::string name;
    std::string container;
    long long bytes = 0;
    int files = 0;
    std::string example;
  };
  std::unordered_map<int, Holder> holders;
  std::set<std::pair<dev_t, ino_t>> seen;
  std::error_code ec;
  for (const auto& proc : fs::directory_iterator("/proc", ec)) {
    const auto pidText = proc.path().filename().string();
    if (pidText.empty() || !std::all_of(pidText.begin(), pidText.end(), ::isdigit)) continue;
    const int pid = std::stoi(pidText);
    std::error_code fdEc;
    for (const auto& fd : fs::directory_iterator(proc.path() / "fd", fdEc)) {
      char target[4096];
      const auto len = ::readlink(fd.path().c_str(), target, sizeof(target) - 1);
      if (len <= 0) continue;
      target[len] = '\0';
      const std::string link(target);
      const std::string suffix = " (deleted)";
      if (link.size() <= suffix.size() || link.compare(link.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
      if (link.rfind("/memfd:", 0) == 0 || link.rfind("/dev/", 0) == 0) continue;
      struct stat st {};
      if (::stat(fd.path().c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
      if (!seen.insert({st.st_dev, st.st_ino}).second) continue;
      auto& holder = holders[pid];
      holder.pid = pid;
      holder.bytes += st.st_size;
      ++holder.files;
      if (holder.example.empty()) holder.example = link.substr(0, link.size() - suffix.size());
    }
  }
  if (holders.empty()) {
    return {{"severity", "ok"}, {"conclusion", message("disk_deleted_none")}};
  }
  std::vector<Holder> sorted;
  long long total = 0;
  for (auto& [pid, holder] : holders) {
    const auto comm = trim(readFile("/proc/" + std::to_string(pid) + "/comm"));
    holder.name = comm;
    holder.container = containerNameForId(containerIdForPid(pid));
    total += holder.bytes;
    sorted.push_back(holder);
  }
  std::sort(sorted.begin(), sorted.end(), [](const Holder& a, const Holder& b) { return a.bytes > b.bytes; });
  json rows = json::array();
  json actions = json::array();
  for (const auto& holder : sorted) {
    if (rows.size() >= 20) break;
    rows.push_back({{"pid", holder.pid},
                    {"process", holder.name},
                    {"container", holder.container.empty() ? "-" : holder.container},
                    {"files", holder.files},
                    {"size", holder.bytes},
                    {"example", holder.example}});
    if (!holder.container.empty() && holder.bytes >= 100 * kMb && actions.size() < 3) {
      actions.push_back(suggestAction("restart_container", {{"name", holder.container}}, holder.bytes, "medium"));
    }
  }
  std::string severity = "ok";
  if (total >= kGb) severity = "warning";
  else if (total >= 50 * kMb) severity = "info";
  return {{"severity", severity},
          {"conclusion", message("disk_deleted_open", {{"bytes", total},
                                                       {"count", sorted.size()},
                                                       {"process", sorted.front().name},
                                                       {"processBytes", sorted.front().bytes}})},
          {"blocks", {tableBlock("deletedOpenFiles",
                                 {{"pid", "number"},
                                  {"process", "text"},
                                  {"container", "text"},
                                  {"files", "number"},
                                  {"size", "bytes"},
                                  {"example", "text"}},
                                 rows)}},
          {"actions", actions}};
}

// ---------------------------------------------------------------- kernels and packages
json checkKernelsAndPackages(const json&) {
  utsname uts{};
  ::uname(&uts);
  const std::string running = uts.release;
  json findings = json::array();
  json actions = json::array();
  json rows = json::array();
  int oldKernels = 0;
  long long oldBytes = 0;
  int autoremove = 0;

  if (commandExists("dpkg-query")) {
    const auto out = runCommand({"dpkg-query", "-W", "-f=${Package}\t${db:Status-Abbrev}\t${Installed-Size}\n",
                                 "linux-image-[0-9]*", "linux-modules-[0-9]*"},
                                30, false);
    std::map<std::string, long long> byVersion;
    for (const auto& line : splitLines(out.out)) {
      const auto fields = splitTabs(line);
      if (fields.size() < 3 || fields[1].rfind("ii", 0) != 0) continue;
      const auto& package = fields[0];
      const auto dash = package.find('-', package.find('-') + 1);
      const auto version = dash == std::string::npos ? package : package.substr(dash + 1);
      byVersion[version] += toLong(fields[2]) * 1024;
    }
    for (const auto& [version, bytes] : byVersion) {
      const bool isRunning = version == running;
      rows.push_back({{"kernel", version}, {"size", bytes}, {"running", isRunning ? "yes" : "no"}});
      if (!isRunning) {
        ++oldKernels;
        oldBytes += bytes;
      }
    }
    if (commandExists("apt-get")) {
      const auto sim = runCommand({"apt-get", "-s", "autoremove"}, 60, false);
      for (const auto& line : splitLines(sim.out)) {
        if (line.rfind("Remv ", 0) == 0) ++autoremove;
      }
    }
  } else if (commandExists("rpm")) {
    const auto out = runCommand({"rpm", "-q", "kernel"}, 30, false);
    for (const auto& line : splitLines(out.out)) {
      if (trim(line).empty()) continue;
      const bool isRunning = line.find(running) != std::string::npos;
      rows.push_back({{"kernel", line}, {"size", 0}, {"running", isRunning ? "yes" : "no"}});
      if (!isRunning) ++oldKernels;
    }
  } else {
    return skipped("disk_kernels_unsupported");
  }

  struct stat root {}, boot {};
  if (::stat("/", &root) == 0 && ::stat("/boot", &boot) == 0 && root.st_dev != boot.st_dev) {
    struct statvfs st {};
    if (::statvfs("/boot", &st) == 0 && st.f_blocks > 0) {
      const double pct = 100.0 * static_cast<double>(st.f_blocks - st.f_bfree) / static_cast<double>(st.f_blocks);
      if (pct >= 80) findings.push_back(message("disk_boot_full", {{"pct", pct}}));
    }
  }
  if (autoremove > 0) {
    findings.push_back(message("disk_autoremove", {{"count", autoremove}}));
    actions.push_back(suggestAction("pkg_autoremove", nullptr, oldBytes, "medium"));
  }
  std::string severity = oldKernels >= 3 ? "warning" : (oldKernels > 0 || autoremove > 0 ? "info" : "ok");
  return {{"severity", severity},
          {"conclusion", message("disk_kernels", {{"old", oldKernels}, {"running", running}, {"bytes", oldBytes}})},
          {"findings", findings},
          {"blocks", {tableBlock("kernels", {{"kernel", "text"}, {"size", "bytes"}, {"running", "text"}}, rows)}},
          {"actions", actions}};
}

}  // namespace

std::vector<CheckSpec> diskChecks() {
  return {{"filesystems", checkFilesystems},
          {"top_dirs", checkTopDirectories},
          {"large_files", checkLargeFiles},
          {"docker", checkDocker},
          {"container_logs", checkContainerLogs},
          {"system_logs", checkSystemLogs},
          {"temp_caches", checkTempAndCaches},
          {"archives_media", checkArchivesAndMedia},
          {"deleted_open", checkDeletedOpenFiles},
          {"kernels_packages", checkKernelsAndPackages}};
}

}  // namespace highcube::agent::audit
