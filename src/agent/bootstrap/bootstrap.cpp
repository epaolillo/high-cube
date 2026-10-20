#include "bootstrap/bootstrap.hpp"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace highcube::agent {

Bootstrap::Bootstrap(AgentConfig config) : config_(std::move(config)) {}

void Bootstrap::ensureLinuxRoot() const {
#ifndef __linux__
  throw std::runtime_error("hc-agent only supports Linux");
#endif
  if (geteuid() != 0) {
    throw std::runtime_error("hc-agent first run must be executed as root");
  }
}

void Bootstrap::writeSystemdUnit() const {
  std::ofstream out("/etc/systemd/system/hc-agent.service");
  out << "[Unit]\n"
      << "Description=High Cube Agent\n"
      << "After=docker.service network-online.target\n"
      << "Requires=docker.service\n"
      << "Wants=network-online.target\n\n"
      << "[Service]\n"
      << "Type=simple\n"
      << "WorkingDirectory=" << config_.workingDirectory << "\n"
      << "ExecStart=" << config_.binaryPath << " --service --config "
      << config_.workingDirectory << "/hc-agent.conf\n"
      << "Restart=always\n"
      << "RestartSec=10\n\n"
      << "[Install]\n"
      << "WantedBy=multi-user.target\n";
  out.close();
  std::system("systemctl daemon-reload");
  std::system("systemctl enable --now hc-agent");
}

int Bootstrap::runInstall() {
  ensureLinuxRoot();
  writeSystemdUnit();
  std::cout << "hc-agent service installed for server " << config_.serverName << "\n";
  return 0;
}

}  // namespace highcube::agent
