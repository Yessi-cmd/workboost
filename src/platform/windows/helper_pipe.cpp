#include "platform/windows/helper_pipe.h"

#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace workboost::windows {
namespace {

constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\WorkBoost-";

class LocalMemory {
 public:
  LocalMemory() = default;
  explicit LocalMemory(HLOCAL value) : value_(value) {}
  ~LocalMemory() {
    if (value_ != nullptr) LocalFree(value_);
  }
  LocalMemory(const LocalMemory&) = delete;
  LocalMemory& operator=(const LocalMemory&) = delete;
  LocalMemory(LocalMemory&& other) noexcept
      : value_(std::exchange(other.value_, nullptr)) {}
  LocalMemory& operator=(LocalMemory&& other) noexcept {
    if (this != &other) {
      if (value_ != nullptr) LocalFree(value_);
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }
  [[nodiscard]] HLOCAL Get() const { return value_; }

 private:
  HLOCAL value_{};
};

bool RandomBytes(void* output, std::size_t size, WindowsError* error) {
  if (size > std::numeric_limits<ULONG>::max()) {
    if (error) *error = LastError("Generate helper nonce", ERROR_INVALID_PARAMETER);
    return false;
  }
  const NTSTATUS status = BCryptGenRandom(
      nullptr, static_cast<PUCHAR>(output), static_cast<ULONG>(size),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status < 0) {
    if (error) {
      *error = LastError("BCryptGenRandom for helper nonce",
                         static_cast<unsigned long>(status));
    }
    return false;
  }
  return true;
}

std::optional<std::wstring> CurrentLogonSid(WindowsError* error) {
  UniqueHandle token;
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    if (error) *error = LastError("OpenProcessToken for pipe ACL");
    return std::nullopt;
  }
  token.Reset(raw_token);
  DWORD bytes_needed = 0;
  GetTokenInformation(token.Get(), TokenGroups, nullptr, 0, &bytes_needed);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes_needed == 0) {
    if (error) *error = LastError("Size token groups for pipe ACL");
    return std::nullopt;
  }
  std::vector<BYTE> buffer(bytes_needed);
  if (!GetTokenInformation(token.Get(), TokenGroups, buffer.data(),
                           static_cast<DWORD>(buffer.size()), &bytes_needed)) {
    if (error) *error = LastError("Read token groups for pipe ACL");
    return std::nullopt;
  }
  const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(buffer.data());
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) ==
        SE_GROUP_LOGON_ID) {
      LPWSTR sid_text = nullptr;
      if (!ConvertSidToStringSidW(groups->Groups[i].Sid, &sid_text)) {
        if (error) *error = LastError("Convert logon SID for pipe ACL");
        return std::nullopt;
      }
      LocalMemory sid_memory(sid_text);
      return std::wstring(sid_text);
    }
  }
  if (error) *error = LastError("Find logon SID for pipe ACL", ERROR_NOT_FOUND);
  return std::nullopt;
}

std::optional<LocalMemory> PipeSecurityDescriptor(WindowsError* error) {
  const auto logon_sid = CurrentLogonSid(error);
  if (!logon_sid) return std::nullopt;
  const std::wstring sddl =
      L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + *logon_sid + L")";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
    if (error) *error = LastError("Build helper pipe security descriptor");
    return std::nullopt;
  }
  return LocalMemory(descriptor);
}

bool IsAllowedPipeName(const std::wstring& value) {
  if (value.size() <= std::size(kPipePrefix) - 1 || value.size() > 160 ||
      value.compare(0, std::size(kPipePrefix) - 1, kPipePrefix) != 0) {
    return false;
  }
  for (std::size_t i = std::size(kPipePrefix) - 1; i < value.size(); ++i) {
    const wchar_t character = value[i];
    if (!((character >= L'0' && character <= L'9') ||
          (character >= L'a' && character <= L'f') || character == L'-')) {
      return false;
    }
  }
  return true;
}

