#pragma once

#include "api/http_types.hpp"
#include "auth/auth_middleware.hpp"

#include <functional>
#include <string>
#include <vector>

namespace highcube::admin {

class Router {
public:
  // Called after a request is handled so it can be recorded in the audit trail.
  // Receives the resolved context, the matched route pattern and the HTTP status.
  using AuditHook = std::function<void(const RequestContext&, const std::string& routePattern,
                                       int statusCode)>;

  explicit Router(AuthMiddleware& auth);

  void add(const std::string& method, const std::string& pattern, bool requireAuth, HandlerFn handler);
  void setAuditHook(AuditHook hook) { auditHook_ = std::move(hook); }
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
  AuditHook auditHook_;
};

}  // namespace highcube::admin
