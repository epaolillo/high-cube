#include "services/services.hpp"

#include <boost/process.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace bp = boost::process;

namespace {

std::string isoUtc(std::chrono::system_clock::time_point tp) {
  const auto tt = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  gmtime_r(&tt, &tm);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << ".000Z";
  return oss.str();
}

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool isValidSubdomainLabel(const std::string& label) {
  if (label.empty() || label.size() > 63) {
    return false;
  }
  if (label.front() == '-' || label.back() == '-') {
    return false;
  }
  for (unsigned char c : label) {
    if (!(std::islower(c) || std::isdigit(c) || c == '-')) {
      return false;
    }
  }
  return true;
}

}  // namespace

namespace highcube::admin {

ApplicationService::ApplicationService(AppContext& ctx) : ctx_(ctx) {}

namespace {

nlohmann::json applicationJsonWithDomains(AppContext& ctx, const Application& app) {
  auto json = applicationToJson(app);
  nlohmann::json domains = nlohmann::json::array();
  if (app.createdBy > 0) {
    for (const auto& domain : ctx.repos->listBaseDomainsForUser(app.createdBy)) {
      domains.push_back({{"id", domain.id}, {"hostname", domain.hostname}});
    }
  }
  json["availableBaseDomains"] = domains;
  return json;
}

}  // namespace

Application ApplicationService::requireAccess(int64_t id, const User& actor) const {
  auto app = ctx_.repos->findApplicationById(id);
  if (!app) {
    throw std::runtime_error("Application not found");
  }
  if (!ctx_.auth->canAccessApplication(actor, *app)) {
    throw ForbiddenError();
  }
  return *app;
}

Application ApplicationService::parseBody(const nlohmann::json& body, const Application* existing,
                                          const User& actor) const {
  Application app = existing ? *existing : Application{};
  app.name = body.value("name", app.name);
  // "type" classifies the application by category. Legacy rows used "web" and
  // are normalized to "other". Unknown values fall back to "other".
  {
    static const std::unordered_set<std::string> kCategories = {
        "other", "db", "storage", "ai_ml", "networking", "monitoring", "security"};
    std::string category = body.value("type", app.type.empty() ? "other" : app.type);
    if (kCategories.find(category) == kCategories.end()) {
      category = "other";
    }
    app.type = category;
  }
  if (body.contains("icon") && !body.at("icon").is_null()) {
    const auto icon = body.at("icon").get<std::string>();
    // Icons are small: a catalog path or a square, downscaled data URL. Cap the
    // payload so an oversized upload cannot bloat the database.
    if (icon.size() > 500000) {
      throw std::invalid_argument("icon is too large");
    }
    app.icon = icon;
  }
  app.branch = body.value("branch", app.branch.empty() ? "main" : app.branch);
  app.baseDirectory = body.value("baseDirectory", app.baseDirectory.empty() ? "/" : app.baseDirectory);
  app.dockerfilePath =
      body.value("dockerfilePath", app.dockerfilePath.empty() ? "Dockerfile" : app.dockerfilePath);
  if (body.contains("portMappings")) {
    if (actor.role != "admin") {
      throw ForbiddenError("Only an admin can set port mappings");
    }
    if (!body.at("portMappings").is_array()) {
      throw std::invalid_argument("portMappings must be an array");
    }
    app.portMappings.clear();
    for (const auto& item : body.at("portMappings")) {
      PortMapping mapping;
      mapping.containerPort = item.value("containerPort", 0);
      mapping.hostPort = item.value("hostPort", 0);
      if (mapping.containerPort < 1 || mapping.containerPort > 65535) {
        throw std::invalid_argument("containerPort must be between 1 and 65535");
      }
      if (mapping.hostPort < 0 || mapping.hostPort > 65535) {
        throw std::invalid_argument("hostPort must be between 0 and 65535");
      }
      app.portMappings.push_back(mapping);
    }
    if (!app.portMappings.empty()) {
      app.portsExposes = app.portMappings.front().containerPort;
    }
  } else if (actor.role != "admin") {
    const int allowed = existing ? existing->portsExposes : 80;
    if (body.contains("portsExposes") && !body.at("portsExposes").is_null()) {
      const int requested = body.at("portsExposes").get<int>();
      if (requested != allowed) {
        throw ForbiddenError("Only an admin can set the exposed port");
      }
    }
    app.portsExposes = allowed;
    if (existing) {
      app.portMappings = existing->portMappings;
    }
  } else {
    app.portsExposes = body.value("portsExposes", app.portsExposes == 0 ? 80 : app.portsExposes);
    if (app.portsExposes < 1 || app.portsExposes > 65535) {
      throw std::invalid_argument("portsExposes must be between 1 and 65535");
    }
  }
  const bool scaleFieldsInBody =
      body.contains("desiredReplicas") || body.contains("scaleMode") || body.contains("minReplicas") ||
      body.contains("maxReplicas") || body.contains("scaleTargetCpuPercent") ||
      body.contains("scaleTargetMemoryPercent") || body.contains("scaleTargetLoad") ||
      body.contains("scaleCooldownSeconds") || body.contains("scaleWindowSeconds");
  if (scaleFieldsInBody && actor.role != "admin" && existing) {
    throw ForbiddenError("Only an admin can change scale settings");
  }
  if (actor.role != "admin") {
    // Non-admin always stays at a single manually scaled replica.
    app.desiredReplicas = existing ? existing->desiredReplicas : 1;
    app.scaleMode = existing ? existing->scaleMode : "manual";
    app.minReplicas = existing ? existing->minReplicas : 1;
    app.maxReplicas = existing ? existing->maxReplicas : 1;
    app.scaleTargetCpuPercent = existing ? existing->scaleTargetCpuPercent : 70.0;
    app.scaleTargetMemoryPercent = existing ? existing->scaleTargetMemoryPercent : 80.0;
    app.scaleTargetLoad = existing ? existing->scaleTargetLoad : 1.0;
    app.scaleCooldownSeconds = existing ? existing->scaleCooldownSeconds : 120;
    app.scaleWindowSeconds = existing ? existing->scaleWindowSeconds : 180;
  } else {
    app.desiredReplicas =
        body.value("desiredReplicas", app.desiredReplicas == 0 ? 1 : app.desiredReplicas);
    if (body.contains("scaleMode")) {
      app.scaleMode = body.at("scaleMode").get<std::string>();
    } else if (!existing) {
      app.scaleMode = app.scaleMode.empty() ? "manual" : app.scaleMode;
    }
    app.minReplicas = body.value("minReplicas", app.minReplicas <= 0 ? 1 : app.minReplicas);
    app.maxReplicas =
        body.value("maxReplicas", app.maxReplicas <= 0 ? app.desiredReplicas : app.maxReplicas);
    app.scaleTargetCpuPercent =
        body.value("scaleTargetCpuPercent", app.scaleTargetCpuPercent <= 0 ? 70.0 : app.scaleTargetCpuPercent);
    app.scaleTargetMemoryPercent = body.value(
        "scaleTargetMemoryPercent", app.scaleTargetMemoryPercent <= 0 ? 80.0 : app.scaleTargetMemoryPercent);
    app.scaleTargetLoad =
        body.value("scaleTargetLoad", app.scaleTargetLoad <= 0 ? 1.0 : app.scaleTargetLoad);
    app.scaleCooldownSeconds = body.value(
        "scaleCooldownSeconds", app.scaleCooldownSeconds <= 0 ? 120 : app.scaleCooldownSeconds);
    app.scaleWindowSeconds =
        body.value("scaleWindowSeconds", app.scaleWindowSeconds <= 0 ? 180 : app.scaleWindowSeconds);
  }
  if (app.scaleMode != "manual" && app.scaleMode != "cpu" && app.scaleMode != "memory" &&
      app.scaleMode != "load" && app.scaleMode != "composite") {
    throw std::invalid_argument("scaleMode must be manual, cpu, memory, load, or composite");
  }
  if (app.desiredReplicas < 0 || app.minReplicas < 1 || app.maxReplicas < 1) {
    throw std::invalid_argument("replica counts must be positive");
  }
  if (!existing && !body.contains("maxReplicas")) {
    app.maxReplicas = std::max(app.maxReplicas, std::max(1, app.desiredReplicas));
  }
  if (app.minReplicas > app.maxReplicas) {
    throw std::invalid_argument("minReplicas cannot exceed maxReplicas");
  }
  if (app.scaleMode == "manual") {
    app.minReplicas = app.desiredReplicas;
    app.maxReplicas = app.desiredReplicas;
  } else {
    app.desiredReplicas = std::clamp(app.desiredReplicas, app.minReplicas, app.maxReplicas);
  }
  if (body.contains("targetServers")) {
    if (actor.role != "admin") {
      throw ForbiddenError("Only an admin can choose deployment servers");
    }
    if (!body.at("targetServers").is_array()) {
      throw std::invalid_argument("targetServers must be an array");
    }
    app.targetServers.clear();
    int fallbackPriority = 0;
    for (const auto& item : body.at("targetServers")) {
      ApplicationServerTarget target;
      target.serverId = item.value("serverId", static_cast<int64_t>(0));
      target.priority = item.value("priority", fallbackPriority);
      if (target.serverId <= 0 || !ctx_.repos->findServerById(target.serverId)) {
        throw std::invalid_argument("targetServers contains an unknown server");
      }
      const bool duplicate = std::any_of(
          app.targetServers.begin(), app.targetServers.end(),
          [&](const ApplicationServerTarget& existing) { return existing.serverId == target.serverId; });
      if (duplicate) {
        throw std::invalid_argument("targetServers contains a duplicate server");
      }
      app.targetServers.push_back(target);
      ++fallbackPriority;
    }
    std::sort(app.targetServers.begin(), app.targetServers.end(),
              [](const ApplicationServerTarget& a, const ApplicationServerTarget& b) {
                return a.priority < b.priority;
              });
    for (size_t i = 0; i < app.targetServers.size(); ++i) {
      app.targetServers[i].priority = static_cast<int>(i);
    }
  }
  if (body.contains("env")) {
    app.envJson = body.at("env").dump();
  }
  app.healthCheckType = body.value("healthCheckType", app.healthCheckType.empty() ? "none" : app.healthCheckType);
  app.healthCheckPath = body.value("healthCheckPath", app.healthCheckPath.empty() ? "/" : app.healthCheckPath);
  app.healthCheckCommand = body.value("healthCheckCommand", app.healthCheckCommand);
  app.maxRestartCount = body.value("maxRestartCount", app.maxRestartCount);

  const bool sourceInBody = body.contains("source") || body.contains("githubAppId") ||
                            body.contains("repoFullName") || body.contains("repoUrl");
  // Partial updates (ports, env, hostname, …) must not re-validate the Git source.
  if (!existing || sourceInBody) {
    const std::string source =
        body.value("source", app.githubAppId ? "github_app" : "public");
    if (source == "github_app") {
      const auto githubAppId = body.contains("githubAppId")
                                   ? body.at("githubAppId").get<int64_t>()
                                   : (app.githubAppId ? *app.githubAppId : int64_t{0});
      const auto repoFullName = body.contains("repoFullName")
                                    ? body.at("repoFullName").get<std::string>()
                                    : app.repoFullName;
      if (githubAppId <= 0 || repoFullName.empty()) {
        throw std::invalid_argument("githubAppId and repoFullName are required for github_app source");
      }
      auto githubApp = ctx_.repos->findGithubAppById(githubAppId);
      if (!githubApp) {
        throw std::invalid_argument("GitHub App not found");
      }
      if (githubApp->userId != actor.id && actor.role != "admin") {
        throw ForbiddenError("GitHub App does not belong to you");
      }
      if (existing && existing->createdBy > 0 && githubApp->userId != existing->createdBy &&
          actor.role != "admin") {
        throw ForbiddenError("GitHub App must belong to the application owner");
      }
      if (!existing && githubApp->userId != actor.id) {
        throw ForbiddenError("GitHub App must belong to you");
      }
      if (githubApp->status != "installed" || !githubApp->installationId) {
        throw std::invalid_argument("GitHub App is not installed");
      }
      if (repoFullName.find('/') == std::string::npos) {
        throw std::invalid_argument("repoFullName must be owner/name");
      }
      app.githubAppId = githubAppId;
      app.repoFullName = repoFullName;
      app.repoUrl = "https://github.com/" + app.repoFullName + ".git";
    } else if (source == "public") {
      app.githubAppId = std::nullopt;
      app.repoFullName.clear();
      app.repoUrl = body.value("repoUrl", app.repoUrl);
      if (app.repoUrl.empty()) {
        throw std::invalid_argument("repoUrl is required for public source");
      }
    } else if (source == "image") {
      app.githubAppId = std::nullopt;
      app.repoFullName.clear();
      const auto image = body.value("image", std::string{});
      if (image.empty() || image.find("://") != std::string::npos ||
          image.find(' ') != std::string::npos || image.find("..") != std::string::npos) {
        throw std::invalid_argument("image must be a Docker image reference");
      }
      // The agent pulls this reference instead of cloning a Git repository.
      app.repoUrl = "docker://" + image;
      app.branch = body.value("branch", std::string{"main"});
      if (app.branch.empty()) {
        app.branch = "main";
      }
    } else {
      throw std::invalid_argument("source must be public, github_app, or image");
    }
  }

  {
    const int64_t ownerId = existing ? existing->createdBy : actor.id;
    if (body.contains("hostnames")) {
      if (!body.at("hostnames").is_array()) {
        throw std::invalid_argument("hostnames must be an array");
      }
      if (body.at("hostnames").empty()) {
        app.hostnames.clear();
        app.hostname.clear();
        app.tlsStatus = "none";
      } else {
      std::unordered_set<std::string> seenHosts;
      std::vector<ApplicationHostname> parsed;
      for (const auto& item : body.at("hostnames")) {
        if (!item.is_object()) {
          throw std::invalid_argument("each hostname entry must be an object");
        }
        auto subdomain = toLower(item.value("subdomain", ""));
        const auto baseDomainId = item.value("baseDomainId", static_cast<int64_t>(0));
        if (!isValidSubdomainLabel(subdomain)) {
          throw std::invalid_argument(
              "subdomain must be a single DNS label (lowercase letters, digits, hyphen)");
        }
        if (baseDomainId <= 0) {
          throw std::invalid_argument("baseDomainId is required for each hostname");
        }
        if (!ctx_.repos->userHasBaseDomain(ownerId, baseDomainId)) {
          throw std::invalid_argument("base domain is not assigned to this user");
        }
        auto domain = ctx_.repos->findBaseDomainById(baseDomainId);
        if (!domain) {
          throw std::invalid_argument("unknown base domain");
        }
        ApplicationHostname row;
        row.hostname = subdomain + "." + toLower(domain->hostname);
        row.baseDomainId = baseDomainId;
        row.tlsStatus = "pending";
        if (existing) {
          for (const auto& prev : existing->hostnames) {
            if (prev.hostname == row.hostname) {
              row.tlsStatus = prev.tlsStatus;
              row.id = prev.id;
              break;
            }
          }
        }
        if (!seenHosts.insert(row.hostname).second) {
          throw std::invalid_argument("duplicate hostname in request");
        }
        if (ctx_.repos->hostnameExists(row.hostname, existing ? std::optional<int64_t>(existing->id)
                                                              : std::nullopt)) {
          throw std::invalid_argument("hostname already exists: " + row.hostname);
        }
        parsed.push_back(row);
      }
      app.hostnames = std::move(parsed);
      app.hostname = app.hostnames.front().hostname;
      app.tlsStatus = app.hostnames.front().tlsStatus;
      }
    }
  }

  if (body.contains("dependencies")) {
    app.dependencies.clear();
    for (const auto& dep : body.at("dependencies")) {
      app.dependencies.push_back(dep.get<int64_t>());
    }
  }
  if (body.contains("volumes")) {
    app.volumes.clear();
    for (const auto& volume : body.at("volumes")) {
      ApplicationVolume v;
      v.name = volume.at("name").get<std::string>();
      v.mountPath = volume.at("mountPath").get<std::string>();
      app.volumes.push_back(v);
    }
  }
  if (!app.volumes.empty()) {
    if (app.desiredReplicas != 1 || app.minReplicas != 1 || app.maxReplicas != 1) {
      throw std::invalid_argument("volume-backed applications must have desiredReplicas = 1");
    }
    if (app.scaleMode != "manual") {
      throw std::invalid_argument("volume-backed applications only support manual scale");
    }
  }
  if (app.healthCheckType == "cmd" && app.healthCheckCommand.empty()) {
    throw std::invalid_argument("healthCheckCommand is required for cmd checks");
  }
  return app;
}

nlohmann::json ApplicationService::list(const ListQuery& query, const User& actor) {
  ListQuery scoped = query;
  if (actor.role != "admin") {
    scoped.ownerId = actor.id;
  }
  const auto page = ctx_.repos->listApplications(scoped);
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& app : page.rows) {
    auto json = applicationToJson(app);
    // Derive a coarse runtime status from the application's replicas so the
    // list can show whether the app is actually running.
    const auto replicas = ctx_.repos->listReplicas(app.id, ListQuery{1, 1000, "", "asc", ""});
    int running = 0;
    int inFlight = 0;
    // Earliest moment a replica entered the running state, used as the app uptime.
    std::string startedAt;
    // Distinct servers that currently host a (non-stopped) replica of this app.
    std::vector<int64_t> serverIds;
    for (const auto& replica : replicas.rows) {
      if (replica.status == "running" || replica.status == "healthy") {
        ++running;
        if (!replica.statusChangedAt.empty() &&
            (startedAt.empty() || replica.statusChangedAt < startedAt)) {
          startedAt = replica.statusChangedAt;
        }
      } else if (replica.status == "pending" || replica.status == "building" ||
                 replica.status == "starting" || replica.status == "deploying") {
        ++inFlight;
      }
      if (replica.status != "stopped" && replica.status != "failed" &&
          std::find(serverIds.begin(), serverIds.end(), replica.serverId) == serverIds.end()) {
        serverIds.push_back(replica.serverId);
      }
    }
    nlohmann::json servers = nlohmann::json::array();
    for (const auto serverId : serverIds) {
      if (auto server = ctx_.repos->findServerById(serverId)) {
        servers.push_back({{"id", server->id}, {"name", server->name}, {"ip", server->ip}});
      }
    }
    json["servers"] = servers;
    std::string runtimeStatus = "stopped";
    if (running > 0) {
      runtimeStatus = "running";
    } else if (inFlight > 0) {
      runtimeStatus = "deploying";
    }
    json["runningReplicas"] = running;
    json["runtimeStatus"] = runtimeStatus;
    json["startedAt"] = startedAt;  // empty when the app is not running
    // Attach the latest resource sample so the list can render real-time bars.
    double cpuPercent = 0.0;
    long long memoryUsedBytes = 0;
    long long memoryLimitBytes = 0;
    long long diskUsedBytes = 0;
    if (running > 0 && ctx_.deployments) {
      const auto to = std::chrono::system_clock::now();
      const auto from = to - std::chrono::minutes(5);
      auto metrics = ctx_.deployments->applicationMetrics(app.id, isoUtc(from), isoUtc(to));
      if (metrics.contains("latest") && metrics["latest"].is_object()) {
        const auto& latest = metrics["latest"];
        cpuPercent = latest.value("cpuPercent", 0.0);
        memoryUsedBytes = latest.value("memoryUsedBytes", 0LL);
        memoryLimitBytes = latest.value("memoryLimitBytes", 0LL);
        diskUsedBytes = latest.value("diskUsedBytes", 0LL);
      }
    }
    json["cpuPercent"] = cpuPercent;
    json["memoryUsedBytes"] = memoryUsedBytes;
    json["memoryLimitBytes"] = memoryLimitBytes;
    json["diskUsedBytes"] = diskUsedBytes;
    rows.push_back(json);
  }
  return {{"rows", rows}, {"total", page.total}};
}

