#include "services/services.hpp"

#include "github/github_app_auth.hpp"
#include "github/github_http_client.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace highcube::admin {
namespace {

GithubAppAuth& authSingleton() {
  static GithubAppAuth auth;
  return auth;
}

GithubHttpClient& httpSingleton() {
  static GithubHttpClient http;
  return http;
}

std::string urlEncode(const std::string& value) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);
  for (unsigned char c : value) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

std::string trimSlash(std::string origin) {
  while (!origin.empty() && origin.back() == '/') {
    origin.pop_back();
  }
  return origin;
}

bool isLoopbackUrl(const std::string& url) {
  return url.find("://localhost") != std::string::npos ||
         url.find("://127.0.0.1") != std::string::npos ||
         url.find("://[::1]") != std::string::npos;
}

std::string lowerCopy(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

// owner/name, without a trailing .git and without the GitHub host.
std::string normalizeRepoName(std::string name) {
  name = lowerCopy(std::move(name));
  const std::string https = "https://github.com/";
  const std::string ssh = "git@github.com:";
  if (name.rfind(https, 0) == 0) {
    name = name.substr(https.size());
  } else if (name.rfind(ssh, 0) == 0) {
    name = name.substr(ssh.size());
  }
  while (!name.empty() && name.back() == '/') {
    name.pop_back();
  }
  if (name.size() >= 4 && name.substr(name.size() - 4) == ".git") {
    name.resize(name.size() - 4);
  }
  return name;
}

std::string applicationRepo(const Application& app) {
  if (!app.repoFullName.empty()) {
    return normalizeRepoName(app.repoFullName);
  }
  return normalizeRepoName(app.repoUrl);
}

std::string toHex(const unsigned char* data, unsigned int len) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.resize(len * 2);
  for (unsigned int i = 0; i < len; ++i) {
    out[i * 2] = digits[data[i] >> 4];
    out[i * 2 + 1] = digits[data[i] & 0x0F];
  }
  return out;
}

bool signatureMatches(const std::string& secret, const std::string& body, std::string header) {
  header = lowerCopy(std::move(header));
  const std::string prefix = "sha256=";
  if (secret.empty() || header.size() != prefix.size() + 64 || header.rfind(prefix, 0) != 0) {
    return false;
  }
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
           reinterpret_cast<const unsigned char*>(body.data()), body.size(), digest, &len) ==
      nullptr) {
    return false;
  }
  const auto expected = prefix + toHex(digest, len);
  unsigned char diff = 0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    diff |= static_cast<unsigned char>(expected[i] ^ header[i]);
  }
  return diff == 0;
}

