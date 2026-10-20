#pragma once

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace highcube {

// Simple key=value config loader. Lines starting with # are comments.
class Config {
public:
  void loadFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
      throw std::runtime_error("cannot open config file: " + path);
    }
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }
      const auto eq = line.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      values_[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
  }

  std::string get(const std::string& key, const std::string& fallback = "") const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
      return fallback;
    }
    return it->second;
  }

  int getInt(const std::string& key, int fallback) const {
    const auto value = get(key);
    if (value.empty()) {
      return fallback;
    }
    return std::stoi(value);
  }

  bool has(const std::string& key) const {
    return values_.find(key) != values_.end();
  }

private:
  static std::string trim(std::string value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
      value.pop_back();
    }
    return value;
  }

  std::unordered_map<std::string, std::string> values_;
};

}  // namespace highcube
