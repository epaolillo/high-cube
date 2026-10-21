#include "agents/terminal_polling.hpp"
#include "api/handlers.hpp"
#include "api/http_server.hpp"
#include "api/router.hpp"
#include "auth/auth_middleware.hpp"
#include "auth/jwt_service.hpp"
#include "bootstrap/bootstrap.hpp"
#include "certificates/certbot_service.hpp"
#include "common/logging.hpp"
#include "config/admin_config.hpp"
#include "db/database.hpp"
#include "db/migration_runner.hpp"
#include "db/repositories.hpp"
#include "proxy/nginx_proxy.hpp"
#include "services/services.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

struct Args {
  bool service = false;
  std::string configPath = "hc-admin.conf";
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
      std::cout << "usage: hc-admin [--service] [--config path]\n";
      std::exit(0);
    }
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace highcube;
  using namespace highcube::admin;

  initLogging("hc-admin");
  const Args args = parseArgs(argc, argv);

  try {
    auto config = AdminConfig::load(args.configPath);
    // Switch from the bootstrap console logger to the configured rotating file
    // logger so logs land in config.logFile (default logs/log.txt, 7 days).
    initFileLogging("hc-admin", config.logFile, config.logRetentionDays, config.logLevel);
    auto database = std::make_unique<Database>(config.sqlitePath);
    MigrationRunner(*database).applyAll(config.migrationsDir);
    auto repos = std::make_unique<Repositories>(*database);

    Bootstrap bootstrap(config, *repos);
    if (!args.service) {
      return bootstrap.runInstall();
    }
    bootstrap.assertReadyForService();

    AppContext ctx;
    ctx.config = config;
    ctx.database = std::move(database);
    ctx.repos = std::move(repos);
    ctx.jwt = std::make_unique<JwtService>(config.sessionSecret);
    ctx.auth = std::make_unique<AuthMiddleware>(*ctx.repos, *ctx.jwt);
    ctx.proxy = std::make_unique<NginxProxy>(config, *ctx.repos);
    ctx.certbot = std::make_unique<CertbotService>(config, *ctx.repos, *ctx.proxy);

    net::io_context ioc{1};
    ctx.agentHub = std::make_shared<AgentHub>(ioc, *ctx.repos);
    ctx.agentHub->setAuth(ctx.auth.get());
    ctx.presence = std::make_shared<PresenceHub>(ioc, *ctx.repos, *ctx.auth);
    ctx.placement = std::make_unique<PlacementService>(ctx);
    ctx.deployments = std::make_unique<DeploymentService>(ctx);
    ctx.githubApps = std::make_unique<GithubAppService>(ctx);
    ctx.agentHub->setDeploymentService(ctx.deployments.get());
    ctx.agentHub->setProxy(ctx.proxy.get());
    ctx.agentHub->setGithubApps(ctx.githubApps.get());

    auto terminalPolling =
        std::make_shared<HostTerminalPolling>(ioc, *ctx.agentHub, *ctx.auth, *ctx.repos);
    terminalPolling->start();
    ctx.terminalPolling = terminalPolling.get();

    ctx.proxy->renderAdminSite();
    MetricsRetentionService(ctx).runOnce();

    Router router(*ctx.auth);
    registerHandlers(router, ctx);

    HttpServer server(ioc, "0.0.0.0", static_cast<unsigned short>(config.httpPort), router,
                      ctx.agentHub, terminalPolling, ctx.presence);
    server.start();
    spdlog::info("hc-admin listening on :{}", config.httpPort);

    net::steady_timer offlineTimer(ioc);
    std::function<void()> scheduleOffline;
    scheduleOffline = [&]() {
      offlineTimer.expires_after(std::chrono::seconds(30));
      offlineTimer.async_wait([&](beast::error_code ec) {
        if (ec) {
          return;
        }
        try {
          ctx.agentHub->checkOfflineServers();
        } catch (const std::exception& ex) {
          spdlog::error("offline check failed: {}", ex.what());
        }
        scheduleOffline();
      });
    };
    scheduleOffline();

    net::steady_timer retentionTimer(ioc);
    std::function<void()> scheduleRetention;
    scheduleRetention = [&]() {
      retentionTimer.expires_after(std::chrono::hours(1));
      retentionTimer.async_wait([&](beast::error_code ec) {
        if (ec) {
          return;
        }
        try {
          MetricsRetentionService(ctx).runOnce();
          ctx.certbot->renewAll();
        } catch (const std::exception& ex) {
          spdlog::error("retention failed: {}", ex.what());
        }
        scheduleRetention();
      });
    };
    scheduleRetention();

    net::steady_timer autoscalerTimer(ioc);
    std::function<void()> scheduleAutoscaler;
    scheduleAutoscaler = [&]() {
      autoscalerTimer.expires_after(std::chrono::seconds(30));
      autoscalerTimer.async_wait([&](beast::error_code ec) {
        if (ec) {
          return;
        }
        try {
          AutoscalerService(ctx).tick();
        } catch (const std::exception& ex) {
          spdlog::error("autoscaler failed: {}", ex.what());
        }
        scheduleAutoscaler();
      });
    };
    scheduleAutoscaler();

    // Supervisor: keep application containers alive and trace their falls.
    net::steady_timer supervisorTimer(ioc);
    std::function<void()> scheduleSupervisor;
    scheduleSupervisor = [&]() {
      supervisorTimer.expires_after(std::chrono::seconds(30));
      supervisorTimer.async_wait([&](beast::error_code ec) {
        if (ec) {
          return;
        }
        try {
          SupervisorService(ctx).tick();
        } catch (const std::exception& ex) {
          spdlog::error("supervisor failed: {}", ex.what());
        }
        scheduleSupervisor();
      });
    };
    scheduleSupervisor();

    // Presence heartbeats: while users keep the dashboard open, record a coarse
    // heartbeat so the activity timeline knows when each user was active.
    net::steady_timer presenceTimer(ioc);
    std::function<void()> schedulePresence;
    schedulePresence = [&]() {
      presenceTimer.expires_after(std::chrono::seconds(60));
      presenceTimer.async_wait([&](beast::error_code ec) {
        if (ec) {
          return;
        }
        try {
          ctx.presence->recordHeartbeats();
        } catch (const std::exception& ex) {
          spdlog::error("presence heartbeat failed: {}", ex.what());
        }
        schedulePresence();
      });
    };
    schedulePresence();

    ioc.run();
  } catch (const std::exception& ex) {
    spdlog::error("fatal: {}", ex.what());
    return 1;
  }
  return 0;
}
