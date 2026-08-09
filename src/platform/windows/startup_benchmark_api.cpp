#include "platform/windows/startup_benchmark_api.h"

#include "platform/windows/windows_utils.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace workboost::windows {
namespace {

struct ProcessIdentity {
  std::uint32_t pid{};
  std::uint64_t start_time_100ns{};

  bool operator==(const ProcessIdentity& other) const {
    return pid == other.pid && start_time_100ns == other.start_time_100ns;
  }
};

struct ProcessIdentityHash {
  std::size_t operator()(const ProcessIdentity& value) const {
    return std::hash<std::uint64_t>{}(
        (value.start_time_100ns << 17) ^ value.pid);
  }
};

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

bool ValidProcessName(const std::string& value) {
  if (value.empty() || value.size() > 260 ||
      value.find('/') != std::string::npos ||
      value.find('\\') != std::string::npos) {
    return false;
  }
  return std::none_of(value.begin(), value.end(), [](unsigned char character) {
    return character < 0x20;
  });
}

std::uint64_t FileTimeValue(const FILETIME& value) {
  ULARGE_INTEGER integer{};
  integer.LowPart = value.dwLowDateTime;
  integer.HighPart = value.dwHighDateTime;
  return integer.QuadPart;
}

std::optional<std::uint64_t> ProcessStartTime(std::uint32_t pid) {
  UniqueHandle process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process.Valid()) return std::nullopt;
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process.Get(), &created, &exited, &kernel, &user)) {
    return std::nullopt;
  }
  return FileTimeValue(created);
}

std::optional<std::vector<ProcessIdentity>> MatchingProcesses(
    const std::string& process_name, WindowsError* error) {
  UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (!snapshot.Valid()) {
    if (error) *error = LastError("Create benchmark process snapshot");
    return std::nullopt;
  }
  std::vector<ProcessIdentity> result;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot.Get(), &entry)) {
    const DWORD code = GetLastError();
    if (code == ERROR_NO_MORE_FILES) return result;
    if (error) *error = LastError("Read benchmark process snapshot", code);
    return std::nullopt;
  }
  const std::string expected = LowerAscii(process_name);
  do {
    if (LowerAscii(WideToUtf8(entry.szExeFile)) != expected) continue;
    const auto start = ProcessStartTime(entry.th32ProcessID);
    if (start) result.push_back({entry.th32ProcessID, *start});
  } while (Process32NextW(snapshot.Get(), &entry));
  const DWORD code = GetLastError();
  if (code != ERROR_NO_MORE_FILES) {
    if (error) *error = LastError("Complete benchmark process snapshot", code);
    return std::nullopt;
  }
  return result;
}

HWND VisibleWindowForProcess(std::uint32_t pid) {
  struct Search {
    DWORD pid{};
    HWND window{};
  } search{pid, nullptr};
  EnumWindows(
      [](HWND window, LPARAM context) -> BOOL {
        auto* search = reinterpret_cast<Search*>(context);
        DWORD owner_pid = 0;
        GetWindowThreadProcessId(window, &owner_pid);
        if (owner_pid == search->pid && IsWindowVisible(window) &&
            GetWindow(window, GW_OWNER) == nullptr) {
          search->window = window;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
  return search.window;
}

bool Responsive(HWND window) {
  DWORD_PTR ignored = 0;
  return SendMessageTimeoutW(window, WM_NULL, 0, 0,
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 100,
                             &ignored) != 0;
}

std::uint64_t ElapsedFromStart(std::uint64_t start_time_100ns) {
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  const std::uint64_t current = FileTimeValue(now);
  return current > start_time_100ns
             ? (current - start_time_100ns) / 10000ULL
             : 0;
}

}  // namespace

StartupObservation StartupBenchmarkApi::ObserveNewProcess(
    const std::string& process_name, std::uint32_t timeout_ms,
    const std::function<void()>& on_ready) {
  StartupObservation result;
  if (!ValidProcessName(process_name) || timeout_ms < 100 ||
      timeout_ms > 600000) {
    result.status = StartupObservationStatus::InvalidTarget;
    result.error_code = ERROR_INVALID_PARAMETER;
    return result;
  }
  WindowsError error;
  const auto initial = MatchingProcesses(process_name, &error);
  if (!initial) {
    result.status = StartupObservationStatus::QueryFailed;
    result.error_code = error.code;
    return result;
  }
  std::unordered_set<ProcessIdentity, ProcessIdentityHash> existing(
      initial->begin(), initial->end());
  if (on_ready) on_ready();
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  struct Candidate {
    ProcessIdentity identity;
    std::optional<std::uint64_t> visible_window_ms;
  };
  std::vector<Candidate> candidates;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto current = MatchingProcesses(process_name, &error);
    if (!current) {
      result.status = StartupObservationStatus::QueryFailed;
      result.error_code = error.code;
      return result;
    }
    for (const auto& process : *current) {
      const auto candidate = std::find_if(
          candidates.begin(), candidates.end(),
          [&process](const Candidate& value) {
            return value.identity == process;
          });
      if (existing.count(process) == 0 && candidate == candidates.end()) {
        candidates.push_back({process, std::nullopt});
      }
    }
    for (auto& candidate : candidates) {
      const bool still_running = std::find(current->begin(), current->end(),
                                           candidate.identity) != current->end();
      if (!still_running) continue;
      const HWND window = VisibleWindowForProcess(candidate.identity.pid);
      if (window != nullptr) {
        if (!candidate.visible_window_ms) {
          candidate.visible_window_ms =
              ElapsedFromStart(candidate.identity.start_time_100ns);
        }
        if (Responsive(window)) {
          result.pid = candidate.identity.pid;
          result.process_start_time_100ns =
              candidate.identity.start_time_100ns;
          result.visible_window_ms = candidate.visible_window_ms;
          result.responsive_window_ms =
              ElapsedFromStart(candidate.identity.start_time_100ns);
          result.status = StartupObservationStatus::Succeeded;
          return result;
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (candidates.empty()) {
    result.status = StartupObservationStatus::TimedOut;
    result.error_code = ERROR_TIMEOUT;
    return result;
  }

  const auto current = MatchingProcesses(process_name, &error);
  if (!current) {
    result.status = StartupObservationStatus::QueryFailed;
    result.error_code = error.code;
    return result;
  }
  const auto visible = std::find_if(
      candidates.begin(), candidates.end(), [](const Candidate& candidate) {
        return candidate.visible_window_ms.has_value();
      });
  if (visible != candidates.end()) {
    result.pid = visible->identity.pid;
    result.process_start_time_100ns = visible->identity.start_time_100ns;
    result.visible_window_ms = visible->visible_window_ms;
    result.status = StartupObservationStatus::WindowUnresponsive;
    result.error_code = ERROR_TIMEOUT;
    return result;
  }
  const auto running = std::find_if(
      candidates.begin(), candidates.end(), [&current](const Candidate& candidate) {
        return std::find(current->begin(), current->end(), candidate.identity) !=
               current->end();
      });
  const Candidate& reported =
      running != candidates.end() ? *running : candidates.front();
  result.pid = reported.identity.pid;
  result.process_start_time_100ns = reported.identity.start_time_100ns;
  if (running != candidates.end()) {
    result.status = StartupObservationStatus::WindowNotFound;
    result.error_code = ERROR_NOT_FOUND;
  } else {
    result.status = StartupObservationStatus::ProcessExited;
    result.error_code = ERROR_PROCESS_ABORTED;
  }
  return result;
}

}  // namespace workboost::windows
