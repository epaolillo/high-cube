#pragma once

#include "agents/agent_hub.hpp"
#include "agents/terminal_polling.hpp"
#include "api/http_types.hpp"
#include "api/router.hpp"
#include "presence/presence_hub.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <memory>
#include <string>

namespace beast = boost::beast;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace highcube::admin {

class HttpServer {
public:
  HttpServer(net::io_context& ioc, const std::string& address, unsigned short port, Router& router,
             std::shared_ptr<AgentHub> agentHub, std::shared_ptr<HostTerminalPolling> terminalPolling,
             std::shared_ptr<PresenceHub> presenceHub);

  void start();

private:
  void doAccept();

  net::io_context& ioc_;
  tcp::acceptor acceptor_;
  Router& router_;
  std::shared_ptr<AgentHub> agentHub_;
  std::shared_ptr<HostTerminalPolling> terminalPolling_;
  std::shared_ptr<PresenceHub> presenceHub_;
};

}  // namespace highcube::admin
