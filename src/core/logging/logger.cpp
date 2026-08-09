#include "core/logging/logger.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <system_error>

namespace workboost {
namespace {

constexpr std::uintmax_t kMaximumLogBytes = 1024 * 1024;

std::int64_t UnixTimeMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

Logger& Logger::Instance() {
  static Logger logger;
  return logger;
}

bool Logger::RotateLocked(std::string* error) {
  stream_.close();
  std::error_code filesystem_error;
  std::filesystem::remove(backup_path_, filesystem_error);
  if (filesystem_error) {
    if (error) *error = "cannot replace local event log backup";
    stream_.open(log_path_, std::ios::binary | std::ios::app);
    return false;
  }
  std::filesystem::rename(log_path_, backup_path_, filesystem_error);
  if (filesystem_error) {
    if (error) *error = "cannot rotate local event log";
    stream_.open(log_path_, std::ios::binary | std::ios::app);
    return false;
  }
  stream_.open(log_path_, std::ios::binary | std::ios::app);
  if (!stream_) {
    if (error) *error = "cannot open new local event log";
    return false;
  }
  current_size_ = 0;
  return true;
}

bool Logger::Initialize(
    const std::filesystem::path& workboost_data_directory,
    LogLevel minimum_level, std::string* error) {
  std::lock_guard<std::mutex> lock(mutex_);
  stream_.close();
  minimum_level_ = minimum_level;
  const auto log_directory = workboost_data_directory / "logs";
  log_path_ = log_directory / "workboost.log";
  backup_path_ = log_directory / "workboost.1.log";
  current_size_ = 0;
  std::error_code filesystem_error;
  std::filesystem::create_directories(log_directory, filesystem_error);
  if (filesystem_error) {
    if (error) *error = "cannot create local log directory";
    return false;
  }
  const auto size = std::filesystem::file_size(log_path_, filesystem_error);
  if (!filesystem_error) current_size_ = size;
  if (!filesystem_error && size >= kMaximumLogBytes) {
    return RotateLocked(error);
  }
  filesystem_error.clear();
  stream_.open(log_path_, std::ios::binary | std::ios::app);
  if (!stream_) {
    if (error) *error = "cannot open local event log";
    return false;
  }
  return true;
}

void Logger::Write(LogLevel level, LogEvent event,
                   std::uint32_t error_code) noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_ || level < minimum_level_) return;
    std::ostringstream record;
    record << "{\"timestamp_unix_ms\":" << UnixTimeMilliseconds()
           << ",\"level\":\"" << ToString(level) << "\",\"event\":\""
           << ToString(event) << "\",\"error_code\":" << error_code
           << "}\n";
    const std::string text = record.str();
    if (current_size_ != 0 &&
        text.size() > kMaximumLogBytes -
                          std::min(current_size_, kMaximumLogBytes)) {
      if (!RotateLocked()) return;
    }
    stream_.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream_) return;
    current_size_ += text.size();
    stream_.flush();
  } catch (...) {
  }
}

void Logger::Shutdown() noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    stream_.close();
    current_size_ = 0;
  } catch (...) {
  }
}

bool Logger::Initialized() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stream_.is_open();
}

std::string ToString(LogLevel value) {
  switch (value) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
  }
  return "ERROR";
}

std::string ToString(LogEvent value) {
  switch (value) {
    case LogEvent::ApplicationStarted: return "ApplicationStarted";
    case LogEvent::ApplicationStopped: return "ApplicationStopped";
    case LogEvent::ConfigurationWarning: return "ConfigurationWarning";
    case LogEvent::MonitorFailed: return "MonitorFailed";
    case LogEvent::SafeModeEntered: return "SafeModeEntered";
    case LogEvent::CodingModeStarted: return "CodingModeStarted";
    case LogEvent::ActionRejected: return "ActionRejected";
    case LogEvent::ActionFailed: return "ActionFailed";
    case LogEvent::RecoveryCompleted: return "RecoveryCompleted";
    case LogEvent::ReportWritten: return "ReportWritten";
  }
  return "MonitorFailed";
}

}  // namespace workboost
