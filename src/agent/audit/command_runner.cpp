#include "audit/command_runner.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

extern char** environ;

namespace highcube::agent::audit {

bool commandExists(const std::string& name) {
  const char* path = std::getenv("PATH");
  std::string dirs = path ? path : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
  std::stringstream stream(dirs);
  std::string dir;
  while (std::getline(stream, dir, ':')) {
    if (dir.empty()) {
      continue;
    }
    const auto candidate = dir + "/" + name;
    if (::access(candidate.c_str(), X_OK) == 0) {
      return true;
    }
  }
  return false;
}

CommandOutput runCommand(const std::vector<std::string>& argvIn, int timeoutSeconds,
                         bool lowPriority, std::size_t maxBytes,
                         const std::map<std::string, std::string>& extraEnv) {
  CommandOutput result;
  if (argvIn.empty()) {
    return result;
  }
  static const bool hasNice = commandExists("nice");
  static const bool hasIonice = commandExists("ionice");

  std::vector<std::string> args;
  if (lowPriority && hasNice) {
    args.insert(args.end(), {"nice", "-n", "19"});
  }
  if (lowPriority && hasIonice) {
    args.insert(args.end(), {"ionice", "-c3"});
  }
  args.insert(args.end(), argvIn.begin(), argvIn.end());

  // Everything the child needs is prepared before fork: only async-signal-safe
  // calls are allowed between fork and exec in a multithreaded process.
  std::vector<char*> argv;
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  std::vector<std::string> envStrings;
  for (char** entry = environ; entry && *entry; ++entry) {
    const std::string value(*entry);
    const auto eq = value.find('=');
    if (eq != std::string::npos && extraEnv.count(value.substr(0, eq))) {
      continue;
    }
    envStrings.push_back(value);
  }
  envStrings.push_back("LC_ALL=C");
  for (const auto& [key, value] : extraEnv) {
    envStrings.push_back(key + "=" + value);
  }
  std::vector<char*> envp;
  for (auto& entry : envStrings) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);

  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    return result;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return result;
  }
  if (pid == 0) {
    ::setpgid(0, 0);
    const int devNull = ::open("/dev/null", O_RDWR);
    if (devNull >= 0) {
      ::dup2(devNull, STDIN_FILENO);
      ::dup2(devNull, STDERR_FILENO);
    }
    ::dup2(fds[1], STDOUT_FILENO);
    ::execvpe(argv[0], argv.data(), envp.data());
    ::_exit(127);
  }
  ::close(fds[1]);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
  char buffer[16384];
  bool truncated = false;
  while (true) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0) {
      result.timedOut = true;
      break;
    }
    pollfd pfd{fds[0], POLLIN, 0};
    const int ready = ::poll(&pfd, 1, static_cast<int>(std::min<long long>(remaining, 500)));
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (ready == 0) {
      continue;
    }
    const ssize_t n = ::read(fds[0], buffer, sizeof(buffer));
    if (n <= 0) {
      break;
    }
    if (!truncated) {
      const auto room = maxBytes > result.out.size() ? maxBytes - result.out.size() : 0;
      result.out.append(buffer, std::min<std::size_t>(room, static_cast<std::size_t>(n)));
      truncated = result.out.size() >= maxBytes;
    }
  }
  ::close(fds[0]);
  if (result.timedOut) {
    ::kill(-pid, SIGKILL);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  if (result.timedOut) {
    result.code = 124;
  } else if (WIFEXITED(status)) {
    result.code = WEXITSTATUS(status);
  } else {
    result.code = -1;
  }
  return result;
}

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::stringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }
  return lines;
}

std::vector<std::string> splitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  std::stringstream stream(line);
  while (std::getline(stream, field, '\t')) {
    fields.push_back(field);
  }
  return fields;
}

std::string trim(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return "";
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

long long parseHumanSize(const std::string& text, bool base1000) {
  const auto value = trim(text);
  std::size_t index = 0;
  while (index < value.size() &&
         (std::isdigit(static_cast<unsigned char>(value[index])) || value[index] == '.')) {
    ++index;
  }
  if (index == 0) {
    return 0;
  }
  double number = 0;
  try {
    number = std::stod(value.substr(0, index));
  } catch (const std::exception&) {
    return 0;
  }
  std::string unit;
  for (std::size_t i = index; i < value.size(); ++i) {
    const char ch = value[i];
    if (std::isalpha(static_cast<unsigned char>(ch))) {
      unit.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    } else if (!unit.empty()) {
      break;
    }
  }
  const double base = (unit.find('I') != std::string::npos || !base1000) ? 1024.0 : 1000.0;
  double multiplier = 1;
  if (!unit.empty()) {
    switch (unit[0]) {
      case 'K': multiplier = base; break;
      case 'M': multiplier = base * base; break;
      case 'G': multiplier = base * base * base; break;
      case 'T': multiplier = base * base * base * base; break;
      case 'P': multiplier = base * base * base * base * base; break;
      default: multiplier = 1; break;
    }
  }
  return static_cast<long long>(number * multiplier);
}

}  // namespace highcube::agent::audit
