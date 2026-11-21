#include "services/services.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace highcube::admin {
namespace {

using json = nlohmann::json;

constexpr long long kMinuteMs = 60LL * 1000;
constexpr long long kContextMs = 10 * kMinuteMs;
// Must match the agent process-snapshot timer in WsClient::startProcessSnapshots.
constexpr long long kSnapshotIntervalMs = 60 * 1000;

// ---------------------------------------------------------------- time helpers
long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string isoUtc(long long ms) {
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << ".000Z";
  return oss.str();
}

long long parseIsoMs(const std::string& value) {
  std::tm tm{};
  std::istringstream in(value);
  in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (in.fail()) {
    return 0;
  }
  return static_cast<long long>(timegm(&tm)) * 1000;
}

// ---------------------------------------------------------------- result builders
json message(const std::string& code, json params = json::object()) {
  return {{"code", code}, {"params", std::move(params)}};
}

json column(const std::string& key, const std::string& type) { return {{"key", key}, {"type", type}}; }

json tableBlock(const std::string& title, json columns, json rows) {
  return {{"kind", "table"}, {"title", title}, {"columns", std::move(columns)}, {"rows", std::move(rows)}};
}

json lineBlock(const std::string& title, json points, const std::string& unit, json highlight = nullptr) {
  json block = {{"kind", "line"}, {"title", title}, {"points", std::move(points)}, {"unit", unit}};
  if (!highlight.is_null()) block["highlight"] = std::move(highlight);
  return block;
}

json barsBlock(const std::string& title, json items, const std::string& unit) {
  return {{"kind", "bars"}, {"title", title}, {"items", std::move(items)}, {"unit", unit}};
}

json kv(const std::string& key, json value, const std::string& type) {
  return {{"key", key}, {"value", std::move(value)}, {"type", type}};
}

json skipped(const std::string& code, json params = json::object()) {
  return {{"status", "skipped"}, {"severity", "info"}, {"conclusion", message(code, std::move(params))}};
}

int severityRank(const std::string& severity) {
  if (severity == "critical") return 3;
  if (severity == "warning") return 2;
  if (severity == "info") return 1;
  return 0;
}

int riskRank(const std::string& risk) {
  if (risk == "low") return 0;
  if (risk == "medium") return 1;
  return 2;
}

// ---------------------------------------------------------------- statistics
struct Regression {
  double slope = 0;
  double intercept = 0;
  double r2 = 0;
};

Regression regress(const std::vector<double>& xs, const std::vector<double>& ys) {
  Regression result;
  const auto n = static_cast<double>(xs.size());
  if (xs.size() < 2) return result;
  double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
  for (std::size_t i = 0; i < xs.size(); ++i) {
    sx += xs[i];
    sy += ys[i];
    sxx += xs[i] * xs[i];
    sxy += xs[i] * ys[i];
    syy += ys[i] * ys[i];
  }
  const double denom = n * sxx - sx * sx;
  if (std::abs(denom) < 1e-12) return result;
  result.slope = (n * sxy - sx * sy) / denom;
  result.intercept = (sy - result.slope * sx) / n;
  const double varY = n * syy - sy * sy;
  result.r2 = varY > 1e-12 ? std::pow(n * sxy - sx * sy, 2) / (denom * varY) : 0;
  return result;
}

double pearson(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() != b.size() || a.size() < 3) return 0;
  const auto r = regress(a, b);
  const double sign = r.slope >= 0 ? 1 : -1;
  return sign * std::sqrt(std::max(0.0, r.r2));
}

// ---------------------------------------------------------------- snapshots
struct Proc {
  std::string key;
  std::string name;
  std::string container;
  std::string cmd;
  int pid = 0;
  unsigned long long start = 0;
  double cpu = 0;
  double rssKb = 0;
};

struct Snap {
  long long at = 0;
  double user = 0, system = 0, iowait = 0, steal = 0, idle = 100;
  int cores = 1;
  double load1 = 0;
  double memTotalKb = 0, memAvailableKb = 0;
  std::vector<Proc> procs;
};

std::vector<Snap> loadSnapshots(AppContext& ctx, int64_t serverId, long long fromMs, long long toMs) {
  std::vector<Snap> snaps;
  for (const auto& row : ctx.repos->listProcessSnapshots(serverId, isoUtc(fromMs), isoUtc(toMs))) {
    const auto payload = json::parse(row.payloadJson, nullptr, false);
    if (!payload.is_object()) continue;
    Snap snap;
    snap.at = parseIsoMs(row.collectedAt);
    const auto cpu = payload.value("cpu", json::object());
    snap.user = cpu.value("user", 0.0);
    snap.system = cpu.value("system", 0.0);
    snap.iowait = cpu.value("iowait", 0.0);
    snap.steal = cpu.value("steal", 0.0);
    snap.idle = cpu.value("idle", 100.0);
    snap.cores = payload.value("cores", 1);
    const auto load = payload.value("load", json::array());
    snap.load1 = load.is_array() && !load.empty() && load[0].is_number() ? load[0].get<double>() : 0;
    const auto mem = payload.value("mem", json::object());
    snap.memTotalKb = mem.value("totalKb", 0.0);
    snap.memAvailableKb = mem.value("availableKb", 0.0);
    for (const auto& item : payload.value("procs", json::array())) {
      if (!item.is_object()) continue;
      Proc proc;
      proc.name = item.value("name", std::string{});
      proc.container = item.value("container", std::string{});
      proc.cmd = item.value("cmd", std::string{});
      proc.pid = item.value("pid", 0);
      proc.start = item.value("start", 0ULL);
      proc.cpu = item.value("cpu", 0.0);
      proc.rssKb = item.value("rssKb", 0.0);
      proc.key = proc.name + "|" + proc.container;
      snap.procs.push_back(std::move(proc));
    }
    snaps.push_back(std::move(snap));
  }
  return snaps;
}

