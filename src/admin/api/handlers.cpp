#include "api/handlers.hpp"

#include "agents/agent_hub.hpp"
#include "agents/terminal_polling.hpp"
#include "auth/password.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace highcube::admin {
namespace {

bool requireAdmin(RequestContext& c, AppContext& ctx) {
  return ctx.auth->isAdmin(*c.user);
}

std::string queryValue(const std::unordered_map<std::string, std::string>& query, const char* key) {
  const auto it = query.find(key);
  return it == query.end() ? std::string{} : it->second;
}

// Dashboard columns are unpaginated: 1000 is the largest page the repositories accept.
ListQuery dashboardList() {
  ListQuery list;
  list.page = 1;
  list.pageSize = 1000;
  return list;
}

Application requireAppAccess(AppContext& ctx, RequestContext& c, int64_t appId) {
  auto app = ctx.repos->findApplicationById(appId);
  if (!app) {
    throw std::runtime_error("Application not found");
  }
  if (!ctx.auth->canAccessApplication(*c.user, *app)) {
    throw ForbiddenError();
  }
  return *app;
}

}  // namespace

void registerHandlers(Router& router, AppContext& ctx) {
  // Record every mutating action in the audit trail. Runs after each handler.
  router.setAuditHook([&ctx](const RequestContext& c, const std::string& routePattern, int status) {
    if (!c.user) {
      return;
    }
    const auto derived = deriveActivity(c.method, routePattern);
    if (derived.action.empty()) {
      return;  // Deliberately not audited (noisy or internal route).
    }
    UserActivity activity;
    activity.userId = c.user->id;
    activity.action = derived.action;
    activity.targetType = derived.targetType;
    activity.method = c.method;
    activity.path = c.targetPath;
    activity.status = status;
    if (const auto it = c.params.find("id"); it != c.params.end()) {
      activity.targetId = it->second;
    }
    const auto realIp = std::string(c.request["X-Real-IP"]);
    const auto forwarded = std::string(c.request["X-Forwarded-For"]);
    activity.ip = !realIp.empty() ? realIp : forwarded;
    ActivityService(ctx).record(activity);
  });

  router.add("POST", "/api/auth/login", false, [&](RequestContext& c) {
    const auto body = nlohmann::json::parse(c.request.body());
    return jsonResponse(http::status::ok,
                        UserService(ctx).login(body.at("email"), body.at("password")));
  });

  router.add("GET", "/api/profile", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok, userToJson(*c.user, true));
  });

  router.add("PATCH", "/api/profile", true, [&](RequestContext& c) {
    const auto body = nlohmann::json::parse(c.request.body().empty() ? "{}" : c.request.body());
    nlohmann::json safe = body;
    safe.erase("role");
    safe.erase("roleId");
    auto updated = UserService(ctx).update(c.user->id, safe, *c.user);
    return jsonResponse(http::status::ok, updated);
  });

  router.add("POST", "/api/profile/avatar", true, [&](RequestContext& c) {
    const auto dir = std::filesystem::path("data/avatars");
    std::filesystem::create_directories(dir);
    const auto path = dir / (std::to_string(c.user->id) + ".bin");
    std::ofstream out(path, std::ios::binary);
    out << c.request.body();
    nlohmann::json body = {{"avatarPath", path.string()}};
    UserService(ctx).update(c.user->id, body, *c.user);
    return jsonResponse(http::status::ok, {{"avatarPath", path.string()}});
  });
  router.add("GET", "/api/profile/base-domains", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok, UserService(ctx).listMyBaseDomains(*c.user));
  });
  router.add("GET", "/api/profile/hostname-suggestion", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok, UserService(ctx).hostnameSuggestion());
  });

  router.add("GET", "/api/state", true, [&](RequestContext& c) {
    auto servers = dashboardList();
    servers.q = queryValue(c.query, "serversQ");
    auto applications = dashboardList();
    applications.order = queryValue(c.query, "appsOrder");
    if (applications.order.empty()) {
      applications.order = "asc";
    }
    applications.typeFilter = queryValue(c.query, "appsType");
    auto users = dashboardList();
    users.order = queryValue(c.query, "usersOrder");
    if (users.order.empty()) {
      users.order = "asc";
    }
    const auto domains = dashboardList();
    return jsonResponse(http::status::ok,
                        StateService(ctx).dashboard(servers, applications, users, domains, *c.user));
  });

  router.add("GET", "/api/users", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok, UserService(ctx).list(parseListQuery(c.query)));
  });
  router.add("GET", "/api/users/autocomplete", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok, UserService(ctx).autocomplete(c.query["q"]));
  });
  router.add("GET", "/api/users/:id", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok, UserService(ctx).get(std::stoll(c.params["id"])));
  });
  router.add("POST", "/api/users", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::created,
                        UserService(ctx).create(nlohmann::json::parse(c.request.body()), *c.user));
  });
  router.add("PATCH", "/api/users/:id", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok,
                        UserService(ctx).update(std::stoll(c.params["id"]),
                                                nlohmann::json::parse(c.request.body()), *c.user));
  });
  router.add("PUT", "/api/users/:id/servers", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok,
                        UserService(ctx).setServers(std::stoll(c.params["id"]),
                                                    nlohmann::json::parse(c.request.body())));
  });
  router.add("PUT", "/api/users/:id/domains", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok,
                        UserService(ctx).setDomains(std::stoll(c.params["id"]),
                                                    nlohmann::json::parse(c.request.body())));
  });
  router.add("DELETE", "/api/users/:id", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    UserService(ctx).remove(std::stoll(c.params["id"]), *c.user);
    return emptyResponse(http::status::no_content);
  });

  // Audit trail for a user: the list of actions and the activity/presence timeline.
  router.add("GET", "/api/users/:id/activity", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok,
                        ActivityService(ctx).list(std::stoll(c.params["id"]), parseListQuery(c.query)));
  });
  router.add("GET", "/api/users/:id/activity-chart", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    const int hours = c.query.count("hours") ? std::stoi(c.query["hours"]) : 12;
    return jsonResponse(http::status::ok,
                        ActivityService(ctx).chart(std::stoll(c.params["id"]), hours));
  });

  // Users that currently have the dashboard open (live presence over WebSocket).
  router.add("GET", "/api/presence/online", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    nlohmann::json ids = nlohmann::json::array();
    if (ctx.presence) {
      for (const int64_t userId : ctx.presence->onlineUserIds()) {
        ids.push_back(userId);
      }
    }
    return jsonResponse(http::status::ok, nlohmann::json{{"userIds", ids}});
  });

  router.add("GET", "/api/settings/base-domains", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok, SettingsService(ctx).listBaseDomains(parseListQuery(c.query)));
  });
  router.add("GET", "/api/settings/base-domains/autocomplete", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok, SettingsService(ctx).autocompleteBaseDomains(c.query["q"]));
  });
  router.add("POST", "/api/settings/base-domains", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(
        http::status::created,
        SettingsService(ctx).createBaseDomain(nlohmann::json::parse(c.request.body()), *c.user));
  });
  router.add("PATCH", "/api/settings/base-domains/:id", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok,
                        SettingsService(ctx).updateBaseDomain(
                            std::stoll(c.params["id"]), nlohmann::json::parse(c.request.body())));
  });
  router.add("DELETE", "/api/settings/base-domains/:id", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    SettingsService(ctx).deleteBaseDomain(std::stoll(c.params["id"]));
    return emptyResponse(http::status::no_content);
  });
  router.add("GET", "/api/settings/metrics", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok, SettingsService(ctx).getMetricsSettings());
  });
  router.add("PATCH", "/api/settings/metrics", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok, SettingsService(ctx).updateMetricsSettings(
                                              nlohmann::json::parse(c.request.body())));
  });

  router.add("GET", "/api/servers", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok, ServerService(ctx).list(parseListQuery(c.query)));
  });
  router.add("GET", "/api/servers/autocomplete", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok, ServerService(ctx).autocomplete(c.query["q"]));
  });
  router.add("GET", "/api/servers/:id", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok, ServerService(ctx).get(std::stoll(c.params["id"])));
  });
  router.add("POST", "/api/servers", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::created,
                        ServerService(ctx).create(nlohmann::json::parse(c.request.body())));
  });
  router.add("PATCH", "/api/servers/:id", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok,
                        ServerService(ctx).update(std::stoll(c.params["id"]),
                                                  nlohmann::json::parse(c.request.body())));
  });
  router.add("POST", "/api/servers/:id/token", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok, ServerService(ctx).rotateToken(std::stoll(c.params["id"])));
  });
  router.add("DELETE", "/api/servers/:id", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    ServerService(ctx).remove(std::stoll(c.params["id"]));
    return emptyResponse(http::status::no_content);
  });
  router.add("GET", "/api/servers/:id/overview", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok, StateService(ctx).serverOverview(std::stoll(c.params["id"])));
  });
  router.add("GET", "/api/servers/:id/metrics", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return jsonResponse(http::status::ok, ServerService(ctx).metrics(std::stoll(c.params["id"]),
                                                                     c.query["from"], c.query["to"]));
  });
  router.add("POST", "/api/servers/:id/terminal/sessions", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return ctx.terminalPolling->open(std::stoll(c.params["id"]), *c.user, c.request.body());
  });
  router.add("POST", "/api/servers/:id/terminal/sessions/:streamId/input", true,
             [&](RequestContext& c) {
               if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
               return ctx.terminalPolling->write(std::stoll(c.params["id"]), c.params["streamId"],
                                                 *c.user, c.request.body());
             });
  router.add("POST", "/api/servers/:id/terminal/sessions/:streamId/resize", true,
             [&](RequestContext& c) {
               if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
               return ctx.terminalPolling->resize(std::stoll(c.params["id"]), c.params["streamId"],
                                                  *c.user, c.request.body());
             });
  router.add("DELETE", "/api/servers/:id/terminal/sessions/:streamId", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) return errorResponse(http::status::forbidden, "Forbidden");
    return ctx.terminalPolling->close(std::stoll(c.params["id"]), c.params["streamId"], *c.user);
  });

  router.add("GET", "/api/github-apps", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok, ctx.githubApps->list(parseListQuery(c.query), *c.user));
  });
  router.add("GET", "/api/github-apps/:id", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        ctx.githubApps->get(std::stoll(c.params["id"]), *c.user));
  });
  router.add("POST", "/api/github-apps/manifest", true, [&](RequestContext& c) {
    return jsonResponse(http::status::created,
                        ctx.githubApps->createManifest(nlohmann::json::parse(c.request.body()),
                                                       *c.user));
  });
  router.add("POST", "/api/github-apps/:id/manifest/complete", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        ctx.githubApps->completeManifest(std::stoll(c.params["id"]),
                                                         nlohmann::json::parse(c.request.body()),
                                                         *c.user));
  });
  router.add("POST", "/api/github-apps/:id/installation", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        ctx.githubApps->setInstallation(std::stoll(c.params["id"]),
                                                        nlohmann::json::parse(c.request.body()),
                                                        *c.user));
  });
  router.add("POST", "/api/github-apps/:id/installation/sync", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        ctx.githubApps->syncInstallation(std::stoll(c.params["id"]), *c.user));
  });
  router.add("GET", "/api/github-apps/:id/repositories", true, [&](RequestContext& c) {
    const int page = c.query.count("page") ? std::stoi(c.query["page"]) : 1;
    const int pageSize = c.query.count("pageSize") ? std::stoi(c.query["pageSize"]) : 30;
    return jsonResponse(http::status::ok,
                        ctx.githubApps->listRepositories(std::stoll(c.params["id"]), *c.user, page,
                                                         pageSize));
  });
  router.add("GET", "/api/github-apps/:id/branches", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        ctx.githubApps->listBranches(std::stoll(c.params["id"]), c.query["repo"],
                                                     *c.user));
  });
  router.add("GET", "/api/dockerfile-ports", true, [&](RequestContext& c) {
    const auto queryValue = [&](const char* key) {
      const auto it = c.query.find(key);
      return it == c.query.end() ? std::string() : it->second;
    };
    std::optional<int64_t> githubAppId;
    const auto githubAppRaw = queryValue("githubAppId");
    if (!githubAppRaw.empty()) {
      githubAppId = std::stoll(githubAppRaw);
    }
    return jsonResponse(http::status::ok,
                        ctx.githubApps->dockerfilePorts(*c.user, githubAppId, queryValue("repo"),
                                                        queryValue("repoUrl"), queryValue("branch"),
                                                        queryValue("path")));
  });
  router.add("DELETE", "/api/github-apps/:id", true, [&](RequestContext& c) {
    ctx.githubApps->remove(std::stoll(c.params["id"]), *c.user);
    return emptyResponse(http::status::no_content);
  });

  router.add("GET", "/api/applications", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        ApplicationService(ctx).list(parseListQuery(c.query), *c.user));
  });
  router.add("GET", "/api/applications/autocomplete", true, [&](RequestContext& c) {
    const bool privateOnly = c.query["type"] == "private";
    return jsonResponse(http::status::ok,
                        ApplicationService(ctx).autocomplete(c.query["q"], privateOnly, *c.user));
  });
  router.add("GET", "/api/applications/:id", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        ApplicationService(ctx).get(std::stoll(c.params["id"]), *c.user));
  });
  router.add("POST", "/api/applications", true, [&](RequestContext& c) {
    return jsonResponse(
        http::status::created,
        ApplicationService(ctx).create(nlohmann::json::parse(c.request.body()), *c.user));
  });
  router.add("PATCH", "/api/applications/:id", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        ApplicationService(ctx).update(std::stoll(c.params["id"]),
                                                       nlohmann::json::parse(c.request.body()),
                                                       *c.user));
  });
  router.add("POST", "/api/applications/:id/scale", true, [&](RequestContext& c) {
    if (!requireAdmin(c, ctx)) {
      return errorResponse(http::status::forbidden, "Forbidden");
    }
    const auto body = nlohmann::json::parse(c.request.body());
    return jsonResponse(http::status::ok,
                        ApplicationService(ctx).scale(std::stoll(c.params["id"]), body, *c.user));
  });
  router.add("POST", "/api/applications/:id/stop", true, [&](RequestContext& c) {
    ApplicationService(ctx).stop(std::stoll(c.params["id"]), *c.user);
    return emptyResponse(http::status::no_content);
  });
  // Starts an async host-port availability check on the application's target servers.
  router.add("POST", "/api/applications/:id/port-check", true, [&](RequestContext& c) {
    const auto app = requireAppAccess(ctx, c, std::stoll(c.params["id"]));
    const auto body = nlohmann::json::parse(c.request.body());
    const int port = body.value("port", 0);
    if (port < 1 || port > 65535) {
      return errorResponse(http::status::bad_request, "Invalid port");
    }
    std::vector<int64_t> serverIds;
    for (const auto& target : app.targetServers) {
      serverIds.push_back(target.serverId);
    }
    const auto checkId = ctx.agentHub->startHostPortCheck(port, serverIds);
    return jsonResponse(http::status::ok, nlohmann::json{{"checkId", checkId}});
  });
  // Polls the outcome of a previously started host-port availability check.
  router.add("GET", "/api/applications/:id/port-check/:checkId", true, [&](RequestContext& c) {
    requireAppAccess(ctx, c, std::stoll(c.params["id"]));
    const auto state = ctx.agentHub->pollHostPortCheck(c.params["checkId"]);
    std::string status = "unknown";
    switch (state) {
      case AgentHub::PortCheckState::Pending:
        status = "pending";
        break;
      case AgentHub::PortCheckState::Available:
        status = "available";
        break;
      case AgentHub::PortCheckState::Unavailable:
        status = "unavailable";
        break;
      case AgentHub::PortCheckState::Unknown:
        status = "unknown";
        break;
    }
    return jsonResponse(http::status::ok, nlohmann::json{{"status", status}});
  });
  router.add("POST", "/api/applications/:id/deploy-key", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        ApplicationService(ctx).createDeployKey(std::stoll(c.params["id"]), *c.user));
  });
  router.add("POST", "/api/applications/:id/certificate", true, [&](RequestContext& c) {
    const auto body = nlohmann::json::parse(c.request.body());
    return jsonResponse(http::status::ok,
                        ApplicationService(ctx).requestCertificate(
                            std::stoll(c.params["id"]), body.value("hostname", ""), *c.user));
  });
  router.add("GET", "/api/applications/:id/certificate-jobs/:jobId", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        ApplicationService(ctx).certificateJob(std::stoll(c.params["id"]),
                                                               c.params["jobId"], *c.user));
  });
  router.add("DELETE", "/api/applications/:id", true, [&](RequestContext& c) {
    ApplicationService(ctx).remove(std::stoll(c.params["id"]), *c.user);
    return emptyResponse(http::status::no_content);
  });
  router.add("POST", "/api/applications/:id/db-manage", true, [&](RequestContext& c) {
    const auto body = nlohmann::json::parse(c.request.body().empty() ? "{}" : c.request.body(),
                                            nullptr, false);
    if (!body.is_object()) {
      return errorResponse(http::status::bad_request, "Invalid JSON");
    }
    return jsonResponse(http::status::ok,
                        DatabaseAdminService(ctx).start(std::stoll(c.params["id"]), body, *c.user));
  });
  router.add("GET", "/api/applications/:id/db-manage/:requestId", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        DatabaseAdminService(ctx).poll(std::stoll(c.params["id"]),
                                                       c.params["requestId"], *c.user));
  });
  router.add("GET", "/api/applications/:id/volumes", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        ApplicationService(ctx).listVolumes(std::stoll(c.params["id"]), *c.user));
  });
  router.add("POST", "/api/applications/:id/volumes", true, [&](RequestContext& c) {
    return jsonResponse(http::status::created,
                        ApplicationService(ctx).addVolume(std::stoll(c.params["id"]),
                                                          nlohmann::json::parse(c.request.body()),
                                                          *c.user));
  });
  router.add("PATCH", "/api/applications/:id/volumes/:volumeId", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        ApplicationService(ctx).updateVolume(
                            std::stoll(c.params["id"]), std::stoll(c.params["volumeId"]),
                            nlohmann::json::parse(c.request.body()), *c.user));
  });
  router.add("DELETE", "/api/applications/:id/volumes/:volumeId", true, [&](RequestContext& c) {
    ApplicationService(ctx).deleteVolume(std::stoll(c.params["id"]),
                                         std::stoll(c.params["volumeId"]), *c.user);
    return emptyResponse(http::status::no_content);
  });
  router.add("GET", "/api/applications/:id/overview", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        StateService(ctx).applicationOverview(std::stoll(c.params["id"]), *c.user));
  });
  router.add("GET", "/api/applications/:id/metrics", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        ApplicationService(ctx).metrics(std::stoll(c.params["id"]), c.query["from"],
                                                        c.query["to"], *c.user));
  });
  router.add("GET", "/api/applications/:id/incidents", true, [&](RequestContext& c) {
    return jsonResponse(http::status::ok,
                        ApplicationService(ctx).incidents(std::stoll(c.params["id"]),
                                                          parseListQuery(c.query), *c.user));
  });

  router.add("GET", "/api/applications/:id/deployments", true, [&](RequestContext& c) {
    requireAppAccess(ctx, c, std::stoll(c.params["id"]));
    return jsonResponse(http::status::ok, ctx.deployments->list(std::stoll(c.params["id"]),
                                                                parseListQuery(c.query)));
  });
  router.add("POST", "/api/applications/:id/deployments", true, [&](RequestContext& c) {
    requireAppAccess(ctx, c, std::stoll(c.params["id"]));
    bool noCache = false;
    if (!c.request.body().empty()) {
      const auto body = nlohmann::json::parse(c.request.body(), nullptr, false);
      if (body.is_object()) {
        noCache = body.value("noCache", false);
      }
    }
    return jsonResponse(http::status::created,
                        ctx.deployments->create(std::stoll(c.params["id"]), *c.user, noCache));
  });
  router.add("GET", "/api/deployments/:id", true, [&](RequestContext& c) {
    auto deployment = ctx.repos->findDeploymentById(std::stoll(c.params["id"]));
    if (!deployment) throw std::runtime_error("Deployment not found");
    requireAppAccess(ctx, c, deployment->applicationId);
    return jsonResponse(http::status::ok, ctx.deployments->get(deployment->id));
  });
  router.add("GET", "/api/deployments/:id/logs", true, [&](RequestContext& c) {
    auto deployment = ctx.repos->findDeploymentById(std::stoll(c.params["id"]));
    if (!deployment) throw std::runtime_error("Deployment not found");
    requireAppAccess(ctx, c, deployment->applicationId);
    const int since = c.query.count("since") ? std::stoi(c.query["since"]) : 0;
    const std::string phase = c.query.count("phase") ? c.query["phase"] : "";
    return jsonResponse(http::status::ok, ctx.deployments->logs(deployment->id, since, phase));
  });

  router.add("GET", "/api/applications/:id/replicas", true, [&](RequestContext& c) {
    requireAppAccess(ctx, c, std::stoll(c.params["id"]));
    return jsonResponse(http::status::ok, ctx.deployments->listReplicas(std::stoll(c.params["id"]),
                                                                        parseListQuery(c.query)));
  });
  router.add("GET", "/api/replicas/:id", true, [&](RequestContext& c) {
    auto replica = ctx.repos->findReplicaById(std::stoll(c.params["id"]));
    if (!replica) throw std::runtime_error("Replica not found");
    requireAppAccess(ctx, c, replica->applicationId);
    return jsonResponse(http::status::ok, ctx.deployments->getReplica(replica->id));
  });
  router.add("POST", "/api/replicas/:id/stop", true, [&](RequestContext& c) {
    auto replica = ctx.repos->findReplicaById(std::stoll(c.params["id"]));
    if (!replica) throw std::runtime_error("Replica not found");
    requireAppAccess(ctx, c, replica->applicationId);
    ctx.deployments->stopReplica(replica->id);
    return emptyResponse(http::status::no_content);
  });
  router.add("POST", "/api/replicas/:id/restart", true, [&](RequestContext& c) {
    auto replica = ctx.repos->findReplicaById(std::stoll(c.params["id"]));
    if (!replica) throw std::runtime_error("Replica not found");
    requireAppAccess(ctx, c, replica->applicationId);
    ctx.deployments->restartReplica(replica->id);
    return emptyResponse(http::status::no_content);
  });
  router.add("GET", "/api/replicas/:id/metrics", true, [&](RequestContext& c) {
    auto replica = ctx.repos->findReplicaById(std::stoll(c.params["id"]));
    if (!replica) throw std::runtime_error("Replica not found");
    requireAppAccess(ctx, c, replica->applicationId);
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& row :
         ctx.repos->listReplicaMetrics(replica->id, c.query["from"], c.query["to"])) {
      rows.push_back({{"id", row.id},
                      {"replicaId", *row.replicaId},
                      {"payload", nlohmann::json::parse(row.payloadJson)},
                      {"collectedAt", row.collectedAt}});
    }
    return jsonResponse(http::status::ok, {{"rows", rows}});
  });
  router.add("POST", "/api/replicas/:id/terminal/sessions", true, [&](RequestContext& c) {
    auto replica = ctx.repos->findReplicaById(std::stoll(c.params["id"]));
    if (!replica) throw std::runtime_error("Replica not found");
    requireAppAccess(ctx, c, replica->applicationId);
    return ctx.terminalPolling->openReplica(replica->id, *c.user, c.request.body());
  });
  router.add("POST", "/api/replicas/:id/terminal/sessions/:streamId/input", true,
             [&](RequestContext& c) {
               auto replica = ctx.repos->findReplicaById(std::stoll(c.params["id"]));
               if (!replica) throw std::runtime_error("Replica not found");
               requireAppAccess(ctx, c, replica->applicationId);
               return ctx.terminalPolling->writeReplica(replica->id, c.params["streamId"], *c.user,
                                                       c.request.body());
             });
  router.add("POST", "/api/replicas/:id/terminal/sessions/:streamId/resize", true,
             [&](RequestContext& c) {
               auto replica = ctx.repos->findReplicaById(std::stoll(c.params["id"]));
               if (!replica) throw std::runtime_error("Replica not found");
               requireAppAccess(ctx, c, replica->applicationId);
               return ctx.terminalPolling->resizeReplica(replica->id, c.params["streamId"], *c.user,
                                                        c.request.body());
             });
  router.add("DELETE", "/api/replicas/:id/terminal/sessions/:streamId", true, [&](RequestContext& c) {
    auto replica = ctx.repos->findReplicaById(std::stoll(c.params["id"]));
    if (!replica) throw std::runtime_error("Replica not found");
    requireAppAccess(ctx, c, replica->applicationId);
    return ctx.terminalPolling->closeReplica(replica->id, c.params["streamId"], *c.user);
  });
  router.add("POST", "/api/replicas/:id/logs/sessions", true, [&](RequestContext& c) {
    auto replica = ctx.repos->findReplicaById(std::stoll(c.params["id"]));
    if (!replica) throw std::runtime_error("Replica not found");
    requireAppAccess(ctx, c, replica->applicationId);
    return ctx.terminalPolling->openReplica(replica->id, *c.user, c.request.body(), true);
  });
  router.add("DELETE", "/api/replicas/:id/logs/sessions/:streamId", true, [&](RequestContext& c) {
    auto replica = ctx.repos->findReplicaById(std::stoll(c.params["id"]));
    if (!replica) throw std::runtime_error("Replica not found");
    requireAppAccess(ctx, c, replica->applicationId);
    return ctx.terminalPolling->closeReplica(replica->id, c.params["streamId"], *c.user);
  });
}

}  // namespace highcube::admin
