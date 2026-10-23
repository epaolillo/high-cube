#include "github/github_http_client.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <chrono>
#include <stdexcept>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace highcube::admin {

GithubHttpResponse GithubHttpClient::request(const std::string& method, const std::string& path,
                                             const std::string& bearerToken, const std::string& body,
                                             const std::string& accept) const {
  try {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(ssl::verify_peer);

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), "api.github.com")) {
      throw std::runtime_error("failed to set SNI hostname");
    }

    const auto results = resolver.resolve("api.github.com", "443");
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(15));
    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(ssl::stream_base::client);

    http::verb verb = http::verb::get;
    if (method == "POST") {
      verb = http::verb::post;
    } else if (method == "DELETE") {
      verb = http::verb::delete_;
    } else if (method == "PATCH") {
      verb = http::verb::patch;
    }

    http::request<http::string_body> req{verb, path, 11};
    req.set(http::field::host, "api.github.com");
    req.set(http::field::user_agent, "HighCube");
    req.set(http::field::accept, accept);
    req.set("X-GitHub-Api-Version", "2022-11-28");
    if (!bearerToken.empty()) {
      req.set(http::field::authorization, "Bearer " + bearerToken);
    }
    if (!body.empty()) {
      req.set(http::field::content_type, "application/json");
      req.body() = body;
      req.prepare_payload();
    }

    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(15));
    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    beast::error_code ec;
    stream.shutdown(ec);

    GithubHttpResponse out;
    out.status = static_cast<int>(res.result_int());
    out.body = res.body();
    return out;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("GitHub API request failed: ") + ex.what());
  }
}

nlohmann::json GithubHttpClient::getJson(const std::string& path,
                                         const std::string& bearerToken) const {
  const auto res = request("GET", path, bearerToken);
  if (res.status < 200 || res.status >= 300) {
    throw std::runtime_error("GitHub API GET " + path + " failed: HTTP " + std::to_string(res.status) +
                             " " + res.body);
  }
  return nlohmann::json::parse(res.body.empty() ? "{}" : res.body);
}

nlohmann::json GithubHttpClient::postJson(const std::string& path, const std::string& bearerToken,
                                          const nlohmann::json& body) const {
  const auto res = request("POST", path, bearerToken, body.dump());
  if (res.status < 200 || res.status >= 300) {
    throw std::runtime_error("GitHub API POST " + path + " failed: HTTP " + std::to_string(res.status) +
                             " " + res.body);
  }
  return nlohmann::json::parse(res.body.empty() ? "{}" : res.body);
}

nlohmann::json GithubHttpClient::patchJson(const std::string& path, const std::string& bearerToken,
                                           const nlohmann::json& body) const {
  const auto res = request("PATCH", path, bearerToken, body.dump());
  if (res.status < 200 || res.status >= 300) {
    throw std::runtime_error("GitHub API PATCH " + path + " failed: HTTP " +
                             std::to_string(res.status) + " " + res.body);
  }
  return nlohmann::json::parse(res.body.empty() ? "{}" : res.body);
}

}  // namespace highcube::admin
