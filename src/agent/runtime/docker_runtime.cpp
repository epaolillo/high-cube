#include "runtime/docker_runtime.hpp"

#include "runtime/git_checkout.hpp"

#include <boost/process.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace bp = boost::process;

namespace highcube::agent {
namespace {

struct VolumeCacheEntry {
  nlohmann::json payload;
  std::chrono::steady_clock::time_point expiresAt;
};

std::mutex volumeCacheMutex;
std::unordered_map<std::string, VolumeCacheEntry> volumeCache;

// Databases need time to flush and close their files; SIGKILL can leave MyISAM tables crashed.
constexpr int kVolumeStopTimeoutSeconds = 120;

double parsePercent(const std::string& value) {
  try {
    return std::stod(value);
  } catch (const std::exception&) {
    return 0.0;
  }
}

// Docker MemUsage looks like "12.3MiB / 1.5GiB"
int64_t parseByteSize(std::string value) {
  while (!value.empty() && value.front() == ' ') {
    value.erase(value.begin());
  }
  while (!value.empty() && value.back() == ' ') {
    value.pop_back();
  }
  if (value.empty()) {
    return 0;
  }
  std::size_t idx = 0;
  while (idx < value.size() &&
         (std::isdigit(static_cast<unsigned char>(value[idx])) || value[idx] == '.')) {
    ++idx;
  }
  double number = 0;
  try {
    number = std::stod(value.substr(0, idx));
  } catch (const std::exception&) {
    return 0;
  }
  std::string unit = value.substr(idx);
  for (char& c : unit) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  double multiplier = 1;
  if (unit == "KB" || unit == "KIB") {
    multiplier = 1024.0;
  } else if (unit == "MB" || unit == "MIB") {
    multiplier = 1024.0 * 1024.0;
  } else if (unit == "GB" || unit == "GIB") {
    multiplier = 1024.0 * 1024.0 * 1024.0;
  } else if (unit == "TB" || unit == "TIB") {
    multiplier = 1024.0 * 1024.0 * 1024.0 * 1024.0;
  }
  return static_cast<int64_t>(number * multiplier);
}

std::string trimRight(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
    value.pop_back();
  }
  return value;
}

// Container names are hc-<application_id>-<replica_id>. Docker may prefix Name with '/'.
bool replicaIdFromContainerName(const std::string& name, int64_t& replicaId) {
  std::string bare = name;
  if (!bare.empty() && bare.front() == '/') {
    bare.erase(bare.begin());
  }
  if (bare.rfind("hc-", 0) != 0) {
    return false;
  }
  const auto lastDash = bare.rfind('-');
  if (lastDash == std::string::npos || lastDash <= 2) {
    return false;
  }
  try {
    std::size_t idx = 0;
    replicaId = std::stoll(bare.substr(lastDash + 1), &idx);
    return idx == bare.size() - lastDash - 1 && replicaId > 0;
  } catch (const std::exception&) {
    return false;
  }
}

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) {
    line = trimRight(std::move(line));
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
  }
  return lines;
}

// Docker repository component: [a-z0-9]+(?:(?:[._]|__|-+)[a-z0-9]+)*
std::string sanitizeImageRepository(const std::string& repoUrl, int64_t applicationId) {
  std::string url = repoUrl;
  const auto query = url.find('?');
  if (query != std::string::npos) {
    url.resize(query);
  }
  if (url.size() >= 4 && url.compare(url.size() - 4, 4, ".git") == 0) {
    url.resize(url.size() - 4);
  }
  while (!url.empty() && (url.back() == '/' || url.back() == ':')) {
    url.pop_back();
  }
  const auto slash = url.find_last_of("/:");
  const std::string raw = slash == std::string::npos ? url : url.substr(slash + 1);

  std::string name;
  bool pendingSeparator = false;
  for (unsigned char c : raw) {
    if (std::isupper(c)) {
      c = static_cast<unsigned char>(std::tolower(c));
    }
    if (std::isalnum(c)) {
      if (pendingSeparator && !name.empty()) {
        name.push_back('-');
      }
      pendingSeparator = false;
      name.push_back(static_cast<char>(c));
    } else if (!name.empty()) {
      pendingSeparator = true;
    }
  }
  if (name.size() > 200) {
    name.resize(200);
    while (!name.empty() && name.back() == '-') {
      name.pop_back();
    }
  }
  if (name.empty()) {
    name = "app-" + std::to_string(applicationId);
  }
  return name;
}

