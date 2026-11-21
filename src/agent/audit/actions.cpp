#include "audit/audit_engine.hpp"
#include "audit/command_runner.hpp"
#include "audit/disk_support.hpp"
#include "metrics/process_snapshot.hpp"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace highcube::agent::audit {
namespace {

long long memAvailableBytes() {
  std::istringstream in(readFile("/proc/meminfo"));
  std::string key;
  long long value = 0;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    fields >> key >> value;
    if (key == "MemAvailable:") return value * 1024;
  }
  return 0;
}

std::string tail(const std::string& text, std::size_t max = 4000) {
  return text.size() <= max ? text : text.substr(text.size() - max);
}

json diskResult(const std::string& path, long long before, const CommandOutput& out) {
  const auto after = availableBytes(path);
  return {{"ok", out.code == 0},
          {"metric", "diskAvailable"},
          {"before", before},
          {"after", after},
          {"freedBytes", std::max(0LL, after - before)},
          {"output", tail(trim(out.out))}};
}

json runDocker(const std::vector<std::string>& args) {
  if (!commandExists("docker")) throw std::invalid_argument("Docker is not installed");
  const std::string path = fs::exists("/var/lib/docker") ? "/var/lib/docker" : "/";
  const auto before = availableBytes(path);
  auto argv = std::vector<std::string>{"docker"};
  argv.insert(argv.end(), args.begin(), args.end());
  return diskResult(path, before, runCommand(argv, 600, true));
}

bool isContainerId(const std::string& value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)); });
}

bool isContainerName(const std::string& value) {
  static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$");
  return std::regex_match(value, pattern);
}

bool isVolumeName(const std::string& value) {
  if (value.empty() || value.size() > 255) return false;
  if (!std::isalnum(static_cast<unsigned char>(value.front()))) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) || ch == '_' || ch == '.' || ch == '-';
  });
}

bool isDanglingVolume(const std::string& name) {
  const auto listed = runCommand({"docker", "volume", "ls", "-qf", "dangling=true"}, 60);
  for (const auto& line : splitLines(listed.out)) {
    if (trim(line) == name) return true;
  }
  return false;
}

std::string trimSlash(std::string path) {
  while (path.size() > 1 && (path.back() == '/' || path.back() == '\r')) path.pop_back();
  return path;
}

bool deviceUsesVolume(const std::string& device, const std::string& mountpoint) {
  if (device.empty() || mountpoint.empty()) return false;
  if (device == mountpoint) return true;
  const auto root = mountpoint.size() > 6 && mountpoint.ends_with("/_data")
                        ? mountpoint.substr(0, mountpoint.size() - 6)
                        : mountpoint;
  return device == root || device.rfind(root + "/", 0) == 0;
}

// True when some other volume bind-mounts this volume's folder. docker volume rm would
// delete those files even though Docker reports this volume as dangling.
bool anotherVolumeUses(const std::string& name) {
  const auto own = json::parse(runCommand({"docker", "volume", "inspect", name}, 60).out, nullptr, false);
  if (!own.is_array() || own.empty() || !own[0].is_object()) {
    throw std::invalid_argument("Could not inspect the volume");
  }
  const auto mountpoint = trimSlash(own[0].value("Mountpoint", std::string{}));
  if (mountpoint.empty()) throw std::invalid_argument("Could not inspect the volume");
  std::vector<std::string> args = {"docker", "volume", "inspect"};
  for (const auto& line : splitLines(runCommand({"docker", "volume", "ls", "-q"}, 60).out)) {
    const auto other = trim(line);
    if (other.empty() || other == name || !isVolumeName(other)) continue;
    args.push_back(other);
  }
  if (args.size() == 2) return false;
  const auto all = json::parse(runCommand(args, 60).out, nullptr, false);
  if (!all.is_array()) throw std::invalid_argument("Could not inspect the volume");
  for (const auto& item : all) {
    if (!item.is_object() || !item.contains("Options") || !item["Options"].is_object()) continue;
    if (deviceUsesVolume(trimSlash(item["Options"].value("device", std::string{})), mountpoint)) return true;
  }
  return false;
}

}  // namespace

