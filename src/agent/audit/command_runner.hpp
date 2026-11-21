#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace highcube::agent::audit {

struct CommandOutput {
  int code = -1;  // exit status, 124 on timeout, -1 when the program could not start
  bool timedOut = false;
  std::string out;
};

// Runs argv directly (no shell) with a hard timeout. lowPriority wraps the call with
// nice/ionice so an audit never competes with the workloads it inspects.
// stderr is discarded; stdout is capped at maxBytes.
CommandOutput runCommand(const std::vector<std::string>& argv, int timeoutSeconds,
                         bool lowPriority = true, std::size_t maxBytes = 8 * 1024 * 1024,
                         const std::map<std::string, std::string>& extraEnv = {});

bool commandExists(const std::string& name);

std::vector<std::string> splitLines(const std::string& text);
std::vector<std::string> splitTabs(const std::string& line);
std::string trim(const std::string& value);
std::string readFile(const std::string& path);

// "1.5GB", "512kB", "1.2G", "300 MiB" -> bytes. base1000 follows Docker's units.
long long parseHumanSize(const std::string& text, bool base1000);

}  // namespace highcube::agent::audit
