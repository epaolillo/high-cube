#include "proxy/nginx_proxy.hpp"

#include <boost/process.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace bp = boost::process;

namespace highcube::admin {

NginxProxy::NginxProxy(AdminConfig config, Repositories& repos)
    : config_(std::move(config)), repos_(repos) {}

std::string NginxProxy::sitePath(const std::string& name) const {
  return (std::filesystem::path(config_.nginxSitesDir) / ("highcube-" + name + ".conf")).string();
}

std::string NginxProxy::applicationSitePath(int64_t applicationId) const {
  return (std::filesystem::path(config_.nginxSitesDir) /
          ("highcube-app-" + std::to_string(applicationId) + ".conf"))
      .string();
}

void NginxProxy::writeFile(const std::string& path, const std::string& content) const {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream out(path);
  out << content;
}

std::string NginxProxy::acmeWebroot() const {
  return (std::filesystem::path(config_.nginxSitesDir).parent_path() / "acme")
      .lexically_normal()
      .string();
}

void NginxProxy::appendAcmeChallenge(std::ostringstream& oss) const {
  // HTTP-01 must be answered by this server_name. A catch-all default_server
  // would otherwise hide the subdomain from Let's Encrypt.
  oss << "  location ^~ /.well-known/acme-challenge/ {\n"
      << "    default_type \"text/plain\";\n"
      << "    root " << acmeWebroot() << ";\n"
      << "  }\n";
}

void NginxProxy::appendProxyLocation(std::ostringstream& oss, int64_t applicationId) const {
  oss << "  location / {\n"
      << "    proxy_pass http://hc_app_" << applicationId << ";\n"
      << "    proxy_set_header Host $host;\n"
      << "    proxy_set_header X-Real-IP $remote_addr;\n"
      << "    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\n"
      << "    proxy_set_header X-Forwarded-Proto $scheme;\n"
      << "  }\n";
}

void NginxProxy::appendServerBlocks(std::ostringstream& oss, const Application& app,
                                    int backends) const {
  for (const auto& row : app.hostnames) {
    if (row.hostname.empty()) {
      continue;
    }
    // Drop legacy per-hostname challenge files once the app site owns the name.
    std::error_code ec;
    std::filesystem::remove(sitePath(row.hostname), ec);
    std::filesystem::remove(sitePath("challenge-" + row.hostname), ec);

    if (backends == 0) {
      oss << "server {\n"
          << "  listen 80;\n"
          << "  server_name " << row.hostname << ";\n";
      appendAcmeChallenge(oss);
      oss << "  location / { return 502; }\n"
          << "}\n";
      if (row.tlsStatus == "active") {
        oss << "server {\n"
            << "  listen 443 ssl;\n"
            << "  server_name " << row.hostname << ";\n"
            << "  ssl_certificate /etc/letsencrypt/live/" << row.hostname << "/fullchain.pem;\n"
            << "  ssl_certificate_key /etc/letsencrypt/live/" << row.hostname << "/privkey.pem;\n"
            << "  location / { return 502; }\n"
            << "}\n";
      }
      continue;
    }

    if (row.tlsStatus == "active") {
      oss << "server {\n"
          << "  listen 80;\n"
          << "  server_name " << row.hostname << ";\n";
      appendAcmeChallenge(oss);
      oss << "  location / { return 301 https://$host$request_uri; }\n"
          << "}\n"
          << "server {\n"
          << "  listen 443 ssl;\n"
          << "  server_name " << row.hostname << ";\n"
          << "  ssl_certificate /etc/letsencrypt/live/" << row.hostname << "/fullchain.pem;\n"
          << "  ssl_certificate_key /etc/letsencrypt/live/" << row.hostname << "/privkey.pem;\n";
      appendProxyLocation(oss, app.id);
      oss << "}\n";
    } else {
      oss << "server {\n"
          << "  listen 80;\n"
          << "  server_name " << row.hostname << ";\n";
      appendAcmeChallenge(oss);
      appendProxyLocation(oss, app.id);
      oss << "}\n";
    }
  }
}

void NginxProxy::renderAdminSite() {
  std::ostringstream oss;
  const auto root = std::filesystem::path(config_.frontendDist).lexically_normal().string();
  oss << "server {\n"
      << "  listen 80 default_server;\n"
      << "  listen [::]:80 default_server;\n"
      << "  server_name _;\n"
      << "  root " << root << ";\n"
      << "  index index.html;\n"
      << "  location /api/ {\n"
      << "    proxy_pass http://127.0.0.1:" << config_.httpPort << ";\n"
      << "    proxy_http_version 1.1;\n"
      << "    proxy_set_header Upgrade $http_upgrade;\n"
      << "    proxy_set_header Connection \"upgrade\";\n"
      << "    proxy_set_header Host $host;\n"
      << "    proxy_set_header X-Real-IP $remote_addr;\n"
      << "  }\n"
      << "  location / {\n"
      << "    try_files $uri /index.html =404;\n"
      << "  }\n"
      << "}\n";
  writeFile(sitePath("admin"), oss.str());
  reload();
}

void NginxProxy::ensureHttpChallengeSite(const std::string& hostname) {
  std::filesystem::create_directories(acmeWebroot());
  std::ostringstream oss;
  oss << "server {\n"
      << "  listen 80;\n"
      << "  server_name " << hostname << ";\n";
  appendAcmeChallenge(oss);
  oss << "  location / {\n"
      << "    return 502;\n"
      << "  }\n"
      << "}\n";
  writeFile(sitePath("challenge-" + hostname), oss.str());
  reload();
}

std::string NginxProxy::renderApplicationSite(const Application& app) {
  if (app.hostnames.empty()) {
    return "nginx site skipped: application has no hostnames";
  }
  auto replicas = repos_.listReplicas(app.id, ListQuery{1, 1000, "", "asc", ""});
  std::ostringstream upstream;
  int backends = 0;
  const int internalPort = app.portsExposes > 0 ? app.portsExposes : 80;
  for (const auto& replica : replicas.rows) {
    if (!(replica.status == "running" || replica.status == "healthy")) {
      continue;
    }
    // Prefer the container IP on the highcube network: public hostnames are
    // served without publishing any host port. Fall back to a published host
    // port only when it was explicitly configured and no container IP is known.
    if (!replica.containerIp.empty()) {
      upstream << "  server " << replica.containerIp << ":" << internalPort << ";\n";
      ++backends;
      continue;
    }
    if (replica.hostPort) {
      auto server = repos_.findServerById(replica.serverId);
      if (!server) {
        continue;
      }
      upstream << "  server " << server->ip << ":" << *replica.hostPort << ";\n";
      ++backends;
    }
  }

  std::ostringstream oss;
  if (backends > 0) {
    oss << "upstream hc_app_" << app.id << " {\n" << upstream.str() << "}\n";
  }
  appendServerBlocks(oss, app, backends);
  std::filesystem::create_directories(acmeWebroot());
  const auto path = applicationSitePath(app.id);
  writeFile(path, oss.str());
  std::ostringstream summary;
  summary << "Wrote nginx site " << path << " (" << backends << " backends)";
  for (const auto& row : app.hostnames) {
    summary << "\n  server_name " << row.hostname << " tls=" << row.tlsStatus;
  }
  summary << "\n  acme webroot " << acmeWebroot();
  summary << "\n" << reload();
  return summary.str();
}

void NginxProxy::removeSite(const std::string& hostname) {
  if (hostname.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove(sitePath(hostname), ec);
  std::filesystem::remove(sitePath("challenge-" + hostname), ec);
  reload();
}

void NginxProxy::removeApplicationSite(const Application& app) {
  std::error_code ec;
  std::filesystem::remove(applicationSitePath(app.id), ec);
  for (const auto& row : app.hostnames) {
    if (!row.hostname.empty()) {
      std::filesystem::remove(sitePath(row.hostname), ec);
      std::filesystem::remove(sitePath("challenge-" + row.hostname), ec);
    }
  }
  if (!app.hostname.empty()) {
    std::filesystem::remove(sitePath(app.hostname), ec);
  }
  reload();
}

std::string NginxProxy::reload() {
  const auto nginx = bp::search_path("nginx");
  if (nginx.empty()) {
    const auto message = "nginx not found; site files were written but not loaded";
    spdlog::warn("{}", message);
    return message;
  }
  try {
    int code = 1;
    std::string command;
    if (geteuid() == 0) {
      command = nginx.string() + " -s reload";
      code = bp::system(nginx, "-s", "reload");
    } else {
      const auto sudoBin = bp::search_path("sudo");
      if (sudoBin.empty()) {
        const auto message = "nginx reload needs root and sudo is not available";
        spdlog::warn("{}", message);
        return message;
      }
      // The service user cannot signal the master process. install.sh allows
      // this exact command in sudoers.
      command = sudoBin.string() + " -n " + nginx.string() + " -s reload";
      code = bp::system(sudoBin, "-n", nginx.string(), "-s", "reload");
    }
    if (code != 0) {
      const auto message = command + " failed with status " + std::to_string(code);
      spdlog::warn("{}", message);
      return message;
    }
    return command + " ok";
  } catch (const std::exception& ex) {
    const auto message = std::string("nginx reload failed: ") + ex.what();
    spdlog::warn("{}", message);
    return message;
  }
}

}  // namespace highcube::admin
