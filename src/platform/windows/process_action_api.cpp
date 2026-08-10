#include "platform/windows/process_action_api.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

namespace workboost::windows {
namespace {

enum class IdentityStatus { Match, Mismatch, Error };

IdentityStatus VerifyProcessIdentity(
    HANDLE process, std::uint64_t expected_start_time_100ns,
    WindowsError* error) {
  if (expected_start_time_100ns == 0) {
    if (error) {
      *error = LastError("Verify process identity", ERROR_INVALID_PARAMETER);
    }
    return IdentityStatus::Error;
  }
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
    if (error) *error = LastError("GetProcessTimes for identity check");
    return IdentityStatus::Error;
  }
  ULARGE_INTEGER value{};
  value.LowPart = created.dwLowDateTime;
  value.HighPart = created.dwHighDateTime;
  if (value.QuadPart != expected_start_time_100ns) {
    if (error) {
      *error = LastError("Verify process identity", ERROR_INVALID_PARAMETER);
    }
    return IdentityStatus::Mismatch;
  }
  return IdentityStatus::Match;
}

struct WindowEnumerationContext {
  DWORD pid{};
  std::vector<HWND> windows;
};

BOOL CALLBACK CollectWindow(HWND window, LPARAM parameter) {
  auto* context = reinterpret_cast<WindowEnumerationContext*>(parameter);
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid != context->pid || !IsWindowVisible(window) ||
      GetWindow(window, GW_OWNER) != nullptr) {
    return TRUE;
  }
  context->windows.push_back(window);
  return TRUE;
}

bool OwnsForegroundWindow(DWORD pid) {
  const HWND foreground = GetForegroundWindow();
  if (foreground == nullptr) return false;
  DWORD foreground_pid = 0;
  GetWindowThreadProcessId(foreground, &foreground_pid);
  return foreground_pid == pid;
}

}  // namespace

PriorityChangeResult ProcessActionApi::SetPriority(
    std::uint32_t pid, std::uint64_t expected_start_time_100ns,
    std::uint32_t target_priority) {
  PriorityChangeResult result;
  UniqueHandle process(OpenProcess(
      PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE, pid));
  if (!process.Valid()) {
    result.error = LastError("OpenProcess for priority action");
    return result;
  }
  if (VerifyProcessIdentity(process.Get(), expected_start_time_100ns,
                            &result.error) != IdentityStatus::Match) {
    return result;
  }
  result.original_priority = GetPriorityClass(process.Get());
  if (result.original_priority == 0) {
    result.error = LastError("GetPriorityClass");
    return result;
  }
  if (!SetPriorityClass(process.Get(), target_priority)) {
    result.error = LastError("SetPriorityClass");
    return result;
  }
  result.success = true;
  return result;
}

bool ProcessActionApi::RestorePriority(
    std::uint32_t pid, std::uint64_t expected_start_time_100ns,
    std::uint32_t original_priority, WindowsError* error) {
  UniqueHandle process(OpenProcess(
      PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE, pid));
  if (!process.Valid()) {
    const DWORD code = GetLastError();
    // The original process no longer exists, so no modified state remains.
    if (code == ERROR_INVALID_PARAMETER) return true;
    if (error) *error = LastError("OpenProcess during priority rollback", code);
    return false;
  }
  WindowsError identity_error;
  const IdentityStatus identity = VerifyProcessIdentity(
      process.Get(), expected_start_time_100ns, &identity_error);
  if (identity == IdentityStatus::Mismatch) {
    // PID reuse means the original process has exited. Never modify the new
    // process that inherited its PID.
    return true;
  }
  if (identity == IdentityStatus::Error) {
    if (error) *error = std::move(identity_error);
    return false;
  }
  if (!SetPriorityClass(process.Get(), original_priority)) {
    if (error) *error = LastError("SetPriorityClass during rollback");
    return false;
  }
  return true;
}

