#pragma once

#include "auth/jwt_service.hpp"
#include "db/repositories.hpp"
#include "domain/models.hpp"

#include <optional>
#include <string>

namespace highcube::admin {

class AuthMiddleware {
public:
  AuthMiddleware(Repositories& repos, JwtService& jwt);

  std::optional<User> authenticate(const std::string& authorizationHeader) const;
  bool isAdmin(const User& user) const;
  bool canAccessApplication(const User& user, const Application& app) const;

private:
  Repositories& repos_;
  JwtService& jwt_;
};

}  // namespace highcube::admin
