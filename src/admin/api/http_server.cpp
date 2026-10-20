#include "api/http_server.hpp"

#include "api/http_types.hpp"

#include <boost/beast/websocket.hpp>
#include <spdlog/spdlog.h>

namespace highcube::admin {
namespace {

namespace websocket = beast::websocket;

class Session : public std::enable_shared_from_this<Session> {
public:
  Session(tcp::socket socket, Router& router, std::shared_ptr<AgentHub> agentHub,
          std::shared_ptr<HostTerminalPolling> terminalPolling)
      : stream_(std::move(socket)),
        router_(router),
        agentHub_(std::move(agentHub)),
        terminalPolling_(std::move(terminalPolling)) {}

  void run() { doRead(); }

private:
  void doRead() {
    auto self = shared_from_this();
    http::async_read(stream_, buffer_, request_,
                     [self](beast::error_code ec, std::size_t) {
                       if (ec) {
                         return;
                       }
                       self->handleRequest();
                     });
  }

  void handleRequest() {
    const std::string target = std::string(request_.target());
    const auto path = pathOnly(target);
    const bool isUpgrade = request_.method() == http::verb::get && websocket::is_upgrade(request_);
    const bool isAgentConnect = path == "/api/agents/connect";
    const bool isHostTerminal = isHostTerminalPath(path);

    if (isUpgrade && agentHub_ && (isAgentConnect || isHostTerminal)) {
      agentHub_->onUpgrade(stream_.release_socket(), std::move(request_));
      return;
    }

    auto self = shared_from_this();
    if (terminalPolling_ &&
        terminalPolling_->tryPollOutput(request_, [self](Response response) {
          self->writeResponse(std::move(response));
        })) {
      return;
    }

    writeResponse(router_.handle(std::move(request_)));
  }

  void writeResponse(Response response) {
    auto shared = std::make_shared<Response>(std::move(response));
    auto self = shared_from_this();
    http::async_write(stream_, *shared,
                      [self, shared](beast::error_code ec, std::size_t) {
                        boost::ignore_unused(ec);
                        beast::error_code ignored;
                        self->stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
                      });
  }

  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
  Request request_;
  Router& router_;
  std::shared_ptr<AgentHub> agentHub_;
  std::shared_ptr<HostTerminalPolling> terminalPolling_;
};

}  // namespace

HttpServer::HttpServer(net::io_context& ioc, const std::string& address, unsigned short port,
                       Router& router, std::shared_ptr<AgentHub> agentHub,
                       std::shared_ptr<HostTerminalPolling> terminalPolling)
    : ioc_(ioc),
      acceptor_(ioc, {net::ip::make_address(address), port}),
      router_(router),
      agentHub_(std::move(agentHub)),
      terminalPolling_(std::move(terminalPolling)) {}

void HttpServer::start() {
  doAccept();
}

void HttpServer::doAccept() {
  acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
    if (!ec) {
      std::make_shared<Session>(std::move(socket), router_, agentHub_, terminalPolling_)->run();
    }
    doAccept();
  });
}

}  // namespace highcube::admin