std::string trimCopy(std::string value) {
  auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

bool startsWithIgnoreCase(const std::string& value, const std::string& prefix) {
  if (value.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(value[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

std::string encodeRepoPath(const std::string& path) {
  std::string out;
  std::string segment;
  auto flush = [&]() {
    if (segment.empty()) {
      return;
    }
    if (!out.empty()) {
      out.push_back('/');
    }
    out += urlEncode(segment);
    segment.clear();
  };
  for (char c : path) {
    if (c == '/') {
      flush();
    } else {
      segment.push_back(c);
    }
  }
  flush();
  return out;
}

std::string decodeBase64(const std::string& raw) {
  std::string filtered;
  filtered.reserve(raw.size());
  for (unsigned char c : raw) {
    if (!std::isspace(c)) {
      filtered.push_back(static_cast<char>(c));
    }
  }
  if (filtered.empty()) {
    return {};
  }
  std::string out((filtered.size() * 3) / 4 + 4, '\0');
  const int decoded =
      EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                      reinterpret_cast<const unsigned char*>(filtered.data()),
                      static_cast<int>(filtered.size()));
  if (decoded < 0) {
    return {};
  }
  int padding = 0;
  if (!filtered.empty() && filtered.back() == '=') {
    ++padding;
  }
  if (filtered.size() > 1 && filtered[filtered.size() - 2] == '=') {
    ++padding;
  }
  out.resize(static_cast<std::size_t>(std::max(0, decoded - padding)));
  return out;
}

std::vector<int> parseExposePorts(const std::string& dockerfile) {
  std::vector<int> ports;
  std::string pending;
  std::istringstream input(dockerfile);
  std::string raw;
  auto consume = [&](std::string line) {
    line = trimCopy(line);
    if (line.empty() || line.front() == '#') {
      return;
    }
    if (!startsWithIgnoreCase(line, "EXPOSE")) {
      return;
    }
    if (line.size() > 6 && !std::isspace(static_cast<unsigned char>(line[6]))) {
      return;
    }
    std::istringstream tokens(line.substr(std::min<std::size_t>(6, line.size())));
    std::string token;
    while (tokens >> token) {
      const auto slash = token.find('/');
      if (slash != std::string::npos) {
        token = token.substr(0, slash);
      }
      if (token.empty() || !std::all_of(token.begin(), token.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
          })) {
        continue;
      }
      try {
        const int port = std::stoi(token);
        if (port < 1 || port > 65535) {
          continue;
        }
        if (std::find(ports.begin(), ports.end(), port) == ports.end()) {
          ports.push_back(port);
        }
      } catch (const std::exception&) {
      }
    }
  };
  while (std::getline(input, raw)) {
    if (!raw.empty() && raw.back() == '\r') {
      raw.pop_back();
    }
    bool continued = false;
    if (!raw.empty() && raw.back() == '\\') {
      raw.pop_back();
      continued = true;
    }
    pending += raw;
    if (continued) {
      pending.push_back(' ');
      continue;
    }
    consume(pending);
    pending.clear();
  }
  if (!pending.empty()) {
    consume(pending);
  }
  return ports;
}

std::string githubFullNameFromUrl(std::string repoUrl) {
  repoUrl = trimCopy(std::move(repoUrl));
  if (repoUrl.size() >= 4 && repoUrl.compare(repoUrl.size() - 4, 4, ".git") == 0) {
    repoUrl.resize(repoUrl.size() - 4);
  }
  while (!repoUrl.empty() && repoUrl.back() == '/') {
    repoUrl.pop_back();
  }
  const auto take = [](std::string rest) {
    const auto slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) {
      return std::string();
    }
    if (rest.find('/', slash + 1) != std::string::npos) {
      return std::string();
    }
    return rest;
  };
  const std::string httpsMark = "github.com/";
  if (const auto pos = repoUrl.find(httpsMark); pos != std::string::npos) {
    return take(repoUrl.substr(pos + httpsMark.size()));
  }
  const std::string sshMark = "github.com:";
  if (const auto pos = repoUrl.find(sshMark); pos != std::string::npos) {
    return take(repoUrl.substr(pos + sshMark.size()));
  }
  return {};
}

std::string dockerfileText(const GithubHttpResponse& response) {
  const auto body = trimCopy(response.body);
  if (body.empty() || body.front() == '[') {
    return {};
  }
  if (body.front() == '{') {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("content")) {
      return {};
    }
    return decodeBase64(json.at("content").get<std::string>());
  }
  return response.body;
}

nlohmann::json portsPayload(const std::vector<int>& ports, bool found, const std::string& reason) {
  nlohmann::json rows = nlohmann::json::array();
  for (const int port : ports) {
    rows.push_back(port);
  }
  nlohmann::json payload = {{"ports", rows}, {"found", found}};
  if (!reason.empty()) {
    payload["reason"] = reason;
  }
  return payload;
}

}  // namespace

GithubAppService::GithubAppService(AppContext& ctx) : ctx_(ctx) {}

GithubApp GithubAppService::requireAccess(int64_t id, const User& actor) const {
  auto app = ctx_.repos->findGithubAppById(id);
  if (!app) {
    throw std::runtime_error("GitHub App not found");
  }
  if (actor.role != "admin" && app->userId != actor.id) {
    throw ForbiddenError();
  }
  return *app;
}

nlohmann::json GithubAppService::list(const ListQuery& query, const User& actor) {
  ListQuery scoped = query;
  if (actor.role != "admin") {
    scoped.ownerId = actor.id;
  }
  const auto page = ctx_.repos->listGithubApps(scoped);
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& app : page.rows) {
    rows.push_back(githubAppToJson(app));
  }
  return {{"rows", rows}, {"total", page.total}};
}

nlohmann::json GithubAppService::get(int64_t id, const User& actor) {
  return githubAppToJson(requireAccess(id, actor));
}

