#include "auth/password.hpp"
#include "services/services.hpp"

namespace highcube::admin {

UserService::UserService(AppContext& ctx) : ctx_(ctx) {}

nlohmann::json UserService::login(const std::string& email, const std::string& password) {
  auto user = ctx_.repos->findUserByEmail(email);
  if (!user || !verifyPassword(password, user->passwordHash)) {
    throw std::invalid_argument("Invalid email or password");
  }
  return {{"token", ctx_.jwt->issue(user->id)}};
}

nlohmann::json UserService::list(const ListQuery& query) {
  const auto page = ctx_.repos->listUsers(query);
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& user : page.rows) {
    rows.push_back(userToJson(user));
  }
  return {{"rows", rows}, {"total", page.total}};
}

nlohmann::json UserService::get(int64_t id) {
  auto user = ctx_.repos->findUserById(id);
  if (!user) {
    throw std::runtime_error("User not found");
  }
  return userToJson(*user, true);
}

nlohmann::json UserService::create(const nlohmann::json& body, const User&) {
  const auto roleCode = body.at("role").get<std::string>();
  if (roleCode != "admin" && roleCode != "user") {
    throw std::invalid_argument("role must be admin or user");
  }
  auto role = ctx_.repos->findRoleByCode(roleCode);
  if (!role) {
    throw std::runtime_error("Role not found");
  }
  User user;
  user.email = body.at("email").get<std::string>();
  user.name = body.value("name", "");
  user.roleId = role->id;
  user.role = roleCode;
  user.passwordHash = hashPassword(body.at("password").get<std::string>());
  if (ctx_.repos->findUserByEmail(user.email)) {
    throw std::runtime_error("Email already exists");
  }
  user.id = ctx_.repos->createUser(user);
  return userToJson(*ctx_.repos->findUserById(user.id), true);
}

nlohmann::json UserService::update(int64_t id, const nlohmann::json& body, const User& actor) {
  auto user = ctx_.repos->findUserById(id);
  if (!user) {
    throw std::runtime_error("User not found");
  }
  // Profile self-edit path may omit role.
  if (body.contains("role")) {
    const auto roleCode = body.at("role").get<std::string>();
    if (roleCode != "admin" && roleCode != "user") {
      throw std::invalid_argument("role must be admin or user");
    }
    if (id == actor.id && roleCode != user->role) {
      throw std::runtime_error("Cannot change your own role");
    }
    if (user->role == "admin" && roleCode != "admin" && ctx_.repos->adminCount() <= 1) {
      throw std::runtime_error("Cannot demote the last administrator");
    }
    auto role = ctx_.repos->findRoleByCode(roleCode);
    if (!role) {
      throw std::runtime_error("Role not found");
    }
    user->roleId = role->id;
    user->role = roleCode;
  }
  user->name = body.value("name", user->name);
  if (body.contains("password") && !body.at("password").get<std::string>().empty()) {
    user->passwordHash = hashPassword(body.at("password").get<std::string>());
  }
  if (body.contains("locale")) {
    user->locale = body.at("locale").get<std::string>();
  }
  if (body.contains("avatarPath")) {
    user->avatarPath = body.at("avatarPath").get<std::string>();
  }
  ctx_.repos->updateUser(*user);
  return userToJson(*ctx_.repos->findUserById(id), true);
}

void UserService::remove(int64_t id, const User& actor) {
  if (id == actor.id) {
    throw std::runtime_error("Cannot delete the current user");
  }
  auto user = ctx_.repos->findUserById(id);
  if (!user) {
    throw std::runtime_error("User not found");
  }
  if (user->role == "admin" && ctx_.repos->adminCount() <= 1) {
    throw std::runtime_error("Cannot delete the last administrator");
  }
  if (ctx_.repos->countApplicationsByUser(id) > 0) {
    throw std::runtime_error("Delete the user's applications first");
  }
  if (ctx_.repos->countGithubAppsByUser(id) > 0) {
    throw std::runtime_error("Delete the user's GitHub integrations first");
  }
  ctx_.repos->deleteUser(id);
}

nlohmann::json UserService::setServers(int64_t id, const nlohmann::json& body) {
  auto user = ctx_.repos->findUserById(id);
  if (!user) {
    throw std::runtime_error("User not found");
  }
  std::vector<int64_t> serverIds;
  for (const auto& item : body.at("serverIds")) {
    const auto serverId = item.get<int64_t>();
    if (!ctx_.repos->findServerById(serverId)) {
      throw std::invalid_argument("Unknown server id");
    }
    serverIds.push_back(serverId);
  }
  ctx_.repos->setUserServers(id, serverIds);
  return userToJson(*ctx_.repos->findUserById(id), true);
}

nlohmann::json UserService::setDomains(int64_t id, const nlohmann::json& body) {
  auto user = ctx_.repos->findUserById(id);
  if (!user) {
    throw std::runtime_error("User not found");
  }
  std::vector<int64_t> domainIds;
  for (const auto& item : body.at("domainIds")) {
    const auto domainId = item.get<int64_t>();
    if (!ctx_.repos->findBaseDomainById(domainId)) {
      throw std::invalid_argument("Unknown domain id");
    }
    domainIds.push_back(domainId);
  }
  ctx_.repos->setUserBaseDomains(id, domainIds);
  return userToJson(*ctx_.repos->findUserById(id), true);
}

nlohmann::json UserService::listMyBaseDomains(const User& actor) {
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& domain : ctx_.repos->listBaseDomainsForUser(actor.id)) {
    rows.push_back({{"id", domain.id}, {"hostname", domain.hostname}});
  }
  return {{"rows", rows}};
}

nlohmann::json UserService::hostnameSuggestion() {
  for (int attempt = 0; attempt < 32; ++attempt) {
    const auto subdomain = randomToken(4);
    if (!ctx_.repos->subdomainLabelExists(subdomain)) {
      return {{"subdomain", subdomain}};
    }
  }
  throw std::runtime_error("Could not generate a unique subdomain");
}

nlohmann::json UserService::autocomplete(const std::string& q) {
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& user : ctx_.repos->autocompleteUsers(q)) {
    rows.push_back({{"id", user.id}, {"label", user.email}});
  }
  return {{"rows", rows}};
}

}  // namespace highcube::admin
