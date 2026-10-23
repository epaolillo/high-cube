#include "db/repositories.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace highcube::admin {
namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

// Columns: id, hostname, created_by, access_server_id.
BaseDomain readBaseDomain(SQLite::Statement& st) {
  BaseDomain domain;
  domain.id = st.getColumn(0).getInt64();
  domain.hostname = st.getColumn(1).getString();
  domain.createdBy = st.getColumn(2).getInt64();
  if (!st.getColumn(3).isNull()) {
    domain.accessServerId = st.getColumn(3).getInt64();
  }
  return domain;
}

std::vector<PortMapping> parsePortMappings(const std::string& json) {
  std::vector<PortMapping> rows;
  const auto parsed = nlohmann::json::parse(json.empty() ? "[]" : json, nullptr, false);
  if (!parsed.is_array()) {
    return rows;
  }
  for (const auto& item : parsed) {
    if (!item.is_object()) {
      continue;
    }
    PortMapping mapping;
    mapping.containerPort = item.value("containerPort", 0);
    mapping.hostPort = item.value("hostPort", 0);
    if (mapping.containerPort > 0) {
      rows.push_back(mapping);
    }
  }
  return rows;
}

std::string portMappingsToJson(const std::vector<PortMapping>& mappings) {
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& mapping : mappings) {
    rows.push_back({{"containerPort", mapping.containerPort}, {"hostPort", mapping.hostPort}});
  }
  return rows.dump();
}

constexpr const char* kApplicationSelect =
    "SELECT a.id,a.name,a.type,a.repo_url,a.branch,a.base_directory,a.dockerfile_path,"
    "a.ports_exposes,a.desired_replicas,a.env_json,a.hostname,a.tls_status,"
    "a.health_check_type,a.health_check_path,a.health_check_command,a.max_restart_count,"
    "a.deploy_key_public,a.deploy_key_private,a.created_by,a.github_app_id,a.repo_full_name,"
    "COALESCE(a.port_mappings_json,'[]'),COALESCE(a.pending_redeploy,0),"
    "COALESCE(a.scale_mode,'manual'),COALESCE(a.min_replicas,1),COALESCE(a.max_replicas,1),"
    "COALESCE(a.scale_target_cpu_percent,70),COALESCE(a.scale_target_memory_percent,80),"
    "COALESCE(a.scale_target_load,1.0),COALESCE(a.scale_cooldown_seconds,120),"
    "COALESCE(a.scale_window_seconds,180),COALESCE(a.scale_last_action_at,''),"
    "COALESCE(u.email,''),COALESCE(a.icon,''),COALESCE(a.basic_auth_users,''),"
    "COALESCE(a.database_flavor,''),COALESCE(a.use_gpu,0) "
    "FROM applications a LEFT JOIN users u ON u.id=a.created_by";

int clampPageSize(int pageSize) {
  if (pageSize == 10 || pageSize == 100 || pageSize == 1000) {
    return pageSize;
  }
  return 10;
}

}  // namespace

Repositories::Repositories(Database& database) : database_(database) {}

User Repositories::hydrateUser(SQLite::Statement& st) {
  User user;
  user.id = st.getColumn(0).getInt64();
  user.email = st.getColumn(1).getString();
  user.passwordHash = st.getColumn(2).getString();
  user.name = st.getColumn(3).getString();
  user.locale = st.getColumn(4).getString();
  user.avatarPath = st.getColumn(5).getString();
  user.roleId = st.getColumn(6).getInt64();
  if (st.getColumnCount() > 7) {
    user.role = st.getColumn(7).getText() ? st.getColumn(7).getString() : "";
  }
  return user;
}

Application Repositories::hydrateApplication(SQLite::Statement& st) {
  Application app;
  app.id = st.getColumn(0).getInt64();
  app.name = st.getColumn(1).getString();
  app.type = st.getColumn(2).getString();
  app.repoUrl = st.getColumn(3).getString();
  app.branch = st.getColumn(4).getString();
  app.baseDirectory = st.getColumn(5).getString();
  app.dockerfilePath = st.getColumn(6).getString();
  app.portsExposes = st.getColumn(7).getInt();
  app.desiredReplicas = st.getColumn(8).getInt();
  app.envJson = st.getColumn(9).getString();
  app.hostname = st.getColumn(10).isNull() ? "" : st.getColumn(10).getString();
  app.tlsStatus = st.getColumn(11).getString();
  app.healthCheckType = st.getColumn(12).getString();
  app.healthCheckPath = st.getColumn(13).getString();
  app.healthCheckCommand = st.getColumn(14).getString();
  app.maxRestartCount = st.getColumn(15).getInt();
  app.deployKeyPublic = st.getColumn(16).getString();
  app.deployKeyPrivate = st.getColumn(17).getString();
  app.createdBy = st.getColumn(18).isNull() ? 0 : st.getColumn(18).getInt64();
  if (st.getColumnCount() > 19 && !st.getColumn(19).isNull()) {
    app.githubAppId = st.getColumn(19).getInt64();
  }
  if (st.getColumnCount() > 20) {
    app.repoFullName = st.getColumn(20).isNull() ? "" : st.getColumn(20).getString();
  }
  if (st.getColumnCount() > 21) {
    app.portMappings = parsePortMappings(st.getColumn(21).isNull() ? "[]" : st.getColumn(21).getString());
  }
  if (st.getColumnCount() > 22) {
    app.pendingRedeploy = st.getColumn(22).getInt() != 0;
  }
  if (st.getColumnCount() > 23) {
    app.scaleMode = st.getColumn(23).isNull() ? "manual" : st.getColumn(23).getString();
  }
  if (st.getColumnCount() > 24) {
    app.minReplicas = st.getColumn(24).getInt();
  }
  if (st.getColumnCount() > 25) {
    app.maxReplicas = st.getColumn(25).getInt();
  }
  if (st.getColumnCount() > 26) {
    app.scaleTargetCpuPercent = st.getColumn(26).getDouble();
  }
  if (st.getColumnCount() > 27) {
    app.scaleTargetMemoryPercent = st.getColumn(27).getDouble();
  }
  if (st.getColumnCount() > 28) {
    app.scaleTargetLoad = st.getColumn(28).getDouble();
  }
  if (st.getColumnCount() > 29) {
    app.scaleCooldownSeconds = st.getColumn(29).getInt();
  }
  if (st.getColumnCount() > 30) {
    app.scaleWindowSeconds = st.getColumn(30).getInt();
  }
  if (st.getColumnCount() > 31) {
    app.scaleLastActionAt = st.getColumn(31).isNull() ? "" : st.getColumn(31).getString();
  }
  if (st.getColumnCount() > 32) {
    app.ownerEmail = st.getColumn(32).isNull() ? "" : st.getColumn(32).getString();
  }
  if (st.getColumnCount() > 33) {
    app.icon = st.getColumn(33).isNull() ? "" : st.getColumn(33).getString();
  }
  if (st.getColumnCount() > 34) {
    app.basicAuthUsers = st.getColumn(34).isNull() ? "" : st.getColumn(34).getString();
  }
  if (st.getColumnCount() > 35) {
    app.databaseFlavor = st.getColumn(35).isNull() ? "" : st.getColumn(35).getString();
  }
  if (st.getColumnCount() > 36) {
    app.useGpu = st.getColumn(36).getInt() != 0;
  }
  return app;
}

GithubApp Repositories::hydrateGithubApp(SQLite::Statement& st) {
  GithubApp app;
  app.id = st.getColumn(0).getInt64();
  app.userId = st.getColumn(1).getInt64();
  app.name = st.getColumn(2).getString();
  if (!st.getColumn(3).isNull()) {
    app.appId = st.getColumn(3).getInt64();
  }
  app.slug = st.getColumn(4).getString();
  app.clientId = st.getColumn(5).getString();
  app.clientSecret = st.getColumn(6).getString();
  app.webhookSecret = st.getColumn(7).getString();
  app.privateKeyPem = st.getColumn(8).getString();
  app.htmlUrl = st.getColumn(9).getString();
  app.ownerLogin = st.getColumn(10).getString();
  if (!st.getColumn(11).isNull()) {
    app.installationId = st.getColumn(11).getInt64();
  }
  app.status = st.getColumn(12).getString();
  app.createdAt = st.getColumn(13).getString();
  if (st.getColumnCount() > 14) {
    app.ownerEmail = st.getColumn(14).isNull() ? "" : st.getColumn(14).getString();
  }
  return app;
}

void Repositories::attachUserRelations(User& user) {
  user.serverIds = getUserServers(user.id);
  user.domainIds = getUserBaseDomains(user.id);
}

void Repositories::attachApplicationRelations(Application& app) {
  app.dependencies = getDependencies(app.id);
  app.volumes = getVolumes(app.id);
  app.targetServers = getTargetServers(app.id);
  app.hostnames = getApplicationHostnames(app.id);
  if (!app.hostnames.empty()) {
    app.hostname = app.hostnames.front().hostname;
    app.tlsStatus = app.hostnames.front().tlsStatus;
  }
}

int64_t Repositories::userCount() {
  return database_.db().execAndGet("SELECT COUNT(*) FROM users").getInt64();
}

int64_t Repositories::adminCount() {
  return database_.db()
      .execAndGet(
          "SELECT COUNT(*) FROM users u JOIN roles r ON r.id=u.role_id WHERE r.code='admin'")
      .getInt64();
}

std::optional<User> Repositories::findUserByEmail(const std::string& email) {
  User user;
  {
    SQLite::Statement st(database_.db(),
                         "SELECT u.id,u.email,u.password_hash,u.name,u.locale,u.avatar_path,u.role_id,"
                         "COALESCE(r.code,'') "
                         "FROM users u LEFT JOIN roles r ON r.id=u.role_id WHERE lower(u.email)=?");
    st.bind(1, lower(email));
    if (!st.executeStep()) {
      return std::nullopt;
    }
    user = hydrateUser(st);
  }
  attachUserRelations(user);
  return user;
}