// "hc-<application_id>-<replica_id>" -> application name, otherwise the container name.
std::string friendlyContainer(AppContext& ctx, const std::string& container) {
  static const std::regex pattern(R"(^hc-(\d+)-(\d+)$)");
  std::smatch match;
  if (std::regex_match(container, match, pattern)) {
    try {
      if (auto app = ctx.repos->findApplicationById(std::stoll(match[1].str()))) {
        return app->name + " (" + container + ")";
      }
    } catch (const std::exception&) {
    }
  }
  return container;
}

// ---------------------------------------------------------------- jobs
struct AuditJob {
  int64_t serverId = 0;
  std::string kind;   // audit | action
  std::string audit;  // disk | cpu | ram
  long long fromMs = 0;
  long long toMs = 0;
  std::vector<std::string> plan;
  std::set<std::string> adminChecks;
  std::map<std::string, json> checks;
  std::string agentRequestId;
  std::size_t agentConsumed = 0;
  bool agentDone = true;
  std::string agentError;
  std::size_t adminPending = 0;
  bool finished = false;
  json summary;
  json context = json::object();
  json actionResult;
  std::string actionStatus;  // pending | ok | error
  std::string actionError;
  std::chrono::steady_clock::time_point createdAt;
};

std::mutex& jobsMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, std::shared_ptr<AuditJob>>& jobs() {
  static std::unordered_map<std::string, std::shared_ptr<AuditJob>> map;
  return map;
}

std::string newJobId() {
  static std::uint64_t seq = 0;
  return "audit-" + std::to_string(++seq) + "-" + randomToken(6);
}

void pruneJobs() {
  const auto now = std::chrono::steady_clock::now();
  auto& map = jobs();
  for (auto it = map.begin(); it != map.end();) {
    if (now - it->second->createdAt > std::chrono::minutes(60)) {
      it = map.erase(it);
    } else {
      ++it;
    }
  }
}

const std::vector<std::string>& planFor(const std::string& audit) {
  static const std::vector<std::string> disk = {"filesystems",   "growth_trend",   "top_dirs",       "large_files",
                                                "docker",        "container_logs", "system_logs",    "temp_caches",
                                                "archives_media", "deleted_open",  "kernels_packages"};
  static const std::vector<std::string> cpu = {"window_chart",      "cpu_breakdown", "process_ranking", "container_attribution",
                                               "platform_events",   "atop_window",   "system_events",   "cgroup_throttling",
                                               "pressure",          "live_state",    "verdict"};
  static const std::vector<std::string> ram = {"window_chart",    "process_ranking", "leak_detection", "container_attribution",
                                               "platform_events", "atop_window",     "oom_events",     "memory_limits",
                                               "swap_pressure",   "system_events",   "live_state",     "verdict"};
  if (audit == "cpu") return cpu;
  if (audit == "ram") return ram;
  return disk;
}

const std::set<std::string>& adminCheckIds() {
  static const std::set<std::string> ids = {"growth_trend",          "window_chart",    "cpu_breakdown",
                                            "process_ranking",       "leak_detection",  "container_attribution",
                                            "platform_events",       "verdict"};
  return ids;
}

const std::set<std::string>& allowedActions() {
  static const std::set<std::string> ids = {
      "docker_images_prune", "docker_builder_prune", "docker_containers_prune", "docker_volumes_prune",
      "container_log_truncate", "tmp_clean",          "pkg_cache_clean",         "pkg_autoremove",
      "journal_vacuum",       "rotated_logs_clean",   "user_caches_clean",       "restart_container",
      "terminate_process",    "drop_page_cache"};
  return ids;
}

// ---------------------------------------------------------------- admin checks
class AdminChecks {
public:
  AdminChecks(AppContext& ctx, AuditJob& job) : ctx_(ctx), job_(job) {}

  json run(const std::string& id) {
    if (id == "growth_trend") return growthTrend();
    if (id == "window_chart") return windowChart();
    if (id == "cpu_breakdown") return cpuBreakdown();
    if (id == "process_ranking") return processRanking();
    if (id == "leak_detection") return leakDetection();
    if (id == "container_attribution") return containerAttribution();
    if (id == "platform_events") return platformEvents();
    throw std::invalid_argument("Unknown check");
  }

  json verdict();

private:
  AppContext& ctx_;
  AuditJob& job_;
  bool memory() const { return job_.audit == "ram"; }

  const std::vector<Snap>& windowSnapshots() {
    if (!snapshotsLoaded_) {
      snapshots_ = loadSnapshots(ctx_, job_.serverId, job_.fromMs, job_.toMs);
      snapshotsLoaded_ = true;
      const double expected = std::max(1.0, static_cast<double>(job_.toMs - job_.fromMs) / kSnapshotIntervalMs);
      job_.context["coverage"] = std::min(1.0, static_cast<double>(snapshots_.size()) / expected);
    }
    return snapshots_;
  }

  json growthTrend();
  json windowChart();
  json cpuBreakdown();
  json processRanking();
  json leakDetection();
  json containerAttribution();
  json platformEvents();

  bool snapshotsLoaded_ = false;
  std::vector<Snap> snapshots_;
};