nlohmann::json ApplicationService::get(int64_t id, const User& actor) {
  return applicationJsonWithDomains(ctx_, requireAccess(id, actor));
}

nlohmann::json ApplicationService::create(const nlohmann::json& body, const User& actor) {
  auto app = parseBody(body, nullptr, actor);
  app.createdBy = actor.id;
  if (!app.hostnames.empty()) {
    for (auto& row : app.hostnames) {
      row.tlsStatus = "pending";
    }
    app.tlsStatus = "pending";
  }
  app.id = ctx_.repos->createApplication(app);
  if (!app.hostnames.empty()) {
    // Publish server_name before certbot so HTTP-01 hits this subdomain.
    ctx_.proxy->renderApplicationSite(app);
    for (const auto& row : app.hostnames) {
      ctx_.certbot->requestCertificate(app.id, row.hostname);
    }
    ctx_.proxy->renderApplicationSite(*ctx_.repos->findApplicationById(app.id));
  }
  return applicationJsonWithDomains(ctx_, *ctx_.repos->findApplicationById(app.id));
}

nlohmann::json ApplicationService::update(int64_t id, const nlohmann::json& body, const User& actor) {
  auto existing = requireAccess(id, actor);
  const auto oldHostnames = existing.hostnames;
  const auto oldEnv = existing.envJson;
  const auto oldPorts = existing.portsExposes;
  const auto oldMappings = existing.portMappings;
  const auto oldDockerfile = existing.dockerfilePath;
  const auto oldBaseDir = existing.baseDirectory;
  const auto oldBranch = existing.branch;
  auto app = parseBody(body, &existing, actor);
  app.id = id;
  app.createdBy = existing.createdBy;
  if (body.contains("createdBy") && !body.at("createdBy").is_null()) {
    if (actor.role != "admin") {
      throw ForbiddenError("Only an admin can change the application owner");
    }
    const auto ownerId = body.at("createdBy").get<int64_t>();
    if (!ctx_.repos->findUserById(ownerId)) {
      throw std::invalid_argument("Unknown user");
    }
    app.createdBy = ownerId;
  }
  app.pendingRedeploy = existing.pendingRedeploy;

  std::unordered_set<std::string> newHosts;
  for (const auto& row : app.hostnames) {
    newHosts.insert(row.hostname);
  }
  std::unordered_set<std::string> oldHosts;
  for (const auto& row : oldHostnames) {
    oldHosts.insert(row.hostname);
  }
  bool hostnamesChanged = false;
  if (body.contains("hostnames")) {
    if (app.hostnames.size() != oldHostnames.size()) {
      hostnamesChanged = true;
    } else {
      for (size_t i = 0; i < app.hostnames.size(); ++i) {
        if (app.hostnames[i].hostname != oldHostnames[i].hostname) {
          hostnamesChanged = true;
          break;
        }
      }
    }
  }

  const bool needsRedeploy =
      app.envJson != oldEnv || app.portsExposes != oldPorts || app.portMappings != oldMappings ||
      app.dockerfilePath != oldDockerfile || app.baseDirectory != oldBaseDir ||
      app.branch != oldBranch || body.contains("portMappings") || body.contains("env");
  if (needsRedeploy) {
    app.pendingRedeploy = true;
  }
  ctx_.repos->updateApplication(app);

  if (hostnamesChanged) {
    for (const auto& row : oldHostnames) {
      if (!newHosts.count(row.hostname)) {
        ctx_.proxy->removeSite(row.hostname);
      }
    }
    ctx_.proxy->renderApplicationSite(app);
    for (const auto& row : app.hostnames) {
      if (!oldHosts.count(row.hostname)) {
        ctx_.certbot->requestCertificate(app.id, row.hostname);
      }
    }
    ctx_.proxy->renderApplicationSite(*ctx_.repos->findApplicationById(id));
  }
  if (body.contains("desiredReplicas")) {
    ctx_.placement->converge(app.id);
  }
  return applicationJsonWithDomains(ctx_, *ctx_.repos->findApplicationById(id));
}

