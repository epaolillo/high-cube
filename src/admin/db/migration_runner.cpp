#include "db/migration_runner.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace highcube::admin {

MigrationRunner::MigrationRunner(Database& database) : database_(database) {}

void MigrationRunner::applyAll(const std::string& migrationsDir) {
  database_.execute(
      "CREATE TABLE IF NOT EXISTS schema_migrations ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "filename TEXT NOT NULL UNIQUE,"
      "applied_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))"
      ");");

  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(migrationsDir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".sql") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());

  for (const auto& file : files) {
    const std::string filename = file.filename().string();
    SQLite::Statement check(database_.db(),
                            "SELECT 1 FROM schema_migrations WHERE filename = ?");
    check.bind(1, filename);
    if (check.executeStep()) {
      continue;
    }

    std::ifstream in(file);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string sql = buffer.str();

    spdlog::info("applying migration {}", filename);
    SQLite::Transaction tx(database_.db());
    database_.execute(sql);
    SQLite::Statement insert(database_.db(),
                             "INSERT INTO schema_migrations (filename) VALUES (?)");
    insert.bind(1, filename);
    insert.exec();
    tx.commit();
  }
}

}  // namespace highcube::admin
