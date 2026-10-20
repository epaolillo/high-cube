#include "auth/jwt_service.hpp"

#include <jwt-cpp/jwt.h>

#include <chrono>

namespace highcube::admin {

JwtService::JwtService(std::string secret) : secret_(std::move(secret)) {}

std::string JwtService::issue(int64_t userId) const {
  const auto now = std::chrono::system_clock::now();
  return jwt::create()
      .set_issuer("highcube")
      .set_type("JWT")
      .set_issued_at(now)
      .set_expires_at(now + std::chrono::hours{24})
      .set_subject(std::to_string(userId))
      .sign(jwt::algorithm::hs256{secret_});
}

std::optional<int64_t> JwtService::verify(const std::string& token) const {
  try {
    const auto decoded = jwt::decode(token);
    const auto verifier = jwt::verify()
                              .allow_algorithm(jwt::algorithm::hs256{secret_})
                              .with_issuer("highcube");
    verifier.verify(decoded);
    return std::stoll(decoded.get_subject());
  } catch (...) {
    return std::nullopt;
  }
}

std::string JwtService::issueState(int64_t userId, int64_t githubAppId) const {
  const auto now = std::chrono::system_clock::now();
  return jwt::create()
      .set_issuer("highcube-github")
      .set_type("JWT")
      .set_issued_at(now)
      .set_expires_at(now + std::chrono::minutes{15})
      .set_subject(std::to_string(userId))
      .set_payload_claim("githubAppId", jwt::claim(std::to_string(githubAppId)))
      .sign(jwt::algorithm::hs256{secret_});
}

std::optional<std::pair<int64_t, int64_t>> JwtService::verifyState(const std::string& token) const {
  try {
    const auto decoded = jwt::decode(token);
    const auto verifier = jwt::verify()
                              .allow_algorithm(jwt::algorithm::hs256{secret_})
                              .with_issuer("highcube-github");
    verifier.verify(decoded);
    const auto userId = std::stoll(decoded.get_subject());
    const auto appId = std::stoll(decoded.get_payload_claim("githubAppId").as_string());
    return std::make_pair(userId, appId);
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace highcube::admin
