#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace highcube::admin {

class JwtService {
public:
  explicit JwtService(std::string secret);

  std::string issue(int64_t userId) const;
  std::optional<int64_t> verify(const std::string& token) const;

  // Short-lived state for GitHub App manifest OAuth round-trip.
  std::string issueState(int64_t userId, int64_t githubAppId) const;
  std::optional<std::pair<int64_t, int64_t>> verifyState(const std::string& token) const;

private:
  std::string secret_;
};

}  // namespace highcube::admin
