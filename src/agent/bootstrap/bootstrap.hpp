#pragma once

#include "config/agent_config.hpp"

namespace highcube::agent {

class Bootstrap {
public:
  explicit Bootstrap(AgentConfig config);
  int runInstall();

private:
  void ensureLinuxRoot() const;
  void writeSystemdUnit() const;
  AgentConfig config_;
};

}  // namespace highcube::agent
