#include "services/services.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace highcube::admin {
namespace {

const std::vector<std::string> kMysqlPrivileges = {
    "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "ALTER", "INDEX", "REFERENCES"};
const std::vector<std::string> kPostgresPrivileges = {
    "CONNECT", "CREATE", "TEMPORARY", "SELECT", "INSERT", "UPDATE", "DELETE", "TRUNCATE"};
const std::unordered_set<std::string> kMysqlReservedDatabases = {
    "information_schema", "performance_schema", "mysql", "sys"};

struct FlavorDef {
  const char* id;
  const char* engine;
  const char* client;
  bool vectorExtension;
};

const FlavorDef kFlavors[] = {
    {"mysql-8", "mysql", "mysql", false},
    {"mariadb-11", "mysql", "mariadb", false},
    {"postgres-16", "postgres", "psql", false},
    {"postgres-17", "postgres", "psql", false},
    {"postgres-18", "postgres", "psql", false},
    {"postgres-17-supabase", "postgres", "psql", false},
    {"postgis-17", "postgres", "psql", false},
    {"pgvector-17", "postgres", "psql", true},
    {"pgvector-18", "postgres", "psql", true},
};

const FlavorDef* findFlavor(const std::string& id) {
  for (const auto& flavor : kFlavors) {
    if (id == flavor.id) {
      return &flavor;
    }
  }
  return nullptr;
}

bool isIdent(const std::string& value) {
  if (value.empty() || value.size() > 63) {
    return false;
  }
  const unsigned char first = static_cast<unsigned char>(value.front());
  if (!std::isalpha(first) && value.front() != '_') {
    return false;
  }
  for (const char ch : value) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (!std::isalnum(c) && ch != '_') {
      return false;
    }
  }
  return true;
}

void requireIdent(const std::string& value, const char* field) {
  if (!isIdent(value)) {
    throw std::invalid_argument(std::string(field) +
                                " must start with a letter and use only letters, digits, and underscores");
  }
}

void requirePassword(const std::string& password, bool required) {
  if (password.empty()) {
    if (required) {
      throw std::invalid_argument("password is required");
    }
    return;
  }
  if (password.size() > 128) {
    throw std::invalid_argument("password is too long");
  }
  for (const unsigned char c : password) {
    if (c < 0x20) {
      throw std::invalid_argument("password cannot contain control characters");
    }
  }
}

std::string envString(const nlohmann::json& env, const char* key) {
  if (!env.contains(key) || env.at(key).is_null()) {
    return {};
  }
  if (env.at(key).is_string()) {
    return env.at(key).get<std::string>();
  }
  return {};
}

struct Credentials {
  std::string engine;
  std::string client;
  std::string adminUser;
  std::string adminPassword;
  std::string passwordEnvKey;
  std::string maintenanceDb;
  bool vectorExtension = false;
};

Credentials credentialsFor(const Application& app) {
  const auto* flavor = findFlavor(app.databaseFlavor);
  if (!flavor) {
    throw std::invalid_argument("This database cannot be managed");
  }
  Credentials creds;
  creds.engine = flavor->engine;
  creds.client = flavor->client;
  creds.vectorExtension = flavor->vectorExtension;
  const auto env = nlohmann::json::parse(app.envJson.empty() ? "{}" : app.envJson, nullptr, false);
  if (!env.is_object()) {
    throw std::invalid_argument("Database credentials are missing");
  }
  if (creds.engine == "postgres") {
    creds.client = "psql";
    creds.adminUser = envString(env, "POSTGRES_USER");
    if (creds.adminUser.empty()) {
      creds.adminUser = envString(env, "POSTGRESQL_USERNAME");
    }
    if (creds.adminUser.empty()) {
      creds.adminUser = envString(env, "PGUSER");
    }
    if (creds.adminUser.empty()) {
      creds.adminUser = "postgres";
    }
    if (!envString(env, "POSTGRES_PASSWORD").empty()) {
      creds.passwordEnvKey = "POSTGRES_PASSWORD";
      creds.adminPassword = envString(env, "POSTGRES_PASSWORD");
    } else {
      creds.passwordEnvKey = "POSTGRESQL_PASSWORD";
      creds.adminPassword = envString(env, "POSTGRESQL_PASSWORD");
    }
    creds.maintenanceDb = envString(env, "POSTGRES_DB");
    if (creds.maintenanceDb.empty()) {
      creds.maintenanceDb = envString(env, "POSTGRESQL_DATABASE");
    }
    if (creds.maintenanceDb.empty()) {
      creds.maintenanceDb = "postgres";
    }
  } else if (creds.client == "mariadb" && !envString(env, "MARIADB_ROOT_PASSWORD").empty()) {
    creds.adminUser = "root";
    creds.passwordEnvKey = "MARIADB_ROOT_PASSWORD";
    creds.adminPassword = envString(env, "MARIADB_ROOT_PASSWORD");
    creds.maintenanceDb = "mysql";
    creds.client = "mariadb";
  } else {
    creds.adminUser = "root";
    creds.passwordEnvKey = "MYSQL_ROOT_PASSWORD";
    creds.adminPassword = envString(env, "MYSQL_ROOT_PASSWORD");
    creds.maintenanceDb = "mysql";
  }
  if (!isIdent(creds.adminUser) || creds.adminPassword.empty() || !isIdent(creds.maintenanceDb)) {
    throw std::invalid_argument("Database management credentials are missing");
  }
  return creds;
}

const std::vector<std::string>& privilegesFor(const std::string& engine) {
  return engine == "postgres" ? kPostgresPrivileges : kMysqlPrivileges;
}

bool knownPrivilege(const std::string& engine, const std::string& privilege) {
  const auto& list = privilegesFor(engine);
  return std::find(list.begin(), list.end(), privilege) != list.end();
}

std::string shellSingleQuote(const std::string& value) {
  std::string out = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      out += "'\\''";
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('\'');
  return out;
}

// End offset of a shell word that may be a single-quoted string using '\'' escapes.
std::size_t endOfShellWord(const std::string& text, std::size_t begin) {
  std::size_t i = begin;
  if (i < text.size() && text[i] == '\'') {
    while (i < text.size() && text[i] == '\'') {
      ++i;
      while (i < text.size() && text[i] != '\'') {
        ++i;
      }
      if (i >= text.size()) {
        return std::string::npos;
      }
      ++i;
      if (i + 1 < text.size() && text[i] == '\\' && text[i + 1] == '\'') {
        i += 2;
        continue;
      }
      return i;
    }
  }
  while (i < text.size() && text[i] != ' ') {
    ++i;
  }
  return i;
}

// Rewrites the mysqladmin password embedded in the container health command.
std::string rewriteHealthPassword(const std::string& command, const std::string& password) {
  if (command.find("mysqladmin") == std::string::npos) {
    return {};
  }
  const std::string replacement = "--password=" + shellSingleQuote(password);
  auto replaceAt = [&](std::size_t flag, std::size_t value) -> std::string {
    const auto end = endOfShellWord(command, value);
    if (end == std::string::npos) {
      return {};
    }
    std::string updated = command;
    updated.replace(flag, end - flag, replacement);
    return updated;
  };
  const auto longFlag = command.find("--password=");
  if (longFlag != std::string::npos) {
    return replaceAt(longFlag, longFlag + std::string("--password=").size());
  }
  std::size_t search = 0;
  while (search < command.size()) {
    const auto pos = command.find("-p", search);
    if (pos == std::string::npos) {
      break;
    }
    const bool boundary = pos == 0 || command[pos - 1] == ' ';
    if (boundary && (pos + 2 >= command.size() || command[pos + 2] != '-')) {
      return replaceAt(pos, pos + 2);
    }
    search = pos + 2;
  }
  return {};
}

Replica pickReplica(AppContext& ctx, int64_t applicationId) {
  ListQuery query;
  query.page = 1;
  query.pageSize = 100;
  const auto page = ctx.repos->listReplicas(applicationId, query);
  const Replica* chosen = nullptr;
  for (const auto& replica : page.rows) {
    if (replica.containerId.empty()) {
      continue;
    }
    if (replica.status != "healthy" && replica.status != "running") {
      continue;
    }
    if (!ctx.agentHub->isConnected(replica.serverId)) {
      continue;
    }
    chosen = &replica;
    if (replica.status == "healthy") {
      break;
    }
  }
  if (!chosen) {
    throw std::runtime_error("The database is not running");
  }
  return *chosen;
}

}  // namespace