int64_t directorySizeBytes(const std::string& path) {
  bp::ipstream out;
  const int code =
      bp::system(bp::search_path("du"), "-sb", path, bp::std_out > out, bp::std_err > bp::null);
  if (code != 0) {
    return 0;
  }
  std::string line;
  std::getline(out, line);
  try {
    return std::stoll(line);
  } catch (const std::exception&) {
    return 0;
  }
}

// cpuPart is docker stats CPUPerc ("1.23%"), memPart its MemUsage ("12MiB / 1GiB").
std::string containerStatsPayload(std::string cpuPart, const std::string& memPart,
                                  int64_t diskBytes, int restartCount,
                                  const std::string& mountsRaw) {
  while (!cpuPart.empty() && cpuPart.back() == '%') {
    cpuPart.pop_back();
  }
  const auto slash = memPart.find('/');
  const std::string usedPart = slash == std::string::npos ? memPart : memPart.substr(0, slash);
  const std::string totalPart = slash == std::string::npos ? "" : memPart.substr(slash + 1);

  nlohmann::json volumes = nlohmann::json::array();
  const auto mounts = nlohmann::json::parse(mountsRaw, nullptr, false);
  if (mounts.is_array()) {
    const auto now = std::chrono::steady_clock::now();
    for (const auto& mount : mounts) {
      if (!mount.is_object() || mount.value("Type", "") != "volume") {
        continue;
      }
      const std::string volName = mount.value("Name", "");
      const std::string hostPath = mount.value("Source", "");
      nlohmann::json item = {{"name", volName},
                             {"mountPath", mount.value("Destination", "")},
                             {"dockerName", volName},
                             {"hostPath", hostPath},
                             {"sizeBytes", nullptr}};
      if (!hostPath.empty()) {
        std::lock_guard lock(volumeCacheMutex);
        auto it = volumeCache.find(volName);
        if (it != volumeCache.end() && it->second.expiresAt > now) {
          item = it->second.payload;
          item["mountPath"] = mount.value("Destination", "");
        } else {
          item["sizeBytes"] = directorySizeBytes(hostPath);
          volumeCache[volName] = VolumeCacheEntry{item, now + std::chrono::minutes(5)};
        }
      }
      volumes.push_back(item);
    }
  }

  nlohmann::json payload = {
      {"cpuPercent", parsePercent(cpuPart)},
      {"memoryUsedBytes", parseByteSize(usedPart)},
      {"memoryLimitBytes", parseByteSize(totalPart)},
      {"diskUsedBytes", diskBytes},
      {"restartCount", restartCount},
      {"volumes", volumes},
  };
  return payload.dump();
}

}  // namespace

DockerRuntime::DockerRuntime(std::string dataDir) : dataDir_(std::move(dataDir)) {}

void DockerRuntime::ensureNetwork() {
  if (dockerCode({"network", "inspect", "highcube"}) != 0) {
    docker({"network", "create", "--attachable", "highcube"});
  }
}

bool DockerRuntime::isHostPortFree(int port) const {
  if (port <= 0 || port > 65535) {
    return false;
  }
  bp::ipstream out;
  bp::system(bp::search_path("ss"), "-ltn", bp::std_out > out, bp::std_err > bp::null);
  std::string line;
  const std::string needle = ":" + std::to_string(port);
  while (std::getline(out, line)) {
    // Match :8080 as a listen address token (avoid matching :80800).
    const auto pos = line.find(needle);
    if (pos == std::string::npos) {
      continue;
    }
    const auto end = pos + needle.size();
    if (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) {
      continue;
    }
    return false;
  }
  return true;
}

