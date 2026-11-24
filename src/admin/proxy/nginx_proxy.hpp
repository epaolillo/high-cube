#pragma once

#include "config/admin_config.hpp"
#include "db/repositories.hpp"
#include "domain/models.hpp"

#include <functional>
#include <string>

namespace highcube::admin {

class NginxProxy {
public:
  // Address nginx should use to reach a remote server, or empty for server.ip.
  using AddressResolver = std::function<std::string(const Server&)>;

  NginxProxy(AdminConfig config, Repositories& repos);

  void setAddressResolver(AddressResolver resolver) { addressResolver_ = std::move(resolver); }
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

  std::string backendAddress(const Server& server) const;

  AdminConfig config_;
  Repositories& repos_;
  AddressResolver addressResolver_;
};

}  // namespace highcube::admin
