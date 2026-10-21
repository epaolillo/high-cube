#pragma once

#include <string>

namespace highcube::agent {

struct DbManageOutput {
  bool ok = false;
  std::string error;
  std::string payloadJson = "{}";
};

// Runs one structured administration operation inside a MySQL or PostgreSQL container.
// The payload selects an operation; SQL is built here and identifiers are validated again.
DbManageOutput manageDatabase(const std::string& containerId, const std::string& payloadJson);

}  // namespace highcube::agent