json runAction(const std::string& actionId, const json& params) {
  const auto target = params.value("target", json::object());

  if (actionId == "docker_images_prune") return runDocker({"image", "prune", "-af"});
  if (actionId == "docker_builder_prune") return runDocker({"builder", "prune", "-af"});
  if (actionId == "docker_containers_prune") return runDocker({"container", "prune", "-f"});
  if (actionId == "docker_volumes_prune") return runDocker({"volume", "prune", "-f"});
  if (actionId == "docker_volume_rm" || actionId == "docker_volume_unlink") {
    const auto name = target.is_object() ? target.value("name", std::string{}) : std::string{};
    if (!isVolumeName(name)) throw std::invalid_argument("Invalid volume name");
    // A volume attached while the report was open must not be removed.
    if (!isDanglingVolume(name)) throw std::invalid_argument("Volume is in use or does not exist");
    // A dangling name can still be the folder where another volume keeps its files.
    if (anotherVolumeUses(name)) {
      throw std::invalid_argument("Another volume stores its data in this volume");
    }
    return runDocker({"volume", "rm", name});
  }

  if (actionId == "container_log_truncate") {
    const auto id = target.is_object() ? target.value("id", std::string{}) : std::string{};
    if (!isContainerId(id)) throw std::invalid_argument("Invalid container id");
    const auto file = "/var/lib/docker/containers/" + id + "/" + id + "-json.log";
    if (!fs::exists(file)) throw std::invalid_argument("Container log not found");
    const auto before = availableBytes("/var/lib/docker");
    std::error_code ec;
    fs::resize_file(file, 0, ec);
    CommandOutput out;
    out.code = ec ? 1 : 0;
    out.out = ec ? ec.message() : "";
    return diskResult("/var/lib/docker", before, out);
  }

  if (actionId == "tmp_clean") {
    const auto before = availableBytes("/tmp");
    const auto out = runCommand({"find", "/tmp", "/var/tmp", "-xdev", "-mindepth", "1", "-type", "f", "-mtime", "+7",
                                 "-delete"},
                                300);
    return diskResult("/tmp", before, out);
  }

  if (actionId == "pkg_cache_clean") {
    const auto before = availableBytes("/var/cache");
    CommandOutput out;
    if (commandExists("apt-get")) out = runCommand({"apt-get", "clean"}, 300);
    else if (commandExists("dnf")) out = runCommand({"dnf", "clean", "all"}, 300);
    else if (commandExists("yum")) out = runCommand({"yum", "clean", "all"}, 300);
    else throw std::invalid_argument("No supported package manager");
    return diskResult("/var/cache", before, out);
  }

  if (actionId == "pkg_autoremove") {
    if (!commandExists("apt-get")) throw std::invalid_argument("apt-get is not available");
    const auto before = availableBytes("/");
    const auto out = runCommand({"apt-get", "-y", "autoremove", "--purge"}, 900, true, 8 * 1024 * 1024,
                                {{"DEBIAN_FRONTEND", "noninteractive"}});
    return diskResult("/", before, out);
  }

  if (actionId == "journal_vacuum") {
    if (!commandExists("journalctl")) throw std::invalid_argument("journalctl is not available");
    const auto before = availableBytes("/var/log");
    return diskResult("/var/log", before, runCommand({"journalctl", "--vacuum-size=200M"}, 300));
  }

  if (actionId == "rotated_logs_clean") {
    const auto before = availableBytes("/var/log");
    auto args = std::vector<std::string>{"find", "/var/log", "-xdev", "-type", "f"};
    const auto predicate = rotatedLogsPredicate();
    args.insert(args.end(), predicate.begin(), predicate.end());
    args.push_back("-delete");
    return diskResult("/var/log", before, runCommand(args, 300));
  }

  if (actionId == "user_caches_clean") {
    const auto before = availableBytes("/home");
    std::ostringstream log;
    int failures = 0;
    for (const auto& home : homeDirectories()) {
      for (const auto& relative : userCacheDirectories()) {
        const fs::path dir = fs::path(home) / relative;
        std::error_code ec;
        if (!fs::is_directory(dir, ec) || fs::is_symlink(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
          std::error_code removeEc;
          fs::remove_all(entry.path(), removeEc);
          if (removeEc) ++failures;
        }
        log << "cleaned " << dir.string() << "\n";
      }
    }
    CommandOutput out;
    out.code = failures == 0 ? 0 : 1;
    out.out = log.str();
    return diskResult("/home", before, out);
  }

  if (actionId == "restart_container") {
    const auto name = target.is_object() ? target.value("name", std::string{}) : std::string{};
    if (!isContainerName(name)) throw std::invalid_argument("Invalid container name");
    if (!commandExists("docker")) throw std::invalid_argument("Docker is not installed");
    const auto out = runCommand({"docker", "restart", name}, 180, false);
    return {{"ok", out.code == 0}, {"metric", "none"}, {"output", tail(trim(out.out))}};
  }

  if (actionId == "terminate_process") {
    if (!target.is_object()) throw std::invalid_argument("Missing process");
    const int pid = target.value("pid", 0);
    const auto start = target.value("start", 0ULL);
    if (pid <= 1 || pid == ::getpid() || pid == ::getppid()) {
      throw std::invalid_argument("This process cannot be terminated");
    }
    if (start == 0 || processStartTicks(pid) != start) {
      throw std::invalid_argument("The process is no longer running");
    }
    const auto before = memAvailableBytes();
    const bool sent = ::kill(pid, SIGTERM) == 0;
    ::sleep(3);
    const bool alive = processStartTicks(pid) == start;
    const auto after = memAvailableBytes();
    return {{"ok", sent},
            {"metric", "memoryAvailable"},
            {"before", before},
            {"after", after},
            {"freedBytes", std::max(0LL, after - before)},
            {"output", alive ? "SIGTERM sent; the process is still shutting down" : "Process terminated"}};
  }

  if (actionId == "drop_page_cache") {
    const auto before = memAvailableBytes();
    ::sync();
    std::ofstream out("/proc/sys/vm/drop_caches");
    out << "1";
    out.close();
    const auto after = memAvailableBytes();
    return {{"ok", static_cast<bool>(out)},
            {"metric", "memoryAvailable"},
            {"before", before},
            {"after", after},
            {"freedBytes", std::max(0LL, after - before)},
            {"output", ""}};
  }

  throw std::invalid_argument("Unknown action");
}

}  // namespace highcube::agent::audit
