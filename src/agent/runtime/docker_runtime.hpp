#pragma once

#include <highcube.pb.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace highcube::agent {

class DockerRuntime {
public:
  using StatusCallback = std::function<void(const ::highcube::ContainerStatus&)>;
  using LogCallback = std::function<void(const std::string& phase, const std::string& chunk)>;

  explicit DockerRuntime(std::string dataDir);

  void ensureNetwork();
  void handleDeploy(const ::highcube::DeployReplica& order, StatusCallback cb, LogCallback logCb = {});
  void handleStop(const ::highcube::StopReplica& order, StatusCallback cb);
  void handleRestart(const ::highcube::RestartReplica& order, StatusCallback cb);
  // Removes the container by id and by the stable name hc-<application_id>-<replica_id>.
  void removeReplicaContainer(int64_t applicationId, int64_t replicaId,
                              const std::string& containerId) const;
  // Drops managed containers whose replica id is not in keepReplicaIds.
  void removeUnkeptReplicas(const std::vector<int64_t>& keepReplicaIds) const;
  void deleteVolume(const std::string& volumeName);
  void renameVolume(const std::string& fromName, const std::string& toName);
  std::string inspectStatus(const std::string& containerId);
  std::string containerStatsJson(const std::string& containerId);
  bool containerHasShell(const std::string& containerId);
  nlohmann::json inspectVolumes(int64_t applicationId, const std::vector<::highcube::VolumeSpec>& volumes);
  // Returns true when the given TCP port is free to bind on this host.
  bool isHostPortAvailable(int port) const { return isHostPortFree(port); }

private:
  bool isHostPortFree(int port) const;
  // Stops containers of this application that still publish one of the fixed host ports.
  void releaseApplicationHostPorts(int64_t applicationId, const std::vector<int>& hostPorts,
                                   const LogCallback& logCb) const;
  std::string docker(const std::vector<std::string>& args) const;
  int dockerCode(const std::vector<std::string>& args) const;
  bool consumeCancelled(int64_t replicaId) const;
  int dockerStreaming(const std::vector<std::string>& args, const LogCallback& logCb,
                      const std::string& phase) const;
  void removePreviousApplicationImages(int64_t applicationId, int64_t deploymentId,
                                       const std::string& currentImage,
                                       const std::string& currentContainerId) const;

  std::string dataDir_;
  mutable std::mutex cancelMutex_;
  mutable std::unordered_set<int64_t> cancelledReplicas_;
};

}  // namespace highcube::agent
