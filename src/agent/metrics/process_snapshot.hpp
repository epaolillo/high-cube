#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace highcube::agent {

struct ProcessInfo {
  int pid = 0;
  unsigned long long startTicks = 0;  // /proc/<pid>/stat starttime, guards against pid reuse
  std::string user;
  std::string name;
  std::string cmd;
  std::string container;
  double cpuPercent = 0;  // share of the whole machine since the previous sample
  long long rssKb = 0;
  long long swapKb = 0;
};

struct CpuBreakdown {
  double user = 0;
  double system = 0;
  double iowait = 0;
  double steal = 0;
  double idle = 0;
};

// Computes per-process CPU usage from the change in utime+stime between two calls.
class ProcessSampler {
public:
  // All processes with their CPU share since the previous call. The first call returns 0% CPU.
  std::vector<ProcessInfo> sample();
  CpuBreakdown breakdown() const { return breakdown_; }
  int cores() const { return cores_; }
  // Total jiffies across all CPUs between the last two samples. Used to convert cgroup usec.
  unsigned long long lastDeltaTotal() const { return lastDeltaTotal_; }

private:
  struct Previous {
    unsigned long long start = 0;
    unsigned long long ticks = 0;
  };
  std::unordered_map<int, Previous> previous_;
  unsigned long long previousTotal_ = 0;
  unsigned long long lastDeltaTotal_ = 0;
  unsigned long long previousParts_[5] = {0, 0, 0, 0, 0};
  CpuBreakdown breakdown_;
  int cores_ = 1;
};

// Fills user, cmd, swap and container for the given processes (expensive fields).
void enrichProcess(ProcessInfo& process);
nlohmann::json processToJson(const ProcessInfo& process);

// Top processes by CPU and by RSS plus host CPU, load and memory context.
// Uses the already-sampled process list so the same walk can feed attribution.
std::string collectProcessSnapshotJson(ProcessSampler& sampler, std::vector<ProcessInfo>& processes);
// Convenience wrapper that samples once.
std::string collectProcessSnapshotJson(ProcessSampler& sampler);

// Per-container CPU share of the machine, with top processes inside each container.
// previousCgroupUsec stores the last usage_usec (or cpuacct.usage converted to usec) per
// container id so the next call can compute a delta.
std::string collectCpuAttributionJson(ProcessSampler& sampler, std::vector<ProcessInfo>& processes,
                                      std::unordered_map<std::string, unsigned long long>& previousCgroupUsec);

// Container names are hc-<application_id>-<replica_id>.
bool parseReplicaIdFromName(const std::string& name, int64_t& replicaId);
// Also returns application_id when the name matches.
bool parseHcContainerName(const std::string& name, int64_t& applicationId, int64_t& replicaId);

// 64-hex Docker container id from /proc/<pid>/cgroup, empty for host processes.
std::string containerIdForPid(int pid);
// Container name from /var/lib/docker/containers/<id>/config.v2.json (cached).
std::string containerNameForId(const std::string& id);

// starttime field of /proc/<pid>/stat, 0 when the process is gone.
unsigned long long processStartTicks(int pid);

}  // namespace highcube::agent
