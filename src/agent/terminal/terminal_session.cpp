#include "terminal/terminal_session.hpp"

#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace highcube::agent {

void TerminalSession::openHost(const std::string& command, TerminalOutputFn onOutput, unsigned cols,
                               unsigned rows) {
  close();
  winsize size{};
  size.ws_col = static_cast<unsigned short>(cols == 0 ? 120 : cols);
  size.ws_row = static_cast<unsigned short>(rows == 0 ? 32 : rows);
  const pid_t child = forkpty(&masterFd_, nullptr, nullptr, &size);
  if (child < 0) {
    masterFd_ = -1;
    return;
  }
  if (child == 0) {
    // systemd services inherit TERM=unknown, which makes htop and other TUIs fail.
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    if (getenv("LANG") == nullptr) {
      setenv("LANG", "C.UTF-8", 0);
    }
    const bool interactiveShell = command.empty() || command == "/bin/bash" || command == "bash";
    if (interactiveShell) {
      execl("/bin/bash", "-bash", nullptr);
    } else {
      execl("/bin/bash", "bash", "-lc", command.c_str(), nullptr);
    }
    _exit(127);
  }
  pid_ = child;
  running_ = true;
  const int fd = masterFd_;
  reader_ = std::thread([this, fd, onOutput]() {
    char buffer[4096];
    while (running_) {
      const ssize_t n = ::read(fd, buffer, sizeof(buffer));
      if (n <= 0) {
        break;
      }
      onOutput(std::string(buffer, static_cast<size_t>(n)));
    }
  });
}

void TerminalSession::openContainer(const std::string& containerId, const std::string& command,
                                    TerminalOutputFn onOutput, unsigned cols, unsigned rows) {
  const std::string cmd = "docker exec -it -e TERM=xterm-256color " + containerId + " " +
                          (command.empty() ? "/bin/sh" : command);
  openHost(cmd, std::move(onOutput), cols, rows);
}

void TerminalSession::openLogs(const std::string& containerId, unsigned tailLines,
                               TerminalOutputFn onOutput) {
  const std::string cmd =
      "docker logs -f --tail " + std::to_string(tailLines) + " " + containerId;
  openHost(cmd, std::move(onOutput), 200, 50);
}

void TerminalSession::write(const std::string& data) {
  std::lock_guard lock(mutex_);
  if (masterFd_ >= 0) {
    const ssize_t written = ::write(masterFd_, data.data(), data.size());
    if (written < 0) {
      return;
    }
  }
}

void TerminalSession::resize(unsigned cols, unsigned rows) {
  std::lock_guard lock(mutex_);
  if (masterFd_ < 0 || cols == 0 || rows == 0) {
    return;
  }
  winsize size{};
  size.ws_col = static_cast<unsigned short>(cols);
  size.ws_row = static_cast<unsigned short>(rows);
  ioctl(masterFd_, TIOCSWINSZ, &size);
}

void TerminalSession::close() {
  running_ = false;
  int fd = -1;
  int child = -1;
  {
    std::lock_guard lock(mutex_);
    fd = masterFd_;
    masterFd_ = -1;
    child = pid_;
    pid_ = -1;
  }
  if (fd >= 0) {
    ::close(fd);
  }
  if (child > 0) {
    kill(child, SIGHUP);
    waitpid(child, nullptr, 0);
  }
  if (reader_.joinable()) {
    reader_.join();
  }
}

}  // namespace highcube::agent