json AdminChecks::growthTrend() {
  const auto to = nowMs();
  const auto from = to - 7LL * 24 * 3600 * 1000;
  const auto rows = ctx_.repos->listServerMetricsSampled(job_.serverId, isoUtc(from), isoUtc(to), 300);
  std::vector<double> xs, ys;
  json points = json::array();
  double total = 0;
  for (const auto& row : rows) {
    const auto payload = json::parse(row.payloadJson, nullptr, false);
    if (!payload.is_object() || !payload.contains("disk") || !payload["disk"].is_object()) continue;
    const double size = payload["disk"].value("totalBytes", 0.0);
    const double available = payload["disk"].value("availableBytes", 0.0);
    if (size <= 0) continue;
    total = size;
    const double pct = 100.0 * (size - available) / size;
    const auto at = parseIsoMs(row.collectedAt);
    xs.push_back(static_cast<double>(at - from) / (24.0 * 3600 * 1000));
    ys.push_back(pct);
    points.push_back({{"at", row.collectedAt}, {"value", pct}});
  }
  if (xs.size() < 10 || xs.back() - xs.front() < 0.25) {
    return {{"severity", "info"}, {"conclusion", message("disk_trend_insufficient")}, {"blocks", {lineBlock("diskUsageTrend", points, "percent")}}};
  }
  const auto fit = regress(xs, ys);
  const double current = ys.back();
  const double ratePerDayBytes = fit.slope / 100.0 * total;
  std::string severity = "ok";
  json conclusion;
  if (fit.slope > 0.01) {
    const double days = (100.0 - current) / fit.slope;
    severity = days < 7 ? "critical" : (days < 30 ? "warning" : "ok");
    conclusion = message("disk_trend_filling", {{"days", days}, {"rateBytes", ratePerDayBytes}, {"pct", current}});
  } else {
    conclusion = message("disk_trend_stable", {{"pct", current}, {"rateBytes", ratePerDayBytes}});
  }
  return {{"severity", severity},
          {"conclusion", conclusion},
          {"blocks", {lineBlock("diskUsageTrend", points, "percent")}}};
}

json AdminChecks::windowChart() {
  const long long ctxFrom = job_.fromMs - kContextMs;
  const long long ctxTo = std::min(nowMs(), job_.toMs + kContextMs);
  const auto chart = ServerService(ctx_).metrics(job_.serverId, isoUtc(ctxFrom), isoUtc(ctxTo));
  const std::string field = memory() ? "memory" : "cpu";
  json points = json::array();
  double sum = 0, peak = 0, baselineSum = 0;
  int count = 0, baselineCount = 0;
  std::string peakAt;
  const auto& raw = chart.value("points", json::array());
  const double bucketMinutes =
      raw.empty() ? 0 : static_cast<double>(ctxTo - ctxFrom) / kMinuteMs / static_cast<double>(raw.size());
  const double threshold = memory() ? 90 : 80;
  double aboveMinutes = 0;
  for (const auto& point : raw) {
    if (!point.contains(field) || !point[field].is_number()) continue;
    const double value = point[field].get<double>();
    const auto atText = point.value("at", std::string{});
    const auto at = parseIsoMs(atText);
    points.push_back({{"at", atText}, {"value", value}});
    if (at >= job_.fromMs && at <= job_.toMs) {
      sum += value;
      ++count;
      if (value > peak) {
        peak = value;
        peakAt = atText;
      }
      if (value >= threshold) aboveMinutes += bucketMinutes;
    } else {
      baselineSum += value;
      ++baselineCount;
    }
  }
  const json highlight = {{"from", isoUtc(job_.fromMs)}, {"to", isoUtc(job_.toMs)}};
  if (count == 0) {
    job_.context["windowPeak"] = 0;
    return {{"severity", "info"},
            {"conclusion", message("res_window_nodata")},
            {"blocks", {lineBlock(memory() ? "memoryWindow" : "cpuWindow", points, "percent", highlight)}}};
  }
  const double avg = sum / count;
  const double baseline = baselineCount > 0 ? baselineSum / baselineCount : avg;
  job_.context["windowPeak"] = peak;
  job_.context["windowPeakAt"] = peakAt;
  job_.context["windowAvg"] = avg;
  const double warning = memory() ? 85 : 75;
  const double critical = memory() ? 95 : 90;
  const std::string severity = peak >= critical ? "critical" : (peak >= warning ? "warning" : "ok");
  json findings = json::array();
  if (avg > baseline * 1.5 && avg - baseline >= 10) {
    findings.push_back(message("res_window_above_baseline", {{"avg", avg}, {"baseline", baseline}}));
  }
  return {{"severity", severity},
          {"conclusion", severity == "ok"
                             ? message("res_window_calm", {{"peak", peak}, {"avg", avg}})
                             : message("res_window_peak", {{"peak", peak}, {"peakAt", peakAt}, {"avg", avg},
                                                           {"aboveMinutes", aboveMinutes}, {"baseline", baseline}})},
          {"findings", findings},
          {"blocks", {lineBlock(memory() ? "memoryWindow" : "cpuWindow", points, "percent", highlight)}}};
}

json AdminChecks::cpuBreakdown() {
  const auto& snaps = windowSnapshots();
  if (snaps.empty()) return skipped("res_no_snapshots");
  double user = 0, system = 0, iowait = 0, steal = 0, load = 0;
  int cores = 1;
  for (const auto& snap : snaps) {
    user += snap.user;
    system += snap.system;
    iowait += snap.iowait;
    steal += snap.steal;
    load += snap.load1;
    cores = std::max(cores, snap.cores);
  }
  const double n = static_cast<double>(snaps.size());
  user /= n;
  system /= n;
  iowait /= n;
  steal /= n;
  load /= n;
  std::string code = "res_breakdown_user";
  double pct = user;
  std::string severity = "info";
  if (iowait >= 20) {
    code = "res_breakdown_iowait";
    pct = iowait;
    severity = "warning";
  } else if (steal >= 10) {
    code = "res_breakdown_steal";
    pct = steal;
    severity = "warning";
  } else if (system > user && system >= 15) {
    code = "res_breakdown_kernel";
    pct = system;
  }
  job_.context["breakdown"] = code;
  json findings = json::array();
  if (load > cores * 1.5) {
    findings.push_back(message("res_load_high", {{"load", load}, {"cores", cores}}));
  }
  return {{"severity", severity},
          {"conclusion", message(code, {{"pct", pct}})},
          {"findings", findings},
          {"blocks", {barsBlock("cpuBreakdown",
                                json::array({{{"label", "user"}, {"value", user}},
                                             {{"label", "system"}, {"value", system}},
                                             {{"label", "iowait"}, {"value", iowait}},
                                             {{"label", "steal"}, {"value", steal}}}),
                                "percent"),
                      {{"kind", "kv"},
                       {"title", "loadSummary"},
                       {"items", json::array({kv("load1", load, "number"), kv("cores", cores, "number"),
                                              kv("samples", snaps.size(), "number")})}}}}};
}

