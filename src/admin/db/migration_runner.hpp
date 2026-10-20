#pragma once

#include "db/database.hpp"

#include <string>

namespace highcube::admin {

class MigrationRunner {
public:
  explicit MigrationRunner(Database& database);

  void applyAll(const std::string& migrationsDir);

private:
  Database& database_;
};

}  // namespace highcube::admin
