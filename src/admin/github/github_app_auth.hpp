#pragma once

#include "domain/models.hpp"
#include "github/github_http_client.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace highcube::admin {

class GithubAppAuth {
public:
  std::string appJwt(const GithubApp& app) const;
  std::string installationToken(const GithubApp& app);

private:
  GithubHttpClient http_;
  std::mutex mutex_;
  struct CachedToken {
    std::string token;
    std::chrono::system_clock::time_point expiresAt;
  };
  std::unordered_map<int64_t, CachedToken> cache_;
};

}  // namespace highcube::admin
