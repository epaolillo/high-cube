#include "runtime/db_manage.hpp"

#include <boost/process.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bp = boost::process;

namespace highcube::agent {
namespace {

const std::vector<std::string> kMysqlPrivileges = {
    "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "ALTER", "INDEX", "REFERENCES"};

const std::vector<std::string> kPostgresPrivileges = {
    "CONNECT", "CREATE", "TEMPORARY", "SELECT", "INSERT", "UPDATE", "DELETE", "TRUNCATE"};

const std::unordered_set<std::string> kMysqlHiddenUsers = {
    "mysql.sys", "mysql.session", "mysql.infoschema", "mariadb.sys", "debian-sys-maint"};

const std::unordered_set<std::string> kMysqlHiddenDatabases = {
    "information_schema", "performance_schema", "mysql", "sys"};

struct ExecResult {
  int code = 1;
  std::string out;
  std::string err;
};

struct Session {
  std::string containerId;
  std::string engine;
  std::string client;
  std::string adminUser;
  std::string adminPassword;
  std::string maintenanceDb;
  bool vectorExtension = false;
  std::string healthCommand;
};

std::string trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
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

bool isHost(const std::string& value) {
  if (value.empty() || value.size() > 64) {
    return false;
  }
  for (const char ch : value) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (!std::isalnum(c) && ch != '_' && ch != '%' && ch != '.') {
      return false;
    }
  }
  return true;
}

bool isSafeContainerId(const std::string& value) {
  if (value.empty() || value.size() > 128) {
    return false;
  }
  for (const char ch : value) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (!std::isalnum(c) && ch != '_' && ch != '.' && ch != '-') {
      return false;
    }
  }
  return true;
}

std::string pgIdent(const std::string& name) { return "\"" + name + "\""; }

std::string myIdent(const std::string& name) { return "`" + name + "`"; }

std::string myAccount(const std::string& name, const std::string& host) {
  return "'" + name + "'@'" + host + "'";
}

std::string sqlString(const std::string& value) {
  std::string out = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      out += "''";
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('\'');
  return out;
}

std::string hexLiteral(const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out = "0x";
  out.reserve(2 + value.size() * 2);
  for (const unsigned char c : value) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0xF]);
  }
  return out;
}

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::stringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  return lines;
}

std::vector<std::string> splitOn(const std::string& text, char separator) {
  std::vector<std::string> parts;
  std::stringstream stream(text);
  std::string part;
  while (std::getline(stream, part, separator)) {
    parts.push_back(part);
  }
  return parts;
}

std::string redact(std::string text, const std::string& secret) {
  if (!secret.empty()) {
    for (auto pos = text.find(secret); pos != std::string::npos; pos = text.find(secret, pos)) {
      text.replace(pos, secret.size(), "***");
      pos += 3;
    }
  }
  std::stringstream kept;
  for (const auto& line : splitLines(text)) {
    if (line.rfind("STATEMENT:", 0) == 0) {
      continue;
    }
    kept << line << '\n';
  }
  return trim(kept.str());
}