std::optional<User> Repositories::findUserById(int64_t id) {
  User user;
  {
    SQLite::Statement st(database_.db(),
                         "SELECT u.id,u.email,u.password_hash,u.name,u.locale,u.avatar_path,u.role_id,"
                         "COALESCE(r.code,'') "
                         "FROM users u LEFT JOIN roles r ON r.id=u.role_id WHERE u.id=?");
    st.bind(1, id);
    if (!st.executeStep()) {
      return std::nullopt;
    }
    user = hydrateUser(st);
  }
  attachUserRelations(user);
  return user;
}

Page<User> Repositories::listUsers(const ListQuery& query) {
  Page<User> page;
  page.total = database_.db().execAndGet("SELECT COUNT(*) FROM users").getInt64();
  const int pageSize = clampPageSize(query.pageSize);
  const int offset = (std::max(1, query.page) - 1) * pageSize;
  std::string orderBy = "u.id";
  if (query.sort == "email" || query.sort == "name") {
    orderBy = "u." + query.sort;
  }
  const std::string direction = query.order == "desc" ? "DESC" : "ASC";
  {
    SQLite::Statement st(database_.db(),
                         "SELECT u.id,u.email,u.password_hash,u.name,u.locale,u.avatar_path,u.role_id,"
                         "COALESCE(r.code,'') "
                         "FROM users u LEFT JOIN roles r ON r.id=u.role_id ORDER BY " +
                             orderBy + " " + direction + " LIMIT ? OFFSET ?");
    st.bind(1, pageSize);
    st.bind(2, offset);
    while (st.executeStep()) {
      page.rows.push_back(hydrateUser(st));
    }
  }
  for (auto& user : page.rows) {
    attachUserRelations(user);
  }
  return page;
}

int64_t Repositories::createUser(const User& user) {
  SQLite::Statement st(database_.db(),
                       "INSERT INTO users (email,password_hash,name,locale,avatar_path,role_id) "
                       "VALUES (?,?,?,?,?,?)");
  st.bind(1, lower(user.email));
  st.bind(2, user.passwordHash);
  st.bind(3, user.name);
  st.bind(4, user.locale);
  st.bind(5, user.avatarPath);
  st.bind(6, user.roleId);
  st.exec();
  return database_.db().getLastInsertRowid();
}

void Repositories::updateUser(const User& user) {
  SQLite::Statement st(database_.db(),
                       "UPDATE users SET name=?, locale=?, avatar_path=?, role_id=?, password_hash=? WHERE id=?");
  st.bind(1, user.name);
  st.bind(2, user.locale);
  st.bind(3, user.avatarPath);
  st.bind(4, user.roleId);
  st.bind(5, user.passwordHash);
  st.bind(6, user.id);
  st.exec();
}

void Repositories::deleteUser(int64_t id) {
  SQLite::Statement st(database_.db(), "DELETE FROM users WHERE id=?");
  st.bind(1, id);
  st.exec();
}

std::vector<User> Repositories::autocompleteUsers(const std::string& q) {
  std::vector<User> rows;
  {
    SQLite::Statement st(database_.db(),
                         "SELECT u.id,u.email,u.password_hash,u.name,u.locale,u.avatar_path,u.role_id,"
                         "COALESCE(r.code,'') "
                         "FROM users u LEFT JOIN roles r ON r.id=u.role_id "
                         "WHERE u.email LIKE ? OR u.name LIKE ? LIMIT 20");
    const std::string like = "%" + q + "%";
    st.bind(1, like);
    st.bind(2, like);
    while (st.executeStep()) {
      rows.push_back(hydrateUser(st));
    }
  }
  for (auto& user : rows) {
    attachUserRelations(user);
  }
  return rows;
}

int Repositories::countApplicationsByUser(int64_t userId) {
  SQLite::Statement st(database_.db(), "SELECT COUNT(*) FROM applications WHERE created_by=?");
  st.bind(1, userId);
  st.executeStep();
  return st.getColumn(0).getInt();
}

int Repositories::countGithubAppsByUser(int64_t userId) {
  SQLite::Statement st(database_.db(), "SELECT COUNT(*) FROM github_apps WHERE user_id=?");
  st.bind(1, userId);
  st.executeStep();
  return st.getColumn(0).getInt();
}

std::optional<Role> Repositories::findRoleByCode(const std::string& code) {
  SQLite::Statement st(database_.db(),
                       "SELECT id,name,COALESCE(code,''),is_builtin FROM roles WHERE code=?");
  st.bind(1, code);
  if (!st.executeStep()) {
    return std::nullopt;
  }
  return Role{st.getColumn(0).getInt64(), st.getColumn(1).getString(), st.getColumn(2).getString(),
              st.getColumn(3).getInt() == 1};
}

std::optional<Role> Repositories::findRoleById(int64_t id) {
  SQLite::Statement st(database_.db(),
                       "SELECT id,name,COALESCE(code,''),is_builtin FROM roles WHERE id=?");
  st.bind(1, id);
  if (!st.executeStep()) {
    return std::nullopt;
  }
  return Role{st.getColumn(0).getInt64(), st.getColumn(1).getString(), st.getColumn(2).getString(),
              st.getColumn(3).getInt() == 1};
}

std::vector<int64_t> Repositories::getUserServers(int64_t userId) {
  std::vector<int64_t> ids;
  SQLite::Statement st(database_.db(), "SELECT server_id FROM user_servers WHERE user_id=?");
  st.bind(1, userId);
  while (st.executeStep()) {
    ids.push_back(st.getColumn(0).getInt64());
  }
  return ids;
}

void Repositories::setUserServers(int64_t userId, const std::vector<int64_t>& serverIds) {
  SQLite::Transaction tx(database_.db());
  SQLite::Statement del(database_.db(), "DELETE FROM user_servers WHERE user_id=?");
  del.bind(1, userId);
  del.exec();
  SQLite::Statement ins(database_.db(),
                        "INSERT INTO user_servers (user_id, server_id) VALUES (?,?)");
  for (const auto serverId : serverIds) {
    ins.bind(1, userId);
    ins.bind(2, serverId);
    ins.exec();
    ins.reset();
  }
  tx.commit();
}

std::vector<int64_t> Repositories::getUserBaseDomains(int64_t userId) {
  std::vector<int64_t> ids;
  SQLite::Statement st(database_.db(),
                       "SELECT base_domain_id FROM user_base_domains WHERE user_id=?");
  st.bind(1, userId);
  while (st.executeStep()) {
    ids.push_back(st.getColumn(0).getInt64());
  }
  return ids;
}

void Repositories::setUserBaseDomains(int64_t userId, const std::vector<int64_t>& domainIds) {
  SQLite::Transaction tx(database_.db());
  SQLite::Statement del(database_.db(), "DELETE FROM user_base_domains WHERE user_id=?");
  del.bind(1, userId);
  del.exec();
  SQLite::Statement ins(database_.db(),
                        "INSERT INTO user_base_domains (user_id, base_domain_id) VALUES (?,?)");
  for (const auto domainId : domainIds) {
    ins.bind(1, userId);
    ins.bind(2, domainId);
    ins.exec();
    ins.reset();
  }
  tx.commit();
}

std::vector<BaseDomain> Repositories::listBaseDomainsForUser(int64_t userId) {
  std::vector<BaseDomain> rows;
  SQLite::Statement st(
      database_.db(),
      "SELECT d.id,d.hostname,COALESCE(d.created_by,0),d.access_server_id FROM base_domains d "
      "INNER JOIN user_base_domains ud ON ud.base_domain_id=d.id "
      "WHERE ud.user_id=? ORDER BY d.hostname ASC");
  st.bind(1, userId);
  while (st.executeStep()) {
    rows.push_back(readBaseDomain(st));
  }
  return rows;
}

std::vector<BaseDomain> Repositories::listBaseDomainsUsedByUserApplications(int64_t userId) {
  std::vector<BaseDomain> rows;
  SQLite::Statement st(
      database_.db(),
      "SELECT id,hostname,COALESCE(created_by,0),access_server_id FROM base_domains WHERE id IN ("
      "SELECT ah.base_domain_id FROM application_hostnames ah "
      "INNER JOIN applications a ON a.id=ah.application_id WHERE a.created_by=?) "
      "ORDER BY hostname ASC");
  st.bind(1, userId);
  while (st.executeStep()) {
    rows.push_back(readBaseDomain(st));
  }
  return rows;
}

bool Repositories::userHasBaseDomain(int64_t userId, int64_t domainId) {
  SQLite::Statement st(database_.db(),
                       "SELECT 1 FROM user_base_domains WHERE user_id=? AND base_domain_id=?");
  st.bind(1, userId);
  st.bind(2, domainId);
  return st.executeStep();
}

std::vector<Server> Repositories::listHealthyServersForUser(int64_t userId) {
  std::vector<Server> rows;
  SQLite::Statement st(database_.db(),
                       "SELECT s.id,s.name,s.ip,s.agent_token_hash,COALESCE(s.last_seen_at,''),s.status,COALESCE(s.icon,'') "
                       "FROM servers s INNER JOIN user_servers us ON us.server_id=s.id "
                       "WHERE us.user_id=? AND s.status='online'");
  st.bind(1, userId);
  while (st.executeStep()) {
    Server s;
    s.id = st.getColumn(0).getInt64();
    s.name = st.getColumn(1).getString();
    s.ip = st.getColumn(2).getString();
    s.agentTokenHash = st.getColumn(3).getString();
    s.lastSeenAt = st.getColumn(4).getString();
    s.status = st.getColumn(5).getString();
    s.icon = st.getColumn(6).getString();
    rows.push_back(s);
  }
  return rows;
}

