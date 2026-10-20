#pragma once

#include <SQLiteCpp/SQLiteCpp.h>

#include <memory>
#include <string>

namespace highcube::admin {

class Database {
public:
  explicit Database(const std::string& path);

  SQLite::Database& db();
  void execute(const std::string& sql);

private:
  std::unique_ptr<SQLite::Database> db_;
};

}  // namespace highcube::admin