ExecResult dockerExec(const std::vector<std::string>& args, const std::string& stdinData) {
  ExecResult result;
  try {
    bp::opstream in;
    bp::ipstream out;
    bp::ipstream err;
    bp::child child(bp::search_path("docker"), bp::args(args), bp::std_in < in, bp::std_out > out,
                    bp::std_err > err);
    in << stdinData;
    in.flush();
    in.pipe().close();
    std::thread outReader([&]() {
      std::stringstream buffer;
      buffer << out.rdbuf();
      result.out = buffer.str();
    });
    std::thread errReader([&]() {
      std::stringstream buffer;
      buffer << err.rdbuf();
      result.err = buffer.str();
    });
    // Always join the reader threads, even if something below throws. Destroying
    // a still-joinable std::thread calls std::terminate and aborts the agent.
    struct JoinGuard {
      std::thread& thread;
      ~JoinGuard() {
        if (thread.joinable()) {
          thread.join();
        }
      }
    } outGuard{outReader}, errGuard{errReader};

    // The timeout watchdog runs on this same thread: boost::process child
    // objects are not safe for concurrent wait()/terminate() calls, and racing
    // them from a separate thread is what aborted the agent.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    bool timedOut = false;
    std::error_code ec;
    while (child.running(ec) && !ec) {
      if (std::chrono::steady_clock::now() >= deadline) {
        timedOut = true;
        child.terminate(ec);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    child.wait(ec);
    // Pipes reach EOF once the child exits, so the readers return and the
    // JoinGuards above can complete without blocking.
    outReader.join();
    errReader.join();
    if (timedOut) {
      result.code = 124;
      result.err = trim(result.err + "\ntimed out");
    } else {
      result.code = child.exit_code();
    }
  } catch (const std::exception& ex) {
    result.code = 1;
    result.err = ex.what();
  }
  return result;
}

int dockerUpdateHealth(const std::string& containerId, const std::string& command) {
  try {
    bp::child child(bp::search_path("docker"),
                    bp::args({"update", "--health-cmd", command, containerId}),
                    bp::std_out > bp::null, bp::std_err > bp::null);
    // Single-threaded watchdog: only this thread touches the child object.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool timedOut = false;
    std::error_code ec;
    while (child.running(ec) && !ec) {
      if (std::chrono::steady_clock::now() >= deadline) {
        timedOut = true;
        child.terminate(ec);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    child.wait(ec);
    return timedOut ? 124 : child.exit_code();
  } catch (const std::exception&) {
    return 1;
  }
}

class Failure : public std::runtime_error {
public:
  explicit Failure(const std::string& message) : std::runtime_error(message) {}
};

std::string failureText(const ExecResult& result, const std::string& secret) {
  std::string text = redact(result.err.empty() ? result.out : result.err, secret);
  if (text.empty()) {
    text = "database command failed";
  }
  if (text.size() > 800) {
    text.resize(800);
  }
  return text;
}

ExecResult runSql(const Session& session, const std::string& database, const std::string& sql) {
  std::vector<std::string> args = {"exec", "-i"};
  const std::string db = database.empty() ? session.maintenanceDb : database;
  if (session.engine == "postgres") {
    args.push_back("-e");
    args.push_back("PGPASSWORD=" + session.adminPassword);
    args.push_back(session.containerId);
    args.push_back("psql");
    args.push_back("-U");
    args.push_back(session.adminUser);
    args.push_back("-d");
    args.push_back(db);
    args.push_back("-v");
    args.push_back("ON_ERROR_STOP=1");
    args.push_back("-X");
    args.push_back("-q");
    args.push_back("-t");
    args.push_back("-A");
    args.push_back("-F");
    args.push_back("|");
  } else {
    args.push_back("-e");
    args.push_back("MYSQL_PWD=" + session.adminPassword);
    args.push_back(session.containerId);
    args.push_back(session.client.empty() ? "mysql" : session.client);
    args.push_back("--user=" + session.adminUser);
    args.push_back("--database=" + (db.empty() ? "mysql" : db));
    args.push_back("--batch");
    args.push_back("--raw");
    args.push_back("--skip-column-names");
    args.push_back("--default-character-set=utf8mb4");
  }
  std::string script = sql;
  if (script.empty() || script.back() != '\n') {
    script.push_back('\n');
  }
  return dockerExec(args, script);
}

void execOrThrow(const Session& session, const std::string& database, const std::string& sql) {
  const auto result = runSql(session, database, sql);
  if (result.code != 0) {
    throw Failure(failureText(result, session.adminPassword));
  }
}

std::vector<std::string> queryLines(const Session& session, const std::string& database,
                                    const std::string& sql) {
  const auto result = runSql(session, database, sql);
  if (result.code != 0) {
    throw Failure(failureText(result, session.adminPassword));
  }
  return splitLines(result.out);
}

std::string requireIdent(const nlohmann::json& body, const char* key) {
  const auto value = body.value(key, std::string{});
  if (!isIdent(value)) {
    throw Failure(std::string(key) + " must start with a letter and use only letters, digits, and underscores");
  }
  return value;
}

const std::vector<std::string>& privilegesFor(const Session& session) {
  return session.engine == "postgres" ? kPostgresPrivileges : kMysqlPrivileges;
}

bool isPrivilege(const Session& session, const std::string& privilege) {
  const auto& list = privilegesFor(session);
  return std::find(list.begin(), list.end(), privilege) != list.end();
}

std::string field(const std::string& line, char separator, std::size_t index) {
  const auto parts = splitOn(line, separator);
  if (index >= parts.size()) {
    return {};
  }
  return parts[index];
}

bool truthy(const std::string& value) { return value == "1" || value == "t" || value == "true"; }

nlohmann::json listMysqlUsers(const Session& session) {
  const auto lines = queryLines(
      session, "mysql",
      "SELECT User, Host FROM mysql.user "
      "WHERE User <> '' ORDER BY User, Host");
  nlohmann::json users = nlohmann::json::array();
  std::string current;
  std::string host;
  auto flush = [&]() {
    if (current.empty() || kMysqlHiddenUsers.count(current) || current.rfind("mysql.", 0) == 0) {
      return;
    }
    const bool management = current == session.adminUser;
    users.push_back({{"name", current},
                     {"host", host},
                     {"protected", management},
                     {"superuser", management}});
  };
  for (const auto& line : lines) {
    const auto parts = splitOn(line, '\t');
    if (parts.size() < 2) {
      continue;
    }
    const auto name = parts[0];
    const auto rowHost = parts[1];
    if (name != current) {
      flush();
      current = name;
      host = rowHost;
    } else if (rowHost == "%" || host != "%") {
      host = rowHost == "%" ? rowHost : host;
    }
  }
  flush();
  return {{"users", users}, {"privileges", privilegesFor(session)}};
}

std::vector<std::string> mysqlHosts(const Session& session, const std::string& name) {
  const auto lines = queryLines(session, "mysql",
                                "SELECT Host FROM mysql.user WHERE User = '" + name + "'");
  std::vector<std::string> hosts;
  for (const auto& line : lines) {
    const auto host = trim(line);
    if (isHost(host)) {
      hosts.push_back(host);
    }
  }
  return hosts;
}

void assertNotManagement(const Session& session, const std::string& name, const char* action) {
  if (name == session.adminUser) {
    throw Failure(std::string("The management user cannot be ") + action);
  }
}

nlohmann::json createMysqlUser(const Session& session, const nlohmann::json& body) {
  const auto name = requireIdent(body, "name");
  const auto password = body.value("password", std::string{});
  if (password.empty() || password.size() > 128) {
    throw Failure("password is required");
  }
  execOrThrow(session, "mysql",
              "CREATE USER " + myAccount(name, "%") + " IDENTIFIED BY " + hexLiteral(password));
  return nlohmann::json::object();
}

nlohmann::json updateMysqlUser(const Session& session, const nlohmann::json& body) {
  const auto name = requireIdent(body, "name");
  const auto newName = body.value("newName", std::string{});
  const auto password = body.value("password", std::string{});
  const bool rename = !newName.empty() && newName != name;
  if (rename) {
    if (!isIdent(newName)) {
      throw Failure("newName must start with a letter and use only letters, digits, and underscores");
    }
    assertNotManagement(session, name, "renamed");
  }
  if (!password.empty() && password.size() > 128) {
    throw Failure("password is too long");
  }
  if (!rename && password.empty()) {
    throw Failure("Nothing to change");
  }
  const auto hosts = mysqlHosts(session, name);
  if (hosts.empty()) {
    throw Failure("User not found");
  }
  std::string sql;
  if (rename) {
    for (const auto& host : hosts) {
      sql += "RENAME USER " + myAccount(name, host) + " TO " + myAccount(newName, host) + ";\n";
    }
  }
  const auto account = rename ? newName : name;
  if (!password.empty()) {
    for (const auto& host : hosts) {
      sql += "ALTER USER " + myAccount(account, host) + " IDENTIFIED BY " + hexLiteral(password) +
             ";\n";
    }
  }
  execOrThrow(session, "mysql", sql);
  nlohmann::json result = nlohmann::json::object();
  if (!session.healthCommand.empty() && name == session.adminUser && !password.empty()) {
    result["healthUpdated"] = dockerUpdateHealth(session.containerId, session.healthCommand) == 0;
  }
  return result;
}

nlohmann::json deleteMysqlUser(const Session& session, const nlohmann::json& body) {
  const auto name = requireIdent(body, "name");
  assertNotManagement(session, name, "deleted");
  const auto hosts = mysqlHosts(session, name);
  if (hosts.empty()) {
    throw Failure("User not found");
  }
  std::string sql;
  for (const auto& host : hosts) {
    sql += "DROP USER " + myAccount(name, host) + ";\n";
  }
  execOrThrow(session, "mysql", sql);
  return nlohmann::json::object();
}

nlohmann::json listMysqlDatabases(const Session& session) {
  const auto lines = queryLines(session, "mysql", "SHOW DATABASES");
  nlohmann::json databases = nlohmann::json::array();
  for (const auto& line : lines) {
    const auto name = trim(line);
    if (!kMysqlHiddenDatabases.count(name)) {
      databases.push_back(name);
    }
  }
  return {{"databases", databases}};
}

nlohmann::json createMysqlDatabase(const Session& session, const nlohmann::json& body) {
  const auto name = requireIdent(body, "name");
  if (kMysqlHiddenDatabases.count(name)) {
    throw Failure("That database name is reserved");
  }
  execOrThrow(session, "mysql",
              "CREATE DATABASE " + myIdent(name) +
                  " CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci");
  return nlohmann::json::object();
}

bool parseGrantee(const std::string& grantee, std::string& user, std::string& host) {
  if (grantee.size() < 5 || grantee.front() != '\'') {
    return false;
  }
  const auto mid = grantee.find("'@'");
  if (mid == std::string::npos) {
    return false;
  }
  user = grantee.substr(1, mid - 1);
  host = grantee.substr(mid + 3);
  if (!host.empty() && host.back() == '\'') {
    host.pop_back();
  }
  return isIdent(user) && isHost(host);
}

nlohmann::json mysqlGrants(const Session& session, const nlohmann::json& body) {
  const auto database = requireIdent(body, "database");
  const auto usersDoc = listMysqlUsers(session);
  const auto lines = queryLines(
      session, "mysql",
      "SELECT GRANTEE, PRIVILEGE_TYPE FROM information_schema.schema_privileges "
      "WHERE TABLE_SCHEMA = '" +
          database + "'");
  std::unordered_set<std::string> held;
  for (const auto& line : lines) {
    const auto parts = splitOn(line, '\t');
    if (parts.size() < 2) {
      continue;
    }
    std::string user;
    std::string host;
    if (!parseGrantee(parts[0], user, host)) {
      continue;
    }
    held.insert(user + "\n" + host + "\n" + parts[1]);
    if (parts[1] == "ALL" || parts[1] == "ALL PRIVILEGES") {
      for (const auto& privilege : kMysqlPrivileges) {
        held.insert(user + "\n" + host + "\n" + privilege);
      }
    }
  }
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& user : usersDoc["users"]) {
    nlohmann::json grants = nlohmann::json::object();
    const auto name = user.value("name", std::string{});
    const auto host = user.value("host", std::string{"%"});
    const bool superuser = user.value("superuser", false);
    for (const auto& privilege : kMysqlPrivileges) {
      grants[privilege] = superuser || held.count(name + "\n" + host + "\n" + privilege) > 0;
    }
    rows.push_back({{"name", name},
                    {"host", host},
                    {"protected", user.value("protected", false)},
                    {"superuser", superuser},
                    {"grants", grants}});
  }
  return {{"database", database}, {"privileges", kMysqlPrivileges}, {"rows", rows}};
}

nlohmann::json setMysqlGrants(const Session& session, const nlohmann::json& body) {
  const auto database = requireIdent(body, "database");
  if (!body.contains("grants") || !body.at("grants").is_object()) {
    throw Failure("grants must be an object");
  }
  const auto hosts = body.value("hosts", nlohmann::json::object());
  std::string sql;
  for (auto it = body.at("grants").begin(); it != body.at("grants").end(); ++it) {
    const auto name = it.key();
    if (!isIdent(name)) {
      throw Failure("Invalid user in grants");
    }
    if (name == session.adminUser) {
      throw Failure("The management user privileges cannot be changed");
    }
    std::string host = "%";
    if (hosts.contains(name)) {
      host = hosts.value(name, std::string{"%"});
    }
    if (!isHost(host)) {
      throw Failure("Invalid host in grants");
    }
    if (!it.value().is_array()) {
      throw Failure("Each grant entry must be a list of privileges");
    }
    std::vector<std::string> selected;
    for (const auto& privilege : it.value()) {
      const auto namePriv = privilege.get<std::string>();
      if (!isPrivilege(session, namePriv)) {
        throw Failure("Unknown privilege: " + namePriv);
      }
      selected.push_back(namePriv);
    }
    // Missing database-level grants are not an error; revoke only what exists.
    const auto existing = runSql(session, "mysql",
                                 "REVOKE ALL PRIVILEGES ON " + myIdent(database) + ".* FROM " +
                                     myAccount(name, host));
    if (existing.code != 0) {
      const auto message = failureText(existing, session.adminPassword);
      if (message.find("no such grant") == std::string::npos &&
          message.find("matching row in the user") == std::string::npos) {
        throw Failure(message);
      }
    }
    if (!selected.empty()) {
      std::string list;
      for (const auto& privilege : selected) {
        if (!list.empty()) {
          list += ", ";
        }
        list += privilege;
      }
      sql += "GRANT " + list + " ON " + myIdent(database) + ".* TO " + myAccount(name, host) +
             ";\n";
    }
  }
  if (!sql.empty()) {
    sql += "FLUSH PRIVILEGES;\n";
    execOrThrow(session, "mysql", sql);
  }
  return nlohmann::json::object();
}

nlohmann::json listPostgresUsers(const Session& session) {
  const auto lines = queryLines(
      session, session.maintenanceDb,
      "SELECT rolname, rolsuper::int FROM pg_roles "
      "WHERE rolcanlogin AND rolname !~ '^pg_' ORDER BY rolname");
  nlohmann::json users = nlohmann::json::array();
  for (const auto& line : lines) {
    const auto name = field(line, '|', 0);
    if (!isIdent(name)) {
      continue;
    }
    const bool superuser = truthy(field(line, '|', 1)) || name == session.adminUser;
    users.push_back({{"name", name},
                     {"protected", name == session.adminUser},
                     {"superuser", superuser}});
  }
  return {{"users", users}, {"privileges", kPostgresPrivileges}};
}

nlohmann::json createPostgresUser(const Session& session, const nlohmann::json& body) {
  const auto name = requireIdent(body, "name");
  const auto password = body.value("password", std::string{});
  if (password.empty() || password.size() > 128 || password.find('\0') != std::string::npos) {
    throw Failure("password is required");
  }
  execOrThrow(session, session.maintenanceDb,
              "CREATE ROLE " + pgIdent(name) + " LOGIN PASSWORD " + sqlString(password));
  return nlohmann::json::object();
}

nlohmann::json updatePostgresUser(const Session& session, const nlohmann::json& body) {
  const auto name = requireIdent(body, "name");
  const auto newName = body.value("newName", std::string{});
  const auto password = body.value("password", std::string{});
  const bool rename = !newName.empty() && newName != name;
  if (rename) {
    if (!isIdent(newName)) {
      throw Failure("newName must start with a letter and use only letters, digits, and underscores");
    }
    assertNotManagement(session, name, "renamed");
  }
  if (!password.empty() && password.size() > 128) {
    throw Failure("password is too long");
  }
  if (!rename && password.empty()) {
    throw Failure("Nothing to change");
  }
  std::string sql;
  std::string current = name;
  if (rename) {
    sql += "ALTER ROLE " + pgIdent(name) + " RENAME TO " + pgIdent(newName) + ";\n";
    current = newName;
  }
  if (!password.empty()) {
    sql += "ALTER ROLE " + pgIdent(current) + " PASSWORD " + sqlString(password) + ";\n";
  }
  execOrThrow(session, session.maintenanceDb, sql);
  return nlohmann::json::object();
}

nlohmann::json deletePostgresUser(const Session& session, const nlohmann::json& body) {
  const auto name = requireIdent(body, "name");
  assertNotManagement(session, name, "deleted");
  const auto databases = queryLines(
      session, session.maintenanceDb,
      "SELECT datname FROM pg_database WHERE datistemplate = false AND datallowconn");
  for (const auto& database : databases) {
    const auto db = trim(database);
    if (!isIdent(db)) {
      continue;
    }
    execOrThrow(session, db,
                "REASSIGN OWNED BY " + pgIdent(name) + " TO " + pgIdent(session.adminUser) +
                    ";\nDROP OWNED BY " + pgIdent(name) + ";\n");
  }
  execOrThrow(session, session.maintenanceDb, "DROP ROLE " + pgIdent(name));
  return nlohmann::json::object();
}

nlohmann::json listPostgresDatabases(const Session& session) {
  const auto lines = queryLines(
      session, session.maintenanceDb,
      "SELECT datname FROM pg_database WHERE datistemplate = false AND datallowconn ORDER BY datname");
  nlohmann::json databases = nlohmann::json::array();
  for (const auto& line : lines) {
    const auto name = trim(line);
    if (isIdent(name) && name != "template0" && name != "template1") {
      databases.push_back(name);
    }
  }
  return {{"databases", databases}};
}

nlohmann::json createPostgresDatabase(const Session& session, const nlohmann::json& body) {
  const auto name = requireIdent(body, "name");
  if (name == "template0" || name == "template1") {
    throw Failure("That database name is reserved");
  }
  execOrThrow(session, session.maintenanceDb, "CREATE DATABASE " + pgIdent(name));
  nlohmann::json result = nlohmann::json::object();
  if (session.vectorExtension) {
    const auto extension = runSql(session, name, "CREATE EXTENSION IF NOT EXISTS vector");
    result["vectorExtension"] = extension.code == 0;
    if (extension.code != 0) {
      result["warning"] = failureText(extension, session.adminPassword);
    }
  }
  return result;
}

std::string unquoteRole(std::string name) {
  name = trim(name);
  if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
    name = name.substr(1, name.size() - 2);
  }
  return name;
}

nlohmann::json postgresGrants(const Session& session, const nlohmann::json& body) {
  const auto database = requireIdent(body, "database");
  const auto dbLines = queryLines(
      session, session.maintenanceDb,
      "SELECT rolname, rolsuper::int, "
      "has_database_privilege(rolname, " +
          sqlString(database) +
          ", 'CONNECT')::int, "
          "has_database_privilege(rolname, " +
          sqlString(database) +
          ", 'CREATE')::int, "
          "has_database_privilege(rolname, " +
          sqlString(database) +
          ", 'TEMPORARY')::int "
          "FROM pg_roles WHERE rolcanlogin AND rolname !~ '^pg_' ORDER BY rolname");
  const auto tableCountLine = queryLines(
      session, database,
      "SELECT COUNT(*)::int FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
      "WHERE n.nspname = 'public' AND c.relkind = 'r'");
  const int tableCount = tableCountLine.empty() ? 0 : std::atoi(tableCountLine.front().c_str());
  std::unordered_set<std::string> tablePrivs;
  if (tableCount > 0) {
    const auto lines = queryLines(
        session, database,
        "SELECT rolname, "
        "COALESCE((SELECT bool_and(has_table_privilege(pg_roles.rolname, c.oid, 'SELECT')) "
        "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'public' AND c.relkind = 'r'), false)::int, "
        "COALESCE((SELECT bool_and(has_table_privilege(pg_roles.rolname, c.oid, 'INSERT')) "
        "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'public' AND c.relkind = 'r'), false)::int, "
        "COALESCE((SELECT bool_and(has_table_privilege(pg_roles.rolname, c.oid, 'UPDATE')) "
        "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'public' AND c.relkind = 'r'), false)::int, "
        "COALESCE((SELECT bool_and(has_table_privilege(pg_roles.rolname, c.oid, 'DELETE')) "
        "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'public' AND c.relkind = 'r'), false)::int, "
        "COALESCE((SELECT bool_and(has_table_privilege(pg_roles.rolname, c.oid, 'TRUNCATE')) "
        "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'public' AND c.relkind = 'r'), false)::int "
        "FROM pg_roles WHERE rolcanlogin AND rolname !~ '^pg_' ORDER BY rolname");
    const char* names[] = {"SELECT", "INSERT", "UPDATE", "DELETE", "TRUNCATE"};
    for (const auto& line : lines) {
      const auto parts = splitOn(line, '|');
      if (parts.empty()) {
        continue;
      }
      for (std::size_t i = 0; i < 5 && i + 1 < parts.size(); ++i) {
        if (truthy(parts[i + 1])) {
          tablePrivs.insert(parts[0] + "\n" + names[i]);
        }
      }
    }
  } else {
    const auto lines = queryLines(
        session, database,
        "SELECT grantee::regrole::text, privilege_type "
        "FROM pg_default_acl d "
        "JOIN pg_namespace n ON n.oid = d.defaclnamespace "
        "CROSS JOIN LATERAL aclexplode(d.defaclacl) AS x "
        "WHERE n.nspname = 'public' AND d.defaclobjtype = 'r'");
    for (const auto& line : lines) {
      const auto role = unquoteRole(field(line, '|', 0));
      const auto privilege = field(line, '|', 1);
      if (isIdent(role) && isPrivilege(session, privilege)) {
        tablePrivs.insert(role + "\n" + privilege);
      }
    }
  }
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& line : dbLines) {
    const auto parts = splitOn(line, '|');
    if (parts.size() < 5 || !isIdent(parts[0])) {
      continue;
    }
    const bool superuser = truthy(parts[1]) || parts[0] == session.adminUser;
    nlohmann::json grants = {
        {"CONNECT", superuser || truthy(parts[2])},
        {"CREATE", superuser || truthy(parts[3])},
        {"TEMPORARY", superuser || truthy(parts[4])},
    };
    for (const auto& privilege : {"SELECT", "INSERT", "UPDATE", "DELETE", "TRUNCATE"}) {
      grants[privilege] = superuser || tablePrivs.count(parts[0] + "\n" + privilege) > 0;
    }
    rows.push_back({{"name", parts[0]},
                    {"protected", parts[0] == session.adminUser},
                    {"superuser", superuser},
                    {"grants", grants}});
  }
  return {{"database", database}, {"privileges", kPostgresPrivileges}, {"rows", rows}};
}

std::string joinPrivileges(const std::vector<std::string>& privileges) {
  std::string list;
  for (const auto& privilege : privileges) {
    if (!list.empty()) {
      list += ", ";
    }
    list += privilege;
  }
  return list;
}

nlohmann::json setPostgresGrants(const Session& session, const nlohmann::json& body) {
  const auto database = requireIdent(body, "database");
  if (!body.contains("grants") || !body.at("grants").is_object()) {
    throw Failure("grants must be an object");
  }
  std::string databaseSql;
  std::string schemaSql;
  for (auto it = body.at("grants").begin(); it != body.at("grants").end(); ++it) {
    const auto name = it.key();
    if (!isIdent(name)) {
      throw Failure("Invalid user in grants");
    }
    if (name == session.adminUser) {
      throw Failure("The management user privileges cannot be changed");
    }
    if (!it.value().is_array()) {
      throw Failure("Each grant entry must be a list of privileges");
    }
    std::unordered_set<std::string> selected;
    for (const auto& privilege : it.value()) {
      const auto priv = privilege.get<std::string>();
      if (!isPrivilege(session, priv)) {
        throw Failure("Unknown privilege: " + priv);
      }
      selected.insert(priv);
    }
    const bool tableAccess = selected.count("SELECT") || selected.count("INSERT") ||
                             selected.count("UPDATE") || selected.count("DELETE") ||
                             selected.count("TRUNCATE");
    if (tableAccess) {
      selected.insert("CONNECT");
    }
    const auto ident = pgIdent(name);
    databaseSql += "REVOKE ALL PRIVILEGES ON DATABASE " + pgIdent(database) + " FROM " + ident +
                   ";\n";
    std::vector<std::string> dbGrants;
    if (selected.count("CONNECT")) {
      dbGrants.push_back("CONNECT");
    }
    if (selected.count("CREATE")) {
      dbGrants.push_back("CREATE");
    }
    if (selected.count("TEMPORARY")) {
      dbGrants.push_back("TEMPORARY");
    }
    if (!dbGrants.empty()) {
      databaseSql += "GRANT " + joinPrivileges(dbGrants) + " ON DATABASE " + pgIdent(database) +
                     " TO " + ident + ";\n";
    }
    schemaSql += "REVOKE ALL PRIVILEGES ON SCHEMA public FROM " + ident + ";\n";
    if (tableAccess || selected.count("CREATE")) {
      schemaSql += "GRANT USAGE ON SCHEMA public TO " + ident + ";\n";
    }
    if (selected.count("CREATE")) {
      schemaSql += "GRANT CREATE ON SCHEMA public TO " + ident + ";\n";
    }
    schemaSql += "REVOKE ALL PRIVILEGES ON ALL TABLES IN SCHEMA public FROM " + ident + ";\n";
    schemaSql += "ALTER DEFAULT PRIVILEGES IN SCHEMA public REVOKE ALL ON TABLES FROM " + ident +
                 ";\n";
    std::vector<std::string> tableGrants;
    for (const auto& privilege : {"SELECT", "INSERT", "UPDATE", "DELETE", "TRUNCATE"}) {
      if (selected.count(privilege)) {
        tableGrants.emplace_back(privilege);
      }
    }
    if (!tableGrants.empty()) {
      const auto list = joinPrivileges(tableGrants);
      schemaSql += "GRANT " + list + " ON ALL TABLES IN SCHEMA public TO " + ident + ";\n";
      schemaSql += "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT " + list + " ON TABLES TO " +
                   ident + ";\n";
    }
    schemaSql += "REVOKE ALL PRIVILEGES ON ALL SEQUENCES IN SCHEMA public FROM " + ident + ";\n";
    schemaSql += "ALTER DEFAULT PRIVILEGES IN SCHEMA public REVOKE ALL ON SEQUENCES FROM " + ident +
                 ";\n";
    if (selected.count("INSERT") || selected.count("UPDATE")) {
      schemaSql += "GRANT USAGE, SELECT, UPDATE ON ALL SEQUENCES IN SCHEMA public TO " + ident +
                   ";\n";
      schemaSql +=
          "ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT USAGE, SELECT, UPDATE ON SEQUENCES TO " +
          ident + ";\n";
    }
  }
  if (!databaseSql.empty()) {
    execOrThrow(session, session.maintenanceDb, databaseSql);
  }
  if (!schemaSql.empty()) {
    execOrThrow(session, database, schemaSql);
  }
  return nlohmann::json::object();
}

Session sessionFrom(const std::string& containerId, const nlohmann::json& body) {
  if (!isSafeContainerId(containerId)) {
    throw Failure("Invalid container");
  }
  Session session;
  session.containerId = containerId;
  session.engine = body.value("engine", std::string{});
  session.client = body.value("client", std::string{});
  session.adminUser = body.value("adminUser", std::string{});
  session.adminPassword = body.value("adminPassword", std::string{});
  session.maintenanceDb = body.value("maintenanceDb", std::string{});
  session.vectorExtension = body.value("vectorExtension", false);
  session.healthCommand = body.value("healthCommand", std::string{});
  if (session.engine != "mysql" && session.engine != "postgres") {
    throw Failure("Unsupported database engine");
  }
  if (!isIdent(session.adminUser) || session.adminPassword.empty()) {
    throw Failure("Database management credentials are missing");
  }
  if (session.engine == "postgres" && !isIdent(session.maintenanceDb)) {
    session.maintenanceDb = "postgres";
  }
  if (session.engine == "mysql" && session.client != "mysql" && session.client != "mariadb") {
    session.client = "mysql";
  }
  if (session.healthCommand.find('\n') != std::string::npos || session.healthCommand.size() > 2000) {
    session.healthCommand.clear();
  }
  return session;
}

nlohmann::json dispatch(const Session& session, const nlohmann::json& body) {
  const auto op = body.value("op", std::string{});
  const bool postgres = session.engine == "postgres";
  if (op == "list_users") {
    return postgres ? listPostgresUsers(session) : listMysqlUsers(session);
  }
  if (op == "create_user") {
    return postgres ? createPostgresUser(session, body) : createMysqlUser(session, body);
  }
  if (op == "update_user") {
    return postgres ? updatePostgresUser(session, body) : updateMysqlUser(session, body);
  }
  if (op == "delete_user") {
    return postgres ? deletePostgresUser(session, body) : deleteMysqlUser(session, body);
  }
  if (op == "list_databases") {
    return postgres ? listPostgresDatabases(session) : listMysqlDatabases(session);
  }
  if (op == "create_database") {
    return postgres ? createPostgresDatabase(session, body) : createMysqlDatabase(session, body);
  }
  if (op == "get_grants") {
    return postgres ? postgresGrants(session, body) : mysqlGrants(session, body);
  }
  if (op == "set_grants") {
    return postgres ? setPostgresGrants(session, body) : setMysqlGrants(session, body);
  }
  throw Failure("Unknown database operation");
}

}  // namespace

DbManageOutput manageDatabase(const std::string& containerId, const std::string& payloadJson) {
  DbManageOutput output;
  try {
    const auto body = nlohmann::json::parse(payloadJson);
    if (!body.is_object()) {
      throw Failure("Invalid management payload");
    }
    const auto session = sessionFrom(containerId, body);
    spdlog::info("db manage op {} engine {} container {}", body.value("op", std::string{}),
                 session.engine, containerId.substr(0, 12));
    output.payloadJson = dispatch(session, body).dump();
    output.ok = true;
  } catch (const Failure& ex) {
    output.ok = false;
    output.error = ex.what();
  } catch (const std::exception& ex) {
    output.ok = false;
    output.error = ex.what();
  }
  return output;
}

}  // namespace highcube::agent
