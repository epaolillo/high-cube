#include "api/router.hpp"

#include "domain/models.hpp"

#include <sstream>

namespace highcube::admin {

Router::Router(AuthMiddleware& auth) : auth_(auth) {}

void Router::add(const std::string& method, const std::string& pattern, bool requireAuth,
                 HandlerFn handler) {
  routes_.push_back(Route{method, pattern, requireAuth, std::move(handler)});
}

bool Router::match(const std::string& pattern, const std::string& path,
                   std::unordered_map<std::string, std::string>& params) const {
  std::stringstream ps(pattern);
  std::stringstream ts(path);
  std::string pp;
  std::string tp;
  while (std::getline(ps, pp, '/')) {
    if (!std::getline(ts, tp, '/')) {
      return false;
    }
    if (pp.empty() && tp.empty()) {
      continue;
    }
    if (!pp.empty() && pp.front() == ':') {
      params[pp.substr(1)] = tp;
      continue;
    }
    if (pp != tp) {
      return false;
    }
  }
  return !std::getline(ts, tp, '/');
}

Response Router::handle(Request request) {
  RequestContext ctx;
  ctx.request = std::move(request);
  ctx.method = std::string(ctx.request.method_string());
  ctx.targetPath = pathOnly(std::string(ctx.request.target()));
  ctx.query = parseQuery(std::string(ctx.request.target()));

  if (ctx.method == "OPTIONS") {
    Response res{http::status::no_content, 11};
    res.set(http::field::access_control_allow_origin, "*");
    res.set(http::field::access_control_allow_headers, "Authorization, Content-Type");
    res.set(http::field::access_control_allow_methods, "GET,POST,PATCH,PUT,DELETE,OPTIONS");
    res.prepare_payload();
    return res;
  }

  for (const auto& route : routes_) {
    if (route.method != ctx.method) {
      continue;
    }
    ctx.params.clear();
    if (!match(route.pattern, ctx.targetPath, ctx.params)) {
      continue;
    }
    if (route.requireAuth) {
      const auto authHeader = std::string(ctx.request[http::field::authorization]);
      ctx.user = auth_.authenticate(authHeader);
      if (!ctx.user) {
        return errorResponse(http::status::unauthorized, "Unauthorized");
      }
    }
    try {
      return route.handler(ctx);
    } catch (const ForbiddenError& ex) {
      return errorResponse(http::status::forbidden, ex.what());
    } catch (const std::invalid_argument& ex) {
      return errorResponse(http::status::bad_request, ex.what());
    } catch (const std::runtime_error& ex) {
      return errorResponse(http::status::conflict, ex.what());
    } catch (const std::exception& ex) {
      return errorResponse(http::status::internal_server_error, ex.what());
    }
  }
  return errorResponse(http::status::not_found, "Not found");
}

}  // namespace highcube::admin
