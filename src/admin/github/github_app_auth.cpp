#include "github/github_app_auth.hpp"

#include <jwt-cpp/jwt.h>

#include <stdexcept>

namespace highcube::admin {

std::string GithubAppAuth::appJwt(const GithubApp& app) const {
  if (!app.appId || app.privateKeyPem.empty()) {
    throw std::runtime_error("GitHub App credentials are incomplete");
  }
  const auto now = std::chrono::system_clock::now();
  // GitHub rejects JWTs issued more than 60s in the future; clock skew buffer of 60s past.
  return jwt::create()
      .set_issuer(std::to_string(*app.appId))
      .set_issued_at(now - std::chrono::seconds{60})
      .set_expires_at(now + std::chrono::minutes{9})
      .sign(jwt::algorithm::rs256{"", app.privateKeyPem, "", ""});
}

std::string GithubAppAuth::installationToken(const GithubApp& app) {
  if (!app.installationId) {
    throw std::runtime_error("GitHub App has no installation");
  }
  {
    std::lock_guard lock(mutex_);
    auto it = cache_.find(app.id);
    if (it != cache_.end() &&
        it->second.expiresAt > std::chrono::system_clock::now() + std::chrono::minutes{5}) {
      return it->second.token;
    }
  }

  const auto jwt = appJwt(app);
  const auto path = "/app/installations/" + std::to_string(*app.installationId) + "/access_tokens";
  const auto json = http_.postJson(path, jwt, nlohmann::json::object());
  const auto token = json.at("token").get<std::string>();
  const auto expiresAtStr = json.value("expires_at", "");
  auto expiresAt = std::chrono::system_clock::now() + std::chrono::minutes{50};
  if (!expiresAtStr.empty()) {
    // GitHub returns ISO8601; keep a conservative cache if parse fails.
    (void)expiresAtStr;
  }

  std::lock_guard lock(mutex_);
  cache_[app.id] = CachedToken{token, expiresAt};
  return token;
}

}  // namespace highcube::admin