json AdminChecks::processRanking() {
  const auto& snaps = windowSnapshots();
  if (snaps.empty()) return skipped("res_no_snapshots");
  const auto n = snaps.size();
  struct Agg {
    std::string name, container, cmd;
    int pid = 0;
    unsigned long long start = 0;
    std::vector<double> series;
    double peak = 0;
    int present = 0;
  };
  std::map<std::string, Agg> byKey;
  std::vector<double> host(n, 0);
  double hostSum = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const auto& snap = snaps[i];
    host[i] = memory() ? std::max(0.0, snap.memTotalKb - snap.memAvailableKb) : std::max(0.0, 100.0 - snap.idle);
    hostSum += host[i];
    for (const auto& proc : snap.procs) {
      auto& agg = byKey[proc.key];
      if (agg.series.empty()) {
        agg.series.assign(n, 0);
        agg.name = proc.name;
        agg.container = proc.container;
      }
      const double value = memory() ? proc.rssKb : proc.cpu;
      agg.series[i] += value;
      agg.cmd = proc.cmd;
      agg.pid = proc.pid;
      agg.start = proc.start;
    }
  }
  struct Row {
    Agg* agg;
    double avg, peak, presence, corr, share, growth, score;
  };
  std::vector<Row> rows;
  const double hostAvg = hostSum / static_cast<double>(n);
  const double memTotalKb = snaps.back().memTotalKb;
  for (auto& [key, agg] : byKey) {
    double sum = 0, peak = 0, first = -1, last = 0;
    int present = 0;
    for (const auto value : agg.series) {
      sum += value;
      peak = std::max(peak, value);
      if (value > 0) {
        ++present;
        if (first < 0) first = value;
        last = value;
      }
    }
    const double avg = sum / static_cast<double>(n);
    const double corr = pearson(agg.series, host);
    const double share = hostAvg > 0 ? 100.0 * avg / hostAvg : 0;
    const double growth = first >= 0 ? last - first : 0;
    double score = 0;
    if (memory()) {
      const double avgPct = memTotalKb > 0 ? 100.0 * avg / memTotalKb : 0;
      const double growthPct = memTotalKb > 0 ? 100.0 * std::max(0.0, growth) / memTotalKb : 0;
      score = avgPct * (0.7 + 0.3 * std::max(0.0, corr)) + 0.5 * growthPct;
    } else {
      score = avg * (0.6 + 0.4 * std::max(0.0, corr)) + 0.15 * peak;
    }
    rows.push_back({&agg, avg, peak, 100.0 * present / static_cast<double>(n), corr, share, growth, score});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.score > b.score; });
  json table = json::array();
  json ranking = json::array();
  for (const auto& row : rows) {
    if (table.size() >= 15) break;
    const auto container = row.agg->container.empty() ? std::string("-") : friendlyContainer(ctx_, row.agg->container);
    if (memory()) {
      table.push_back({{"process", row.agg->name},
                       {"container", container},
                       {"avgRss", static_cast<long long>(row.avg * 1024)},
                       {"peakRss", static_cast<long long>(row.peak * 1024)},
                       {"growth", static_cast<long long>(row.growth * 1024)},
                       {"sharePct", row.share},
                       {"presencePct", row.presence},
                       {"correlation", row.corr},
                       {"cmd", row.agg->cmd}});
    } else {
      table.push_back({{"process", row.agg->name},
                       {"container", container},
                       {"avgCpu", row.avg},
                       {"peakCpu", row.peak},
                       {"sharePct", row.share},
                       {"presencePct", row.presence},
                       {"correlation", row.corr},
                       {"cmd", row.agg->cmd}});
    }
    if (ranking.size() < 5) {
      ranking.push_back({{"name", row.agg->name},
                         {"container", row.agg->container},
                         {"avg", memory() ? row.avg * 1024 : row.avg},
                         {"peak", memory() ? row.peak * 1024 : row.peak},
                         {"share", row.share},
                         {"corr", row.corr},
                         {"pid", row.agg->pid},
                         {"start", row.agg->start}});
    }
  }
  job_.context["ranking"] = ranking;
  json findings = json::array();
  const double coverage = job_.context.value("coverage", 1.0);
  if (coverage < 0.8) findings.push_back(message("res_partial_coverage", {{"pct", coverage * 100}}));
  if (rows.empty()) return skipped("res_no_snapshots");
  const auto& top = rows.front();
  json conclusion = memory()
                        ? message("res_ranking_top_ram", {{"name", top.agg->name},
                                                          {"avgBytes", top.avg * 1024},
                                                          {"peakBytes", top.peak * 1024},
                                                          {"share", top.share}})
                        : message("res_ranking_top_cpu", {{"name", top.agg->name},
                                                          {"avg", top.avg},
                                                          {"peak", top.peak},
                                                          {"share", top.share}});
  if (top.corr >= 0.6) findings.push_back(message("res_ranking_correlated", {{"name", top.agg->name}, {"corr", top.corr}}));
  json columns = memory() ? json::array({column("process", "text"), column("container", "text"), column("avgRss", "bytes"),
                                         column("peakRss", "bytes"), column("growth", "bytes"), column("sharePct", "percent"),
                                         column("presencePct", "percent"), column("correlation", "number"), column("cmd", "text")})
                          : json::array({column("process", "text"), column("container", "text"), column("avgCpu", "percent"),
                                         column("peakCpu", "percent"), column("sharePct", "percent"),
                                         column("presencePct", "percent"), column("correlation", "number"), column("cmd", "text")});
  return {{"severity", top.share >= 50 ? "warning" : "info"},
          {"conclusion", conclusion},
          {"findings", findings},
          {"blocks", {tableBlock("processRanking", columns, table)}}};
}

