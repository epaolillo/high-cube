#pragma once

#include "config/admin_config.hpp"
#include "db/repositories.hpp"

#include "domain/models.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace highcube::admin {

class NginxProxy;

struct CertificateJobView {
  std::string jobId;
  std::string hostname;
  std::string status;
  std::vector<std::string> lines;
  int exitCode = -1;
};

class CertbotService {
public:
  CertbotService(AdminConfig config, Repositories& repos, NginxProxy& proxy);
  ~CertbotService();

  void requestCertificate(int64_t applicationId, const std::string& hostname);
  CertificateJobView startCertificateJob(const Application& app, const std::string& hostname);
  CertificateJobView pollCertificateJob(int64_t applicationId, const std::string& jobId);
  void renewAll();

private:
  struct Job {
    std::string id;
    int64_t applicationId = 0;
    std::string hostname;
    std::string status = "running";
    std::vector<std::string> lines;
    int exitCode = -1;
    bool finished = false;
    bool finalized = false;
    std::mutex mu;
    std::thread worker;
  };

  int run(const std::vector<std::string>& args,
          const std::function<void(const std::string&)>& onLine) const;
  void appendLine(const std::shared_ptr<Job>& job, const std::string& line) const;
  void appendBlock(const std::shared_ptr<Job>& job, const std::string& text) const;
  CertificateJobView viewOf(const std::shared_ptr<Job>& job) const;
  void finalizeJob(const std::shared_ptr<Job>& job);

  AdminConfig config_;
  Repositories& repos_;
  NginxProxy& proxy_;
  std::mutex jobsMu_;
  std::vector<std::shared_ptr<Job>> jobs_;
};

}  // namespace highcube::admin
