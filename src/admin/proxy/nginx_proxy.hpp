#pragma once

#include "config/admin_config.hpp"
#include "db/repositories.hpp"
#include "domain/models.hpp"

#include <string>

namespace highcube::admin {

class NginxProxy {
public:
  NginxProxy(AdminConfig config, Repositories& repos);

  void renderAdminSite();
  void ensureHttpChallengeSite(const std::string& hostname);
  std::string renderApplicationSite(const Application& app);
  std::string acmeWebroot() const;
  void removeSite(const std::string& hostname);
  void removeApplicationSite(const Application& app);
  std::string reload();

private:
  std::string sitePath(const std::string& name) const;
  std::string applicationSitePath(int64_t applicationId) const;
  std::string basicAuthFilePath(int64_t applicationId) const;
  void writeFile(const std::string& path, const std::string& content) const;
  void syncBasicAuthFile(const Application& app) const;
  void appendProxyLocation(std::ostringstream& oss, const Application& app) const;
  void appendAcmeChallenge(std::ostringstream& oss) const;
  void appendServerBlocks(std::ostringstream& oss, const Application& app, int backends) const;

  AdminConfig config_;
  Repositories& repos_;
};

}  // namespace highcube::admin
