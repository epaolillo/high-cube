#pragma once

#include "domain/models.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <cctype>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace highcube::admin {

namespace http = boost::beast::http;
using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;

struct RequestContext {
  Request request;
  std::string targetPath;
  std::string method;
  std::unordered_map<std::string, std::string> params;
  std::unordered_map<std::string, std::string> query;
  std::optional<User> user;
};

inline Response jsonResponse(http::status status, const nlohmann::json& body) {
  Response res{status, 11};
  res.set(http::field::content_type, "application/json");
  res.set(http::field::access_control_allow_origin, "*");
  res.keep_alive(false);
  res.body() = body.dump();
  res.prepare_payload();
  return res;
}

inline Response errorResponse(http::status status, const std::string& message) {
  return jsonResponse(status, {{"error", message}});
}

inline Response emptyResponse(http::status status) {
  Response res{status, 11};
  res.set(http::field::access_control_allow_origin, "*");
  res.keep_alive(false);
  res.prepare_payload();
  return res;
}

inline ListQuery parseListQuery(const std::unordered_map<std::string, std::string>& query) {
  ListQuery list;
  if (auto it = query.find("page"); it != query.end()) {
    list.page = std::stoi(it->second);
  }
  if (auto it = query.find("pageSize"); it != query.end()) {
    list.pageSize = std::stoi(it->second);
    if (list.pageSize != 10 && list.pageSize != 100 && list.pageSize != 1000) {
      throw std::invalid_argument("pageSize must be 10, 100, or 1000");
    }
  }
  if (auto it = query.find("sort"); it != query.end()) {
    list.sort = it->second;
  }
  if (auto it = query.find("order"); it != query.end()) {
    list.order = it->second;
  }
  if (auto it = query.find("q"); it != query.end()) {
    list.q = it->second;
  }
  if (auto it = query.find("type"); it != query.end()) {
    list.typeFilter = it->second;
  }
  return list;
}

inline std::string urlDecode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
      const auto hex = in.substr(i + 1, 2);
      out.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
      i += 2;
      continue;
    }
    out.push_back(in[i] == '+' ? ' ' : in[i]);
  }
  return out;
}

inline std::unordered_map<std::string, std::string> parseQuery(const std::string& target) {
  std::unordered_map<std::string, std::string> out;
  const auto qpos = target.find('?');
  if (qpos == std::string::npos) {
    return out;
  }
  std::string qs = target.substr(qpos + 1);
  while (!qs.empty()) {
    auto amp = qs.find('&');
    std::string part = amp == std::string::npos ? qs : qs.substr(0, amp);
    auto eq = part.find('=');
    if (eq != std::string::npos) {
      out[urlDecode(part.substr(0, eq))] = urlDecode(part.substr(eq + 1));
    }
    if (amp == std::string::npos) {
      break;
    }
    qs = qs.substr(amp + 1);
  }
  return out;
}

inline std::string pathOnly(const std::string& target) {
  const auto qpos = target.find('?');
  return qpos == std::string::npos ? target : target.substr(0, qpos);
}

using HandlerFn = std::function<Response(RequestContext&)>;

}  // namespace highcube::admin
