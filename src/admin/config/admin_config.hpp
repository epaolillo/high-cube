#pragma once

#include "common/config.hpp"

#include <string>

namespace highcube::admin {

struct AdminConfig {
  int httpPort = 3000;
  std::string sqlitePath = "data/highcube.db";
  std::string publicUrl = "http://localhost:3000";
  std::string frontendDist = "src/admin/frontend/dist";
  std::string nginxSitesDir = "/etc/nginx/sites-enabled";
  std::string certbotEmail;
  std::string sessionSecret = "change-me";
  std::string migrationsDir = HIGH_CUBE_MIGRATIONS_DIR;
  std::string workingDirectory;
  std::string binaryPath;
  std::string logFile = "logs/log.txt";
  int logRetentionDays = 7;  // keep about one week of daily log files
  std::string logLevel = "info";

  static AdminConfig load(const std::string& path);
};

}  // namespace highcube::admin