json AdminChecks::leakDetection() {
  const auto to = nowMs();
  const auto snaps = loadSnapshots(ctx_, job_.serverId, to - 24LL * 3600 * 1000, to);
  if (snaps.size() < 20) return skipped("res_leak_insufficient");
  struct Series {
    std::string name, container;
    std::vector<double> xs, ys;
    std::vector<std::pair<long long, double>> points;
  };
  std::map<std::string, Series> byProcess;
  for (const auto& snap : snaps) {
    for (const auto& proc : snap.procs) {
      auto& series = byProcess[std::to_string(proc.pid) + ":" + std::to_string(proc.start)];
      series.name = proc.name;
      series.container = proc.container;
      series.xs.push_back(static_cast<double>(snap.at) / 3600000.0);
      series.ys.push_back(proc.rssKb * 1024);
      series.points.emplace_back(snap.at, proc.rssKb * 1024);
    }
  }
  struct Suspect {
    const Series* series;
    double first, last, rate, r2;
  };
  std::vector<Suspect> suspects;
  for (const auto& [key, series] : byProcess) {
    if (series.xs.size() < 20 || series.xs.back() - series.xs.front() < 1.0) continue;
    const auto fit = regress(series.xs, series.ys);
    const double first = series.ys.front();
    const double last = series.ys.back();
    const double growth = last - first;
    if (fit.slope > 0 && fit.r2 >= 0.8 && growth >= 100.0 * 1024 * 1024 && growth >= 0.2 * first) {
      suspects.push_back({&series, first, last, fit.slope, fit.r2});
    }
  }
  std::sort(suspects.begin(), suspects.end(), [](const Suspect& a, const Suspect& b) { return a.rate > b.rate; });
  if (suspects.empty()) {
    return {{"severity", "ok"}, {"conclusion", message("res_leak_none", {{"processes", byProcess.size()}})}};
  }
  json rows = json::array();
  for (const auto& suspect : suspects) {
    if (rows.size() >= 10) break;
    rows.push_back({{"process", suspect.series->name},
                    {"container", suspect.series->container.empty() ? "-" : friendlyContainer(ctx_, suspect.series->container)},
                    {"firstRss", static_cast<long long>(suspect.first)},
                    {"lastRss", static_cast<long long>(suspect.last)},
                    {"ratePerHour", static_cast<long long>(suspect.rate)},
                    {"r2", suspect.r2}});
  }
  json points = json::array();
  for (const auto& [at, value] : suspects.front().series->points) {
    points.push_back({{"at", isoUtc(at)}, {"value", value}});
  }
  const auto& top = suspects.front();
  job_.context["leak"] = {{"name", top.series->name}, {"container", top.series->container}};
  return {{"severity", "warning"},
          {"conclusion", message("res_leak_suspect", {{"name", top.series->name},
                                                      {"rateBytes", top.rate},
                                                      {"growthBytes", top.last - top.first},
                                                      {"count", suspects.size()}})},
          {"blocks", {tableBlock("leakSuspects",
                                 json::array({column("process", "text"), column("container", "text"),
                                              column("firstRss", "bytes"), column("lastRss", "bytes"),
                                              column("ratePerHour", "bytes"), column("r2", "number")}),
                                 rows),
                      lineBlock("leakTopSeries", points, "bytes")}}};
}

json AdminChecks::containerAttribution() {
  const auto& snaps = windowSnapshots();
  if (snaps.empty()) return skipped("res_no_snapshots");
  std::map<std::string, double> totals;
  for (const auto& snap : snaps) {
    for (const auto& proc : snap.procs) {
      totals[proc.container] += memory() ? proc.rssKb * 1024 : proc.cpu;
    }
  }
  const double n = static_cast<double>(snaps.size());
  std::vector<std::pair<std::string, double>> sorted;
  double sum = 0;
  for (const auto& [container, total] : totals) {
    sorted.emplace_back(container, total / n);
    sum += total / n;
  }
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  json bars = json::array();
  for (const auto& [container, value] : sorted) {
    if (bars.size() >= 10) break;
    bars.push_back({{"label", container.empty() ? std::string("host") : friendlyContainer(ctx_, container)}, {"value", value}});
  }
  if (sorted.empty()) return skipped("res_no_snapshots");
  const auto& top = sorted.front();
  const double share = sum > 0 ? 100.0 * top.second / sum : 0;
  return {{"severity", "info"},
          {"conclusion", message(top.first.empty() ? "res_containers_host" : "res_containers_top",
                                 {{"name", top.first.empty() ? std::string("host") : friendlyContainer(ctx_, top.first)},
                                  {"share", share}})},
          {"blocks", {barsBlock(memory() ? "memoryByContainer" : "cpuByContainer", bars, memory() ? "bytes" : "percent")}}};
}

