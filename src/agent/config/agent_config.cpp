#include "config/agent_config.hpp"

#include <filesystem>
#include <stdexcept>
#include <unistd.h>

namespace highcube::agent {

AgentConfig AgentConfig::load(const std::string& path) {
  Config raw;
  raw.loadFile(path);
  AgentConfig cfg;
  cfg.adminUrl = raw.get("admin_url");
  cfg.agentToken = raw.get("agent_token");
  cfg.serverName = raw.get("server_name");
  cfg.serverIp = raw.get("server_ip");
  cfg.dataDir = raw.get("data_dir", "/var/lib/highcube");
  cfg.workingDirectory = std::filesystem::current_path().string();
  char buffer[4096];
  const ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (n > 0) {
    buffer[n] = '\0';
    cfg.binaryPath = buffer;
  }
  if (cfg.adminUrl.empty() || cfg.agentToken.empty() || cfg.serverName.empty()) {
    throw std::runtime_error("admin_url, agent_token and server_name are required");
  }
  return cfg;
}

}  // namespace highcube::agent
