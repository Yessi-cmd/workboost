#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace workboost {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

enum class LogEvent {
  ApplicationStarted,
  ApplicationStopped,
  ConfigurationWarning,
  MonitorFailed,
  SafeModeEntered,
  CodingModeStarted,
  ActionRejected,
  ActionFailed,
  RecoveryCompleted,
  ReportWritten,
};

// Local structured event log. The API deliberately accepts no arbitrary text,
// process identity, path, address, or payload fields.
class Logger {
 public:
  static Logger& Instance();

  bool Initialize(const std::filesystem::path& workboost_data_directory,
                  LogLevel minimum_level = LogLevel::Info,
                  std::string* error = nullptr);
  void Write(LogLevel level, LogEvent event,
             std::uint32_t error_code = 0) noexcept;
  void Shutdown() noexcept;
  [[nodiscard]] bool Initialized() const;

 private:
  Logger() = default;
  bool RotateLocked(std::string* error = nullptr);

  mutable std::mutex mutex_;
  std::ofstream stream_;
  LogLevel minimum_level_{LogLevel::Info};
  std::filesystem::path log_path_;
  std::filesystem::path backup_path_;
  std::uintmax_t current_size_{};
};

std::string ToString(LogLevel value);
std::string ToString(LogEvent value);

}  // namespace workboost