void DockerRuntime::releaseApplicationHostPorts(int64_t applicationId,
                                                const std::vector<int>& hostPorts,
                                                const LogCallback& logCb) const {
  if (hostPorts.empty()) {
    return;
  }
  const std::string listing =
      docker({"ps", "--filter", "label=highcube.application=" + std::to_string(applicationId),
              "--format", "{{.ID}}\t{{.Names}}\t{{.Ports}}"});
  for (const auto& line : splitLines(listing)) {
    const auto first = line.find('\t');
    if (first == std::string::npos) {
      continue;
    }
    const auto second = line.find('\t', first + 1);
    const std::string id = line.substr(0, first);
    const std::string name = second == std::string::npos
                                 ? line.substr(first + 1)
                                 : line.substr(first + 1, second - first - 1);
    const std::string ports = second == std::string::npos ? std::string() : line.substr(second + 1);
    bool holds = false;
    for (const int port : hostPorts) {
      const std::string needle = ":" + std::to_string(port) + "->";
      if (ports.find(needle) != std::string::npos) {
        holds = true;
        break;
      }
    }
    if (!holds || id.empty()) {
      continue;
    }
    if (logCb) {
      logCb("deploy", "Stopping previous container " + name +
                          " so the configured host port can be published\n");
    }
    removeContainer(id);
  }
}

bool DockerRuntime::mountsVolume(const std::string& container) const {
  const auto mounts = nlohmann::json::parse(
      trimRight(docker({"container", "inspect", "--format", "{{json .Mounts}}", container})), nullptr,
      false);
  if (!mounts.is_array()) {
    return false;
  }
  for (const auto& mount : mounts) {
    if (mount.is_object() && mount.value("Type", "") == "volume") {
      return true;
    }
  }
  return false;
}

void DockerRuntime::removeContainer(const std::string& container) const {
  if (mountsVolume(container)) {
    dockerCode({"stop", "--time", std::to_string(kVolumeStopTimeoutSeconds), container});
  }
  dockerCode({"rm", "-f", container});
}

std::string DockerRuntime::docker(const std::vector<std::string>& args) const {
  bp::ipstream out;
  bp::child c(bp::search_path("docker"), bp::args(args), bp::std_out > out,
              bp::std_err > bp::null);
  // Drain before waiting: output larger than the pipe buffer would block docker forever.
  std::stringstream ss;
  ss << out.rdbuf();
  c.wait();
  return ss.str();
}

int DockerRuntime::dockerCode(const std::vector<std::string>& args) const {
  return bp::system(bp::search_path("docker"), bp::args(args), bp::std_out > bp::null,
                    bp::std_err > bp::null);
}

bool DockerRuntime::consumeCancelled(int64_t replicaId) const {
  std::lock_guard lock(cancelMutex_);
  return cancelledReplicas_.erase(replicaId) > 0;
}

int DockerRuntime::dockerStreaming(const std::vector<std::string>& args, const LogCallback& logCb,
                                   const std::string& phase) const {
  bp::ipstream out;
  // Merge stderr into stdout so docker build progress is captured without pipe deadlocks.
  bp::child c(bp::search_path("docker"), bp::args(args), (bp::std_out & bp::std_err) > out);
  std::string line;
  while (std::getline(out, line)) {
    if (logCb) {
      logCb(phase, line + "\n");
    }
  }
  c.wait();
  return c.exit_code();
}