Page<Server> Repositories::listServers(const ListQuery& query) {
  Page<Server> page;
  page.total = database_.db().execAndGet("SELECT COUNT(*) FROM servers").getInt64();
  const int pageSize = clampPageSize(query.pageSize);
  const int offset = (std::max(1, query.page) - 1) * pageSize;
  SQLite::Statement st(database_.db(),
                       "SELECT id,name,ip,agent_token_hash,COALESCE(last_seen_at,''),status,COALESCE(icon,'') "
                       "FROM servers ORDER BY name ASC LIMIT ? OFFSET ?");
  st.bind(1, pageSize);
  st.bind(2, offset);
  while (st.executeStep()) {
    Server s;
    s.id = st.getColumn(0).getInt64();
    s.name = st.getColumn(1).getString();
    s.ip = st.getColumn(2).getString();
    s.agentTokenHash = st.getColumn(3).getString();
    s.lastSeenAt = st.getColumn(4).getString();
    s.status = st.getColumn(5).getString();
    s.icon = st.getColumn(6).getString();
    page.rows.push_back(s);
  }
  return page;
}

std::optional<Server> Repositories::findServerById(int64_t id) {
  SQLite::Statement st(database_.db(),
                       "SELECT id,name,ip,agent_token_hash,COALESCE(last_seen_at,''),status,COALESCE(icon,'') FROM servers WHERE id=?");
  st.bind(1, id);
  if (!st.executeStep()) {
    return std::nullopt;
  }
  Server s;
  s.id = st.getColumn(0).getInt64();
  s.name = st.getColumn(1).getString();
  s.ip = st.getColumn(2).getString();
  s.agentTokenHash = st.getColumn(3).getString();
  s.lastSeenAt = st.getColumn(4).getString();
  s.status = st.getColumn(5).getString();
  s.icon = st.getColumn(6).getString();
  return s;
}

std::optional<Server> Repositories::findServerByTokenHash(const std::string& hash) {
  SQLite::Statement st(database_.db(),
                       "SELECT id,name,ip,agent_token_hash,COALESCE(last_seen_at,''),status,COALESCE(icon,'') FROM servers WHERE agent_token_hash=?");
  st.bind(1, hash);
  if (!st.executeStep()) {
    return std::nullopt;
  }
  Server s;
  s.id = st.getColumn(0).getInt64();
  s.name = st.getColumn(1).getString();
  s.ip = st.getColumn(2).getString();
  s.agentTokenHash = st.getColumn(3).getString();
  s.lastSeenAt = st.getColumn(4).getString();
  s.status = st.getColumn(5).getString();
  s.icon = st.getColumn(6).getString();
  return s;
}

std::optional<Server> Repositories::findServerByName(const std::string& name) {
  SQLite::Statement st(database_.db(),
                       "SELECT id,name,ip,agent_token_hash,COALESCE(last_seen_at,''),status,COALESCE(icon,'') FROM servers WHERE name=?");
  st.bind(1, name);
  if (!st.executeStep()) {
    return std::nullopt;
  }
  Server s;
  s.id = st.getColumn(0).getInt64();
  s.name = st.getColumn(1).getString();
  s.ip = st.getColumn(2).getString();
  s.agentTokenHash = st.getColumn(3).getString();
  s.lastSeenAt = st.getColumn(4).getString();
  s.status = st.getColumn(5).getString();
  s.icon = st.getColumn(6).getString();
  return s;
}

int64_t Repositories::createServer(const Server& server) {
  SQLite::Statement st(database_.db(),
                       "INSERT INTO servers (name,ip,agent_token_hash,status) VALUES (?,?,?,?)");
  st.bind(1, server.name);
  st.bind(2, server.ip);
  st.bind(3, server.agentTokenHash);
  st.bind(4, server.status);
  st.exec();
  return database_.db().getLastInsertRowid();
}

void Repositories::updateServer(const Server& server) {
  SQLite::Statement st(database_.db(),
                       "UPDATE servers SET name=?, ip=?, agent_token_hash=?, status=?, icon=? WHERE id=?");
  st.bind(1, server.name);
  st.bind(2, server.ip);
  st.bind(3, server.agentTokenHash);
  st.bind(4, server.status);
  st.bind(5, server.icon);
  st.bind(6, server.id);
  st.exec();
}

void Repositories::touchServer(int64_t id, const std::string& status) {
  SQLite::Statement st(database_.db(),
                       "UPDATE servers SET last_seen_at=strftime('%Y-%m-%dT%H:%M:%fZ','now'), status=? WHERE id=?");
  st.bind(1, status);
  st.bind(2, id);
  st.exec();
}

void Repositories::setServerStatus(int64_t id, const std::string& status) {
  SQLite::Statement st(database_.db(), "UPDATE servers SET status=? WHERE id=?");
  st.bind(1, status);
  st.bind(2, id);
  st.exec();
}

void Repositories::markOnlineServersOffline() {
  database_.db().exec("UPDATE servers SET status='offline' WHERE status='online'");
}

void Repositories::deleteServer(int64_t id) {
  SQLite::Transaction tx(database_.db());
  SQLite::Statement domains(database_.db(),
                            "UPDATE base_domains SET access_server_id=NULL WHERE access_server_id=?");
  domains.bind(1, id);
  domains.exec();
  // replicas.server_id has no ON DELETE CASCADE, so children must go first.
  SQLite::Statement replicas(database_.db(), "DELETE FROM replicas WHERE server_id=?");
  replicas.bind(1, id);
  replicas.exec();
  SQLite::Statement st(database_.db(), "DELETE FROM servers WHERE id=?");
  st.bind(1, id);
  st.exec();
  tx.commit();
}

std::vector<Server> Repositories::autocompleteServers(const std::string& q) {
  std::vector<Server> rows;
  SQLite::Statement st(database_.db(),
                       "SELECT id,name,ip,agent_token_hash,COALESCE(last_seen_at,''),status,COALESCE(icon,'') "
                       "FROM servers WHERE name LIKE ? OR ip LIKE ? LIMIT 20");
  const std::string like = "%" + q + "%";
  st.bind(1, like);
  st.bind(2, like);
  while (st.executeStep()) {
    Server s;
    s.id = st.getColumn(0).getInt64();
    s.name = st.getColumn(1).getString();
    s.ip = st.getColumn(2).getString();
    s.agentTokenHash = st.getColumn(3).getString();
    s.lastSeenAt = st.getColumn(4).getString();
    s.status = st.getColumn(5).getString();
    s.icon = st.getColumn(6).getString();
    rows.push_back(s);
  }
  return rows;
}

std::vector<Server> Repositories::listServersHostingUserApplications(int64_t userId) {
  std::vector<Server> rows;
  SQLite::Statement st(database_.db(),
                       "SELECT id,name,ip,agent_token_hash,COALESCE(last_seen_at,''),status,COALESCE(icon,'') "
                       "FROM servers WHERE id IN ("
                       "SELECT r.server_id FROM replicas r "
                       "INNER JOIN applications a ON a.id=r.application_id WHERE a.created_by=?) "
                       "ORDER BY name ASC");
  st.bind(1, userId);
  while (st.executeStep()) {
    Server s;
    s.id = st.getColumn(0).getInt64();
    s.name = st.getColumn(1).getString();
    s.ip = st.getColumn(2).getString();
    s.agentTokenHash = st.getColumn(3).getString();
    s.lastSeenAt = st.getColumn(4).getString();
    s.status = st.getColumn(5).getString();
    s.icon = st.getColumn(6).getString();
    rows.push_back(s);
  }
  return rows;
}

std::vector<Server> Repositories::listHealthyServers() {
  std::vector<Server> rows;
  SQLite::Statement st(database_.db(),
                       "SELECT id,name,ip,agent_token_hash,COALESCE(last_seen_at,''),status,COALESCE(icon,'') "
                       "FROM servers WHERE status='online'");
  while (st.executeStep()) {
    Server s;
    s.id = st.getColumn(0).getInt64();
    s.name = st.getColumn(1).getString();
    s.ip = st.getColumn(2).getString();
    s.agentTokenHash = st.getColumn(3).getString();
    s.lastSeenAt = st.getColumn(4).getString();
    s.status = st.getColumn(5).getString();
    s.icon = st.getColumn(6).getString();
    rows.push_back(s);
  }
  return rows;
}

std::string Repositories::getSetting(const std::string& key, const std::string& fallback) {
  SQLite::Statement st(database_.db(), "SELECT value FROM settings WHERE key=?");
  st.bind(1, key);
  if (!st.executeStep()) {
    return fallback;
  }
  return st.getColumn(0).getString();
}