nlohmann::json GithubAppService::createManifest(const nlohmann::json& body, const User& actor) {
  const auto name = body.at("name").get<std::string>();
  const auto origin = trimSlash(body.at("origin").get<std::string>());
  const auto organization = body.value("organization", "");

  GithubApp pending;
  pending.userId = actor.id;
  pending.name = name;
  pending.status = "pending";
  pending.id = ctx_.repos->createGithubApp(pending);

  const auto redirectUrl = origin + "/integrations/github/" + std::to_string(pending.id) + "/callback";
  const auto setupUrl = origin + "/integrations/github/" + std::to_string(pending.id) + "/setup";

  // GitHub rejects a localhost webhook URL. A public origin subscribes to push so a
  // commit on the tracked branch redeploys the application.
  const bool loopback = isLoopbackUrl(origin);
  const auto hookUrl =
      loopback ? std::string("https://example.com/high-cube/webhook")
               : origin + "/api/github-apps/" + std::to_string(pending.id) + "/webhook";

  nlohmann::json manifest = {
      {"name", name},
      {"url", origin.empty() ? "https://example.com" : origin},
      {"hook_attributes", {{"url", hookUrl}, {"active", !loopback}}},
      {"redirect_url", redirectUrl},
      {"setup_url", setupUrl},
      {"callback_urls", nlohmann::json::array({redirectUrl})},
      {"public", false},
      {"default_events", nlohmann::json::array({"push"})},
      {"default_permissions", {{"contents", "read"}, {"metadata", "read"}}},
  };

  const auto state = ctx_.jwt->issueState(actor.id, pending.id);
  const auto encodedState = urlEncode(state);
  std::string postUrl = "https://github.com/settings/apps/new?state=" + encodedState;
  if (!organization.empty()) {
    postUrl = "https://github.com/organizations/" + organization +
              "/settings/apps/new?state=" + encodedState;
  }

  return {
      {"id", pending.id},
      {"postUrl", postUrl},
      {"manifest", manifest},
      {"state", state},
  };
}

nlohmann::json GithubAppService::completeManifest(int64_t id, const nlohmann::json& body,
                                                  const User& actor) {
  auto app = requireAccess(id, actor);
  const auto code = body.at("code").get<std::string>();
  const auto state = body.at("state").get<std::string>();
  const auto verified = ctx_.jwt->verifyState(state);
  if (!verified || verified->first != actor.id || verified->second != id) {
    throw ForbiddenError("Invalid GitHub App state");
  }

  const auto path = "/app-manifests/" + code + "/conversions";
  const auto json = httpSingleton().postJson(path, "", nlohmann::json::object());

  app.appId = json.at("id").get<int64_t>();
  app.slug = json.value("slug", "");
  app.clientId = json.value("client_id", "");
  app.clientSecret = json.value("client_secret", "");
  app.webhookSecret = json.value("webhook_secret", "");
  app.privateKeyPem = json.value("pem", "");
  app.htmlUrl = json.value("html_url", "");
  if (json.contains("owner") && json.at("owner").is_object()) {
    app.ownerLogin = json.at("owner").value("login", "");
  }
  app.status = "registered";
  ctx_.repos->updateGithubApp(app);

  std::string installUrl = "https://github.com/apps/" + app.slug + "/installations/new";
  return {{"app", githubAppToJson(*ctx_.repos->findGithubAppById(id))}, {"installUrl", installUrl}};
}

nlohmann::json GithubAppService::setInstallation(int64_t id, const nlohmann::json& body,
                                                 const User& actor) {
  auto app = requireAccess(id, actor);
  const auto installationId = body.at("installationId").get<int64_t>();
  const auto jwt = authSingleton().appJwt(app);
  const auto json =
      httpSingleton().getJson("/app/installations/" + std::to_string(installationId), jwt);
  app.installationId = installationId;
  if (json.contains("account") && json.at("account").is_object()) {
    app.ownerLogin = json.at("account").value("login", app.ownerLogin);
  }
  app.status = "installed";
  ctx_.repos->updateGithubApp(app);
  return githubAppToJson(*ctx_.repos->findGithubAppById(id));
}

nlohmann::json GithubAppService::syncInstallation(int64_t id, const User& actor) {
  auto app = requireAccess(id, actor);
  const auto jwt = authSingleton().appJwt(app);
  const auto json = httpSingleton().getJson("/app/installations?per_page=100", jwt);
  if (!json.is_array() || json.empty()) {
    throw std::runtime_error("No GitHub App installations found");
  }
  const auto& first = json.front();
  app.installationId = first.at("id").get<int64_t>();
  if (first.contains("account") && first.at("account").is_object()) {
    app.ownerLogin = first.at("account").value("login", app.ownerLogin);
  }
  app.status = "installed";
  ctx_.repos->updateGithubApp(app);
  return githubAppToJson(*ctx_.repos->findGithubAppById(id));
}

