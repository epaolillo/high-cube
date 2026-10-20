#include "metrics/container_metrics.hpp"

namespace highcube::agent {

std::string collectContainerMetricsJson(DockerRuntime& runtime, const std::string& containerId) {
  return runtime.containerStatsJson(containerId);
}

}  // namespace highcube::agent