bool isKnownDatabaseFlavor(const std::string& flavor) { return findFlavor(flavor) != nullptr; }

std::string databaseFlavorEngine(const std::string& flavor) {
  const auto* found = findFlavor(flavor);
  return found ? found->engine : std::string{};
}

std::string managedDatabaseEngine(const Application& app) {
  return databaseFlavorEngine(app.databaseFlavor);
}

DatabaseAdminService::DatabaseAdminService(AppContext& ctx) : ctx_(ctx) {}

nlohmann::json DatabaseAdminService::start(int64_t applicationId, const nlohmann::json& body,
                                           const User& actor) {
  auto app = ctx_.repos->findApplicationById(applicationId);
  if (!app) {
    throw std::runtime_error("Application not found");
  }
  if (!ctx_.auth->canAccessApplication(actor, *app)) {
    throw ForbiddenError();
  }
  const auto creds = credentialsFor(*app);
  const auto op = body.value("op", std::string{});
  nlohmann::json payload = {{"op", op},
                            {"engine", creds.engine},
                            {"client", creds.client},
                            {"adminUser", creds.adminUser},
                            {"adminPassword", creds.adminPassword},
                            {"maintenanceDb", creds.maintenanceDb},
                            {"vectorExtension", creds.vectorExtension}};
  AgentHub::DbManageEffect effect;
  effect.applicationId = app->id;

  if (op == "list_users" || op == "list_databases" || op == "catalog") {
    // No extra fields. catalog returns users and databases in one agent round-trip.
  } else if (op == "create_user") {
    const auto name = body.value("name", std::string{});
    const auto password = body.value("password", std::string{});
    requireIdent(name, "name");
    requirePassword(password, true);
    payload["name"] = name;
    payload["password"] = password;
  } else if (op == "update_user") {
    const auto name = body.value("name", std::string{});
    const auto newName = body.value("newName", std::string{});
    const auto password = body.value("password", std::string{});
    requireIdent(name, "name");
    const bool rename = !newName.empty() && newName != name;
    if (rename) {
      requireIdent(newName, "newName");
      if (name == creds.adminUser) {
        throw std::invalid_argument("The management user cannot be renamed");
      }
    }
    requirePassword(password, false);
    if (!rename && password.empty()) {
      throw std::invalid_argument("Nothing to change");
    }
    payload["name"] = name;
    if (rename) {
      payload["newName"] = newName;
    }
    if (!password.empty()) {
      payload["password"] = password;
      if (name == creds.adminUser) {
        effect.passwordEnvKey = creds.passwordEnvKey;
        effect.newPassword = password;
        effect.healthCommand = rewriteHealthPassword(app->healthCheckCommand, password);
        if (!effect.healthCommand.empty()) {
          payload["healthCommand"] = effect.healthCommand;
        }
      }
    }
  } else if (op == "delete_user") {
    const auto name = body.value("name", std::string{});
    requireIdent(name, "name");
    if (name == creds.adminUser) {
      throw std::invalid_argument("The management user cannot be deleted");
    }
    payload["name"] = name;
  } else if (op == "create_database") {
    const auto name = body.value("name", std::string{});
    requireIdent(name, "name");
    if (kMysqlReservedDatabases.count(name) || name == "template0" || name == "template1") {
      throw std::invalid_argument("That database name is reserved");
    }
    payload["name"] = name;
  } else if (op == "get_grants") {
    const auto database = body.value("database", std::string{});
    requireIdent(database, "database");
    payload["database"] = database;
  } else if (op == "set_grants") {
    const auto database = body.value("database", std::string{});
    requireIdent(database, "database");
    if (!body.contains("grants") || !body.at("grants").is_object()) {
      throw std::invalid_argument("grants must be an object");
    }
    nlohmann::json grants = nlohmann::json::object();
    nlohmann::json hosts = nlohmann::json::object();
    const auto hostMap = body.value("hosts", nlohmann::json::object());
    for (auto it = body.at("grants").begin(); it != body.at("grants").end(); ++it) {
      requireIdent(it.key(), "user");
      if (it.key() == creds.adminUser) {
        throw std::invalid_argument("The management user privileges cannot be changed");
      }
      if (!it.value().is_array()) {
        throw std::invalid_argument("Each grant entry must be a list of privileges");
      }
      nlohmann::json privileges = nlohmann::json::array();
      for (const auto& privilege : it.value()) {
        if (!privilege.is_string()) {
          throw std::invalid_argument("Invalid privilege");
        }
        const auto name = privilege.get<std::string>();
        if (!knownPrivilege(creds.engine, name)) {
          throw std::invalid_argument("Unknown privilege");
        }
        privileges.push_back(name);
      }
      grants[it.key()] = privileges;
      if (hostMap.contains(it.key()) && hostMap.at(it.key()).is_string()) {
        const auto host = hostMap.at(it.key()).get<std::string>();
        if (host.empty() || host.size() > 64) {
          throw std::invalid_argument("Invalid host");
        }
        hosts[it.key()] = host;
      }
    }
    payload["database"] = database;
    payload["grants"] = grants;
    payload["hosts"] = hosts;
  } else {
    throw std::invalid_argument("Unknown database operation");
  }

  const auto replica = pickReplica(ctx_, app->id);
  const auto requestId = ctx_.agentHub->startDbManage(replica.serverId, app->id, replica.containerId,
                                                      payload.dump(), effect);
  return {{"requestId", requestId}};
}

nlohmann::json DatabaseAdminService::poll(int64_t applicationId, const std::string& requestId,
                                          const User& actor) {
  auto app = ctx_.repos->findApplicationById(applicationId);
  if (!app) {
    throw std::runtime_error("Application not found");
  }
  if (!ctx_.auth->canAccessApplication(actor, *app)) {
    throw ForbiddenError();
  }
  const auto poll = ctx_.agentHub->pollDbManage(requestId, applicationId);
  nlohmann::json body = {{"status", poll.status}};
  if (!poll.error.empty()) {
    body["error"] = poll.error;
  } else if (poll.status == "unknown") {
    body["error"] = "The management request was not found";
  }
  if (poll.status == "ok") {
    auto result = nlohmann::json::parse(poll.resultJson.empty() ? "{}" : poll.resultJson, nullptr, false);
    body["result"] = result.is_discarded() ? nlohmann::json::object() : result;
  }
  return body;
}

}  // namespace highcube::admin