GracefulCloseResult RequestGracefulCloseWithDeadline(
    std::uint32_t pid, std::uint64_t expected_start_time_100ns,
    ULONGLONG deadline) {
  GracefulCloseResult result;
  UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                   FALSE, pid));
  if (!process.Valid()) {
    result.error = LastError("OpenProcess for graceful close");
    return result;
  }
  if (VerifyProcessIdentity(process.Get(), expected_start_time_100ns,
                            &result.error) != IdentityStatus::Match) {
    return result;
  }
  if (OwnsForegroundWindow(pid)) {
    result.error = LastError("Reject foreground graceful close",
                             ERROR_ACCESS_DISABLED_BY_POLICY);
    return result;
  }

  WindowEnumerationContext context;
  context.pid = pid;
  if (!EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&context))) {
    result.error = LastError("EnumWindows for graceful close");
    return result;
  }
  DWORD last_error = ERROR_SUCCESS;
  for (const HWND window : context.windows) {
    if (OwnsForegroundWindow(pid)) {
      last_error = ERROR_ACCESS_DISABLED_BY_POLICY;
      break;
    }
    DWORD current_pid = 0;
    GetWindowThreadProcessId(window, &current_pid);
    if (current_pid != pid || !IsWindowVisible(window)) continue;
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) {
      result.delivery_uncertain = true;
      last_error = ERROR_TIMEOUT;
      break;
    }
    const DWORD remaining = static_cast<DWORD>(deadline - now);
    DWORD_PTR message_result = 0;
    SetLastError(ERROR_SUCCESS);
    if (SendMessageTimeoutW(window, WM_CLOSE, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, remaining,
                            &message_result) != 0) {
      ++result.windows_signaled;
    } else {
      result.delivery_uncertain = true;
      last_error = GetLastError();
      if (last_error == ERROR_SUCCESS) last_error = ERROR_TIMEOUT;
    }
  }
  const ULONGLONG now = GetTickCount64();
  const DWORD remaining = now >= deadline
                              ? 0
                              : static_cast<DWORD>(deadline - now);
  result.process_exited =
      WaitForSingleObject(process.Get(), remaining) == WAIT_OBJECT_0;
  if (result.process_exited) {
    result.success = true;
    return result;
  }
  if (result.delivery_uncertain || result.windows_signaled == 0) {
    result.error = LastError(
        "Send WM_CLOSE", last_error == ERROR_SUCCESS
                              ? static_cast<DWORD>(ERROR_NOT_FOUND)
                              : last_error);
    return result;
  }
  result.success = true;
  return result;
}

GracefulCloseResult ProcessActionApi::RequestGracefulClose(
    std::uint32_t pid, std::uint64_t expected_start_time_100ns,
    std::uint32_t timeout_ms) {
  const DWORD bounded_timeout = std::clamp<DWORD>(timeout_ms, 100, 5000);
  return RequestGracefulCloseWithDeadline(
      pid, expected_start_time_100ns, GetTickCount64() + bounded_timeout);
}

GracefulCloseBatchResult ProcessActionApi::RequestGracefulCloseBatch(
    const std::vector<GracefulCloseRequest>& requests,
    std::uint32_t shared_timeout_ms) {
  constexpr std::size_t kMaximumBatchSize = 64;
  constexpr std::size_t kMaximumWorkerThreads = 16;
  GracefulCloseBatchResult result;
  if (requests.empty() || requests.size() > kMaximumBatchSize) {
    result.error = LastError("Validate graceful close batch",
                             ERROR_INVALID_PARAMETER);
    return result;
  }
  const DWORD bounded_timeout =
      std::clamp<DWORD>(shared_timeout_ms, 100, 30000);
  const ULONGLONG deadline = GetTickCount64() + bounded_timeout;
  result.results.resize(requests.size());
  std::atomic<std::size_t> next{0};
  const std::size_t worker_count =
      std::min<std::size_t>(kMaximumWorkerThreads, requests.size());
  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  try {
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
      workers.emplace_back([&requests, &result, &next, deadline] {
        for (;;) {
          const std::size_t index = next.fetch_add(1);
          if (index >= requests.size()) return;
          result.results[index] = RequestGracefulCloseWithDeadline(
              requests[index].pid,
              requests[index].expected_start_time_100ns, deadline);
        }
      });
    }
  } catch (...) {
    result.error = LastError("Start graceful close batch workers",
                             ERROR_NOT_ENOUGH_MEMORY);
  }
  for (auto& worker : workers) {
    if (worker.joinable()) worker.join();
  }
  if (result.error.code != 0) {
    result.results.clear();
    return result;
  }
  return result;
}

}  // namespace workboost::windows
