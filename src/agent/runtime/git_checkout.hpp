#pragma once

#include <functional>
#include <string>

namespace highcube::agent {

struct CheckoutResult {
  std::string path;
  std::string commitSha;
  std::string commitMessage;
};

// onSpawn is called with the git pid while it runs, and with 0 after it exits.
// isCancelled is polled so a stop can abort the clone.
CheckoutResult checkoutRepository(const std::string& dataDir, int64_t applicationId,
                                  int64_t deploymentId, const std::string& repoUrl,
                                  const std::string& branch, const std::string& commitSha,
                                  const std::string& deployKeyPrivate,
                                  const std::string& gitAccessToken = "",
                                  const std::function<void(int)>& onSpawn = {},
                                  const std::function<bool()>& isCancelled = {});

}  // namespace highcube::agent