void DockerRuntime::handleDeploy(const ::highcube::DeployReplica& order, StatusCallback cb,
                                 LogCallback logCb) {
  ::highcube::ContainerStatus status;
  status.set_replica_id(order.replica_id());
  status.set_desired_state("running");
  const std::string imagePrefix = "docker://";
  const bool pullImage = order.repo_url().rfind(imagePrefix, 0) == 0;
  status.set_status("building");
  status.set_message(pullImage ? "pulling image" : "cloning repository");
  cb(status);
  if (logCb && !pullImage) {
    logCb("deploy", "Cloning repository...\n");
  }

  try {
    ensureNetwork();
    std::string image;
    if (pullImage) {
      // Prebuilt app image: pull the registry reference and run it.
      image = order.repo_url().substr(imagePrefix.size());
      if (image.empty() || image.find(' ') != std::string::npos ||
          image.find('\n') != std::string::npos || image.find("..") != std::string::npos) {
        throw std::runtime_error("invalid image reference");
      }
      if (logCb) {
        logCb("deploy", "Pulling image " + image + "\n");
      }
      const int pullCode = dockerStreaming({"pull", image}, logCb, "deploy");
      if (pullCode != 0) {
        status.set_status("failed");
        status.set_message("docker pull failed");
        if (logCb) {
          logCb("deploy", "docker pull failed\n");
        }
        cb(status);
        return;
      }
    } else {
      auto checkout =
          checkoutRepository(dataDir_, order.application_id(), order.deployment_id(), order.repo_url(),
                             order.branch(), order.commit_sha(), order.deploy_key_private(),
                             order.git_access_token());
      status.set_git_sha(checkout.commitSha);
      status.set_git_message(checkout.commitMessage);
      status.set_message("building image");
      cb(status);
      image = sanitizeImageRepository(order.repo_url(), order.application_id()) + ":" +
              std::to_string(order.deployment_id());
      if (logCb) {
        logCb("deploy", "Checked out " + checkout.commitSha.substr(0, 12) + " " +
                            checkout.commitMessage + "\n");
        logCb("build", "Building image " + image + "\n");
      }
      std::filesystem::path context = checkout.path;
      if (order.base_directory() != "/" && !order.base_directory().empty()) {
        context /= order.base_directory().front() == '/' ? order.base_directory().substr(1)
                                                         : order.base_directory();
      }
      const auto dockerfile = context / order.dockerfile_path();
      std::vector<std::string> buildArgs = {
          "build",
          "--label",
          "highcube.application=" + std::to_string(order.application_id()),
          "-t",
          image,
          "-f",
          dockerfile.string()};
      if (order.no_cache()) {
        buildArgs.push_back("--no-cache");
      }
      buildArgs.push_back(context.string());
      const int buildCode = dockerStreaming(buildArgs, logCb, "build");
      if (buildCode != 0) {
        status.set_status("failed");
        status.set_message("docker build failed");
        if (logCb) {
          logCb("build", "docker build failed\n");
          logCb("deploy", "Build failed\n");
        }
        cb(status);
        return;
      }
    }

    const bool hasVolumes = order.volumes_size() > 0;
    const std::string name =
        "hc-" + std::to_string(order.application_id()) + "-" + std::to_string(order.replica_id());
    if (hasVolumes) {
      // A volume app runs a single replica: anything else of this application on this host
      // is the previous generation and must release the volume before the new one starts.
      const auto previous =
          splitLines(docker({"ps", "-aq", "--no-trunc", "--filter",
                             "label=highcube.application=" + std::to_string(order.application_id())}));
      for (const auto& container : previous) {
        if (logCb) {
          logCb("deploy", "Stopping previous container " + container.substr(0, 12) +
                              " before reusing its volumes\n");
        }
        removeContainer(container);
      }
      removeContainer(name);
    }

    std::vector<std::string> runArgs = {
        "run",
        "-d",
        "--name",
        name,
        "--label",
        "highcube.application=" + std::to_string(order.application_id()),
        "--label",
        "highcube.deployment=" + std::to_string(order.deployment_id()),
        "--restart",
        "unless-stopped",
        "--network",
        "highcube",
        "--network-alias",
        order.network_alias()};
    for (const auto& [key, value] : order.env()) {
      runArgs.push_back("-e");
      runArgs.push_back(key + "=" + value);
    }
    for (const auto& volume : order.volumes()) {
      const std::string volName =
          "highcube-" + std::to_string(order.application_id()) + "-" + volume.name();
      runArgs.push_back("-v");
      runArgs.push_back(volName + ":" + volume.mount_path());
    }
    if (hasVolumes) {
      // Also honoured when dockerd stops containers on shutdown.
      runArgs.push_back("--stop-timeout");
      runArgs.push_back(std::to_string(kVolumeStopTimeoutSeconds));
    }

    int hostPort = 0;
    bool fixedHostPorts = false;
    {
      // Ports are only published to the host when explicitly configured. A mapping with
      // host_port == 0 is ignored (the container stays reachable only on the internal
      // highcube network via its alias). Nothing is auto-published and no random port is
      // picked. A fixed port still held by a previous container of this application is freed
      // first, which causes a brief downtime.
      std::vector<int> requestedFixed;
      for (const auto& mapping : order.port_mappings()) {
        if (mapping.host_port() > 0) {
          requestedFixed.push_back(mapping.host_port());
        }
      }
      std::vector<int> busyFixed;
      for (const int port : requestedFixed) {
        if (!isHostPortFree(port)) {
          busyFixed.push_back(port);
        }
      }
      if (!busyFixed.empty()) {
        releaseApplicationHostPorts(order.application_id(), busyFixed, logCb);
      }
      for (const auto& mapping : order.port_mappings()) {
        if (mapping.host_port() <= 0) {
          continue;  // not published to the host
        }
        fixedHostPorts = true;
        if (hostPort == 0) {
          hostPort = mapping.host_port();
        }
        runArgs.push_back("-p");
        runArgs.push_back(std::to_string(mapping.host_port()) + ":" +
                          std::to_string(mapping.container_port()));
      }
    }

    if (order.health_check_type() == "http") {
      const std::string cmd =
          "curl -f http://localhost:" + std::to_string(order.ports_exposes()) +
          order.health_check_path() + " || wget -q -O /dev/null http://localhost:" +
          std::to_string(order.ports_exposes()) + order.health_check_path();
      runArgs.push_back("--health-cmd");
      runArgs.push_back(cmd);
      runArgs.push_back("--health-interval");
      runArgs.push_back("30s");
      runArgs.push_back("--health-timeout");
      runArgs.push_back("5s");
      runArgs.push_back("--health-retries");
      runArgs.push_back("3");
      runArgs.push_back("--health-start-period");
      runArgs.push_back("20s");
    } else if (order.health_check_type() == "cmd") {
      runArgs.push_back("--health-cmd");
      runArgs.push_back(order.health_check_command());
      runArgs.push_back("--health-interval");
      runArgs.push_back("30s");
      runArgs.push_back("--health-timeout");
      runArgs.push_back("5s");
      runArgs.push_back("--health-retries");
      runArgs.push_back("3");
      runArgs.push_back("--health-start-period");
      runArgs.push_back("20s");
    }

    runArgs.push_back(image);
    if (consumeCancelled(order.replica_id())) {
      removeReplicaContainer(order.application_id(), order.replica_id(), "");
      status.set_status("stopped");
      status.set_desired_state("stopped");
      status.set_message("deploy cancelled");
      if (logCb) {
        logCb("deploy", "Deploy cancelled before the container started\n");
      }
      cb(status);
      return;
    }
    status.set_status("starting");
    status.set_message("starting container");
    cb(status);
    if (logCb) {
      logCb("deploy", "Starting container " + name + "\n");
    }

    int runCode = dockerStreaming(runArgs, logCb, "deploy");
    if (runCode != 0) {
      removeContainer(name);
      status.set_status("failed");
      std::string failure = "docker run failed";
      if (fixedHostPorts) {
        std::string busy;
        for (const auto& mapping : order.port_mappings()) {
          if (mapping.host_port() > 0 && !isHostPortFree(mapping.host_port())) {
            if (!busy.empty()) {
              busy += ", ";
            }
            busy += std::to_string(mapping.host_port());
          }
        }
        if (!busy.empty()) {
          failure = "configured host port is in use: " + busy;
        }
      }
      status.set_message(failure);
      if (logCb) {
        logCb("deploy", failure + "\n");
      }
      cb(status);
      return;
    }

    const auto id = docker({"inspect", "--format", "{{.Id}}", name});
    std::string containerId = id;
    while (!containerId.empty() && (containerId.back() == '\n' || containerId.back() == '\r')) {
      containerId.pop_back();
    }
    status.set_container_id(containerId);
    if (const std::string ip = highcubeIp(containerId); !ip.empty()) {
      status.set_container_ip(ip);
    }
    if (hostPort > 0) {
      status.set_host_port(hostPort);
    }

    const std::string health = waitWhileStarting(containerId);
    if (health == "healthy" || health == "running") {
      status.set_status(health == "healthy" ? "healthy" : "running");
      status.set_message("replica ready");
      if (logCb) {
        logCb("deploy", "Replica ready (" + status.status() + ")\n");
      }
    } else if (health == "unhealthy") {
      status.set_status("unhealthy");
      status.set_message("health check failed");
      if (logCb) {
        logCb("deploy", "Health check failed\n");
      }
    } else {
      status.set_status(health.empty() ? "running" : health);
      status.set_message("container started");
      if (logCb) {
        logCb("deploy", "Container started (" + status.status() + ")\n");
      }
    }
    if (consumeCancelled(order.replica_id())) {
      removeReplicaContainer(order.application_id(), order.replica_id(), containerId);
      status.set_status("stopped");
      status.set_desired_state("stopped");
      status.set_message("deploy cancelled");
      if (logCb) {
        logCb("deploy", "Deploy cancelled after the container started\n");
      }
      cb(status);
      return;
    }
    removePreviousApplicationImages(order.application_id(), order.deployment_id(), image, containerId);
    cb(status);
  } catch (const std::exception& ex) {
    status.set_status("failed");
    status.set_message(ex.what());
    if (logCb) {
      logCb("deploy", std::string(ex.what()) + "\n");
    }
    cb(status);
  }
}