void ApplicationService::remove(int64_t id, const User& actor) {
  auto app = requireAccess(id, actor);
  if (!app.volumes.empty()) {
    throw std::runtime_error("Delete volumes before deleting the application");
  }
  if (ctx_.deployments) {
    auto replicas = ctx_.repos->listReplicas(id, ListQuery{1, 1000, "", "asc", ""});
    for (const auto& replica : replicas.rows) {
      ctx_.deployments->retireReplica(replica.id, false);
    }
  }
  ctx_.proxy->removeApplicationSite(app);
  ctx_.repos->deleteApplication(id);
}

void ApplicationService::stop(int64_t id, const User& actor) {
  auto app = requireAccess(id, actor);
  // Pin the desired scale to zero and force manual mode so neither converge nor the
  // autoscaler brings the containers back up. This keeps the application stopped so
  // the operator can delete the associated volumes and then the application itself.
  app.desiredReplicas = 0;
  app.minReplicas = 0;
  app.maxReplicas = 0;
  app.scaleMode = "manual";
  ctx_.repos->updateApplication(app);
  if (ctx_.deployments) {
    auto replicas = ctx_.repos->listReplicas(id, ListQuery{1, 1000, "", "asc", ""});
    for (const auto& replica : replicas.rows) {
      ctx_.deployments->retireReplica(replica.id, false);
    }
  }
  ctx_.proxy->renderApplicationSite(app);
}

