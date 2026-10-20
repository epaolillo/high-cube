#include "certificates/certbot_service.hpp"

#include "proxy/nginx_proxy.hpp"

#include <boost/process.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <sstream>
#include <unistd.h>

namespace bp = boost::process;

namespace highcube::admin {

CertbotService::CertbotService(AdminConfig config, Repositories& repos, NginxProxy& proxy)
    : config_(std::move(config)), repos_(repos), proxy_(proxy) {}

CertbotService::~CertbotService() {
  std::lock_guard<std::mutex> lock(jobsMu_);
  for (const auto& job : jobs_) {
    if (job->worker.joinable()) {
      job->worker.join();
    }
  }
}

void CertbotService::appendLine(const std::shared_ptr<Job>& job, const std::string& line) const {
  std::lock_guard<std::mutex> lock(job->mu);
  job->lines.push_back(line);
}

void CertbotService::appendBlock(const std::shared_ptr<Job>& job, const std::string& text) const {
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    appendLine(job, line);
  }
}

CertificateJobView CertbotService::viewOf(const std::shared_ptr<Job>& job) const {
  std::lock_guard<std::mutex> lock(job->mu);
  return CertificateJobView{job->id, job->hostname, job->status, job->lines, job->exitCode};
}

int CertbotService::run(const std::vector<std::string>& args,
                        const std::function<void(const std::string&)>& onLine) const {
  const auto certbot = bp::search_path("certbot");
  if (certbot.empty()) {
    onLine("certbot is not installed");
    return 127;
  }
  bp::ipstream pipe;
  bp::child child;
  try {
    if (geteuid() == 0) {
      child = bp::child(certbot, bp::args(args), (bp::std_out & bp::std_err) > pipe);
    } else {
      const auto sudoBin = bp::search_path("sudo");
      if (sudoBin.empty()) {
        onLine("certbot needs root and sudo is not available");
        return 127;
      }
      std::vector<std::string> command = {"-n", certbot.string()};
      command.insert(command.end(), args.begin(), args.end());
      child = bp::child(sudoBin, bp::args(command), (bp::std_out & bp::std_err) > pipe);
    }
  } catch (const std::exception& ex) {
    onLine(std::string("certbot failed to start: ") + ex.what());
    return 1;
  }

  std::string line;
  while (std::getline(pipe, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    onLine(line);
  }
  child.wait();
  const int code = child.exit_code();
  if (code != 0) {
    spdlog::error("certbot exited {}", code);
  }
  return code;
}

void CertbotService::requestCertificate(int64_t applicationId, const std::string& hostname) {
  repos_.setHostnameTlsStatus(applicationId, hostname, "pending");
  if (config_.certbotEmail.empty()) {
    spdlog::warn("certbot_email is empty; marking {} as failed", hostname);
    repos_.setHostnameTlsStatus(applicationId, hostname, "failed");
    return;
  }
  try {
    const auto webroot = proxy_.acmeWebroot();
    std::filesystem::create_directories(webroot);
    const int code = run({"certonly", "--webroot", "-w", webroot, "-n", "--agree-tos",
                          "--keep-until-expiring", "-m", config_.certbotEmail, "-d", hostname},
                         [](const std::string& line) { spdlog::info("certbot: {}", line); });
    if (code == 0) {
      repos_.setHostnameTlsStatus(applicationId, hostname, "active");
      auto app = repos_.findApplicationById(applicationId);
      if (app) {
        proxy_.renderApplicationSite(*app);
      }
    } else {
      repos_.setHostnameTlsStatus(applicationId, hostname, "failed");
    }
  } catch (const std::exception& ex) {
    spdlog::error("certbot failed for {}: {}", hostname, ex.what());
    repos_.setHostnameTlsStatus(applicationId, hostname, "failed");
  }
}

void CertbotService::finalizeJob(const std::shared_ptr<Job>& job) {
  if (job->worker.joinable()) {
    job->worker.join();
  }
  const int code = job->exitCode;
  appendLine(job, "certbot exit code " + std::to_string(code));
  try {
    if (code == 0) {
      repos_.setHostnameTlsStatus(job->applicationId, job->hostname, "active");
      appendLine(job, "TLS status set to active for " + job->hostname);
      auto app = repos_.findApplicationById(job->applicationId);
      if (!app) {
        appendLine(job, "Application disappeared before nginx could be updated");
        std::lock_guard<std::mutex> lock(job->mu);
        job->status = "failed";
        return;
      }
      appendLine(job, "Installing the certificate in nginx");
      const auto rendered = proxy_.renderApplicationSite(*app);
      appendBlock(job, rendered);
      const bool reloaded = rendered.find(" -s reload ok") != std::string::npos ||
                            rendered.find("nginx reload ok") != std::string::npos;
      std::lock_guard<std::mutex> lock(job->mu);
      job->status = reloaded ? "active" : "failed";
      if (reloaded) {
        job->lines.push_back("Certificate is active for " + job->hostname);
        job->lines.push_back("Files: /etc/letsencrypt/live/" + job->hostname + "/fullchain.pem");
      } else {
        job->lines.push_back(
            "Certificate was issued, but nginx did not reload. The site is still on HTTP.");
      }
      return;
    }
    repos_.setHostnameTlsStatus(job->applicationId, job->hostname, "failed");
    appendLine(job, "TLS status set to failed for " + job->hostname);
    appendLine(job, "Port 80 stays in place so this hostname can be retried.");
  } catch (const std::exception& ex) {
    appendLine(job, std::string("Failed while saving the result: ") + ex.what());
    try {
      repos_.setHostnameTlsStatus(job->applicationId, job->hostname, "failed");
    } catch (const std::exception& writeError) {
      appendLine(job, std::string("Could not store TLS status: ") + writeError.what());
    }
  }
  std::lock_guard<std::mutex> lock(job->mu);
  job->status = "failed";
}

CertificateJobView CertbotService::startCertificateJob(const Application& app,
                                                       const std::string& hostname) {
  std::shared_ptr<Job> job;
  {
    std::lock_guard<std::mutex> lock(jobsMu_);
    for (auto it = jobs_.begin(); it != jobs_.end();) {
      const auto& existing = *it;
      if (existing->applicationId != app.id || existing->hostname != hostname) {
        ++it;
        continue;
      }
      bool stillOpen = false;
      {
        std::lock_guard<std::mutex> jobLock(existing->mu);
        stillOpen = !existing->finalized;
      }
      if (stillOpen) {
        return viewOf(existing);
      }
      if (existing->worker.joinable()) {
        existing->worker.join();
      }
      it = jobs_.erase(it);
    }

    job = std::make_shared<Job>();
    job->applicationId = app.id;
    job->hostname = hostname;
    job->id = std::to_string(app.id) + "-" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    jobs_.push_back(job);
  }

  appendLine(job, "Certificate request for " + hostname);
  appendLine(job, "Application " + std::to_string(app.id) + " (" + app.name + ")");
  std::string previous = "unknown";
  for (const auto& row : app.hostnames) {
    if (row.hostname == hostname) {
      previous = row.tlsStatus;
    }
  }
  appendLine(job, "Previous TLS status: " + previous);

  repos_.setHostnameTlsStatus(app.id, hostname, "pending");
  appendLine(job, "TLS status set to pending");

  auto pendingApp = repos_.findApplicationById(app.id);
  if (!pendingApp) {
    appendLine(job, "Application not found");
    {
      std::lock_guard<std::mutex> lock(job->mu);
      job->status = "failed";
      job->finished = true;
      job->finalized = true;
      job->exitCode = 1;
    }
    return viewOf(job);
  }
  appendLine(job, "Publishing the port 80 server block before the HTTP-01 challenge");
  appendBlock(job, proxy_.renderApplicationSite(*pendingApp));

  if (config_.certbotEmail.empty()) {
    appendLine(job, "certbot_email is empty in hc-admin.conf");
    appendLine(job, "Let's Encrypt requires an email. Set certbot_email and deploy again.");
    repos_.setHostnameTlsStatus(app.id, hostname, "failed");
    appendLine(job, "TLS status set to failed");
    {
      std::lock_guard<std::mutex> lock(job->mu);
      job->status = "failed";
      job->finished = true;
      job->finalized = true;
      job->exitCode = 1;
    }
    return viewOf(job);
  }

  const auto webroot = proxy_.acmeWebroot();
  std::error_code ec;
  std::filesystem::create_directories(webroot, ec);
  if (ec) {
    appendLine(job, "Could not create ACME webroot " + webroot + ": " + ec.message());
  } else {
    appendLine(job, "ACME webroot: " + webroot);
  }
  appendLine(job, "Challenge URL: http://" + hostname + "/.well-known/acme-challenge/<token>");
  appendLine(job, "Certificate email: " + config_.certbotEmail);

  const std::vector<std::string> args = {"certonly",
                                         "--webroot",
                                         "-w",
                                         webroot,
                                         "-n",
                                         "--agree-tos",
                                         "--keep-until-expiring",
                                         "-m",
                                         config_.certbotEmail,
                                         "-d",
                                         hostname};
  std::ostringstream command;
  if (geteuid() != 0) {
    command << "sudo -n ";
  }
  command << "certbot";
  for (const auto& arg : args) {
    command << " " << arg;
  }
  appendLine(job, "Command: " + command.str());
  appendLine(job, "Waiting for certbot. Let's Encrypt will request the challenge over port 80.");

  std::weak_ptr<Job> weak = job;
  job->worker = std::thread([this, args, weak]() {
    const int code = run(args, [weak](const std::string& line) {
      if (auto live = weak.lock()) {
        std::lock_guard<std::mutex> lock(live->mu);
        live->lines.push_back(line);
      }
    });
    if (auto live = weak.lock()) {
      std::lock_guard<std::mutex> lock(live->mu);
      live->exitCode = code;
      live->finished = true;
    }
  });

  return viewOf(job);
}

CertificateJobView CertbotService::pollCertificateJob(int64_t applicationId, const std::string& jobId) {
  std::shared_ptr<Job> job;
  {
    std::lock_guard<std::mutex> lock(jobsMu_);
    for (const auto& candidate : jobs_) {
      if (candidate->id == jobId && candidate->applicationId == applicationId) {
        job = candidate;
        break;
      }
    }
  }
  if (!job) {
    throw std::invalid_argument("certificate job not found");
  }

  bool shouldFinalize = false;
  {
    std::lock_guard<std::mutex> lock(job->mu);
    if (job->finished && !job->finalized) {
      job->finalized = true;
      shouldFinalize = true;
    }
  }
  if (shouldFinalize) {
    finalizeJob(job);
  }
  return viewOf(job);
}

void CertbotService::renewAll() {
  try {
    if (run({"renew", "--quiet"}, [](const std::string& line) { spdlog::info("certbot: {}", line); }) !=
        0) {
      spdlog::warn("certbot renew failed");
    }
  } catch (const std::exception& ex) {
    spdlog::warn("certbot renew failed: {}", ex.what());
  }
}

}  // namespace highcube::admin