json AdminChecks::platformEvents() {
  const auto events = ctx_.repos->listServerEvents(job_.serverId, isoUtc(job_.fromMs - kContextMs),
                                                   isoUtc(job_.toMs + kContextMs));
  if (events.empty()) {
    return {{"severity", "ok"}, {"conclusion", message("res_platform_none")}};
  }
  json rows = json::array();
  int deployments = 0, incidents = 0;
  std::string firstApp;
  for (const auto& event : events) {
    if (event.kind == "deployment") {
      ++deployments;
      if (firstApp.empty()) firstApp = event.applicationName;
    } else {
      ++incidents;
    }
    if (rows.size() < 40) {
      rows.push_back({{"at", event.at}, {"kind", event.kind}, {"application", event.applicationName}, {"detail", event.detail}});
    }
  }
  job_.context["deployments"] = deployments;
  job_.context["deployApp"] = firstApp;
  return {{"severity", incidents > 0 ? "warning" : "info"},
          {"conclusion", message("res_platform_events", {{"deployments", deployments}, {"incidents", incidents}})},
          {"blocks", {tableBlock("platformEvents",
                                 json::array({column("at", "date"), column("kind", "text"), column("application", "text"),
                                              column("detail", "text")}),
                                 rows)}}};
}

json AdminChecks::verdict() {
  const double peak = job_.context.value("windowPeak", 0.0);
  const double calmThreshold = memory() ? 80 : 60;
  json findings = json::array();
  json actions = json::array();
  auto checkRaw = [&](const std::string& id) -> json {
    const auto it = job_.checks.find(id);
    if (it == job_.checks.end() || !it->second.contains("raw")) return json::array();
    return it->second["raw"];
  };
  const auto ranking = job_.context.value("ranking", json::array());
  const auto atop = checkRaw("atop_window");
  const auto live = checkRaw("live_state");
  const auto breakdown = job_.context.value("breakdown", std::string{});

  if (peak > 0 && peak < calmThreshold && breakdown != "res_breakdown_iowait" && breakdown != "res_breakdown_steal") {
    return {{"severity", "ok"}, {"conclusion", message("res_verdict_calm", {{"peak", peak}})}};
  }

  std::string name, container;
  double avg = 0, share = 0, corr = 0;
  std::string source;
  if (!ranking.empty()) {
    name = ranking[0].value("name", std::string{});
    container = ranking[0].value("container", std::string{});
    avg = ranking[0].value("avg", 0.0);
    share = ranking[0].value("share", 0.0);
    corr = ranking[0].value("corr", 0.0);
    source = "snapshots";
  } else if (atop.is_array() && !atop.empty()) {
    name = atop[0].value("name", std::string{});
    avg = atop[0].value("avg", 0.0);
    source = "atop";
  } else if (live.is_array() && !live.empty()) {
    name = live[0].value("name", std::string{});
    container = live[0].value("container", std::string{});
    source = "live";
  }
  if (name.empty()) {
    return {{"severity", "info"}, {"conclusion", message("res_verdict_unknown")}};
  }

  int level = 0;  // 0 low, 1 medium, 2 high
  if (source == "snapshots") {
    if (memory()) {
      level = share >= 40 ? 2 : (share >= 20 ? 1 : 0);
    } else {
      level = share >= 50 && corr >= 0.6 ? 2 : (share >= 25 || corr >= 0.4 ? 1 : 0);
    }
  }
  if (atop.is_array() && !atop.empty() && atop[0].value("name", std::string{}) == name) {
    level = std::min(2, level + 1);
    findings.push_back(message("res_verdict_atop_agrees", {{"name", name}}));
  }
  static const char* levels[] = {"low", "medium", "high"};

  if (breakdown == "res_breakdown_iowait") findings.push_back(message("res_verdict_iowait"));
  if (breakdown == "res_breakdown_steal") findings.push_back(message("res_verdict_steal"));

  bool stillRunning = false;
  json liveMatch;
  if (live.is_array()) {
    for (const auto& item : live) {
      if (item.value("name", std::string{}) == name) {
        stillRunning = true;
        liveMatch = item;
        break;
      }
    }
  }
  findings.push_back(message(stillRunning ? "res_verdict_still_running" : "res_verdict_gone", {{"name", name}}));
  if (job_.context.value("deployments", 0) > 0) {
    findings.push_back(message("res_verdict_deploy", {{"app", job_.context.value("deployApp", std::string{})},
                                                      {"count", job_.context.value("deployments", 0)}}));
  }
  if (memory()) {
    const auto oom = job_.checks.find("oom_events");
    if (oom != job_.checks.end() && oom->second.value("severity", std::string{}) == "critical") {
      findings.push_back(message("res_verdict_oom"));
    }
    if (job_.context.contains("leak") && job_.context["leak"].value("name", std::string{}) == name) {
      findings.push_back(message("res_verdict_leak", {{"name", name}}));
      level = std::min(2, level + 1);
    }
  }

  if (stillRunning) {
    const auto liveContainer = liveMatch.value("container", std::string{});
    if (!liveContainer.empty()) {
      actions.push_back({{"id", "restart_container"},
                         {"target", {{"name", liveContainer}}},
                         {"estimateBytes", 0},
                         {"risk", "medium"}});
    } else if (liveMatch.value("pid", 0) > 1 && liveMatch.value("cmd", std::string{}).rfind('[', 0) != 0) {
      actions.push_back({{"id", "terminate_process"},
                         {"target", {{"pid", liveMatch.value("pid", 0)},
                                     {"start", liveMatch.value("start", 0ULL)},
                                     {"name", name}}},
                         {"estimateBytes", 0},
                         {"risk", "high"}});
    }
  }
  const auto displayContainer = container.empty() ? std::string("host") : friendlyContainer(ctx_, container);
  json params = {{"name", name},
                 {"container", displayContainer},
                 {"confidence", levels[level]},
                 {"source", source},
                 {"share", share}};
  if (memory()) {
    params["avgBytes"] = avg;
  } else {
    params["avg"] = avg;
  }
  return {{"severity", peak >= (memory() ? 95 : 90) ? "critical" : "warning"},
          {"conclusion", message(memory() ? "res_verdict_culprit_ram" : "res_verdict_culprit_cpu", params)},
          {"findings", findings},
          {"actions", actions},
          {"culprit", {{"name", name}, {"container", displayContainer}, {"confidence", levels[level]}}}};
}