nlohmann::json GithubAppService::listRepositories(int64_t id, const User& actor, int page,
                                                  int pageSize) {
  auto app = requireAccess(id, actor);
  if (app.status != "installed" || !app.installationId) {
    throw std::runtime_error("GitHub App is not installed");
  }
  if (page < 1) {
    page = 1;
  }
  if (pageSize != 10 && pageSize != 100 && pageSize != 1000) {
    pageSize = 30;
  }
  if (pageSize > 100) {
    pageSize = 100;
  }
  const auto token = authSingleton().installationToken(app);
  const auto path = "/installation/repositories?per_page=" + std::to_string(pageSize) +
                    "&page=" + std::to_string(page);
  const auto json = httpSingleton().getJson(path, token);
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& repo : json.value("repositories", nlohmann::json::array())) {
    rows.push_back({{"id", repo.value("id", 0)},
                    {"fullName", repo.value("full_name", "")},
                    {"private", repo.value("private", false)},
                    {"defaultBranch", repo.value("default_branch", "main")}});
  }
  return {{"rows", rows}, {"total", json.value("total_count", static_cast<int>(rows.size()))}};
}

nlohmann::json GithubAppService::dockerfilePorts(const User& actor,
                                                         std::optional<int64_t> githubAppId,
                                                         const std::string& repo,
                                                         const std::string& repoUrl,
                                                         const std::string& branch,
                                                         const std::string& path) {
  std::string fullName;
  std::string token;
  if (githubAppId) {
    auto app = requireAccess(*githubAppId, actor);
    if (app.status != "installed" || !app.installationId) {
      throw std::runtime_error("GitHub App is not installed");
    }
    fullName = trimCopy(repo);
    if (fullName.find('/') == std::string::npos || fullName.find('/') != fullName.rfind('/')) {
      throw std::invalid_argument("repo must be owner/name");
    }
    token = authSingleton().installationToken(app);
  } else {
    fullName = githubFullNameFromUrl(repoUrl);
    if (fullName.empty()) {
      return portsPayload({}, false, "unsupported");
    }
  }

  std::string ref = trimCopy(branch);
  if (ref.empty()) {
    ref = "main";
  }
  std::string filePath = trimCopy(path);
  while (!filePath.empty() && filePath.front() == '/') {
    filePath.erase(filePath.begin());
  }
  if (filePath.empty()) {
    filePath = "Dockerfile";
  }

  const auto apiPath = "/repos/" + encodeRepoPath(fullName) + "/contents/" + encodeRepoPath(filePath) +
                       "?ref=" + urlEncode(ref);
  const auto response =
      httpSingleton().request("GET", apiPath, token, "", "application/vnd.github.raw");
  if (response.status == 404) {
    return portsPayload({}, false, "not_found");
  }
  if (response.status < 200 || response.status >= 300) {
    return portsPayload({}, false, "not_found");
  }
  const auto text = dockerfileText(response);
  if (text.empty() && !trimCopy(response.body).empty() && trimCopy(response.body).front() == '[') {
    return portsPayload({}, false, "not_found");
  }
  return portsPayload(parseExposePorts(text), true, "");
}

nlohmann::json GithubAppService::listBranches(int64_t id, const std::string& repo,
                                              const User& actor) {
  auto app = requireAccess(id, actor);
  if (app.status != "installed" || !app.installationId) {
    throw std::runtime_error("GitHub App is not installed");
  }
  if (repo.find('/') == std::string::npos) {
    throw std::invalid_argument("repo must be owner/name");
  }
  const auto token = authSingleton().installationToken(app);
  const auto path = "/repos/" + repo + "/branches?per_page=100";
  const auto json = httpSingleton().getJson(path, token);
  nlohmann::json rows = nlohmann::json::array();
  for (const auto& branch : json) {
    rows.push_back({{"name", branch.value("name", "")}});
  }
  return {{"rows", rows}};
}

