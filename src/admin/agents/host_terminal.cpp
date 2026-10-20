#include "agents/host_terminal.hpp"

#include "agents/agent_hub.hpp"

#include <nlohmann/json.hpp>

#include <chrono>

namespace highcube::admin {

HostTerminalRelay::HostTerminalRelay(tcp::socket socket, AgentHub& hub, int64_t serverId,
                                     std::string streamId, std::uint32_t cols, std::uint32_t rows,
                                     Kind kind, int64_t replicaId)
    : ws_(std::move(socket)),
      hub_(hub),
      serverId_(serverId),
      replicaId_(replicaId),
      streamId_(std::move(streamId)),
      cols_(cols),
      rows_(rows),
      kind_(kind) {}

void HostTerminalRelay::start(http::request<http::string_body> req) {
  auto self = shared_from_this();
  auto timeouts = websocket::stream_base::timeout::suggested(beast::role_type::server);
  // Pings every 20s keep nginx (60s) and Cloudflare (100s) from closing an idle terminal.
  timeouts.idle_timeout = std::chrono::seconds(40);
  timeouts.keep_alive_pings = true;
  ws_.set_option(timeouts);
  ws_.async_accept(req, [self](beast::error_code ec) {
    if (ec) {
      return;
    }
    self->ws_.binary(true);
    std::weak_ptr<HostTerminalRelay> weak = self;
    self->hub_.registerHostTerminal(
        self->streamId_, self->serverId_,
        [weak](const std::string& data) {
          if (auto relay = weak.lock()) {
            relay->onOutput(data);
          }
        },
        [weak]() {
          if (auto relay = weak.lock()) {
            relay->closeSocket();
          }
        });
    if (self->kind_ == Kind::Container) {
      self->hub_.openContainerTerminal(self->replicaId_, self->streamId_, self->cols_, self->rows_);
    } else {
      self->hub_.openHostTerminal(self->serverId_, self->streamId_, self->cols_, self->rows_);
    }
    self->doRead();
  });
}

void HostTerminalRelay::onOutput(const std::string& data) {
  auto chunk = std::make_shared<std::string>(data);
  net::post(ws_.get_executor(), [self = shared_from_this(), chunk]() {
    if (self->closed_) {
      return;
    }
    self->outgoing_.push_back(chunk);
    if (!self->writing_) {
      self->doWrite();
    }
  });
}

void HostTerminalRelay::closeSocket() {
  net::post(ws_.get_executor(), [self = shared_from_this()]() {
    if (self->closed_) {
      return;
    }
    self->closed_ = true;
    self->outgoing_.clear();
    beast::error_code ec;
    self->ws_.close(websocket::close_code::going_away, ec);
  });
}

void HostTerminalRelay::doRead() {
  auto self = shared_from_this();
  ws_.async_read(buffer_, [self](beast::error_code ec, std::size_t) {
    if (ec) {
      self->shutdown();
      return;
    }
    const auto bytes = self->buffer_.cdata();
    std::string data(static_cast<const char*>(bytes.data()), bytes.size());
    self->buffer_.consume(self->buffer_.size());
    if (self->ws_.got_text()) {
      const auto message = nlohmann::json::parse(data, nullptr, false);
      if (!message.is_discarded() && message.value("type", "") == "resize") {
        self->hub_.resizeHostTerminal(self->serverId_, self->streamId_, message.value("cols", 0u),
                                      message.value("rows", 0u));
        self->doRead();
        return;
      }
    }
    self->hub_.writeHostTerminal(self->serverId_, self->streamId_, data);
    self->doRead();
  });
}

void HostTerminalRelay::doWrite() {
  if (closed_ || outgoing_.empty()) {
    writing_ = false;
    return;
  }
  writing_ = true;
  auto data = outgoing_.front();
  ws_.async_write(net::buffer(*data), [self = shared_from_this(), data](beast::error_code ec, std::size_t) {
    if (!self->outgoing_.empty() && self->outgoing_.front() == data) {
      self->outgoing_.pop_front();
    }
    if (ec) {
      self->writing_ = false;
      self->shutdown();
      return;
    }
    self->doWrite();
  });
}

void HostTerminalRelay::shutdown() {
  if (closed_) {
    return;
  }
  closed_ = true;
  outgoing_.clear();
  hub_.closeHostTerminal(serverId_, streamId_);
  beast::error_code ec;
  ws_.close(websocket::close_code::normal, ec);
}

}  // namespace highcube::admin
