#include "config/admin_config.hpp"

#include <filesystem>
#include <unistd.h>

namespace highcube::admin {

AdminConfig AdminConfig::load(const std::string& path) {
  Config raw;
  raw.loadFile(path);

  AdminConfig cfg;
  cfg.httpPort = raw.getInt("http_port", 3000);
  cfg.sqlitePath = raw.get("sqlite_path", "data/highcube.db");
  cfg.publicUrl = raw.get("public_url", "http://localhost:3000");
  cfg.frontendDist = raw.get("frontend_dist", "src/admin/frontend/dist");
  cfg.nginxSitesDir = raw.get("nginx_sites_dir", "/etc/nginx/sites-enabled");
  cfg.certbotEmail = raw.get("certbot_email", "");
  cfg.sessionSecret = raw.get("session_secret", "change-me");
  cfg.migrationsDir = raw.get("migrations_dir", HIGH_CUBE_MIGRATIONS_DIR);
  cfg.logFile = raw.get("log_file", "logs/log.txt");
  cfg.logRetentionDays = raw.getInt("log_retention_days", 7);
  cfg.logLevel = raw.get("log_level", "info");
  cfg.workingDirectory = std::filesystem::current_path().string();

  const auto absolutize = [&](const std::string& value) {
    std::filesystem::path p(value);
    if (!p.is_absolute()) {
      p = std::filesystem::path(cfg.workingDirectory) / p;
    }
    return p.lexically_normal().string();
  };
  cfg.sqlitePath = absolutize(cfg.sqlitePath);
  cfg.frontendDist = absolutize(cfg.frontendDist);
  cfg.migrationsDir = absolutize(cfg.migrationsDir);
  cfg.logFile = absolutize(cfg.logFile);

  char buffer[4096];
  const ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (n > 0) {
    buffer[n] = '\0';
    cfg.binaryPath = buffer;
  }
  return cfg;
}

}  // namespace highcube::admin