bool WaitForOverlapped(HANDLE pipe, OVERLAPPED* operation,
                       std::uint32_t timeout_ms, DWORD* transferred,
                       const std::string& context, WindowsError* error) {
  const DWORD wait = WaitForSingleObject(operation->hEvent, timeout_ms);
  if (wait == WAIT_TIMEOUT) {
    CancelIoEx(pipe, operation);
    WaitForSingleObject(operation->hEvent, INFINITE);
    if (error) *error = LastError(context, ERROR_TIMEOUT);
    return false;
  }
  if (wait != WAIT_OBJECT_0) {
    if (error) *error = LastError(context + " wait");
    return false;
  }
  if (!GetOverlappedResult(pipe, operation, transferred, FALSE)) {
    if (error) *error = LastError(context);
    return false;
  }
  return true;
}

bool OverlappedWrite(HANDLE pipe, const std::vector<std::uint8_t>& message,
                     std::uint32_t timeout_ms, WindowsError* error) {
  UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event.Valid()) {
    if (error) *error = LastError("Create helper pipe write event");
    return false;
  }
  OVERLAPPED operation{};
  operation.hEvent = event.Get();
  DWORD transferred = 0;
  if (!WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()),
                 &transferred, &operation)) {
    const DWORD code = GetLastError();
    if (code != ERROR_IO_PENDING) {
      if (error) *error = LastError("Write helper pipe message", code);
      return false;
    }
    if (!WaitForOverlapped(pipe, &operation, timeout_ms, &transferred,
                           "Write helper pipe message", error)) {
      return false;
    }
  }
  if (transferred != message.size()) {
    if (error) {
      *error = LastError("Write complete helper pipe message",
                         ERROR_WRITE_FAULT);
    }
    return false;
  }
  return true;
}

std::optional<std::vector<std::uint8_t>> OverlappedRead(
    HANDLE pipe, std::uint32_t timeout_ms, WindowsError* error) {
  std::vector<std::uint8_t> message(helper::kMaximumMessageBytes);
  UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event.Valid()) {
    if (error) *error = LastError("Create helper pipe read event");
    return std::nullopt;
  }
  OVERLAPPED operation{};
  operation.hEvent = event.Get();
  DWORD transferred = 0;
  if (!ReadFile(pipe, message.data(), static_cast<DWORD>(message.size()),
                &transferred, &operation)) {
    const DWORD code = GetLastError();
    if (code != ERROR_IO_PENDING) {
      if (error) *error = LastError("Read helper pipe message", code);
      return std::nullopt;
    }
    if (!WaitForOverlapped(pipe, &operation, timeout_ms, &transferred,
                           "Read helper pipe message", error)) {
      return std::nullopt;
    }
  }
  if (transferred == 0 || transferred > message.size()) {
    if (error) {
      *error = LastError("Read complete helper pipe message", ERROR_BAD_LENGTH);
    }
    return std::nullopt;
  }
  message.resize(transferred);
  return message;
}

int HexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

}  // namespace