nlohmann::json ApplicationService::scale(int64_t id, const nlohmann::json& body, const User& actor) {
  if (actor.role != "admin") {
    throw ForbiddenError("Only an admin can scale applications");
  }
  auto app = requireAccess(id, actor);
  nlohmann::json merged = body;
  if (!merged.contains("desiredReplicas") && !merged.contains("minReplicas") &&
      !merged.contains("maxReplicas") && !merged.contains("scaleMode")) {
    throw std::invalid_argument("scale body requires desiredReplicas or scale policy fields");
  }
  auto updated = parseBody(merged, &app, actor);
  updated.id = id;
  updated.createdBy = app.createdBy;
  updated.pendingRedeploy = app.pendingRedeploy;
  updated.scaleLastActionAt = app.scaleLastActionAt;
  if (!updated.volumes.empty()) {
    updated.desiredReplicas = 1;
    updated.minReplicas = 1;
    updated.maxReplicas = 1;
    updated.scaleMode = "manual";
  }
  ctx_.repos->updateApplication(updated);
  ctx_.placement->converge(id);
  return applicationToJson(*ctx_.repos->findApplicationById(id));
}

nlohmann::json ApplicationService::createDeployKey(int64_t id, const User& actor) {
  requireAccess(id, actor);
  const auto dir = std::filesystem::temp_directory_path() / ("hc-key-" + std::to_string(id));
  std::filesystem::create_directories(dir);
  const auto keyPath = dir / "id_ed25519";
  bp::system(bp::search_path("ssh-keygen"), "-t", "ed25519", "-N", "", "-f", keyPath.string(),
             "-C", "highcube-deploy");
  std::ifstream priv(keyPath);
  std::ifstream pub(keyPath.string() + ".pub");
  std::stringstream privBuf;
  std::stringstream pubBuf;
  privBuf << priv.rdbuf();
  pubBuf << pub.rdbuf();
  ctx_.repos->setDeployKey(id, pubBuf.str(), privBuf.str());
  std::filesystem::remove_all(dir);
  return {{"publicKey", pubBuf.str()}};
}

