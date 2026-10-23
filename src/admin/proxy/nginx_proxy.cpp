#include "proxy/nginx_proxy.hpp"

#include <spdlog/spdlog.h>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

extern char** environ;

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

std::string NginxProxy::basicAuthFilePath(int64_t applicationId) const {
  // Outside the highcube-*.conf include glob; nginx workers read it per request.
  return (std::filesystem::path(config_.nginxSitesDir) / "htpasswd" /
          ("app-" + std::to_string(applicationId)))
      .string();
}

void NginxProxy::writeFile(const std::string& path, const std::string& content) const {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream out(path);
  out << content;
}

void NginxProxy::syncBasicAuthFile(const Application& app) const {
  const auto path = basicAuthFilePath(app.id);
  if (app.basicAuthUsers.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return;
  }
  writeFile(path, app.basicAuthUsers);
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

void NginxProxy::appendProxyLocation(std::ostringstream& oss, const Application& app) const {
  oss << "  location / {\n";
  if (!app.basicAuthUsers.empty()) {
    oss << "    auth_basic \"Restricted\";\n"
        << "    auth_basic_user_file " << basicAuthFilePath(app.id) << ";\n";
  }
  // Behave like a plain reverse proxy (Traefik): no body cap, streamed bodies
  // and responses, WebSocket upgrades, and long-lived connections.
  oss << "    client_max_body_size 0;\n"
      << "    proxy_pass http://hc_app_" << app.id << ";\n"
      << "    proxy_http_version 1.1;\n"
      << "    proxy_set_header Upgrade $http_upgrade;\n"
      << "    proxy_set_header Connection $http_connection;\n"
      << "    proxy_set_header Host $host;\n"
      << "    proxy_set_header X-Real-IP $remote_addr;\n"
      << "    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\n"
      << "    proxy_set_header X-Forwarded-Proto $scheme;\n"
      << "    proxy_set_header X-Forwarded-Host $host;\n"
      << "    proxy_set_header X-Forwarded-Port $server_port;\n"
      << "    proxy_buffering off;\n"
      << "    proxy_request_buffering off;\n"
      << "    proxy_read_timeout 3600s;\n"
      << "    proxy_send_timeout 3600s;\n"
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
      appendProxyLocation(oss, app);
      oss << "}\n";
    } else {
      oss << "server {\n"
          << "  listen 80;\n"
          << "  server_name " << row.hostname << ";\n";
      appendAcmeChallenge(oss);
      appendProxyLocation(oss, app);
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
      << "    client_max_body_size 25m;\n"
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
  // Servers that receive public traffic for this application's domains.
  // A replica on any other server is reached through its VPN address and a
  // published host port, because the Docker bridge is not routed.
  std::unordered_set<int64_t> accessServerIds;
  for (const auto& row : app.hostnames) {
    if (row.baseDomainId <= 0) {
      continue;
    }
    const auto domain = repos_.findBaseDomainById(row.baseDomainId);
    if (domain && domain->accessServerId) {
      accessServerIds.insert(*domain->accessServerId);
    }
  }
  for (const auto& replica : replicas.rows) {
    if (!(replica.status == "running" || replica.status == "healthy")) {
      continue;
    }
    bool remoteFromAccessPoint = false;
    for (const auto accessId : accessServerIds) {
      if (accessId != replica.serverId) {
        remoteFromAccessPoint = true;
        break;
      }
    }
    if (remoteFromAccessPoint && replica.hostPort) {
      auto server = repos_.findServerById(replica.serverId);
      if (server && !server->ip.empty()) {
        upstream << "  server " << server->ip << ":" << *replica.hostPort << ";\n";
        ++backends;
        continue;
      }
    }
    // Same host as nginx: the container IP on the highcube network is enough,
    // so no host port has to be published. A published host port is the
    // fallback when the container IP is not known yet.
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
  // Written before the site so a reload never references a missing file.
  syncBasicAuthFile(app);
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
  std::filesystem::remove(basicAuthFilePath(app.id), ec);
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

namespace {

std::string which(const char* name) {
  const char* path = ::getenv("PATH");
  if (path == nullptr || *path == '\0') {
    path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
  }
  std::stringstream stream(path);
  std::string dir;
  while (std::getline(stream, dir, ':')) {
    if (dir.empty()) {
      continue;
    }
    const auto candidate = std::filesystem::path(dir) / name;
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) {
      return candidate.string();
    }
  }
  return {};
}

// posix_spawn instead of Boost.Process. bp::system() forks from the io_context
// thread (which also has spdlog's flush thread) and has taken hc-admin down
// with SIGSEGV while the supervisor reloads nginx.
int runAndWait(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int spawned = ::posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
  if (spawned != 0) {
    return -1;
  }
  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
}

}  // namespace

std::string NginxProxy::reload() {
  const auto nginx = which("nginx");
  if (nginx.empty()) {
    const auto message = "nginx not found; site files were written but not loaded";
    spdlog::warn("{}", message);
    return message;
  }
  try {
    std::vector<std::string> args;
    std::string command;
    if (geteuid() == 0) {
      args = {nginx, "-s", "reload"};
      command = nginx + " -s reload";
    } else {
      const auto sudoBin = which("sudo");
      if (sudoBin.empty()) {
        const auto message = "nginx reload needs root and sudo is not available";
        spdlog::warn("{}", message);
        return message;
      }
      // The service user cannot signal the master process. install.sh allows
      // this exact command in sudoers.
      args = {sudoBin, "-n", nginx, "-s", "reload"};
      command = sudoBin + " -n " + nginx + " -s reload";
    }
    const int code = runAndWait(args);
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