json buildSummary(const AuditJob& job) {
  int score = 100;
  std::vector<json> ranked;
  json actions = json::array();
  std::set<std::string> seenActions;
  long long reclaimable = 0;
  for (const auto& id : job.plan) {
    const auto it = job.checks.find(id);
    if (it == job.checks.end()) continue;
    const auto& check = it->second;
    const auto severity = check.value("severity", std::string{"info"});
    const auto status = check.value("status", std::string{});
    if (severity == "critical") score -= 25;
    else if (severity == "warning") score -= 10;
    if (status == "error") score -= 3;
    if ((severity == "critical" || severity == "warning") && id != "verdict") {
      ranked.push_back({{"checkId", id}, {"severity", severity}, {"conclusion", check.value("conclusion", json::object())}});
    }
    for (const auto& action : check.value("actions", json::array())) {
      const auto key = action.value("id", std::string{}) + action.value("target", json()).dump();
      if (!seenActions.insert(key).second) continue;
      auto copy = action;
      copy["checkId"] = id;
      actions.push_back(copy);
      if (job.audit == "disk") reclaimable += action.value("estimateBytes", 0LL);
    }
  }
  std::stable_sort(ranked.begin(), ranked.end(), [](const json& a, const json& b) {
    return severityRank(a.value("severity", std::string{})) > severityRank(b.value("severity", std::string{}));
  });
  json keyFindings = json::array();
  for (const auto& item : ranked) {
    if (keyFindings.size() >= 5) break;
    keyFindings.push_back(item);
  }
  std::vector<json> sortedActions(actions.begin(), actions.end());
  std::stable_sort(sortedActions.begin(), sortedActions.end(), [](const json& a, const json& b) {
    const auto ea = a.value("estimateBytes", 0LL);
    const auto eb = b.value("estimateBytes", 0LL);
    if (ea != eb) return ea > eb;
    return riskRank(a.value("risk", std::string{})) < riskRank(b.value("risk", std::string{}));
  });
  json summary = {{"score", std::clamp(score, 0, 100)},
                  {"keyFindings", keyFindings},
                  {"actions", json(sortedActions)},
                  {"reclaimableBytes", reclaimable}};
  if (job.audit != "disk") {
    summary["coverage"] = job.context.value("coverage", 0.0);
    const auto verdict = job.checks.find("verdict");
    if (verdict != job.checks.end()) {
      summary["verdict"] = verdict->second.value("conclusion", json::object());
    }
  }
  return summary;
}

// Runs one admin-side check on the io thread, then schedules the next one so the
// event loop keeps serving requests between checks.
void runAdminChecks(AppContext& ctx, std::shared_ptr<AuditJob> job, std::vector<std::string> pending) {
  if (pending.empty()) return;
  const auto id = pending.front();
  pending.erase(pending.begin());
  net::post(ctx.agentHub->ioContext(), [&ctx, job, id, pending]() mutable {
    {
      std::lock_guard lock(jobsMutex());
      job->checks[id] = {{"id", id}, {"status", "running"}};
    }
    const auto started = std::chrono::steady_clock::now();
    json result;
    try {
      AdminChecks checks(ctx, *job);
      result = checks.run(id);
    } catch (const std::exception& ex) {
      spdlog::warn("admin audit check {} failed: {}", id, ex.what());
      result = {{"status", "error"}, {"severity", "info"}, {"conclusion", message("check_error", {{"message", ex.what()}})}};
    }
    if (!result.contains("status")) result["status"] = "done";
    result["id"] = id;
    result["durationMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    {
      std::lock_guard lock(jobsMutex());
      job->checks[id] = result;
      if (job->adminPending > 0) --job->adminPending;
    }
    runAdminChecks(ctx, job, pending);
  });
}

void finalize(AppContext& ctx, AuditJob& job) {
  if (job.finished || job.adminPending > 0 || !job.agentDone) return;
  if (std::find(job.plan.begin(), job.plan.end(), "verdict") != job.plan.end()) {
    // Window-dependent context (ranking, peak) must exist even when only the verdict reruns.
    AdminChecks checks(ctx, job);
    json result;
    try {
      result = checks.verdict();
    } catch (const std::exception& ex) {
      result = {{"severity", "info"}, {"conclusion", message("check_error", {{"message", ex.what()}})}};
    }
    result["id"] = "verdict";
    result["status"] = "done";
    job.checks["verdict"] = result;
  }
  job.summary = buildSummary(job);
  job.finished = true;
}

}  // namespace

AuditService::AuditService(AppContext& ctx) : ctx_(ctx) {}

json AuditService::start(int64_t serverId, const json& body) {
  if (!ctx_.repos->findServerById(serverId)) throw std::runtime_error("Server not found");
  if (!ctx_.agentHub || !ctx_.agentHub->isConnected(serverId)) throw std::runtime_error("The server agent is offline");
  const auto audit = body.value("audit", std::string{});
  if (audit != "disk" && audit != "cpu" && audit != "ram") throw std::invalid_argument("Unknown audit");

  auto job = std::make_shared<AuditJob>();
  job->serverId = serverId;
  job->kind = "audit";
  job->audit = audit;
  job->createdAt = std::chrono::steady_clock::now();
  if (audit != "disk") {
    job->fromMs = body.value("from", 0LL);
    job->toMs = body.value("to", 0LL);
    if (job->fromMs <= 0 || job->toMs <= job->fromMs) throw std::invalid_argument("Choose a valid time window");
    if (job->toMs - job->fromMs > 7LL * 24 * 3600 * 1000) throw std::invalid_argument("The window cannot exceed 7 days");
    if (job->fromMs > nowMs()) throw std::invalid_argument("The window starts in the future");
  }
  const auto only = body.value("only", std::string{});
  const auto& fullPlan = planFor(audit);
  if (!only.empty()) {
    if (std::find(fullPlan.begin(), fullPlan.end(), only) == fullPlan.end() || only == "verdict") {
      throw std::invalid_argument("Unknown check");
    }
    job->plan = {only};
  } else {
    job->plan = fullPlan;
  }

  std::vector<std::string> adminPending;
  bool needsAgent = false;
  for (const auto& id : job->plan) {
    if (id == "verdict") continue;
    if (adminCheckIds().count(id)) {
      adminPending.push_back(id);
    } else {
      needsAgent = true;
    }
  }
  job->adminPending = adminPending.size();
  if (needsAgent) {
    json payload = json::object();
    if (audit != "disk") {
      payload["from"] = job->fromMs;
      payload["to"] = std::min(job->toMs, nowMs());
    }
    if (!only.empty()) payload["only"] = only;
    job->agentRequestId = ctx_.agentHub->startQuickCommand(serverId, "audit", audit, payload.dump());
    job->agentDone = false;
  }
  const auto id = newJobId();
  {
    std::lock_guard lock(jobsMutex());
    pruneJobs();
    jobs()[id] = job;
  }
  runAdminChecks(ctx_, job, adminPending);
  return {{"jobId", id}, {"plan", job->plan}};
}