nlohmann::json certificateJobJson(const CertificateJobView& job) {
  nlohmann::json lines = nlohmann::json::array();
  for (const auto& line : job.lines) {
    lines.push_back(line);
  }
  return {{"jobId", job.jobId},
          {"hostname", job.hostname},
          {"status", job.status},
          {"exitCode", job.exitCode},
          {"lines", lines}};
}

nlohmann::json ApplicationService::requestCertificate(int64_t id, const std::string& hostname,
                                                      const User& actor) {
  auto app = requireAccess(id, actor);
  if (hostname.empty()) {
    throw std::invalid_argument("hostname is required");
  }
  bool found = false;
  for (const auto& row : app.hostnames) {
    if (row.hostname == hostname) {
      found = true;
      break;
    }
  }
  if (!found) {
    throw std::invalid_argument("hostname is not configured on this application");
  }
  return certificateJobJson(ctx_.certbot->startCertificateJob(app, hostname));
}

nlohmann::json ApplicationService::certificateJob(int64_t id, const std::string& jobId,
                                                  const User& actor) {
  requireAccess(id, actor);
  return certificateJobJson(ctx_.certbot->pollCertificateJob(id, jobId));
}

nlohmann::json ApplicationService::autocomplete(const std::string& q, bool privateOnly,
                                                const User& actor) {
  std::optional<int64_t> ownerId;
  if (actor.role != "admin") {
    ownerId = actor.id;
  }
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& app : ctx_.repos->autocompleteApplications(q, privateOnly, ownerId)) {
    rows.push_back({{"id", app.id}, {"label", app.name}});
  }
  return {{"rows", rows}};
}

