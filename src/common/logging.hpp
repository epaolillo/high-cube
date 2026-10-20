#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace highcube {

inline const char* kLogPattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v";

inline void initLogging(const std::string& name) {
  auto logger = spdlog::stdout_color_mt(name);
  logger->set_pattern(kLogPattern);
  logger->set_level(spdlog::level::info);
  spdlog::set_default_logger(logger);
}

// Reconfigures the default logger to write to both the console and a rotating
// file. The file rotates daily and keeps `retentionDays` files (about one week
// by default). All settings come from the configuration file.
inline void initFileLogging(const std::string& name, const std::string& file, int retentionDays,
                            const std::string& level) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto parent = fs::path(file).parent_path();
  if (!parent.empty()) {
    fs::create_directories(parent, ec);
  }

  std::vector<spdlog::sink_ptr> sinks;
  sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
  // Rotate at midnight. `max_files` bounds how many daily files are kept.
  const int keep = retentionDays > 0 ? retentionDays : 7;
  sinks.push_back(std::make_shared<spdlog::sinks::daily_file_sink_mt>(
      file, 0, 0, false, static_cast<uint16_t>(keep)));

  auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
  logger->set_pattern(kLogPattern);
  auto parsedLevel = spdlog::level::from_str(level);
  // from_str returns "off" for unknown values; fall back to info in that case
  // unless the user explicitly asked to turn logging off.
  if (parsedLevel == spdlog::level::off && level != "off") {
    parsedLevel = spdlog::level::info;
  }
  logger->set_level(parsedLevel);
  logger->flush_on(spdlog::level::info);
  spdlog::set_default_logger(logger);
  spdlog::flush_every(std::chrono::seconds(3));
}

}  // namespace highcube
