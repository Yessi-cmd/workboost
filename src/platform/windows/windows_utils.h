#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

namespace workboost::windows {

struct WindowsError {
  unsigned long code{};
  std::string context;
  std::string message;

  [[nodiscard]] std::string Describe() const;
};

WindowsError LastError(const std::string& context,
                       unsigned long code = ::GetLastError());

class UniqueHandle {
 public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
  ~UniqueHandle();

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;
  UniqueHandle(UniqueHandle&& other) noexcept;
  UniqueHandle& operator=(UniqueHandle&& other) noexcept;

  [[nodiscard]] HANDLE Get() const { return handle_; }
  [[nodiscard]] bool Valid() const {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }
  HANDLE Release();
  void Reset(HANDLE handle = nullptr);

 private:
  HANDLE handle_{};
};

std::string WideToUtf8(const std::wstring& value);
std::wstring Utf8ToWide(const std::string& value);
std::filesystem::path ExecutableDirectory();
std::filesystem::path LocalAppDataDirectory();
std::string Iso8601Now();
std::string JsonEscape(const std::string& value);
std::string MaskIpAddress(const std::string& address);

bool AtomicWriteUtf8(const std::filesystem::path& path,
                     const std::string& content, std::string* error);
std::optional<std::string> ReadUtf8(const std::filesystem::path& path,
                                    std::string* error = nullptr);

}  // namespace workboost::windows
