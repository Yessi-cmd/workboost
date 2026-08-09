#include "platform/windows/elevated_helper_client.h"

#include "platform/windows/helper_pipe.h"

#include <windows.h>
#include <shellapi.h>

#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>

namespace workboost::windows {
namespace {

constexpr std::uint32_t kTransportTimeoutMs = 15000;

std::uint64_t RequestId(
    const std::array<std::uint8_t, helper::kNonceBytes>& nonce) {
  std::uint64_t value = 0;
  static_assert(sizeof(value) <= helper::kNonceBytes);
  std::memcpy(&value, nonce.data(), sizeof(value));
  return value == 0 ? 1 : value;
}

ElevatedHelperResult Failure(const std::string& context,
                             unsigned long error_code,
                             const std::string& detail = {}) {
  ElevatedHelperResult result;
  result.error = LastError(context, error_code);
  if (!detail.empty()) result.error.message = detail;
  return result;
}

}  // namespace

ElevatedHelperResult ElevatedHelperClient::Execute(
    helper::Command command, const std::string& service_name,
    const std::string& expected_identity_token, std::uint32_t timeout_ms) {
  WindowsError error;
  auto server = HelperPipeServer::Create(&error);
  if (!server) {
    ElevatedHelperResult result;
    result.error = error;
    return result;
  }

  helper::Request request;
  request.request_id = RequestId(server->Nonce());
  request.command = command;
  request.timeout_ms = timeout_ms;
  request.nonce = server->Nonce();
  request.service_name = service_name;
  request.expected_identity_token = expected_identity_token;
  std::string protocol_error;
  const auto request_bytes = helper::Encode(request, &protocol_error);
  if (!request_bytes) {
    return Failure("Encode elevated helper request", ERROR_INVALID_DATA,
                   protocol_error);
  }

  const auto executable_path = CurrentExecutablePath(&error);
  if (!executable_path) {
    ElevatedHelperResult result;
    result.error = error;
    return result;
  }
  const std::filesystem::path helper_path =
      executable_path->parent_path() / "WorkBoostElevated.exe";
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(helper_path, filesystem_error)) {
    return Failure("Locate WorkBoost elevated helper",
                   filesystem_error ? static_cast<unsigned long>(
                                          filesystem_error.value())
                                    : ERROR_FILE_NOT_FOUND,
                   filesystem_error.message());
  }

  std::wostringstream parameters;
  parameters << L"--pipe \"" << server->Name() << L"\" --nonce "
             << Utf8ToWide(HexNonce(server->Nonce())) << L" --parent-pid "
             << GetCurrentProcessId();
  const std::wstring helper_file = helper_path.wstring();
  const std::wstring helper_parameters = parameters.str();
  const std::wstring helper_directory = helper_path.parent_path().wstring();
  SHELLEXECUTEINFOW launch{};
  launch.cbSize = sizeof(launch);
  launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  launch.lpVerb = L"runas";
  launch.lpFile = helper_file.c_str();
  launch.lpParameters = helper_parameters.c_str();
  launch.lpDirectory = helper_directory.c_str();
  launch.nShow = SW_HIDE;
  if (!ShellExecuteExW(&launch)) {
    const DWORD code = GetLastError();
    ElevatedHelperResult result =
        Failure("Launch WorkBoost elevated helper", code);
    result.user_cancelled = code == ERROR_CANCELLED;
    return result;
  }
  UniqueHandle helper_process(launch.hProcess);
  if (!helper_process.Valid()) {
    return Failure("Acquire elevated helper process", ERROR_INVALID_HANDLE);
  }
  const DWORD helper_pid = GetProcessId(helper_process.Get());
  if (helper_pid == 0) {
    return Failure("Identify elevated helper process", GetLastError());
  }

  auto connection = server->Accept(kTransportTimeoutMs, &error);
  if (!connection) {
    ElevatedHelperResult result;
    result.error = error;
    return result;
  }
  const auto peer_pid = connection->PeerProcessId(&error);
  if (!peer_pid) {
    ElevatedHelperResult result;
    result.error = error;
    return result;
  }
  if (*peer_pid != helper_pid) {
    return Failure("Verify elevated helper pipe peer",
                   ERROR_ACCESS_DENIED);
  }
  const auto peer_image = ProcessImagePath(*peer_pid, &error);
  if (!peer_image) {
    ElevatedHelperResult result;
    result.error = error;
    return result;
  }
  if (!PathEqualsInsensitive(*peer_image, helper_path)) {
    return Failure("Verify elevated helper image", ERROR_ACCESS_DENIED);
  }
  bool elevated = false;
  if (!ProcessIsElevated(*peer_pid, &elevated, &error)) {
    ElevatedHelperResult result;
    result.error = error;
    return result;
  }
  if (!elevated) {
    return Failure("Verify elevated helper token", ERROR_ELEVATION_REQUIRED);
  }

  ElevatedHelperResult result;
  if (!connection->WriteMessage(*request_bytes, kTransportTimeoutMs, &error)) {
    result.error = error;
    return result;
  }
  result.request_sent = true;
  const auto response_bytes =
      connection->ReadMessage(kTransportTimeoutMs + timeout_ms, &error);
  if (!response_bytes) {
    result.error = error;
    return result;
  }
  const auto response = helper::DecodeResponse(*response_bytes, &protocol_error);
  if (!response) {
    result.error = LastError("Decode elevated helper response",
                             ERROR_INVALID_DATA);
    result.error.message = protocol_error;
    return result;
  }
  if (response->request_id != request.request_id ||
      response->nonce != request.nonce) {
    result.error = LastError("Authenticate elevated helper response",
                             ERROR_ACCESS_DENIED);
    return result;
  }
  result.transport_success = true;
  result.response = *response;
  return result;
}

}  // namespace workboost::windows
