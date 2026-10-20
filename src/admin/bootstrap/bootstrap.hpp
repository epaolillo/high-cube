#pragma once

#include "config/admin_config.hpp"
#include "db/repositories.hpp"

#include <string>

namespace highcube::admin {

class Bootstrap {
public:
  Bootstrap(AdminConfig config, Repositories& repos);

  // Interactive first-run: create admin user if needed, install systemd unit, enable service.
  int runInstall();

  // Service mode guard: refuse to start when no users exist.
  void assertReadyForService() const;

private:
  void ensureLinuxRoot() const;
  void promptFirstAdmin();
  void writeSystemdUnit() const;

  AdminConfig config_;
  Repositories& repos_;
};

}  // namespace highcube::admin
