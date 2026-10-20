#pragma once

#include "runtime/docker_runtime.hpp"

#include <string>

namespace highcube::agent {

std::string collectContainerMetricsJson(DockerRuntime& runtime, const std::string& containerId);

}  // namespace highcube::agent
