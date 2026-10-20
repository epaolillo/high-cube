#pragma once

#include <string>

namespace highcube::agent {

struct CheckoutResult {
  std::string path;
  std::string commitSha;
  std::string commitMessage;
};

CheckoutResult checkoutRepository(const std::string& dataDir, int64_t applicationId,
                                  int64_t deploymentId, const std::string& repoUrl,
                                  const std::string& branch, const std::string& commitSha,
                                  const std::string& deployKeyPrivate,
                                  const std::string& gitAccessToken = "");

}  // namespace highcube::agent