nlohmann::json ApplicationService::listVolumes(int64_t id, const User& actor) {
  requireAccess(id, actor);
  nlohmann::json rows = nlohmann::json::array();
  nlohmann::json liveVolumes = nlohmann::json::array();
  std::optional<int64_t> ownerServerId;
  auto replicas = ctx_.repos->listReplicas(id, ListQuery{1, 100, "", "asc", ""});
  for (const auto& replica : replicas.rows) {
    if (!ownerServerId) {
      ownerServerId = replica.serverId;
    }
  }
  if (!replicas.rows.empty() && ctx_.deployments) {
    const auto to = std::chrono::system_clock::now();
    const auto from = to - std::chrono::hours(2);
    auto metrics = ctx_.deployments->applicationMetrics(id, isoUtc(from), isoUtc(to));
    if (metrics.contains("volumes") && metrics["volumes"].is_array()) {
      liveVolumes = metrics["volumes"];
    }
  }
  for (const auto& volume : ctx_.repos->getVolumes(id)) {
    const std::string dockerName = "highcube-" + std::to_string(id) + "-" + volume.name;
    nlohmann::json row = {{"id", volume.id},
                          {"name", volume.name},
                          {"mountPath", volume.mountPath},
                          {"dockerName", dockerName},
                          {"hostPath", nullptr},
                          {"sizeBytes", nullptr},
                          {"serverId", ownerServerId ? nlohmann::json(*ownerServerId) : nullptr}};
    for (const auto& live : liveVolumes) {
      if (!live.is_object()) {
        continue;
      }
      const auto liveName = live.value("dockerName", live.value("name", ""));
      if (liveName == dockerName || liveName == volume.name) {
        if (live.contains("hostPath") && !live["hostPath"].is_null()) {
          row["hostPath"] = live["hostPath"];
        }
        if (live.contains("sizeBytes") && !live["sizeBytes"].is_null()) {
          row["sizeBytes"] = live["sizeBytes"];
        }
        break;
      }
    }
    rows.push_back(row);
  }
  return {{"rows", rows}};
}

