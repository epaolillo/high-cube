#include "bootstrap/bootstrap.hpp"
#include "common/logging.hpp"
#include "config/agent_config.hpp"
#include "connection/ws_client.hpp"
#include "runtime/docker_runtime.hpp"

#include <spdlog/spdlog.h>

#include <iostream>
#include <memory>
#include <string>

namespace {

struct Args {
  bool service = false;
  std::string configPath = "hc-agent.conf";
};

Args parseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--service") {
      args.service = true;
    } else if (arg == "--config" && i + 1 < argc) {
      args.configPath = argv[++i];
    } else if (arg == "--help") {
      std::cout << "usage: hc-agent [--service] [--config path]\n";
      std::exit(0);
    }
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace highcube;
  using namespace highcube::agent;

  initLogging("hc-agent");
  const Args args = parseArgs(argc, argv);

  try {
    auto config = AgentConfig::load(args.configPath);
    Bootstrap bootstrap(config);
    if (!args.service) {
      return bootstrap.runInstall();
    }

    DockerRuntime runtime(config.dataDir);
    runtime.ensureNetwork();

    net::io_context ioc{1};
    auto client = std::make_shared<WsClient>(ioc, config, runtime);
    client->start();
    spdlog::info("hc-agent connecting to {}", config.adminUrl);
    ioc.run();
  } catch (const std::exception& ex) {
    spdlog::error("fatal: {}", ex.what());
    return 1;
  }
  return 0;
}
