#pragma once

#include "agents/session.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace highcube::admin {

class AgentHub;

class HostTerminalRelay : public std::enable_shared_from_this<HostTerminalRelay> {
public:
  enum class Kind { Host, Container, Logs };

  HostTerminalRelay(tcp::socket socket, AgentHub& hub, int64_t serverId, std::string streamId,
                    std::uint32_t cols, std::uint32_t rows, Kind kind = Kind::Host,
                    int64_t replicaId = 0);

  void start(http::request<http::string_body> req);
  void onOutput(const std::string& data);
  void closeSocket();

private:
  void doRead();
  void doWrite();
  void shutdown();

  websocket::stream<beast::tcp_stream> ws_;
  beast::flat_buffer buffer_;
  AgentHub& hub_;
  int64_t serverId_;
  int64_t replicaId_;
  std::string streamId_;
  std::uint32_t cols_;
  std::uint32_t rows_;
  Kind kind_;
  std::deque<std::shared_ptr<std::string>> outgoing_;
  bool writing_ = false;
  bool closed_ = false;
};

}  // namespace highcube::admin