nlohmann::json ApplicationService::addVolume(int64_t id, const nlohmann::json& body,
                                             const User& actor) {
  auto app = requireAccess(id, actor);
  const auto name = body.at("name").get<std::string>();
  const auto mountPath = body.at("mountPath").get<std::string>();
  if (name.empty() || mountPath.empty()) {
    throw std::invalid_argument("name and mountPath are required");
  }
  if (name.find('/') != std::string::npos || name.find(' ') != std::string::npos) {
    throw std::invalid_argument("volume name must be a single path segment");
  }
  if (mountPath.front() != '/') {
    throw std::invalid_argument("mountPath must be absolute");
  }
  for (const auto& existing : app.volumes) {
    if (existing.name == name) {
      throw std::invalid_argument("volume name already exists on this application");
    }
    if (existing.mountPath == mountPath) {
      throw std::invalid_argument("mountPath already used by another volume");
    }
  }
  if (app.volumes.empty()) {
    // First volume forces single-replica manual scale.
    app.desiredReplicas = 1;
    app.minReplicas = 1;
    app.maxReplicas = 1;
    app.scaleMode = "manual";
    ctx_.repos->updateApplication(app);
  }
  const auto volumeId = ctx_.repos->addVolume(id, name, mountPath);
  auto volume = ctx_.repos->findVolumeById(volumeId);
  return {{"id", volume->id},
          {"name", volume->name},
          {"mountPath", volume->mountPath},
          {"dockerName", "highcube-" + std::to_string(id) + "-" + volume->name}};
}

