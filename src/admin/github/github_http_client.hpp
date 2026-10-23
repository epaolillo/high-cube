#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace highcube::admin {

struct GithubHttpResponse {
  int status = 0;
  std::string body;
};

class GithubHttpClient {
public:
  GithubHttpResponse request(const std::string& method, const std::string& path,
                             const std::string& bearerToken = "",
                             const std::string& body = "",
                             const std::string& accept = "application/vnd.github+json") const;

  nlohmann::json getJson(const std::string& path, const std::string& bearerToken) const;
  nlohmann::json postJson(const std::string& path, const std::string& bearerToken,
                          const nlohmann::json& body) const;
  nlohmann::json patchJson(const std::string& path, const std::string& bearerToken,
                           const nlohmann::json& body) const;
};

}  // namespace highcube::admin