std::optional<std::uint32_t> HelperPipeConnection::PeerProcessId(
    WindowsError* error) const {
  ULONG pid = 0;
  const BOOL success = server_side_
                           ? GetNamedPipeClientProcessId(pipe_.Get(), &pid)
                           : GetNamedPipeServerProcessId(pipe_.Get(), &pid);
  if (!success || pid == 0) {
    if (error) *error = LastError("Identify helper pipe peer process");
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(pid);
}

bool HelperPipeConnection::WriteMessage(
    const std::vector<std::uint8_t>& message, std::uint32_t timeout_ms,
    WindowsError* error) const {
  if (!Valid() || message.empty() ||
      message.size() > helper::kMaximumMessageBytes) {
    if (error) {
      *error = LastError("Validate helper pipe write", ERROR_INVALID_PARAMETER);
    }
    return false;
  }
  return OverlappedWrite(pipe_.Get(), message, timeout_ms, error);
}

std::optional<std::vector<std::uint8_t>> HelperPipeConnection::ReadMessage(
    std::uint32_t timeout_ms, WindowsError* error) const {
  if (!Valid()) {
    if (error) {
      *error = LastError("Validate helper pipe read", ERROR_INVALID_HANDLE);
    }
    return std::nullopt;
  }
  return OverlappedRead(pipe_.Get(), timeout_ms, error);
}

std::optional<HelperPipeServer> HelperPipeServer::Create(
    WindowsError* error) {
  std::array<std::uint8_t, 16> name_random{};
  HelperPipeServer server;
  if (!RandomBytes(name_random.data(), name_random.size(), error) ||
      !RandomBytes(server.nonce_.data(), server.nonce_.size(), error)) {
    return std::nullopt;
  }
  while (std::all_of(server.nonce_.begin(), server.nonce_.end(),
                     [](std::uint8_t value) { return value == 0; })) {
    if (!RandomBytes(server.nonce_.data(), server.nonce_.size(), error)) {
      return std::nullopt;
    }
  }
  std::ostringstream suffix;
  suffix << GetCurrentProcessId() << '-';
  for (const std::uint8_t byte : name_random) {
    suffix << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<unsigned int>(byte);
  }
  server.name_ = kPipePrefix + Utf8ToWide(suffix.str());
  const auto security = PipeSecurityDescriptor(error);
  if (!security) return std::nullopt;
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = security->Get();
  server.pipe_.Reset(CreateNamedPipeW(
      server.name_.c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
          FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      1, static_cast<DWORD>(helper::kMaximumMessageBytes),
      static_cast<DWORD>(helper::kMaximumMessageBytes), 0, &attributes));
  if (!server.pipe_.Valid()) {
    if (error) *error = LastError("Create authenticated helper pipe");
    return std::nullopt;
  }
  return server;
}

std::optional<HelperPipeConnection> HelperPipeServer::Accept(
    std::uint32_t timeout_ms, WindowsError* error) {
  if (!pipe_.Valid()) {
    if (error) *error = LastError("Accept helper pipe", ERROR_INVALID_HANDLE);
    return std::nullopt;
  }
  UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event.Valid()) {
    if (error) *error = LastError("Create helper pipe connection event");
    return std::nullopt;
  }
  OVERLAPPED operation{};
  operation.hEvent = event.Get();
  DWORD transferred = 0;
  if (!ConnectNamedPipe(pipe_.Get(), &operation)) {
    const DWORD code = GetLastError();
    if (code != ERROR_PIPE_CONNECTED) {
      if (code != ERROR_IO_PENDING ||
          !WaitForOverlapped(pipe_.Get(), &operation, timeout_ms, &transferred,
                             "Connect authenticated helper pipe", error)) {
        if (code != ERROR_IO_PENDING && error) {
          *error = LastError("Connect authenticated helper pipe", code);
        }
        return std::nullopt;
      }
    }
  }
  return HelperPipeConnection(std::move(pipe_), true);
}

std::optional<HelperPipeConnection> ConnectToHelperPipe(
    const std::wstring& pipe_name, std::uint32_t timeout_ms,
    WindowsError* error) {
  if (!IsAllowedPipeName(pipe_name)) {
    if (error) {
      *error = LastError("Validate helper pipe name", ERROR_INVALID_NAME);
    }
    return std::nullopt;
  }
  if (!WaitNamedPipeW(pipe_name.c_str(), timeout_ms)) {
    if (error) *error = LastError("Wait for helper pipe server");
    return std::nullopt;
  }
  UniqueHandle pipe(CreateFileW(
      pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
      nullptr));
  if (!pipe.Valid()) {
    if (error) *error = LastError("Connect to helper pipe server");
    return std::nullopt;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe.Get(), &mode, nullptr, nullptr)) {
    if (error) *error = LastError("Set helper pipe message mode");
    return std::nullopt;
  }
  return HelperPipeConnection(std::move(pipe), false);
}

std::string HexNonce(
    const std::array<std::uint8_t, helper::kNonceBytes>& nonce) {
  std::ostringstream output;
  for (const std::uint8_t byte : nonce) {
    output << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<unsigned int>(byte);
  }
  return output.str();
}

std::optional<std::array<std::uint8_t, helper::kNonceBytes>> ParseHexNonce(
    const std::string& value) {
  if (value.size() != helper::kNonceBytes * 2) return std::nullopt;
  std::array<std::uint8_t, helper::kNonceBytes> nonce{};
  for (std::size_t i = 0; i < nonce.size(); ++i) {
    const int high = HexValue(value[i * 2]);
    const int low = HexValue(value[i * 2 + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    nonce[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  if (std::all_of(nonce.begin(), nonce.end(),
                  [](std::uint8_t byte) { return byte == 0; })) {
    return std::nullopt;
  }
  return nonce;
}

}  // namespace workboost::windows