namespace {

void requireApplicationStopped(Repositories& repos, int64_t applicationId) {
  auto replicas = repos.listReplicas(applicationId, ListQuery{1, 1000, "", "asc", ""});
  for (const auto& replica : replicas.rows) {
    if (replica.desiredState == "running" || replica.status == "running" ||
        replica.status == "healthy" || replica.status == "starting" ||
        replica.status == "pending" || replica.status == "building") {
      throw std::runtime_error("Stop the application before changing volumes");
    }
  }
}

void validateVolumeFields(const std::string& name, const std::string& mountPath) {
  if (name.empty() || mountPath.empty()) {
    throw std::invalid_argument("name and mountPath are required");
  }
  if (name.find('/') != std::string::npos || name.find(' ') != std::string::npos) {
    throw std::invalid_argument("volume name must be a single path segment");
  }
  if (mountPath.front() != '/') {
    throw std::invalid_argument("mountPath must be absolute");
  }
}

}  // namespace

nlohmann::json ApplicationService::updateVolume(int64_t applicationId, int64_t volumeId,
                                                const nlohmann::json& body, const User& actor) {
  auto app = requireAccess(applicationId, actor);
  auto volume = ctx_.repos->findVolumeById(volumeId);
  if (!volume || volume->applicationId != applicationId) {
    throw std::runtime_error("Volume not found");
  }
  const auto name = body.value("name", volume->name);
  const auto mountPath = body.value("mountPath", volume->mountPath);
  validateVolumeFields(name, mountPath);
  for (const auto& existing : app.volumes) {
    if (existing.id == volumeId) {
      continue;
    }
    if (existing.name == name) {
      throw std::invalid_argument("volume name already exists on this application");
    }
    if (existing.mountPath == mountPath) {
      throw std::invalid_argument("mountPath already used by another volume");
    }
  }
  const bool nameChanged = name != volume->name;
  if (nameChanged || mountPath != volume->mountPath) {
    requireApplicationStopped(*ctx_.repos, applicationId);
  }
  if (nameChanged && ctx_.agentHub) {
    ctx_.agentHub->renameVolumeOnServers(
        "highcube-" + std::to_string(applicationId) + "-" + volume->name,
        "highcube-" + std::to_string(applicationId) + "-" + name);
  }
  ctx_.repos->updateVolume(volumeId, name, mountPath);
  auto updated = ctx_.repos->findVolumeById(volumeId);
  return {{"id", updated->id},
          {"name", updated->name},
          {"mountPath", updated->mountPath},
          {"dockerName", "highcube-" + std::to_string(applicationId) + "-" + updated->name}};
}

nlohmann::json ApplicationService::metrics(int64_t id, const std::string& from,
                                           const std::string& to, const User& actor) {
  requireAccess(id, actor);
  return ctx_.deployments->applicationMetrics(id, from, to);
}

nlohmann::json ApplicationService::incidents(int64_t id, const ListQuery& query,
                                             const User& actor) {
  requireAccess(id, actor);
  const auto page = ctx_.repos->listReplicaIncidents(id, query);
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& incident : page.rows) {
    rows.push_back(replicaIncidentToJson(incident));
  }
  return {{"rows", rows}, {"total", page.total}};
}

void ApplicationService::deleteVolume(int64_t applicationId, int64_t volumeId, const User& actor) {
  requireAccess(applicationId, actor);
  auto volume = ctx_.repos->findVolumeById(volumeId);
  if (!volume || volume->applicationId != applicationId) {
    throw std::runtime_error("Volume not found");
  }
  requireApplicationStopped(*ctx_.repos, applicationId);
  if (ctx_.agentHub) {
    ctx_.agentHub->deleteVolumeOnServers("highcube-" + std::to_string(applicationId) + "-" +
                                         volume->name);
  }
  ctx_.repos->deleteVolume(volumeId);
}

}  // namespace highcube::admin
