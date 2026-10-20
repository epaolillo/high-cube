#include "auth/auth_middleware.hpp"

namespace highcube::admin {

AuthMiddleware::AuthMiddleware(Repositories& repos, JwtService& jwt) : repos_(repos), jwt_(jwt) {}

std::optional<User> AuthMiddleware::authenticate(const std::string& authorizationHeader) const {
  const std::string prefix = "Bearer ";
  if (authorizationHeader.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  const auto userId = jwt_.verify(authorizationHeader.substr(prefix.size()));
  if (!userId) {
    return std::nullopt;
  }
  return repos_.findUserById(*userId);
}

bool AuthMiddleware::isAdmin(const User& user) const {
  return user.role == "admin";
}

bool AuthMiddleware::canAccessApplication(const User& user, const Application& app) const {
  return isAdmin(user) || app.createdBy == user.id;
}

}  // namespace highcube::admin
