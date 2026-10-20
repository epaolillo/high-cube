#include "bootstrap/bootstrap.hpp"

#include "auth/password.hpp"

#include <spdlog/spdlog.h>
#include <termios.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace highcube::admin {
namespace {

std::string readHidden(const std::string& prompt) {
  std::cout << prompt << std::flush;
  termios oldt{};
  tcgetattr(STDIN_FILENO, &oldt);
  termios newt = oldt;
  newt.c_lflag &= ~static_cast<tcflag_t>(ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &newt);
  std::string value;
  std::getline(std::cin, value);
  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  std::cout << "\n";
  return value;
}

}  // namespace

Bootstrap::Bootstrap(AdminConfig config, Repositories& repos)
    : config_(std::move(config)), repos_(repos) {}

void Bootstrap::ensureLinuxRoot() const {
#ifndef __linux__
  throw std::runtime_error("hc-admin only supports Linux");
#endif
  if (geteuid() != 0) {
    throw std::runtime_error("hc-admin first run must be executed as root");
  }
  if (!std::filesystem::exists("/run/systemd/system") &&
      !std::filesystem::exists("/etc/systemd/system")) {
    throw std::runtime_error("systemd is required");
  }
}

void Bootstrap::promptFirstAdmin() {
  if (repos_.userCount() > 0) {
    return;
  }
  std::cout << "First High Cube administrator\n";
  std::string email;
  std::cout << "Email: " << std::flush;
  std::getline(std::cin, email);
  const auto password = readHidden("Password: ");
  const auto confirm = readHidden("Confirm password: ");
  if (password.empty() || password != confirm) {
    throw std::runtime_error("passwords do not match");
  }

  auto adminRole = repos_.findRoleByCode("admin");
  if (!adminRole || !repos_.findRoleByCode("user")) {
    throw std::runtime_error(
        "builtin roles missing; ensure migrations/002_roles_and_github.sql was applied");
  }

  User user;
  user.email = email;
  user.name = "Administrator";
  user.roleId = adminRole->id;
  user.role = "admin";
  user.passwordHash = hashPassword(password);
  repos_.createUser(user);
  spdlog::info("created first administrator {}", email);
}

void Bootstrap::writeSystemdUnit() const {
  const std::string path = "/etc/systemd/system/hc-admin.service";
  std::ofstream out(path);
  out << "[Unit]\n"
      << "Description=High Cube Admin\n"
      << "After=network-online.target\n"
      << "Wants=network-online.target\n\n"
      << "[Service]\n"
      << "Type=simple\n"
      << "WorkingDirectory=" << config_.workingDirectory << "\n"
      << "ExecStart=" << config_.binaryPath << " --service --config "
      << config_.workingDirectory << "/hc-admin.conf\n"
      << "Restart=always\n"
      << "RestartSec=3\n\n"
      << "[Install]\n"
      << "WantedBy=multi-user.target\n";
  out.close();
  std::system("systemctl daemon-reload");
  std::system("systemctl enable --now hc-admin");
}

int Bootstrap::runInstall() {
  ensureLinuxRoot();
  promptFirstAdmin();
  writeSystemdUnit();
  std::cout << "hc-admin service installed.\nAdmin URL: " << config_.publicUrl << "\n";
  return 0;
}

void Bootstrap::assertReadyForService() const {
  if (repos_.userCount() == 0) {
    throw std::runtime_error("no users configured; run sudo ./hc-admin from a terminal first");
  }
}

}  // namespace highcube::admin
