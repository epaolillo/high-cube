#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <highcube.pb.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace highcube::admin {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class AgentSession : public std::enable_shared_from_this<AgentSession> {
public:
  using Pointer = std::shared_ptr<AgentSession>;
  using MessageHandler = std::function<void(Pointer, const ::highcube::Envelope&)>;
  using CloseHandler = std::function<void(Pointer)>;

  AgentSession(tcp::socket socket, MessageHandler onMessage);

  void start(http::request<http::string_body> req, CloseHandler onClose = {});
  void send(const ::highcube::Envelope& envelope);
  void close();

  int64_t serverId() const { return serverId_; }
  void setServerId(int64_t id) { serverId_ = id; }
  const std::string& serverName() const { return serverName_; }
  void setServerName(std::string name) { serverName_ = std::move(name); }

private:
  void doRead();
  void doWrite();
  void fail(beast::error_code ec, const char* what);

  websocket::stream<beast::tcp_stream> ws_;
  // Captured up front. send() must not query the websocket while a read is pending.
  net::any_io_executor executor_;
  beast::flat_buffer buffer_;
  MessageHandler onMessage_;
  CloseHandler onClose_;
  std::deque<std::shared_ptr<std::string>> outgoing_;
  int64_t serverId_ = 0;
  std::string serverName_;
  bool writing_ = false;
  bool closed_ = false;
};

}  // namespace highcube::admin
