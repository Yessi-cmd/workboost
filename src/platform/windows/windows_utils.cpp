#include "platform/windows/windows_utils.h"

#include <shlobj.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace workboost::windows {

constexpr std::uintmax_t kMaximumLocalStateBytes = 16 * 1024 * 1024;

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

std::optional<std::filesystem::path> CurrentExecutablePath(
    WindowsError* error) {
  std::vector<wchar_t> buffer(1024);
  for (;;) {
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      if (error) *error = LastError("Get current executable path");
      return std::nullopt;
    }
    if (length < buffer.size() - 1) {
      return std::filesystem::path(
          std::wstring(buffer.data(), static_cast<std::size_t>(length)));
    }
    if (buffer.size() >= 32768) {
      if (error) {
        *error = LastError("Get complete current executable path",
                           ERROR_INSUFFICIENT_BUFFER);
      }
      return std::nullopt;
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::filesystem::path ExecutableDirectory() {
  const auto executable = CurrentExecutablePath();
  if (executable) return executable->parent_path();
  std::error_code filesystem_error;
  auto fallback = std::filesystem::current_path(filesystem_error);
  if (!filesystem_error) return fallback;
  fallback = std::filesystem::temp_directory_path(filesystem_error);
  return filesystem_error ? std::filesystem::path{} : fallback;
}

std::optional<std::filesystem::path> KnownLocalAppDataDirectory(
    WindowsError* error) {
  std::array<wchar_t, MAX_PATH> buffer{};
  const HRESULT result = SHGetFolderPathW(
      nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_DONT_VERIFY, nullptr,
      SHGFP_TYPE_CURRENT, buffer.data());
  if (FAILED(result) || buffer[0] == L'\0') {
    if (error) {
      *error = WindowsError{static_cast<unsigned long>(result),
                            "Resolve Local AppData known folder",
                            "SHGetFolderPathW returned a failing HRESULT"};
    }
    return std::nullopt;
  }
  return std::filesystem::path(buffer.data()) / L"WorkBoost";
}

std::filesystem::path LocalAppDataDirectory() {
  const auto known_folder = KnownLocalAppDataDirectory();
  if (known_folder) return *known_folder;
  std::error_code filesystem_error;
  const auto temporary =
      std::filesystem::temp_directory_path(filesystem_error);
  return (filesystem_error ? std::filesystem::path{} : temporary) /
         L"WorkBoost";
}

std::optional<std::filesystem::path> ProcessImagePath(
    std::uint32_t pid, WindowsError* error) {
  UniqueHandle process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process.Valid()) {
    if (error) *error = LastError("OpenProcess for image verification");
    return std::nullopt;
  }
  std::vector<wchar_t> buffer(1024);
  for (;;) {
    DWORD size = static_cast<DWORD>(buffer.size());
    if (QueryFullProcessImageNameW(process.Get(), 0, buffer.data(), &size)) {
      return std::filesystem::path(
          std::wstring(buffer.data(), static_cast<std::size_t>(size)));
    }
    const DWORD code = GetLastError();
    if (code != ERROR_INSUFFICIENT_BUFFER || buffer.size() >= 32768) {
      if (error) *error = LastError("QueryFullProcessImageNameW", code);
      return std::nullopt;
    }
    buffer.resize(buffer.size() * 2);
  }
}

bool ProcessIsElevated(std::uint32_t pid, bool* elevated,
                       WindowsError* error) {
  if (elevated == nullptr) {
    if (error) {
      *error = LastError("Validate elevation output", ERROR_INVALID_PARAMETER);
    }
    return false;
  }
  UniqueHandle process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process.Valid()) {
    if (error) *error = LastError("OpenProcess for elevation verification");
    return false;
  }
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(process.Get(), TOKEN_QUERY, &raw_token)) {
    if (error) *error = LastError("OpenProcessToken for elevation verification");
    return false;
  }
  UniqueHandle token(raw_token);
  TOKEN_ELEVATION elevation{};
  DWORD bytes = 0;
  if (!GetTokenInformation(token.Get(), TokenElevation, &elevation,
                           sizeof(elevation), &bytes)) {
    if (error) *error = LastError("GetTokenInformation TokenElevation");
    return false;
  }
  *elevated = elevation.TokenIsElevated != 0;
  return true;
}

bool PathEqualsInsensitive(const std::filesystem::path& left,
                           const std::filesystem::path& right) {
  std::wstring left_value = left.lexically_normal().wstring();
  std::wstring right_value = right.lexically_normal().wstring();
  std::transform(left_value.begin(), left_value.end(), left_value.begin(),
                 [](wchar_t value) {
                   return static_cast<wchar_t>(towlower(value));
                 });
  std::transform(right_value.begin(), right_value.end(), right_value.begin(),
                 [](wchar_t value) {
                   return static_cast<wchar_t>(towlower(value));
                 });
  return left_value == right_value;
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

std::string DetectUILanguage() {
  return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? "zh"
                                                                    : "en";
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
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      if (error) *error = "cannot create directory: " + ec.message();
      return false;
    }
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
  std::error_code size_error;
  const auto size = std::filesystem::file_size(path, size_error);
  if (size_error || size > kMaximumLocalStateBytes) {
    if (error) {
      *error = size_error
                   ? "cannot size " + path.string() + ": " +
                         size_error.message()
                   : "local state exceeds 16 MiB limit";
    }
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

}  // namespace workboost::windows
