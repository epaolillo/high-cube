#include "services/services.hpp"

#include <algorithm>
#include <vector>

namespace highcube::admin {

ServerService::ServerService(AppContext& ctx) : ctx_(ctx) {}

nlohmann::json ServerService::list(const ListQuery& query) {
  const auto page = ctx_.repos->listServers(query);
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& server : page.rows) {
    rows.push_back(serverToJson(server));
  }
  return {{"rows", rows}, {"total", page.total}};
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
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& row : ctx_.repos->listServerMetrics(id, from, to)) {
    rows.push_back({{"id", row.id},
                    {"serverId", row.serverId},
                    {"payload", nlohmann::json::parse(row.payloadJson)},
                    {"collectedAt", row.collectedAt}});
  }
  return {{"rows", rows}};
}

}  // namespace highcube::admin
