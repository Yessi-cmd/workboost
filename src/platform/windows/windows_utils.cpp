#include "platform/windows/windows_utils.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace workboost::windows {

std::string WindowsError::Describe() const {
  std::ostringstream output;
  output << context << " failed with Windows error " << code;
  if (!message.empty()) output << ": " << message;
  return output.str();
}

WindowsError LastError(const std::string& context, unsigned long code) {
  LPWSTR message_buffer = nullptr;
  const DWORD size = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<LPWSTR>(&message_buffer), 0, nullptr);
  std::string message;
  if (size != 0 && message_buffer != nullptr) {
    message = WideToUtf8(std::wstring(message_buffer, size));
    while (!message.empty() &&
           (message.back() == '\r' || message.back() == '\n' ||
            message.back() == ' ')) {
      message.pop_back();
    }
  }
  if (message_buffer != nullptr) LocalFree(message_buffer);
  return WindowsError{code, context, std::move(message)};
}

UniqueHandle::~UniqueHandle() { Reset(); }

UniqueHandle::UniqueHandle(UniqueHandle&& other) noexcept
    : handle_(other.Release()) {}

UniqueHandle& UniqueHandle::operator=(UniqueHandle&& other) noexcept {
  if (this != &other) Reset(other.Release());
  return *this;
}

HANDLE UniqueHandle::Release() {
  const HANDLE result = handle_;
  handle_ = nullptr;
  return result;
}

void UniqueHandle::Reset(HANDLE handle) {
  if (Valid()) CloseHandle(handle_);
  handle_ = handle;
}

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                            static_cast<int>(value.size()),
                                            nullptr, 0, nullptr, nullptr);
  if (required <= 0) return {};
  std::string result(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), required, nullptr, nullptr);
  return result;
}

std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) return {};
  const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            value.data(),
                                            static_cast<int>(value.size()),
                                            nullptr, 0);
  if (required <= 0) return {};
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), required);
  return result;
}

std::filesystem::path ExecutableDirectory() {
  std::vector<wchar_t> buffer(1024);
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0) return std::filesystem::current_path();
    if (length < buffer.size() - 1) {
      return std::filesystem::path(
                 std::wstring(buffer.data(), static_cast<std::size_t>(length)))
          .parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::filesystem::path LocalAppDataDirectory() {
  std::array<wchar_t, 32768> buffer{};
  const DWORD length = GetEnvironmentVariableW(
      L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length > 0 && length < buffer.size()) {
    return std::filesystem::path(std::wstring(buffer.data(), length)) /
           L"WorkBoost";
  }
  return std::filesystem::temp_directory_path() / L"WorkBoost";
}

std::string Iso8601Now() {
  SYSTEMTIME time{};
  GetSystemTime(&time);
  char buffer[40]{};
  std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                time.wSecond, time.wMilliseconds);
  return buffer;
}

std::string JsonEscape(const std::string& value) {
  std::ostringstream output;
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"': output << "\\\""; break;
      case '\\': output << "\\\\"; break;
      case '\b': output << "\\b"; break;
      case '\f': output << "\\f"; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default:
        if (ch < 0x20) {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<int>(ch) << std::dec;
        } else {
          output << static_cast<char>(ch);
        }
    }
  }
  return output.str();
}

std::string MaskIpAddress(const std::string& address) {
  if (address.find(':') != std::string::npos) {
    const auto first = address.find(':');
    const auto second = address.find(':', first + 1);
    if (second == std::string::npos) return address;
    return address.substr(0, second) + ":****:****";
  }
  const auto last_dot = address.rfind('.');
  if (last_dot == std::string::npos) return address;
  return address.substr(0, last_dot + 1) + "x";
}

bool AtomicWriteUtf8(const std::filesystem::path& path,
                     const std::string& content, std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    if (error) *error = "cannot create directory: " + ec.message();
    return false;
  }
  const auto temporary = path.wstring() + L".tmp";
  {
    std::ofstream stream(std::filesystem::path(temporary),
                         std::ios::binary | std::ios::trunc);
    if (!stream) {
      if (error) *error = "cannot write temporary file";
      return false;
    }
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.flush();
    if (!stream) {
      if (error) *error = "failed while writing temporary file";
      return false;
    }
  }
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    if (error) *error = LastError("MoveFileExW").Describe();
    DeleteFileW(temporary.c_str());
    return false;
  }
  return true;
}

std::optional<std::string> ReadUtf8(const std::filesystem::path& path,
                                    std::string* error) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    if (error) *error = "cannot open " + path.string();
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

}  // namespace workboost::windows
