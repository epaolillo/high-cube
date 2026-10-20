#pragma once

#include "common/config.hpp"

#include <string>

namespace highcube::agent {

struct AgentConfig {
  std::string adminUrl;
  std::string agentToken;
  std::string serverName;
  std::string serverIp;
  std::string workingDirectory;
  std::string binaryPath;
  std::string dataDir = "/var/lib/highcube";

  static AgentConfig load(const std::string& path);
};

}  // namespace highcube::agent