nlohmann::json GithubAppService::handleWebhook(int64_t id, const std::string& event,
                                                    const std::string& signature,
                                                    const std::string& body) {
  auto app = ctx_.repos->findGithubAppById(id);
  if (!app) {
    throw std::runtime_error("GitHub App not found");
  }
  if (!signatureMatches(app->webhookSecret, body, signature)) {
    throw ForbiddenError("Invalid webhook signature");
  }
  if (event != "push") {
    return {{"ok", true}, {"ignored", event.empty() ? "ping" : event}};
  }

  const auto payload = nlohmann::json::parse(body.empty() ? "{}" : body);
  if (payload.value("deleted", false)) {
    return {{"ok", true}, {"ignored", "branch deleted"}};
  }
  const auto ref = payload.value("ref", "");
  const std::string heads = "refs/heads/";
  if (ref.rfind(heads, 0) != 0) {
    return {{"ok", true}, {"ignored", "not a branch"}};
  }
  const auto branch = ref.substr(heads.size());
  const auto repo = normalizeRepoName(payload.value("repository", nlohmann::json::object())
                                          .value("full_name", ""));
  if (repo.empty() || branch.empty()) {
    throw std::invalid_argument("Push payload is missing repository or branch");
  }
  const auto sha = payload.value("after", "");
  std::string message;
  if (payload.contains("head_commit") && payload.at("head_commit").is_object()) {
    message = payload.at("head_commit").value("message", "");
  }

  nlohmann::json redeployed = nlohmann::json::array();
  const auto page = ctx_.repos->listApplications(ListQuery{1, 1000, "", "asc", ""});
  for (const auto& application : page.rows) {
    if (!application.githubAppId || *application.githubAppId != id) {
      continue;
    }
    if (applicationRepo(application) != repo || application.branch != branch) {
      continue;
    }
    if (!ctx_.deployments) {
      throw std::runtime_error("Deployment service is not available");
    }
    User actor;
    actor.id = application.createdBy;
    spdlog::info("github push {}@{} redeploys {}", repo, branch, application.name);
    ctx_.deployments->create(application.id, actor, false, sha, message);
    redeployed.push_back(application.id);
  }
  if (redeployed.empty()) {
    spdlog::info("github push {}@{} matches no application", repo, branch);
  }
  return {{"ok", true}, {"redeployed", redeployed}};
}

void GithubAppService::ensurePushWebhooks() {
  std::string configured = trimSlash(ctx_.config.publicUrl);
  const bool publicOrigin = !configured.empty() && !isLoopbackUrl(configured) &&
                            (configured.rfind("https://", 0) == 0 || configured.rfind("http://", 0) == 0);
  const auto page = ctx_.repos->listGithubApps(ListQuery{1, 1000, "", "asc", ""});
  for (const auto& app : page.rows) {
    if (!app.appId || app.privateKeyPem.empty() || app.webhookSecret.empty()) {
      continue;
    }
    try {
      const auto jwt = authSingleton().appJwt(app);
      std::string url;
      // public_url wins when it is a real origin. Otherwise keep the URL stored on the
      // GitHub App, which was the browser origin used to create it.
      if (publicOrigin && configured.find("://admin.example.com") == std::string::npos &&
          configured.find("example.com/high-cube") == std::string::npos) {
        url = configured + "/api/github-apps/" + std::to_string(app.id) + "/webhook";
      } else {
        const auto current = httpSingleton().getJson("/app/hook/config", jwt);
        url = current.value("url", "");
      }
      if (url.empty() || isLoopbackUrl(url) || url.find("example.com") != std::string::npos) {
        spdlog::warn("github app {} has no public webhook url; set public_url and restart", app.name);
        continue;
      }
      httpSingleton().patchJson(
          "/app", jwt,
          {{"hook_attributes", {{"url", url}, {"active", true}}},
           {"events", nlohmann::json::array({"push"})}});
      spdlog::info("github app {} push webhook enabled at {}", app.name, url);
    } catch (const std::exception& ex) {
      spdlog::error("could not enable push webhook for github app {}: {}", app.name, ex.what());
    }
  }
}

void GithubAppService::remove(int64_t id, const User& actor) {
  requireAccess(id, actor);
  if (ctx_.repos->countApplicationsUsingGithubApp(id) > 0) {
    throw std::runtime_error("Delete applications that use this GitHub App first");
  }
  ctx_.repos->deleteGithubApp(id);
}

std::string GithubAppService::installationAccessToken(int64_t githubAppId) {
  auto app = ctx_.repos->findGithubAppById(githubAppId);
  if (!app) {
    throw std::runtime_error("GitHub App not found");
  }
  return authSingleton().installationToken(*app);
}

}  // namespace highcube::admin