void DockerRuntime::removePreviousApplicationImages(int64_t applicationId, int64_t deploymentId,
                                                     const std::string& currentImage,
                                                     const std::string& currentContainerId) const {
  const auto canonical = [](std::string id) {
    id = trimRight(std::move(id));
    const std::string prefix = "sha256:";
    if (id.rfind(prefix, 0) == 0) {
      id.erase(0, prefix.size());
    }
    if (id == "<no value>") {
      return std::string();
    }
    return id;
  };

  const std::string currentImageId = canonical(docker({"inspect", "--format", "{{.Id}}", currentImage}));
  const std::string currentContainer = canonical(currentContainerId);
  const std::string appId = std::to_string(applicationId);
  const std::string deploymentIdText = std::to_string(deploymentId);
  const std::string namePrefix = "/hc-" + appId + "-";

  std::unordered_set<std::string> oldImageIds;
  auto collect = [&](const std::string& text) {
    for (const auto& line : splitLines(text)) {
      const auto id = canonical(line);
      if (!id.empty() && id != currentImageId) {
        oldImageIds.insert(id);
      }
    }
  };
  collect(docker({"images", "--no-trunc", "--filter", "label=highcube.application=" + appId,
                  "--format", "{{.ID}}"}));
  collect(docker({"images", "--no-trunc", "highcube-" + appId, "--format", "{{.ID}}"}));

  for (const auto& imageId : oldImageIds) {
    const auto users =
        splitLines(docker({"ps", "-aq", "--no-trunc", "--filter", "ancestor=" + imageId}));
    for (const auto& containerId : users) {
      if (canonical(containerId) == currentContainer) {
        continue;
      }
      const auto containerName = trimRight(docker({"inspect", "--format", "{{.Name}}", containerId}));
      const auto deploymentLabel = canonical(docker(
          {"inspect", "--format", "{{index .Config.Labels \"highcube.deployment\"}}", containerId}));
      const auto applicationLabel = canonical(docker(
          {"inspect", "--format", "{{index .Config.Labels \"highcube.application\"}}", containerId}));
      const bool sameApp =
          applicationLabel == appId || containerName.rfind(namePrefix, 0) == 0;
      const bool sameDeployment = deploymentLabel == deploymentIdText;
      if (sameApp && !sameDeployment) {
        removeContainer(containerId);
      }
    }
    dockerCode({"rmi", imageId});
  }
}