json AuditService::startAction(int64_t serverId, const json& body) {
  if (!ctx_.repos->findServerById(serverId)) throw std::runtime_error("Server not found");
  const auto actionId = body.value("actionId", std::string{});
  if (!allowedActions().count(actionId)) throw std::invalid_argument("Unknown action");
  const auto target = body.value("target", json());

  auto job = std::make_shared<AuditJob>();
  job->serverId = serverId;
  job->kind = "action";
  job->createdAt = std::chrono::steady_clock::now();
  job->actionStatus = "pending";

  // High Cube replicas restart through the orchestrator so its state stays consistent.
  static const std::regex replicaName(R"(^hc-(\d+)-(\d+)$)");
  std::smatch match;
  const auto name = target.is_object() ? target.value("name", std::string{}) : std::string{};
  if (actionId == "restart_container" && std::regex_match(name, match, replicaName)) {
    const auto replicaId = std::stoll(match[2].str());
    const auto replica = ctx_.repos->findReplicaById(replicaId);
    if (!replica || replica->serverId != serverId) throw std::invalid_argument("Replica not found on this server");
    ctx_.deployments->restartReplica(replicaId);
    job->actionStatus = "ok";
    job->actionResult = {{"ok", true}, {"metric", "none"}, {"output", "Replica restart requested"}};
  } else {
    if (!ctx_.agentHub || !ctx_.agentHub->isConnected(serverId)) throw std::runtime_error("The server agent is offline");
    job->agentRequestId = ctx_.agentHub->startQuickCommand(serverId, "action", actionId, json{{"target", target}}.dump());
    job->agentDone = false;
  }
  const auto id = newJobId();
  {
    std::lock_guard lock(jobsMutex());
    pruneJobs();
    jobs()[id] = job;
  }
  return {{"jobId", id}};
}

json AuditService::poll(int64_t serverId, const std::string& jobId) {
  std::shared_ptr<AuditJob> job;
  {
    std::lock_guard lock(jobsMutex());
    const auto it = jobs().find(jobId);
    if (it == jobs().end() || it->second->serverId != serverId) throw std::runtime_error("The analysis was not found");
    job = it->second;
  }
  std::lock_guard lock(jobsMutex());
  if (!job->agentDone && !job->agentRequestId.empty()) {
    const auto agent = ctx_.agentHub->pollQuickCommand(job->agentRequestId, serverId, job->agentConsumed);
    job->agentConsumed = agent.progressTotal;
    for (const auto& item : agent.progress) {
      auto check = json::parse(item, nullptr, false);
      if (check.is_object() && check.contains("id")) {
        job->checks[check["id"].get<std::string>()] = check;
      }
    }
    if (agent.status != "pending") {
      job->agentDone = true;
      if (agent.status != "ok") {
        job->agentError = agent.error.empty() ? "The agent could not finish the analysis" : agent.error;
      }
      if (job->kind == "action") {
        job->actionStatus = agent.status == "ok" ? "ok" : "error";
        job->actionError = job->agentError;
        job->actionResult = json::parse(agent.resultJson.empty() ? "{}" : agent.resultJson, nullptr, false);
      }
    }
  }
  if (job->kind == "action") {
    json body = {{"kind", "action"}, {"status", job->actionStatus == "pending" ? "running" : job->actionStatus}};
    if (!job->actionResult.is_null() && !job->actionResult.is_discarded()) body["result"] = job->actionResult;
    if (!job->actionError.empty()) body["error"] = job->actionError;
    return body;
  }
  if (job->agentDone && !job->agentError.empty()) {
    for (const auto& id : job->plan) {
      if (adminCheckIds().count(id)) continue;
      auto& check = job->checks[id];
      const auto status = check.value("status", std::string{});
      if (status != "done" && status != "skipped" && status != "error") {
        check = {{"id", id},
                 {"status", "error"},
                 {"severity", "info"},
                 {"conclusion", message("check_error", {{"message", job->agentError}})}};
      }
    }
  }
  finalize(ctx_, *job);
  json checks = json::array();
  for (const auto& id : job->plan) {
    const auto it = job->checks.find(id);
    checks.push_back(it == job->checks.end() ? json{{"id", id}, {"status", "pending"}} : it->second);
  }
  json body = {{"kind", "audit"},
               {"audit", job->audit},
               {"status", job->finished ? "done" : "running"},
               {"plan", job->plan},
               {"checks", checks}};
  if (job->audit != "disk") body["window"] = {{"from", job->fromMs}, {"to", job->toMs}};
  if (job->finished) body["summary"] = job->summary;
  return body;
}

}  // namespace highcube::admin