void Repositories::setSetting(const std::string& key, const std::string& value) {
  SQLite::Statement st(database_.db(),
                       "INSERT INTO settings (key,value) VALUES (?,?) "
                       "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
  st.bind(1, key);
  st.bind(2, value);
  st.exec();
}

Page<BaseDomain> Repositories::listBaseDomains(const ListQuery& query) {
  Page<BaseDomain> page;
  page.total = database_.db().execAndGet("SELECT COUNT(*) FROM base_domains").getInt64();
  const int pageSize = clampPageSize(query.pageSize);
  const int offset = (std::max(1, query.page) - 1) * pageSize;
  SQLite::Statement st(database_.db(),
                       "SELECT id,hostname,COALESCE(created_by,0),access_server_id FROM base_domains "
                       "ORDER BY hostname ASC LIMIT ? OFFSET ?");
  st.bind(1, pageSize);
  st.bind(2, offset);
  while (st.executeStep()) {
    page.rows.push_back(readBaseDomain(st));
  }
  return page;
}

int64_t Repositories::createBaseDomain(const BaseDomain& domain) {
  SQLite::Statement st(database_.db(),
                       "INSERT INTO base_domains (hostname, created_by, access_server_id) VALUES (?,?,?)");
  st.bind(1, lower(domain.hostname));
  st.bind(2, domain.createdBy);
  if (domain.accessServerId) {
    st.bind(3, *domain.accessServerId);
  } else {
    st.bind(3);
  }
  st.exec();
  return database_.db().getLastInsertRowid();
}

void Repositories::deleteBaseDomain(int64_t id) {
  SQLite::Statement st(database_.db(), "DELETE FROM base_domains WHERE id=?");
  st.bind(1, id);
  st.exec();
}

void Repositories::setBaseDomainAccessServer(int64_t id, std::optional<int64_t> serverId) {
  SQLite::Statement st(database_.db(), "UPDATE base_domains SET access_server_id=? WHERE id=?");
  if (serverId) {
    st.bind(1, *serverId);
  } else {
    st.bind(1);
  }
  st.bind(2, id);
  st.exec();
}

std::optional<BaseDomain> Repositories::findBaseDomainById(int64_t id) {
  SQLite::Statement st(database_.db(),
                       "SELECT id,hostname,COALESCE(created_by,0),access_server_id FROM base_domains WHERE id=?");
  st.bind(1, id);
  if (!st.executeStep()) {
    return std::nullopt;
  }
  return readBaseDomain(st);
}

bool Repositories::hostnameMatchesBaseDomain(const std::string& hostname) {
  const auto host = lower(hostname);
  SQLite::Statement st(database_.db(), "SELECT hostname FROM base_domains");
  while (st.executeStep()) {
    const auto base = st.getColumn(0).getString();
    const auto suffix = "." + base;
    if (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) {
      const auto label = host.substr(0, host.size() - suffix.size());
      if (label.find('.') == std::string::npos && !label.empty()) {
        return true;
      }
    }
  }
  return false;
}

int Repositories::applicationsUsingBaseDomain(int64_t id) {
  SQLite::Statement st(database_.db(),
                       "SELECT COUNT(DISTINCT application_id) FROM application_hostnames "
                       "WHERE base_domain_id=?");
  st.bind(1, id);
  st.executeStep();
  return st.getColumn(0).getInt();
}

std::vector<int64_t> Repositories::applicationIdsUsingBaseDomain(int64_t id) {
  std::vector<int64_t> ids;
  SQLite::Statement st(database_.db(),
                       "SELECT DISTINCT application_id FROM application_hostnames WHERE base_domain_id=?");
  st.bind(1, id);
  while (st.executeStep()) {
    ids.push_back(st.getColumn(0).getInt64());
  }
  return ids;
}

std::vector<ApplicationHostname> Repositories::getApplicationHostnames(int64_t applicationId) {
  std::vector<ApplicationHostname> rows;
  SQLite::Statement st(
      database_.db(),
      "SELECT ah.id,ah.application_id,ah.hostname,ah.base_domain_id,ah.tls_status,ah.position,"
      "COALESCE(bd.hostname,'') "
      "FROM application_hostnames ah LEFT JOIN base_domains bd ON bd.id=ah.base_domain_id "
      "WHERE ah.application_id=? ORDER BY ah.position ASC, ah.id ASC");
  st.bind(1, applicationId);
  while (st.executeStep()) {
    ApplicationHostname row;
    row.id = st.getColumn(0).getInt64();
    row.applicationId = st.getColumn(1).getInt64();
    row.hostname = st.getColumn(2).getString();
    row.baseDomainId = st.getColumn(3).getInt64();
    row.tlsStatus = st.getColumn(4).getString();
    row.position = st.getColumn(5).getInt();
    row.baseDomain = lower(st.getColumn(6).getString());
    rows.push_back(row);
  }
  return rows;
}

void Repositories::setApplicationHostnames(int64_t applicationId,
                                           const std::vector<ApplicationHostname>& hostnames) {
  SQLite::Transaction tx(database_.db());
  SQLite::Statement del(database_.db(), "DELETE FROM application_hostnames WHERE application_id=?");
  del.bind(1, applicationId);
  del.exec();
  SQLite::Statement ins(
      database_.db(),
      "INSERT INTO application_hostnames "
      "(application_id, hostname, base_domain_id, tls_status, position) VALUES (?,?,?,?,?)");
  for (size_t i = 0; i < hostnames.size(); ++i) {
    const auto& row = hostnames[i];
    ins.bind(1, applicationId);
    ins.bind(2, lower(row.hostname));
    ins.bind(3, row.baseDomainId);
    ins.bind(4, row.tlsStatus.empty() ? "pending" : row.tlsStatus);
    ins.bind(5, static_cast<int>(i));
    ins.exec();
    ins.reset();
  }
  tx.commit();
  syncPrimaryHostname(applicationId);
}

bool Repositories::hostnameExists(const std::string& hostname,
                                  std::optional<int64_t> excludeApplicationId) {
  const auto host = lower(hostname);
  if (excludeApplicationId) {
    SQLite::Statement st(database_.db(),
                         "SELECT 1 FROM application_hostnames WHERE hostname=? AND application_id!=?");
    st.bind(1, host);
    st.bind(2, *excludeApplicationId);
    if (st.executeStep()) {
      return true;
    }
  } else {
    SQLite::Statement st(database_.db(), "SELECT 1 FROM application_hostnames WHERE hostname=?");
    st.bind(1, host);
    if (st.executeStep()) {
      return true;
    }
  }
  return false;
}

bool Repositories::subdomainLabelExists(const std::string& subdomain) {
  const auto label = lower(subdomain);
  SQLite::Statement st(database_.db(), "SELECT hostname FROM application_hostnames");
  while (st.executeStep()) {
    const auto host = st.getColumn(0).getString();
    const auto dot = host.find('.');
    if (dot != std::string::npos && host.substr(0, dot) == label) {
      return true;
    }
  }
  return false;
}

void Repositories::setHostnameTlsStatus(int64_t applicationId, const std::string& hostname,
                                        const std::string& status) {
  SQLite::Statement st(database_.db(),
                       "UPDATE application_hostnames SET tls_status=? "
                       "WHERE application_id=? AND hostname=?");
  st.bind(1, status);
  st.bind(2, applicationId);
  st.bind(3, lower(hostname));
  st.exec();
  syncPrimaryHostname(applicationId);
}

void Repositories::syncPrimaryHostname(int64_t applicationId) {
  auto rows = getApplicationHostnames(applicationId);
  SQLite::Statement st(database_.db(),
                       "UPDATE applications SET hostname=?, tls_status=? WHERE id=?");
  if (rows.empty()) {
    st.bind(1);
    st.bind(2, "none");
  } else {
    st.bind(1, rows.front().hostname);
    st.bind(2, rows.front().tlsStatus);
  }
  st.bind(3, applicationId);
  st.exec();
}

std::vector<BaseDomain> Repositories::autocompleteBaseDomains(const std::string& q) {
  std::vector<BaseDomain> rows;
  SQLite::Statement st(database_.db(),
                       "SELECT id,hostname,COALESCE(created_by,0),access_server_id FROM base_domains "
                       "WHERE hostname LIKE ? LIMIT 20");
  st.bind(1, "%" + q + "%");
  while (st.executeStep()) {
    rows.push_back(readBaseDomain(st));
  }
  return rows;
}

Page<Application> Repositories::listApplications(const ListQuery& query) {
  Page<Application> page;
  const int pageSize = clampPageSize(query.pageSize);
  const int offset = (std::max(1, query.page) - 1) * pageSize;
  {
    // Count respects the same filters as the page query so pagination totals
    // stay accurate per owner and per category.
    std::string countSql = "SELECT COUNT(*) FROM applications";
    std::string where;
    if (query.ownerId) {
      where += where.empty() ? " WHERE " : " AND ";
      where += "created_by=?";
    }
    if (!query.typeFilter.empty()) {
      where += where.empty() ? " WHERE " : " AND ";
      where += "type=?";
    }
    countSql += where;
    SQLite::Statement count(database_.db(), countSql);
    int bind = 1;
    if (query.ownerId) {
      count.bind(bind++, *query.ownerId);
    }
    if (!query.typeFilter.empty()) {
      count.bind(bind++, query.typeFilter);
    }
    count.executeStep();
    page.total = count.getColumn(0).getInt64();
  }
  {
    std::string sql = std::string(kApplicationSelect);
    std::string where;
    if (query.ownerId) {
      where += where.empty() ? " WHERE " : " AND ";
      where += "a.created_by=?";
    }
    if (!query.typeFilter.empty()) {
      where += where.empty() ? " WHERE " : " AND ";
      where += "a.type=?";
    }
    sql += where;
    sql += " ORDER BY a.name ASC LIMIT ? OFFSET ?";
    SQLite::Statement st(database_.db(), sql);
    int bind = 1;
    if (query.ownerId) {
      st.bind(bind++, *query.ownerId);
    }
    if (!query.typeFilter.empty()) {
      st.bind(bind++, query.typeFilter);
    }
    st.bind(bind++, pageSize);
    st.bind(bind, offset);
    while (st.executeStep()) {
      page.rows.push_back(hydrateApplication(st));
    }
  }
  for (auto& app : page.rows) {
    attachApplicationRelations(app);
  }
  return page;
}

std::optional<Application> Repositories::findApplicationById(int64_t id) {
  Application app;
  {
    SQLite::Statement st(database_.db(), std::string(kApplicationSelect) + " WHERE a.id=?");
    st.bind(1, id);
    if (!st.executeStep()) {
      return std::nullopt;
    }
    app = hydrateApplication(st);
  }
  attachApplicationRelations(app);
  return app;
}

int64_t Repositories::createApplication(const Application& app) {
  SQLite::Statement st(
      database_.db(),
      "INSERT INTO applications (name,type,repo_url,branch,base_directory,dockerfile_path,"
      "ports_exposes,desired_replicas,env_json,hostname,tls_status,health_check_type,"
      "health_check_path,health_check_command,max_restart_count,created_by,github_app_id,"
      "repo_full_name,port_mappings_json,pending_redeploy,scale_mode,min_replicas,max_replicas,"
      "scale_target_cpu_percent,scale_target_memory_percent,scale_target_load,"
      "scale_cooldown_seconds,scale_window_seconds,scale_last_action_at,icon,basic_auth_users,"
      "database_flavor,use_gpu) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  st.bind(1, app.name);
  st.bind(2, app.type);
  st.bind(3, app.repoUrl);
  st.bind(4, app.branch);
  st.bind(5, app.baseDirectory);
  st.bind(6, app.dockerfilePath);
  st.bind(7, app.portsExposes);
  st.bind(8, app.desiredReplicas);
  st.bind(9, app.envJson);
  if (app.hostname.empty()) {
    st.bind(10);
  } else {
    st.bind(10, lower(app.hostname));
  }
  st.bind(11, app.tlsStatus);
  st.bind(12, app.healthCheckType);
  st.bind(13, app.healthCheckPath);
  st.bind(14, app.healthCheckCommand);
  st.bind(15, app.maxRestartCount);
  st.bind(16, app.createdBy);
  if (app.githubAppId) {
    st.bind(17, *app.githubAppId);
  } else {
    st.bind(17);
  }
  st.bind(18, app.repoFullName);
  st.bind(19, portMappingsToJson(app.portMappings));
  st.bind(20, app.pendingRedeploy ? 1 : 0);
  st.bind(21, app.scaleMode.empty() ? "manual" : app.scaleMode);
  st.bind(22, app.minReplicas);
  st.bind(23, app.maxReplicas);
  st.bind(24, app.scaleTargetCpuPercent);
  st.bind(25, app.scaleTargetMemoryPercent);
  st.bind(26, app.scaleTargetLoad);
  st.bind(27, app.scaleCooldownSeconds);
  st.bind(28, app.scaleWindowSeconds);
  st.bind(29, app.scaleLastActionAt);
  st.bind(30, app.icon);
  st.bind(31, app.basicAuthUsers);
  st.bind(32, app.databaseFlavor);
  st.bind(33, app.useGpu ? 1 : 0);
  st.exec();
  const int64_t id = database_.db().getLastInsertRowid();
  setDependencies(id, app.dependencies);
  setTargetServers(id, app.targetServers);
  if (!app.hostnames.empty()) {
    setApplicationHostnames(id, app.hostnames);
  }
  for (const auto& volume : app.volumes) {
    addVolume(id, volume.name, volume.mountPath, volume.hostPath);
  }
  return id;
}

void Repositories::updateApplication(const Application& app) {
  SQLite::Statement st(
      database_.db(),
      "UPDATE applications SET name=?, type=?, repo_url=?, branch=?, base_directory=?, "
      "dockerfile_path=?, ports_exposes=?, desired_replicas=?, env_json=?, hostname=?, "
      "tls_status=?, health_check_type=?, health_check_path=?, health_check_command=?, "
      "max_restart_count=?, github_app_id=?, repo_full_name=?, port_mappings_json=?, "
      "pending_redeploy=?, scale_mode=?, min_replicas=?, max_replicas=?, "
      "scale_target_cpu_percent=?, scale_target_memory_percent=?, scale_target_load=?, "
      "scale_cooldown_seconds=?, scale_window_seconds=?, scale_last_action_at=?, icon=?, "
      "basic_auth_users=?, database_flavor=?, created_by=?, use_gpu=? WHERE id=?");
  st.bind(1, app.name);
  st.bind(2, app.type);
  st.bind(3, app.repoUrl);
  st.bind(4, app.branch);
  st.bind(5, app.baseDirectory);
  st.bind(6, app.dockerfilePath);
  st.bind(7, app.portsExposes);
  st.bind(8, app.desiredReplicas);
  st.bind(9, app.envJson);
  if (app.hostname.empty()) {
    st.bind(10);
  } else {
    st.bind(10, lower(app.hostname));
  }
  st.bind(11, app.tlsStatus);
  st.bind(12, app.healthCheckType);
  st.bind(13, app.healthCheckPath);
  st.bind(14, app.healthCheckCommand);
  st.bind(15, app.maxRestartCount);
  if (app.githubAppId) {
    st.bind(16, *app.githubAppId);
  } else {
    st.bind(16);
  }
  st.bind(17, app.repoFullName);
  st.bind(18, portMappingsToJson(app.portMappings));
  st.bind(19, app.pendingRedeploy ? 1 : 0);
  st.bind(20, app.scaleMode.empty() ? "manual" : app.scaleMode);
  st.bind(21, app.minReplicas);
  st.bind(22, app.maxReplicas);
  st.bind(23, app.scaleTargetCpuPercent);
  st.bind(24, app.scaleTargetMemoryPercent);
  st.bind(25, app.scaleTargetLoad);
  st.bind(26, app.scaleCooldownSeconds);
  st.bind(27, app.scaleWindowSeconds);
  st.bind(28, app.scaleLastActionAt);
  st.bind(29, app.icon);
  st.bind(30, app.basicAuthUsers);
  st.bind(31, app.databaseFlavor);
  if (app.createdBy > 0) {
    st.bind(32, app.createdBy);
  } else {
    st.bind(32);
  }
  st.bind(33, app.useGpu ? 1 : 0);
  st.bind(34, app.id);
  st.exec();
  setDependencies(app.id, app.dependencies);
  setTargetServers(app.id, app.targetServers);
  setApplicationHostnames(app.id, app.hostnames);
}

void Repositories::updateApplicationEnv(int64_t id, const std::string& envJson) {
  SQLite::Statement st(database_.db(), "UPDATE applications SET env_json=? WHERE id=?");
  st.bind(1, envJson);
  st.bind(2, id);
  st.exec();
}

void Repositories::updateHealthCheckCommand(int64_t id, const std::string& command) {
  SQLite::Statement st(database_.db(),
                       "UPDATE applications SET health_check_command=? WHERE id=?");
  st.bind(1, command);
  st.bind(2, id);
  st.exec();
}

void Repositories::setPendingRedeploy(int64_t id, bool pending) {
  SQLite::Statement st(database_.db(), "UPDATE applications SET pending_redeploy=? WHERE id=?");
  st.bind(1, pending ? 1 : 0);
  st.bind(2, id);
  st.exec();
}

void Repositories::deleteApplication(int64_t id) {
  SQLite::Statement st(database_.db(), "DELETE FROM applications WHERE id=?");
  st.bind(1, id);
  st.exec();
}

void Repositories::setTlsStatus(int64_t id, const std::string& status) {
  SQLite::Statement st(database_.db(), "UPDATE applications SET tls_status=? WHERE id=?");
  st.bind(1, status);
  st.bind(2, id);
  st.exec();
}

void Repositories::setDeployKey(int64_t id, const std::string& pub, const std::string& priv) {
  SQLite::Statement st(database_.db(),
                       "UPDATE applications SET deploy_key_public=?, deploy_key_private=? WHERE id=?");
  st.bind(1, pub);
  st.bind(2, priv);
  st.bind(3, id);
  st.exec();
}

std::vector<Application> Repositories::autocompleteApplications(const std::string& q,
                                                                bool privateOnly,
                                                                std::optional<int64_t> ownerId) {
  std::vector<Application> rows;
  {
    std::string sql = std::string(kApplicationSelect) + " WHERE a.name LIKE ?";
    (void)privateOnly;
    if (ownerId) {
      sql += " AND a.created_by=?";
    }
    sql += " LIMIT 20";
    SQLite::Statement st(database_.db(), sql);
    st.bind(1, "%" + q + "%");
    if (ownerId) {
      st.bind(2, *ownerId);
    }
    while (st.executeStep()) {
      rows.push_back(hydrateApplication(st));
    }
  }
  for (auto& app : rows) {
    attachApplicationRelations(app);
  }
  return rows;
}

std::vector<int64_t> Repositories::getDependencies(int64_t applicationId) {
  std::vector<int64_t> deps;
  SQLite::Statement st(database_.db(),
                       "SELECT dependency_application_id FROM application_dependencies WHERE application_id=?");
  st.bind(1, applicationId);
  while (st.executeStep()) {
    deps.push_back(st.getColumn(0).getInt64());
  }
  return deps;
}

void Repositories::setDependencies(int64_t applicationId, const std::vector<int64_t>& deps) {
  SQLite::Statement del(database_.db(),
                        "DELETE FROM application_dependencies WHERE application_id=?");
  del.bind(1, applicationId);
  del.exec();
  SQLite::Statement ins(database_.db(),
                        "INSERT INTO application_dependencies (application_id, dependency_application_id) VALUES (?,?)");
  for (const auto dep : deps) {
    ins.bind(1, applicationId);
    ins.bind(2, dep);
    ins.exec();
    ins.reset();
  }
}

std::vector<ApplicationServerTarget> Repositories::getTargetServers(int64_t applicationId) {
  std::vector<ApplicationServerTarget> rows;
  SQLite::Statement st(database_.db(),
                       "SELECT server_id, priority FROM application_target_servers "
                       "WHERE application_id=? ORDER BY priority ASC, server_id ASC");
  st.bind(1, applicationId);
  while (st.executeStep()) {
    ApplicationServerTarget target;
    target.serverId = st.getColumn(0).getInt64();
    target.priority = st.getColumn(1).getInt();
    rows.push_back(target);
  }
  return rows;
}

void Repositories::setTargetServers(int64_t applicationId,
                                    const std::vector<ApplicationServerTarget>& targets) {
  SQLite::Statement del(database_.db(),
                        "DELETE FROM application_target_servers WHERE application_id=?");
  del.bind(1, applicationId);
  del.exec();
  SQLite::Statement ins(database_.db(),
                        "INSERT INTO application_target_servers (application_id, server_id, priority) "
                        "VALUES (?,?,?)");
  for (const auto& target : targets) {
    ins.bind(1, applicationId);
    ins.bind(2, target.serverId);
    ins.bind(3, target.priority);
    ins.exec();
    ins.reset();
  }
}

namespace {

ApplicationVolume hydrateVolumeRow(SQLite::Statement& st) {
  return ApplicationVolume{st.getColumn(0).getInt64(), st.getColumn(1).getInt64(),
                           st.getColumn(2).getString(), st.getColumn(3).getString(),
                           st.getColumn(4).getString()};
}

}  // namespace

std::vector<ApplicationVolume> Repositories::getVolumes(int64_t applicationId) {
  std::vector<ApplicationVolume> volumes;
  SQLite::Statement st(database_.db(),
                       "SELECT id,application_id,name,mount_path,host_path FROM application_volumes "
                       "WHERE application_id=?");
  st.bind(1, applicationId);
  while (st.executeStep()) {
    volumes.push_back(hydrateVolumeRow(st));
  }
  return volumes;
}

std::vector<ApplicationVolume> Repositories::listVolumesWithHostPath() {
  std::vector<ApplicationVolume> volumes;
  SQLite::Statement st(database_.db(),
                       "SELECT id,application_id,name,mount_path,host_path FROM application_volumes "
                       "WHERE host_path <> ''");
  while (st.executeStep()) {
    volumes.push_back(hydrateVolumeRow(st));
  }
  return volumes;
}

int64_t Repositories::addVolume(int64_t applicationId, const std::string& name,
                                const std::string& mountPath, const std::string& hostPath) {
  SQLite::Statement st(database_.db(),
                       "INSERT INTO application_volumes (application_id,name,mount_path,host_path) "
                       "VALUES (?,?,?,?)");
  st.bind(1, applicationId);
  st.bind(2, name);
  st.bind(3, mountPath);
  st.bind(4, hostPath);
  st.exec();
  return database_.db().getLastInsertRowid();
}

void Repositories::updateVolume(int64_t volumeId, const std::string& name,
                                const std::string& mountPath) {
  SQLite::Statement st(database_.db(),
                       "UPDATE application_volumes SET name=?, mount_path=? WHERE id=?");
  st.bind(1, name);
  st.bind(2, mountPath);
  st.bind(3, volumeId);
  st.exec();
}

void Repositories::deleteVolume(int64_t volumeId) {
  SQLite::Statement st(database_.db(), "DELETE FROM application_volumes WHERE id=?");
  st.bind(1, volumeId);
  st.exec();
}

std::optional<ApplicationVolume> Repositories::findVolumeById(int64_t volumeId) {
  SQLite::Statement st(database_.db(),
                       "SELECT id,application_id,name,mount_path,host_path FROM application_volumes "
                       "WHERE id=?");
  st.bind(1, volumeId);
  if (!st.executeStep()) {
    return std::nullopt;
  }
  return hydrateVolumeRow(st);
}

namespace {

Deployment hydrateDeploymentRow(SQLite::Statement& st) {
  Deployment d;
  d.id = st.getColumn(0).getInt64();
  d.applicationId = st.getColumn(1).getInt64();
  d.gitSha = st.getColumn(2).getString();
  d.gitMessage = st.getColumn(3).getString();
  d.status = st.getColumn(4).getString();
  d.logText = st.getColumn(5).getString();
  d.buildLog = st.getColumn(6).getString();
  d.deployLog = st.getColumn(7).getString();
  d.noCache = st.getColumn(8).getInt() != 0;
  d.createdBy = st.getColumn(9).getInt64();
  d.createdAt = st.getColumn(10).getString();
  d.deployAttempts = st.getColumn(11).getInt();
  return d;
}

constexpr const char* kDeploymentSelect =
    "SELECT id,application_id,git_sha,COALESCE(git_message,''),status,log_text,"
    "COALESCE(build_log,''),COALESCE(deploy_log,''),COALESCE(no_cache,0),"
    "COALESCE(created_by,0),created_at,COALESCE(deploy_attempts,0) FROM deployments";

Replica hydrateReplicaRow(SQLite::Statement& st) {
  Replica r;
  r.id = st.getColumn(0).getInt64();
  r.deploymentId = st.getColumn(1).getInt64();
  r.applicationId = st.getColumn(2).getInt64();
  r.serverId = st.getColumn(3).getInt64();
  r.containerId = st.getColumn(4).getString();
  if (!st.getColumn(5).isNull()) {
    r.hostPort = st.getColumn(5).getInt();
  }
  r.status = st.getColumn(6).getString();
  r.desiredState = st.getColumn(7).getString();
  r.restartCount = st.getColumn(8).getInt();
  r.statusChangedAt = st.getColumn(9).isNull() ? "" : st.getColumn(9).getString();
  if (st.getColumnCount() > 10 && !st.getColumn(10).isNull()) {
    r.replacesReplicaId = st.getColumn(10).getInt64();
  }
  if (st.getColumnCount() > 11 && !st.getColumn(11).isNull()) {
    r.lastSeenAt = st.getColumn(11).getString();
  }
  if (st.getColumnCount() > 12 && !st.getColumn(12).isNull()) {
    r.containerIp = st.getColumn(12).getString();
  }
  return r;
}

constexpr const char* kReplicaSelect =
    "SELECT id,deployment_id,application_id,server_id,container_id,host_port,"
    "status,desired_state,restart_count,COALESCE(status_changed_at,''),replaces_replica_id,"
    "COALESCE(last_seen_at,''),COALESCE(container_ip,'') "
    "FROM replicas";

}  // namespace

Page<Deployment> Repositories::listDeployments(int64_t applicationId, const ListQuery& query) {
  Page<Deployment> page;
  SQLite::Statement count(database_.db(),
                          "SELECT COUNT(*) FROM deployments WHERE application_id=?");
  count.bind(1, applicationId);
  count.executeStep();
  page.total = count.getColumn(0).getInt64();
  const int pageSize = clampPageSize(query.pageSize);
  const int offset = (std::max(1, query.page) - 1) * pageSize;
  SQLite::Statement st(database_.db(),
                       std::string(kDeploymentSelect) +
                           " WHERE application_id=? ORDER BY id DESC LIMIT ? OFFSET ?");
  st.bind(1, applicationId);
  st.bind(2, pageSize);
  st.bind(3, offset);
  while (st.executeStep()) {
    page.rows.push_back(hydrateDeploymentRow(st));
  }
  return page;
}

std::optional<Deployment> Repositories::findDeploymentById(int64_t id) {
  SQLite::Statement st(database_.db(), std::string(kDeploymentSelect) + " WHERE id=?");
  st.bind(1, id);
  if (!st.executeStep()) {
    return std::nullopt;
  }
  return hydrateDeploymentRow(st);
}

int64_t Repositories::createDeployment(const Deployment& deployment) {
  SQLite::Statement st(database_.db(),
                       "INSERT INTO deployments (application_id,git_sha,status,created_by,no_cache) "
                       "VALUES (?,?,?,?,?)");
  st.bind(1, deployment.applicationId);
  st.bind(2, deployment.gitSha);
  st.bind(3, deployment.status);
  st.bind(4, deployment.createdBy);
  st.bind(5, deployment.noCache ? 1 : 0);
  st.exec();
  return database_.db().getLastInsertRowid();
}

void Repositories::updateDeployment(const Deployment& deployment) {
  SQLite::Statement st(database_.db(),
                       "UPDATE deployments SET git_sha=?, git_message=?, status=?, log_text=?, "
                       "build_log=?, deploy_log=?, no_cache=?, deploy_attempts=? WHERE id=?");
  st.bind(1, deployment.gitSha);
  st.bind(2, deployment.gitMessage);
  st.bind(3, deployment.status);
  st.bind(4, deployment.logText);
  st.bind(5, deployment.buildLog);
  st.bind(6, deployment.deployLog);
  st.bind(7, deployment.noCache ? 1 : 0);
  st.bind(8, deployment.deployAttempts);
  st.bind(9, deployment.id);
  st.exec();
}

void Repositories::appendDeploymentLog(int64_t id, const std::string& chunk) {
  SQLite::Statement st(database_.db(),
                       "UPDATE deployments SET log_text = log_text || ? WHERE id=?");
  st.bind(1, chunk);
  st.bind(2, id);
  st.exec();
}

void Repositories::appendDeploymentPhaseLog(int64_t id, const std::string& phase,
                                            const std::string& chunk) {
  const char* sql = phase == "build"
                        ? "UPDATE deployments SET build_log = build_log || ? WHERE id=?"
                        : "UPDATE deployments SET deploy_log = deploy_log || ? WHERE id=?";
  SQLite::Statement st(database_.db(), sql);
  st.bind(1, chunk);
  st.bind(2, id);
  st.exec();
}

Page<Replica> Repositories::listReplicas(int64_t applicationId, const ListQuery& query) {
  Page<Replica> page;
  SQLite::Statement count(database_.db(),
                          "SELECT COUNT(*) FROM replicas WHERE application_id=?");
  count.bind(1, applicationId);
  count.executeStep();
  page.total = count.getColumn(0).getInt64();
  const int pageSize = clampPageSize(query.pageSize);
  const int offset = (std::max(1, query.page) - 1) * pageSize;
  SQLite::Statement st(database_.db(),
                       std::string(kReplicaSelect) +
                           " WHERE application_id=? ORDER BY id DESC LIMIT ? OFFSET ?");
  st.bind(1, applicationId);
  st.bind(2, pageSize);
  st.bind(3, offset);
  while (st.executeStep()) {
    page.rows.push_back(hydrateReplicaRow(st));
  }
  return page;
}

std::optional<Replica> Repositories::findReplicaById(int64_t id) {
  SQLite::Statement st(database_.db(), std::string(kReplicaSelect) + " WHERE id=?");
  st.bind(1, id);
  if (!st.executeStep()) {
    return std::nullopt;
  }
  return hydrateReplicaRow(st);
}

int64_t Repositories::createReplica(const Replica& replica) {
  SQLite::Statement st(database_.db(),
                       "INSERT INTO replicas (deployment_id,application_id,server_id,container_id,"
                       "host_port,status,desired_state,restart_count,status_changed_at,"
                       "replaces_replica_id,container_ip) "
                       "VALUES (?,?,?,?,?,?,?,?,strftime('%Y-%m-%dT%H:%M:%fZ', 'now'),?,?)");
  st.bind(1, replica.deploymentId);
  st.bind(2, replica.applicationId);
  st.bind(3, replica.serverId);
  st.bind(4, replica.containerId);
  if (replica.hostPort) {
    st.bind(5, *replica.hostPort);
  } else {
    st.bind(5);
  }
  st.bind(6, replica.status);
  st.bind(7, replica.desiredState);
  st.bind(8, replica.restartCount);
  if (replica.replacesReplicaId) {
    st.bind(9, *replica.replacesReplicaId);
  } else {
    st.bind(9);
  }
  st.bind(10, replica.containerIp);
  st.exec();
  return database_.db().getLastInsertRowid();
}

void Repositories::updateReplica(const Replica& replica) {
  SQLite::Statement st(database_.db(),
                       "UPDATE replicas SET container_id=?, host_port=?, status=?, desired_state=?, "
                       "restart_count=?, status_changed_at=COALESCE(NULLIF(?,''), status_changed_at), "
                       "replaces_replica_id=?, "
                       "container_ip=COALESCE(NULLIF(?,''), container_ip) WHERE id=?");
  st.bind(1, replica.containerId);
  if (replica.hostPort) {
    st.bind(2, *replica.hostPort);
  } else {
    st.bind(2);
  }
  st.bind(3, replica.status);
  st.bind(4, replica.desiredState);
  st.bind(5, replica.restartCount);
  st.bind(6, replica.statusChangedAt);
  if (replica.replacesReplicaId) {
    st.bind(7, *replica.replacesReplicaId);
  } else {
    st.bind(7);
  }
  st.bind(8, replica.containerIp);
  st.bind(9, replica.id);
  st.exec();
}

void Repositories::deleteReplica(int64_t id) {
  SQLite::Statement st(database_.db(), "DELETE FROM replicas WHERE id=?");
  st.bind(1, id);
  st.exec();
}

void Repositories::touchReplicaSeen(int64_t id, int restartCount, const std::string& seenAt) {
  SQLite::Statement st(database_.db(),
                       "UPDATE replicas SET last_seen_at=?, restart_count=? WHERE id=?");
  st.bind(1, seenAt);
  st.bind(2, restartCount);
  st.bind(3, id);
  st.exec();
}

int64_t Repositories::recordReplicaIncident(const ReplicaIncident& incident) {
  SQLite::Statement st(
      database_.db(),
      "INSERT INTO replica_incidents (application_id,replica_id,server_id,kind,detail,restart_count) "
      "VALUES (?,?,?,?,?,?)");
  st.bind(1, incident.applicationId);
  if (incident.replicaId) {
    st.bind(2, *incident.replicaId);
  } else {
    st.bind(2);
  }
  if (incident.serverId) {
    st.bind(3, *incident.serverId);
  } else {
    st.bind(3);
  }
  st.bind(4, incident.kind);
  st.bind(5, incident.detail);
  st.bind(6, incident.restartCount);
  st.exec();
  return database_.db().getLastInsertRowid();
}

Page<ReplicaIncident> Repositories::listReplicaIncidents(int64_t applicationId,
                                                         const ListQuery& query) {
  Page<ReplicaIncident> page;
  SQLite::Statement count(database_.db(),
                          "SELECT COUNT(*) FROM replica_incidents WHERE application_id=?");
  count.bind(1, applicationId);
  count.executeStep();
  page.total = count.getColumn(0).getInt64();
  const int pageSize = clampPageSize(query.pageSize);
  const int offset = (std::max(1, query.page) - 1) * pageSize;
  SQLite::Statement st(
      database_.db(),
      "SELECT id,application_id,replica_id,server_id,kind,detail,restart_count,created_at "
      "FROM replica_incidents WHERE application_id=? ORDER BY id DESC LIMIT ? OFFSET ?");
  st.bind(1, applicationId);
  st.bind(2, pageSize);
  st.bind(3, offset);
  while (st.executeStep()) {
    ReplicaIncident incident;
    incident.id = st.getColumn(0).getInt64();
    incident.applicationId = st.getColumn(1).getInt64();
    if (!st.getColumn(2).isNull()) {
      incident.replicaId = st.getColumn(2).getInt64();
    }
    if (!st.getColumn(3).isNull()) {
      incident.serverId = st.getColumn(3).getInt64();
    }
    incident.kind = st.getColumn(4).getString();
    incident.detail = st.getColumn(5).getString();
    incident.restartCount = st.getColumn(6).getInt();
    incident.createdAt = st.getColumn(7).getString();
    page.rows.push_back(incident);
  }
  return page;
}

int64_t Repositories::deleteIncidentsOlderThan(const std::string& cutoffIso) {
  SQLite::Statement st(database_.db(),
                       "DELETE FROM replica_incidents WHERE created_at < ?");
  st.bind(1, cutoffIso);
  return st.exec();
}

int Repositories::countReplicasForApplication(int64_t applicationId) {
  SQLite::Statement st(database_.db(),
                       "SELECT COUNT(*) FROM replicas WHERE application_id=? AND desired_state='running'");
  st.bind(1, applicationId);
  st.executeStep();
  return st.getColumn(0).getInt();
}

int Repositories::countReplicasOnServer(int64_t serverId, int64_t applicationId) {
  SQLite::Statement st(database_.db(),
                       "SELECT COUNT(*) FROM replicas WHERE server_id=? AND application_id=?");
  st.bind(1, serverId);
  st.bind(2, applicationId);
  st.executeStep();
  return st.getColumn(0).getInt();
}

std::vector<Replica> Repositories::listReplicasForServer(int64_t serverId) {
  std::vector<Replica> rows;
  SQLite::Statement st(database_.db(), std::string(kReplicaSelect) + " WHERE server_id=?");
  st.bind(1, serverId);
  while (st.executeStep()) {
    rows.push_back(hydrateReplicaRow(st));
  }
  return rows;
}

std::vector<MetricRow> Repositories::listApplicationMetrics(int64_t applicationId,
                                                            const std::string& from,
                                                            const std::string& to) {
  std::vector<MetricRow> rows;
  SQLite::Statement st(database_.db(),
                       "SELECT m.id,m.server_id,m.replica_id,m.payload_json,m.collected_at "
                       "FROM metrics m "
                       "INNER JOIN replicas r ON r.id = m.replica_id "
                       "WHERE r.application_id=? AND m.collected_at>=? AND m.collected_at<=? "
                       "ORDER BY m.collected_at ASC");
  st.bind(1, applicationId);
  st.bind(2, from);
  st.bind(3, to);
  while (st.executeStep()) {
    MetricRow row;
    row.id = st.getColumn(0).getInt64();
    row.serverId = st.getColumn(1).getInt64();
    if (!st.getColumn(2).isNull()) {
      row.replicaId = st.getColumn(2).getInt64();
    }
    row.payloadJson = st.getColumn(3).getString();
    row.collectedAt = st.getColumn(4).getString();
    rows.push_back(row);
  }
  return rows;
}

void Repositories::insertMetric(int64_t serverId, std::optional<int64_t> replicaId,
                                const std::string& payloadJson, const std::string& collectedAt) {
  SQLite::Statement st(database_.db(),
                       "INSERT INTO metrics (server_id,replica_id,payload_json,collected_at) VALUES (?,?,?,?)");
  st.bind(1, serverId);
  if (replicaId) {
    st.bind(2, *replicaId);
  } else {
    st.bind(2);
  }
  st.bind(3, payloadJson);
  st.bind(4, collectedAt);
  st.exec();
}

std::vector<MetricRow> Repositories::listServerMetrics(int64_t serverId, const std::string& from,
                                                       const std::string& to) {
  std::vector<MetricRow> rows;
  SQLite::Statement st(database_.db(),
                       "SELECT id,server_id,replica_id,payload_json,collected_at FROM metrics "
                       "WHERE server_id=? AND replica_id IS NULL AND collected_at>=? AND collected_at<=? "
                       "ORDER BY collected_at ASC");
  st.bind(1, serverId);
  st.bind(2, from);
  st.bind(3, to);
  while (st.executeStep()) {
    MetricRow row;
    row.id = st.getColumn(0).getInt64();
    row.serverId = st.getColumn(1).getInt64();
    if (!st.getColumn(2).isNull()) {
      row.replicaId = st.getColumn(2).getInt64();
    }
    row.payloadJson = st.getColumn(3).getString();
    row.collectedAt = st.getColumn(4).getString();
    rows.push_back(row);
  }
  return rows;
}

std::vector<MetricRow> Repositories::listRecentServerMetrics(int64_t serverId, int limit) {
  std::vector<MetricRow> rows;
  if (limit <= 0) {
    return rows;
  }
  SQLite::Statement st(database_.db(),
                       "SELECT id,server_id,replica_id,payload_json,collected_at FROM metrics "
                       "WHERE server_id=? AND replica_id IS NULL "
                       "ORDER BY collected_at DESC LIMIT ?");
  st.bind(1, serverId);
  st.bind(2, limit);
  while (st.executeStep()) {
    MetricRow row;
    row.id = st.getColumn(0).getInt64();
    row.serverId = st.getColumn(1).getInt64();
    if (!st.getColumn(2).isNull()) {
      row.replicaId = st.getColumn(2).getInt64();
    }
    row.payloadJson = st.getColumn(3).getString();
    row.collectedAt = st.getColumn(4).getString();
    rows.push_back(row);
  }
  return rows;
}

std::vector<MetricRow> Repositories::listReplicaMetrics(int64_t replicaId, const std::string& from,
                                                        const std::string& to) {
  std::vector<MetricRow> rows;
  SQLite::Statement st(database_.db(),
                       "SELECT id,server_id,replica_id,payload_json,collected_at FROM metrics "
                       "WHERE replica_id=? AND collected_at>=? AND collected_at<=? ORDER BY collected_at ASC");
  st.bind(1, replicaId);
  st.bind(2, from);
  st.bind(3, to);
  while (st.executeStep()) {
    MetricRow row;
    row.id = st.getColumn(0).getInt64();
    row.serverId = st.getColumn(1).getInt64();
    row.replicaId = st.getColumn(2).getInt64();
    row.payloadJson = st.getColumn(3).getString();
    row.collectedAt = st.getColumn(4).getString();
    rows.push_back(row);
  }
  return rows;
}

int64_t Repositories::deleteMetricsOlderThan(const std::string& cutoffIso) {
  SQLite::Statement st(database_.db(), "DELETE FROM metrics WHERE collected_at < ?");
  st.bind(1, cutoffIso);
  return st.exec();
}

Page<GithubApp> Repositories::listGithubApps(const ListQuery& query) {
  Page<GithubApp> page;
  const int pageSize = clampPageSize(query.pageSize);
  const int offset = (std::max(1, query.page) - 1) * pageSize;
  if (query.ownerId) {
    SQLite::Statement count(database_.db(), "SELECT COUNT(*) FROM github_apps WHERE user_id=?");
    count.bind(1, *query.ownerId);
    count.executeStep();
    page.total = count.getColumn(0).getInt64();
  } else {
    page.total = database_.db().execAndGet("SELECT COUNT(*) FROM github_apps").getInt64();
  }
  std::string sql =
      "SELECT g.id,g.user_id,g.name,g.app_id,g.slug,g.client_id,g.client_secret,g.webhook_secret,"
      "g.private_key_pem,g.html_url,g.owner_login,g.installation_id,g.status,g.created_at,"
      "COALESCE(u.email,'') "
      "FROM github_apps g LEFT JOIN users u ON u.id=g.user_id";
  if (query.ownerId) {
    sql += " WHERE g.user_id=?";
  }
  sql += " ORDER BY g.id DESC LIMIT ? OFFSET ?";
  SQLite::Statement st(database_.db(), sql);
  int bind = 1;
  if (query.ownerId) {
    st.bind(bind++, *query.ownerId);
  }
  st.bind(bind++, pageSize);
  st.bind(bind, offset);
  while (st.executeStep()) {
    page.rows.push_back(hydrateGithubApp(st));
  }
  return page;
}

std::optional<GithubApp> Repositories::findGithubAppById(int64_t id) {
  SQLite::Statement st(
      database_.db(),
      "SELECT g.id,g.user_id,g.name,g.app_id,g.slug,g.client_id,g.client_secret,g.webhook_secret,"
      "g.private_key_pem,g.html_url,g.owner_login,g.installation_id,g.status,g.created_at,"
      "COALESCE(u.email,'') "
      "FROM github_apps g LEFT JOIN users u ON u.id=g.user_id WHERE g.id=?");
  st.bind(1, id);
  if (!st.executeStep()) {
    return std::nullopt;
  }
  return hydrateGithubApp(st);
}

int64_t Repositories::createGithubApp(const GithubApp& app) {
  SQLite::Statement st(database_.db(),
                       "INSERT INTO github_apps (user_id,name,status) VALUES (?,?,?)");
  st.bind(1, app.userId);
  st.bind(2, app.name);
  st.bind(3, app.status.empty() ? "pending" : app.status);
  st.exec();
  return database_.db().getLastInsertRowid();
}

void Repositories::updateGithubApp(const GithubApp& app) {
  SQLite::Statement st(
      database_.db(),
      "UPDATE github_apps SET name=?, app_id=?, slug=?, client_id=?, client_secret=?, "
      "webhook_secret=?, private_key_pem=?, html_url=?, owner_login=?, installation_id=?, "
      "status=? WHERE id=?");
  st.bind(1, app.name);
  if (app.appId) {
    st.bind(2, *app.appId);
  } else {
    st.bind(2);
  }
  st.bind(3, app.slug);
  st.bind(4, app.clientId);
  st.bind(5, app.clientSecret);
  st.bind(6, app.webhookSecret);
  st.bind(7, app.privateKeyPem);
  st.bind(8, app.htmlUrl);
  st.bind(9, app.ownerLogin);
  if (app.installationId) {
    st.bind(10, *app.installationId);
  } else {
    st.bind(10);
  }
  st.bind(11, app.status);
  st.bind(12, app.id);
  st.exec();
}

void Repositories::deleteGithubApp(int64_t id) {
  SQLite::Statement st(database_.db(), "DELETE FROM github_apps WHERE id=?");
  st.bind(1, id);
  st.exec();
}

int Repositories::countApplicationsUsingGithubApp(int64_t githubAppId) {
  SQLite::Statement st(database_.db(), "SELECT COUNT(*) FROM applications WHERE github_app_id=?");
  st.bind(1, githubAppId);
  st.executeStep();
  return st.getColumn(0).getInt();
}

int64_t Repositories::recordUserActivity(const UserActivity& activity) {
  SQLite::Statement st(
      database_.db(),
      "INSERT INTO user_activity (user_id,action,method,path,target_type,target_id,detail,status,ip) "
      "VALUES (?,?,?,?,?,?,?,?,?)");
  st.bind(1, activity.userId);
  st.bind(2, activity.action);
  st.bind(3, activity.method);
  st.bind(4, activity.path);
  st.bind(5, activity.targetType);
  st.bind(6, activity.targetId);
  st.bind(7, activity.detail);
  st.bind(8, activity.status);
  st.bind(9, activity.ip);
  st.exec();
  return database_.db().getLastInsertRowid();
}

Page<UserActivity> Repositories::listUserActivity(int64_t userId, const ListQuery& query) {
  Page<UserActivity> page;
  SQLite::Statement count(database_.db(),
                          "SELECT COUNT(*) FROM user_activity WHERE user_id=?");
  count.bind(1, userId);
  count.executeStep();
  page.total = count.getColumn(0).getInt64();
  const int pageSize = clampPageSize(query.pageSize);
  const int offset = (std::max(1, query.page) - 1) * pageSize;
  SQLite::Statement st(
      database_.db(),
      "SELECT id,user_id,action,method,path,target_type,target_id,detail,status,ip,created_at "
      "FROM user_activity WHERE user_id=? ORDER BY id DESC LIMIT ? OFFSET ?");
  st.bind(1, userId);
  st.bind(2, pageSize);
  st.bind(3, offset);
  while (st.executeStep()) {
    UserActivity activity;
    activity.id = st.getColumn(0).getInt64();
    activity.userId = st.getColumn(1).getInt64();
    activity.action = st.getColumn(2).getString();
    activity.method = st.getColumn(3).getString();
    activity.path = st.getColumn(4).getString();
    activity.targetType = st.getColumn(5).getString();
    activity.targetId = st.getColumn(6).getString();
    activity.detail = st.getColumn(7).getString();
    activity.status = st.getColumn(8).getInt();
    activity.ip = st.getColumn(9).getString();
    activity.createdAt = st.getColumn(10).getString();
    page.rows.push_back(activity);
  }
  return page;
}

std::vector<std::string> Repositories::listUserActivityTimestamps(int64_t userId,
                                                                  const std::string& from,
                                                                  const std::string& to) {
  std::vector<std::string> rows;
  SQLite::Statement st(database_.db(),
                       "SELECT created_at FROM user_activity "
                       "WHERE user_id=? AND created_at>=? AND created_at<=? "
                       "ORDER BY created_at ASC");
  st.bind(1, userId);
  st.bind(2, from);
  st.bind(3, to);
  while (st.executeStep()) {
    rows.push_back(st.getColumn(0).getString());
  }
  return rows;
}

int64_t Repositories::deleteActivityOlderThan(const std::string& cutoffIso) {
  SQLite::Statement st(database_.db(), "DELETE FROM user_activity WHERE created_at < ?");
  st.bind(1, cutoffIso);
  return st.exec();
}

void Repositories::recordPresence(int64_t userId, const std::string& atIso) {
  SQLite::Statement st(database_.db(), "INSERT INTO user_presence (user_id,at) VALUES (?,?)");
  st.bind(1, userId);
  st.bind(2, atIso);
  st.exec();
}

std::vector<std::string> Repositories::listPresenceTimestamps(int64_t userId,
                                                              const std::string& from,
                                                              const std::string& to) {
  std::vector<std::string> rows;
  SQLite::Statement st(database_.db(),
                       "SELECT at FROM user_presence "
                       "WHERE user_id=? AND at>=? AND at<=? ORDER BY at ASC");
  st.bind(1, userId);
  st.bind(2, from);
  st.bind(3, to);
  while (st.executeStep()) {
    rows.push_back(st.getColumn(0).getString());
  }
  return rows;
}

int64_t Repositories::deletePresenceOlderThan(const std::string& cutoffIso) {
  SQLite::Statement st(database_.db(), "DELETE FROM user_presence WHERE at < ?");
  st.bind(1, cutoffIso);
  return st.exec();
}

}  // namespace highcube::admin