void DockerRuntime::removeReplicaContainer(int64_t applicationId, int64_t replicaId,
                                            const std::string& containerId) const {
  if (!containerId.empty()) {
    removeContainer(containerId);
  }
  if (applicationId > 0 && replicaId > 0) {
    removeContainer("hc-" + std::to_string(applicationId) + "-" + std::to_string(replicaId));
    return;
  }
  if (replicaId <= 0) {
    return;
  }
  const auto names = splitLines(docker({"ps", "-a", "--format", "{{.Names}}"}));
  for (const auto& name : names) {
    int64_t parsed = 0;
    if (replicaIdFromContainerName(name, parsed) && parsed == replicaId) {
      removeContainer(name);
    }
  }
}

void DockerRuntime::removeUnkeptReplicas(const std::vector<int64_t>& keepReplicaIds) const {
  const std::unordered_set<int64_t> keep(keepReplicaIds.begin(), keepReplicaIds.end());
  const auto names = splitLines(docker({"ps", "-a", "--format", "{{.Names}}"}));
  for (const auto& name : names) {
    int64_t replicaId = 0;
    if (!replicaIdFromContainerName(name, replicaId) || keep.count(replicaId) != 0) {
      continue;
    }
    spdlog::info("removing container {} because replica {} is not desired", name, replicaId);
    removeContainer(name);
  }
}

