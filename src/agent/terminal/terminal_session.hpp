#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace highcube::agent {

using TerminalOutputFn = std::function<void(const std::string& data)>;

class TerminalSession {
public:
  void openHost(const std::string& command, TerminalOutputFn onOutput, unsigned cols = 120,
                unsigned rows = 32);
  void openContainer(const std::string& containerId, const std::string& command,
                     TerminalOutputFn onOutput, unsigned cols = 120, unsigned rows = 32);
  // Follows the container output (stdout and stderr), starting with its last tailLines lines.
  void openLogs(const std::string& containerId, unsigned tailLines, TerminalOutputFn onOutput);
  void write(const std::string& data);
  void resize(unsigned cols, unsigned rows);
  void close();

private:
  std::mutex mutex_;
  std::thread reader_;
  int masterFd_ = -1;
  int pid_ = -1;
  bool running_ = false;
};

}  // namespace highcube::agent
