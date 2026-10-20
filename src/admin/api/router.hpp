#pragma once

#include "api/http_types.hpp"
#include "auth/auth_middleware.hpp"

#include <string>
#include <vector>

namespace highcube::admin {

class Router {
public:
  explicit Router(AuthMiddleware& auth);

  void add(const std::string& method, const std::string& pattern, bool requireAuth, HandlerFn handler);
  Response handle(Request request);

private:
  struct Route {
    std::string method;
    std::string pattern;
    bool requireAuth = true;
    HandlerFn handler;
  };

  bool match(const std::string& pattern, const std::string& path,
             std::unordered_map<std::string, std::string>& params) const;

  AuthMiddleware& auth_;
  std::vector<Route> routes_;
};

}  // namespace highcube::admin