void DockerRuntime::handleStop(const ::highcube::StopReplica& order, StatusCallback cb) {
  // A stop that arrives while docker build is still running must cancel that deploy.
  // Otherwise the build finishes later and publishes a container the control plane already forgot.
  if (order.replica_id() > 0) {
    std::lock_guard lock(cancelMutex_);
    cancelledReplicas_.insert(order.replica_id());
  }
  removeReplicaContainer(order.application_id(), order.replica_id(), order.container_id());
  ::highcube::ContainerStatus status;
  status.set_replica_id(order.replica_id());
  status.set_container_id(order.container_id());
  status.set_status("stopped");
  status.set_desired_state("stopped");
  cb(status);
}

void DockerRuntime::handleRestart(const ::highcube::RestartReplica& order, StatusCallback cb) {
  if (!order.container_id().empty()) {
    dockerCode({"restart", order.container_id()});
  }
  ::highcube::ContainerStatus status;
  status.set_replica_id(order.replica_id());
  status.set_container_id(order.container_id());
  // A restart can hand out another address on the highcube network.
  if (const std::string ip = highcubeIp(order.container_id()); !ip.empty()) {
    status.set_container_ip(ip);
  }
  status.set_status(waitWhileStarting(order.container_id()));
  status.set_desired_state("running");
  cb(status);
}

void DockerRuntime::deleteVolume(const std::string& volumeName) {
  dockerCode({"volume", "rm", "-f", volumeName});
}

void DockerRuntime::renameVolume(const std::string& fromName, const std::string& toName) {
  if (fromName.empty() || toName.empty() || fromName == toName) {
    return;
  }
  // No-op when the source volume was never created on this host.
  if (dockerCode({"volume", "inspect", fromName}) != 0) {
    return;
  }
  if (dockerCode({"volume", "inspect", toName}) == 0) {
    throw std::runtime_error("target volume already exists: " + toName);
  }
  if (dockerCode({"volume", "create", toName}) != 0) {
    throw std::runtime_error("failed to create volume: " + toName);
  }
  const int copyCode = dockerCode({"run", "--rm", "-v", fromName + ":/from:ro", "-v",
                                   toName + ":/to", "alpine", "sh", "-c", "cp -a /from/. /to/"});
  if (copyCode != 0) {
    dockerCode({"volume", "rm", "-f", toName});
    throw std::runtime_error("failed to copy volume data from " + fromName + " to " + toName);
  }
  dockerCode({"volume", "rm", "-f", fromName});
}

std::string DockerRuntime::inspectStatus(const std::string& containerId) {
  if (containerId.empty()) {
    return "";
  }
  auto health = docker({"inspect", "--format",
                        "{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}",
                        containerId});
  while (!health.empty() && (health.back() == '\n' || health.back() == '\r')) {
    health.pop_back();
  }
  return health;
}

std::string DockerRuntime::highcubeIp(const std::string& containerId) const {
  // nginx (on the admin host) proxies public hostnames to this IP, so no host port
  // needs to be published.
  std::string ip = docker({"inspect", "--format",
                           "{{with index .NetworkSettings.Networks \"highcube\"}}{{.IPAddress}}{{end}}",
                           containerId});
  while (!ip.empty() && (ip.back() == '\n' || ip.back() == '\r' || ip.back() == ' ')) {
    ip.pop_back();
  }
  return ip;
}

std::string DockerRuntime::waitWhileStarting(const std::string& containerId) {
  // 30s interval + 3 retries of up to 5s each; 150s also covers a slow first start.
  std::string health = inspectStatus(containerId);
  for (int i = 0; i < 75 && health == "starting"; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    health = inspectStatus(containerId);
  }
  return health;
}

