#include "db/database.hpp"

#include <filesystem>

namespace highcube::admin {

Database::Database(const std::string& path) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  db_ = std::make_unique<SQLite::Database>(
      path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
  db_->exec("PRAGMA journal_mode=WAL;");
  db_->exec("PRAGMA foreign_keys=ON;");
  db_->exec("PRAGMA busy_timeout=5000;");
}

SQLite::Database& Database::db() {
  return *db_;
}

void Database::execute(const std::string& sql) {
  db_->exec(sql);
}

}  // namespace highcube::admin
