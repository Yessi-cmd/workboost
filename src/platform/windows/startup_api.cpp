#include "platform/windows/startup_api.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace workboost::windows {
namespace {

constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr DWORD kMaximumRegistryValueBytes = 64 * 1024;

class UniqueRegistryKey {
 public:
  UniqueRegistryKey() = default;
  explicit UniqueRegistryKey(HKEY value) : value_(value) {}
  ~UniqueRegistryKey() {
    if (value_ != nullptr) RegCloseKey(value_);
  }

  UniqueRegistryKey(const UniqueRegistryKey&) = delete;
  UniqueRegistryKey& operator=(const UniqueRegistryKey&) = delete;

  [[nodiscard]] HKEY Get() const { return value_; }
  [[nodiscard]] bool Valid() const { return value_ != nullptr; }

 private:
  HKEY value_{};
};

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

std::wstring ExpandEnvironment(const std::wstring& value) {
  const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
  if (required == 0 || required > 32768) return value;
  std::vector<wchar_t> buffer(required, L'\0');
  if (ExpandEnvironmentStringsW(value.c_str(), buffer.data(), required) == 0) {
    return value;
  }
  return buffer.data();
}

std::wstring ExecutableToken(const std::wstring& command) {
  const std::size_t begin = command.find_first_not_of(L" \t");
  if (begin == std::wstring::npos) return {};
  if (command[begin] == L'"') {
    const std::size_t end = command.find(L'"', begin + 1);
    if (end != std::wstring::npos) {
      return command.substr(begin + 1, end - begin - 1);
    }
  }
  std::wstring lower = command;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](wchar_t character) {
                   return static_cast<wchar_t>(towlower(character));
                 });
  std::size_t extension_end = std::wstring::npos;
  for (const wchar_t* extension : {L".exe", L".com", L".bat", L".cmd"}) {
    const std::size_t position = lower.find(extension, begin);
    if (position != std::wstring::npos) {
      extension_end = std::min(extension_end, position + 4);
    }
  }
  if (extension_end != std::wstring::npos) {
    return command.substr(begin, extension_end - begin);
  }
  return {};
}

std::string ExecutableNameFromCommand(const std::wstring& command) {
  std::wstring executable = ExecutableToken(command);
  if (!executable.empty()) {
    executable = ExpandEnvironment(executable);
    return WideToUtf8(std::filesystem::path(executable).filename().wstring());
  }
  int argument_count = 0;
  LPWSTR* arguments = CommandLineToArgvW(command.c_str(), &argument_count);
  if (arguments == nullptr || argument_count == 0) {
    if (arguments != nullptr) LocalFree(arguments);
    return {};
  }
  executable = ExpandEnvironment(arguments[0]);
  LocalFree(arguments);
  return WideToUtf8(std::filesystem::path(executable).filename().wstring());
}

bool AppendRegistryRun(HKEY root, REGSAM view, StartupScope scope,
                       const char* location,
                       std::vector<StartupEntrySnapshot>* entries,
                       WindowsError* error) {
  HKEY raw_key = nullptr;
  const LSTATUS opened =
      RegOpenKeyExW(root, kRunKey, 0, KEY_QUERY_VALUE | view, &raw_key);
  if (opened == ERROR_FILE_NOT_FOUND) return true;
  if (opened != ERROR_SUCCESS) {
    if (error) *error = LastError("Open startup Run key", opened);
    return false;
  }
  UniqueRegistryKey key(raw_key);
  DWORD value_count = 0;
  DWORD maximum_name = 0;
  DWORD maximum_data = 0;
  const LSTATUS queried = RegQueryInfoKeyW(
      key.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
      &value_count, &maximum_name, &maximum_data, nullptr, nullptr);
  if (queried != ERROR_SUCCESS) {
    if (error) *error = LastError("Size startup Run values", queried);
    return false;
  }
  maximum_name = std::min<DWORD>(maximum_name + 1, 32768);
  maximum_data = std::min<DWORD>(maximum_data + sizeof(wchar_t),
                                 kMaximumRegistryValueBytes);
  std::vector<wchar_t> name(std::max<DWORD>(maximum_name, 2), L'\0');
  std::vector<BYTE> data(std::max<DWORD>(maximum_data, sizeof(wchar_t)), 0);
  for (DWORD index = 0; index < value_count; ++index) {
    DWORD name_length = static_cast<DWORD>(name.size());
    DWORD data_length = static_cast<DWORD>(data.size());
    DWORD type = 0;
    const LSTATUS enumerated = RegEnumValueW(
        key.Get(), index, name.data(), &name_length, nullptr, &type,
        data.data(), &data_length);
    if (enumerated == ERROR_MORE_DATA) continue;
    if (enumerated != ERROR_SUCCESS) {
      if (error) *error = LastError("Enumerate startup Run value", enumerated);
      return false;
    }
    if ((type != REG_SZ && type != REG_EXPAND_SZ) ||
        data_length < sizeof(wchar_t)) {
      continue;
    }
    if (data_length + sizeof(wchar_t) <= data.size()) {
      std::fill(data.begin() + data_length,
                data.begin() + data_length + sizeof(wchar_t), 0);
    }
    const auto* command = reinterpret_cast<const wchar_t*>(data.data());
    entries->push_back(StartupEntrySnapshot{
        scope, StartupSource::RegistryRun, location,
        WideToUtf8(std::wstring(name.data(), name_length)),
        ExecutableNameFromCommand(command)});
    std::fill(data.begin(), data.end(), 0);
  }
  return true;
}

