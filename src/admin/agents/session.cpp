#include "agents/session.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace highcube::admin {

AgentSession::AgentSession(tcp::socket socket, MessageHandler onMessage)
    : ws_(std::move(socket)), onMessage_(std::move(onMessage)) {}

void AgentSession::start(http::request<http::string_body> req, CloseHandler onClose) {
  onClose_ = std::move(onClose);
  auto self = shared_from_this();
  ws_.async_accept(req, [self](beast::error_code ec) {
    if (ec) {
      self->fail(ec, "accept");
      return;
    }
    self->ws_.binary(true);
    self->doRead();
  });
}

void AgentSession::send(const ::highcube::Envelope& envelope) {
  if (closed_) {
    return;
  }
  auto data = std::make_shared<std::string>(envelope.SerializeAsString());
  net::post(ws_.get_executor(), [self = shared_from_this(), data]() {
    if (self->closed_) {
      return;
    }
    self->outgoing_.push_back(data);
    if (!self->writing_) {
      self->doWrite();
    }
  });
}

void AgentSession::doWrite() {
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
      self->fail(ec, "write");
      return;
    }
    self->doWrite();
  });
}

void AgentSession::close() {
  beast::error_code ec;
  ws_.close(websocket::close_code::normal, ec);
}

void AgentSession::doRead() {
  auto self = shared_from_this();
  ws_.async_read(buffer_, [self](beast::error_code ec, std::size_t) {
    if (ec) {
      self->fail(ec, "read");
      return;
    }
    ::highcube::Envelope envelope;
    const auto bytes = self->buffer_.cdata();
    if (envelope.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
      if (self->onMessage_) {
        self->onMessage_(self, envelope);
      }
    }
    self->buffer_.consume(self->buffer_.size());
    self->doRead();
  });
}

void AgentSession::fail(beast::error_code ec, const char* what) {
  if (!closed_) {
    closed_ = true;
    outgoing_.clear();
    writing_ = false;
    if (onClose_) {
      onClose_(shared_from_this());
    }
  }
  if (ec == net::error::operation_aborted || ec == websocket::error::closed) {
    return;
  }
  spdlog::warn("agent session {}: {}", what, ec.message());
}

}  // namespace highcube::admin