std::string DockerRuntime::containerStatsJson(const std::string& containerId) {
  if (containerId.empty()) {
    return "{}";
  }
  const auto payloads = containerStatsByName({containerId});
  return payloads.empty() ? "{}" : payloads.begin()->second;
}

std::unordered_map<std::string, std::string> DockerRuntime::containerStatsByName(
    const std::vector<std::string>& containers) {
  std::unordered_map<std::string, std::string> payloads;
  if (containers.empty()) {
    return payloads;
  }
  // docker stats --no-stream samples for about two seconds per call. Without arguments it
  // samples every running container in that single window, and a container that exits
  // meanwhile cannot make the whole call fail.
  std::unordered_map<std::string, std::pair<std::string, std::string>> usage;
  for (const auto& line : splitLines(docker(
           {"stats", "--no-stream", "--format", "{{.Name}}|{{.CPUPerc}}|{{.MemUsage}}"}))) {
    const auto first = line.find('|');
    const auto second = first == std::string::npos ? std::string::npos : line.find('|', first + 1);
    if (second == std::string::npos) {
      continue;
    }
    usage[line.substr(0, first)] = {line.substr(first + 1, second - first - 1),
                                    line.substr(second + 1)};
  }

  std::vector<std::string> args{"inspect", "--size", "--format",
                                "{{.Name}}|{{.SizeRw}}|{{.RestartCount}}|{{json .Mounts}}"};
  args.insert(args.end(), containers.begin(), containers.end());
  for (const auto& line : splitLines(docker(args))) {
    const auto first = line.find('|');
    const auto second = first == std::string::npos ? std::string::npos : line.find('|', first + 1);
    const auto third = second == std::string::npos ? std::string::npos : line.find('|', second + 1);
    if (third == std::string::npos) {
      continue;
    }
    std::string name = line.substr(0, first);
    if (!name.empty() && name.front() == '/') {
      name.erase(name.begin());
    }
    int64_t diskBytes = 0;
    int restartCount = 0;
    try {
      diskBytes = std::stoll(line.substr(first + 1, second - first - 1));
    } catch (const std::exception&) {
      diskBytes = 0;
    }
    try {
      restartCount = std::stoi(line.substr(second + 1, third - second - 1));
    } catch (const std::exception&) {
      restartCount = 0;
    }
    const auto it = usage.find(name);
    payloads[name] = containerStatsPayload(it == usage.end() ? "" : it->second.first,
                                           it == usage.end() ? "" : it->second.second,
                                           diskBytes, restartCount, line.substr(third + 1));
  }
  return payloads;
}

bool DockerRuntime::containerHasShell(const std::string& containerId) {
  if (containerId.empty()) {
    return false;
  }
  return dockerCode({"exec", containerId, "/bin/sh", "-c", "true"}) == 0;
}

nlohmann::json DockerRuntime::inspectVolumes(int64_t applicationId,
                                             const std::vector<::highcube::VolumeSpec>& volumes) {
  nlohmann::json rows = nlohmann::json::array();
  const auto now = std::chrono::steady_clock::now();
  for (const auto& volume : volumes) {
    const std::string volName =
        "highcube-" + std::to_string(applicationId) + "-" + volume.name();
    nlohmann::json item = {{"name", volume.name()},
                           {"mountPath", volume.mount_path()},
                           {"dockerName", volName},
                           {"hostPath", ""},
                           {"sizeBytes", nullptr}};
    {
      std::lock_guard lock(volumeCacheMutex);
      auto it = volumeCache.find(volName);
      if (it != volumeCache.end() && it->second.expiresAt > now) {
        rows.push_back(it->second.payload);
        continue;
      }
    }
    auto mount = docker({"volume", "inspect", "--format", "{{.Mountpoint}}", volName});
    while (!mount.empty() && (mount.back() == '\n' || mount.back() == '\r')) {
      mount.pop_back();
    }
    if (!mount.empty()) {
      item["hostPath"] = mount;
      item["sizeBytes"] = directorySizeBytes(mount);
    }
    {
      std::lock_guard lock(volumeCacheMutex);
      volumeCache[volName] = VolumeCacheEntry{item, now + std::chrono::minutes(5)};
    }
    rows.push_back(item);
  }
  return rows;
}

}  // namespace highcube::agent