bool AppendStartupFolder(int folder, StartupScope scope, const char* location,
                         std::vector<StartupEntrySnapshot>* entries,
                         WindowsError* error) {
  std::vector<wchar_t> path(MAX_PATH, L'\0');
  const HRESULT resolved = SHGetFolderPathW(
      nullptr, folder | CSIDL_FLAG_DONT_VERIFY, nullptr, SHGFP_TYPE_CURRENT,
      path.data());
  if (FAILED(resolved)) {
    if (error) {
      *error = WindowsError{static_cast<unsigned long>(resolved),
                            "Resolve Startup folder",
                            "SHGetFolderPathW returned a failing HRESULT"};
    }
    return false;
  }
  const std::filesystem::path directory(path.data());
  std::error_code filesystem_error;
  if (!std::filesystem::exists(directory, filesystem_error)) {
    if (filesystem_error) {
      if (error) {
        *error = WindowsError{
            static_cast<unsigned long>(filesystem_error.value()),
            "Read Startup folder", filesystem_error.message()};
      }
      return false;
    }
    return true;
  }
  std::filesystem::directory_iterator iterator(directory, filesystem_error);
  const std::filesystem::directory_iterator end;
  for (; !filesystem_error && iterator != end;
       iterator.increment(filesystem_error)) {
    const auto& item = *iterator;
    if (!item.is_regular_file(filesystem_error)) {
      if (filesystem_error) break;
      continue;
    }
    const std::filesystem::path filename = item.path().filename();
    std::string executable_name;
    if (LowerAscii(WideToUtf8(filename.extension().wstring())) == ".exe") {
      executable_name = WideToUtf8(filename.wstring());
    } else {
      executable_name = WideToUtf8(filename.stem().wstring());
    }
    entries->push_back(StartupEntrySnapshot{
        scope, StartupSource::StartupFolder, location,
        WideToUtf8(filename.wstring()), std::move(executable_name)});
  }
  if (filesystem_error) {
    if (error) {
      *error = WindowsError{
          static_cast<unsigned long>(filesystem_error.value()),
          "Enumerate Startup folder", filesystem_error.message()};
    }
    return false;
  }
  return true;
}

}  // namespace

StartupQueryResult StartupApi::QueryAll() {
  StartupQueryResult result;
  if (!AppendRegistryRun(HKEY_CURRENT_USER, 0, StartupScope::CurrentUser,
                         "HKCU Run", &result.entries, &result.error) ||
      !AppendRegistryRun(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY,
                         StartupScope::LocalMachine, "HKLM Run (64-bit)",
                         &result.entries, &result.error) ||
      !AppendRegistryRun(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY,
                         StartupScope::LocalMachine, "HKLM Run (32-bit)",
                         &result.entries, &result.error) ||
      !AppendStartupFolder(CSIDL_STARTUP, StartupScope::CurrentUser,
                           "User Startup Folder", &result.entries,
                           &result.error) ||
      !AppendStartupFolder(CSIDL_COMMON_STARTUP, StartupScope::AllUsers,
                           "Common Startup Folder", &result.entries,
                           &result.error)) {
    return result;
  }
  std::sort(result.entries.begin(), result.entries.end(),
            [](const StartupEntrySnapshot& left,
               const StartupEntrySnapshot& right) {
              if (left.scope != right.scope) return left.scope < right.scope;
              if (left.source != right.source) return left.source < right.source;
              if (left.location != right.location) {
                return left.location < right.location;
              }
              return LowerAscii(left.name) < LowerAscii(right.name);
            });
  result.success = true;
  return result;
}

}  // namespace workboost::windows
