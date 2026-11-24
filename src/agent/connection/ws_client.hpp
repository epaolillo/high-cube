#pragma once

#include "config/agent_config.hpp"
#include "metrics/process_snapshot.hpp"
#include "runtime/docker_runtime.hpp"
#include "terminal/terminal_session.hpp"
#include "vpn/vpn_manager.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <highcube.pb.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace highcube::agent {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class WsClient : public std::enable_shared_from_this<WsClient> {
public:
  WsClient(net::io_context& ioc, AgentConfig config, DockerRuntime& runtime);

  void start();
  void send(const ::highcube::Envelope& envelope);

private:
  void resolve();
  void onResolve(beast::error_code ec, tcp::resolver::results_type results);
  void onConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type);
  void onHandshake(beast::error_code ec);
  void onSslHandshake(beast::error_code ec);
  void doRead();
  void handleEnvelope(const ::highcube::Envelope& envelope);
  void scheduleReconnect();
  void startHeartbeat();
  void startMetrics();
  void startContainerMetrics();
  void reportContainerMetrics();
  void startProcessSnapshots();
  void startVpnStatus();
  void reportVpnStatus();
  void runQuickCommand(const ::highcube::QuickCommand& order);
  void doWrite();
  void closeTerminals();
  void fail(beast::error_code ec, const char* what);

  bool useTls_ = false;
  std::string host_;
  std::string port_;
  std::string path_;

  net::io_context& ioc_;
  AgentConfig config_;
  DockerRuntime& runtime_;
  ssl::context sslCtx_{ssl::context::tlsv12_client};
  tcp::resolver resolver_;
  std::unique_ptr<websocket::stream<beast::tcp_stream>> ws_;
  std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> wss_;
  beast::flat_buffer buffer_;
  net::steady_timer reconnectTimer_;
  net::steady_timer heartbeatTimer_;
  net::steady_timer metricsTimer_;
  net::steady_timer containerMetricsTimer_;
  net::steady_timer processSnapshotTimer_;
  net::steady_timer vpnStatusTimer_;
  std::deque<std::shared_ptr<std::string>> outgoing_;
  std::unordered_map<std::string, std::shared_ptr<TerminalSession>> terminals_;
  bool connected_ = false;
  bool writing_ = false;
  bool reconnectPending_ = false;
  std::atomic<bool> containerReportRunning_{false};
  std::atomic<bool> processSnapshotRunning_{false};
  std::atomic<int> quickCommandsRunning_{0};
  std::atomic<bool> vpnStatusRunning_{false};
  // Shared with detached worker threads that run wg-quick and apt-get.
  std::shared_ptr<VpnManager> vpn_ = std::make_shared<VpnManager>();
  // Only touched by the snapshot worker thread; processSnapshotRunning_ serializes it.
  ProcessSampler processSampler_;
  // Last cgroup usage_usec per container id, for CPU attribution deltas.
  std::unordered_map<std::string, unsigned long long> previousCgroupUsec_;
};

}  // namespace highcube::agent
