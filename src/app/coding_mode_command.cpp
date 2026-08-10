#include "app/coding_mode_command.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace workboost {
namespace {

constexpr std::size_t kMaximumCapturedOutputBytes = 1024 * 1024;
constexpr std::size_t kMaximumCleanupProcesses = 64;

CodingModeCommandResult Failure(const std::string& context,
                                std::uint32_t error_code) {
  CodingModeCommandResult result;
  result.error = windows::LastError(context, error_code);
  return result;
}

std::wstring FixedArguments(
    CodingModeCommand command, int baseline_seconds,
    const std::vector<ProcessSelection>& cleanup_processes) {
  switch (command) {
    case CodingModeCommand::Enter: {
      std::wstring arguments = L" coding enter --baseline-duration " +
                               std::to_wstring(baseline_seconds);
      for (const auto& process : cleanup_processes) {
        arguments += L" --close-process " + std::to_wstring(process.pid) +
                     L":" +
                     std::to_wstring(process.expected_start_time_100ns);
      }
      return arguments;
    }
    case CodingModeCommand::RetryClose: {
      std::wstring arguments = L" coding retry-close";
      for (const auto& process : cleanup_processes) {
        arguments += L" --close-process " + std::to_wstring(process.pid) +
                     L":" +
                     std::to_wstring(process.expected_start_time_100ns);
      }
      return arguments;
    }
    case CodingModeCommand::Exit: return L" coding exit";
    case CodingModeCommand::Restore: return L" recovery restore";
  }
  return {};
}

}  // namespace

CodingModeCommandResult CodingModeCommandClient::Execute(
    CodingModeCommand command, int baseline_seconds,
    const std::vector<ProcessSelection>& cleanup_processes) {
  if (command == CodingModeCommand::Enter &&
      (baseline_seconds < 10 || baseline_seconds > 600)) {
    return Failure("Validate Coding Mode baseline", ERROR_INVALID_PARAMETER);
  }
  if (command == CodingModeCommand::RetryClose &&
      cleanup_processes.empty()) {
    return Failure("Validate Coding Mode retry pool",
                   ERROR_INVALID_PARAMETER);
  }
  if ((command != CodingModeCommand::Enter &&
       command != CodingModeCommand::RetryClose &&
       !cleanup_processes.empty()) ||
      cleanup_processes.size() > kMaximumCleanupProcesses ||
      std::any_of(cleanup_processes.begin(), cleanup_processes.end(),
                  [](const ProcessSelection& process) {
                    return process.pid == 0 ||
                           process.expected_start_time_100ns == 0;
                  })) {
    return Failure("Validate Coding Mode cleanup pool",
                   ERROR_INVALID_PARAMETER);
  }
  windows::WindowsError path_error;
  const auto executable = windows::CurrentExecutablePath(&path_error);
  if (!executable) {
    CodingModeCommandResult result;
    result.error = std::move(path_error);
    return result;
  }

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  HANDLE read_handle = nullptr;
  HANDLE write_handle = nullptr;
  if (!CreatePipe(&read_handle, &write_handle, &security, 0)) {
    return Failure("Create Coding Mode output pipe", GetLastError());
  }
  windows::UniqueHandle output_read(read_handle);
  windows::UniqueHandle output_write(write_handle);
  if (!SetHandleInformation(output_read.Get(), HANDLE_FLAG_INHERIT, 0)) {
    return Failure("Secure Coding Mode output pipe", GetLastError());
  }
  HANDLE input_handle =
      CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  windows::UniqueHandle null_input(input_handle);
  if (!null_input.Valid()) {
    return Failure("Open Coding Mode null input", GetLastError());
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = null_input.Get();
  startup.hStdOutput = output_write.Get();
  startup.hStdError = output_write.Get();
  PROCESS_INFORMATION process{};
  std::wstring command_line = L"\"" + executable->wstring() + L"\"" +
                              FixedArguments(command, baseline_seconds,
                                             cleanup_processes);
  std::wstring working_directory = executable->parent_path().wstring();
  if (!CreateProcessW(executable->c_str(), command_line.data(), nullptr,
                      nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                      working_directory.c_str(), &startup, &process)) {
    return Failure("Launch typed Coding Mode command", GetLastError());
  }
  CodingModeCommandResult result;
  result.launched = true;
  windows::UniqueHandle process_handle(process.hProcess);
  windows::UniqueHandle thread_handle(process.hThread);
  output_write.Reset();

  std::string captured;
  char buffer[4096];
  for (;;) {
    DWORD bytes_read = 0;
    if (!ReadFile(output_read.Get(), buffer, sizeof(buffer), &bytes_read,
                  nullptr)) {
      const DWORD code = GetLastError();
      if (code != ERROR_BROKEN_PIPE) {
        result.error = windows::LastError("Read Coding Mode output", code);
      }
      break;
    }
    if (bytes_read == 0) break;
    if (captured.size() < kMaximumCapturedOutputBytes) {
      const std::size_t remaining =
          kMaximumCapturedOutputBytes - captured.size();
      captured.append(buffer,
                      std::min<std::size_t>(remaining, bytes_read));
    }
  }
  if (WaitForSingleObject(process_handle.Get(), INFINITE) != WAIT_OBJECT_0 &&
      result.error.code == 0) {
    result.error = windows::LastError("Wait for Coding Mode command");
  }
  DWORD exit_code = 1;
  if (!GetExitCodeProcess(process_handle.Get(), &exit_code)) {
    result.error =
        windows::LastError("Read Coding Mode command exit code");
  }
  result.exit_code = exit_code;
  result.output = std::move(captured);
  return result;
}

}  // namespace workboost
