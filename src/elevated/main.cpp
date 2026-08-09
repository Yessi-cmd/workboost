#include "app/elevated_action_handler.h"
#include "core/config/config.h"
#include "core/helper/helper_protocol.h"
#include "core/model/types.h"
#include "platform/windows/helper_pipe.h"
#include "platform/windows/system_collector.h"
#include "platform/windows/windows_utils.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>

namespace {

std::optional<std::uint32_t> ParsePid(const std::string& value) {
  try {
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(value, &consumed);
    if (consumed != value.size() || parsed == 0 ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(parsed);
  } catch (...) {
    return std::nullopt;
  }
}

bool SameSession(std::uint32_t left_pid, std::uint32_t right_pid) {
  DWORD left_session = 0;
  DWORD right_session = 0;
  return ProcessIdToSessionId(left_pid, &left_session) &&
         ProcessIdToSessionId(right_pid, &right_session) &&
         left_session == right_session;
}

workboost::Config LoadTrustedConfig(
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& local_app_data, bool* success) {
  workboost::Config config = workboost::Config::Defaults();
  std::string warning;
  *success =
      config.LoadDirectory(executable_directory / "config", &warning);
  std::string user_warning;
  if (!config.LoadDirectory(local_app_data, &user_warning)) {
    *success = false;
  }
  return config;
}

workboost::helper::Response ConfigurationFailure(
    const workboost::helper::Request& request) {
  workboost::helper::Response response;
  response.request_id = request.request_id;
  response.status = workboost::helper::Status::Failed;
  response.error_code = ERROR_BAD_CONFIGURATION;
  response.nonce = request.nonce;
  response.message = "trusted helper configuration could not be loaded";
  return response;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 7 || std::string(argv[1]) != "--pipe" ||
      std::string(argv[3]) != "--nonce" ||
      std::string(argv[5]) != "--parent-pid") {
    return 64;
  }
  const std::wstring pipe_name = workboost::windows::Utf8ToWide(argv[2]);
  const auto expected_nonce = workboost::windows::ParseHexNonce(argv[4]);
  const auto parent_pid = ParsePid(argv[6]);
  if (pipe_name.empty() || !expected_nonce || !parent_pid) return 64;

  workboost::windows::WindowsError error;
  bool elevated = false;
  if (!workboost::windows::ProcessIsElevated(GetCurrentProcessId(), &elevated,
                                             &error) ||
      !elevated) {
    return 5;
  }
  auto connection =
      workboost::windows::ConnectToHelperPipe(pipe_name, 15000, &error);
  if (!connection) return 1;
  const auto peer_pid = connection->PeerProcessId(&error);
  if (!peer_pid || *peer_pid != *parent_pid ||
      !SameSession(*peer_pid, GetCurrentProcessId())) {
    return 5;
  }
  const auto peer_image =
      workboost::windows::ProcessImagePath(*peer_pid, &error);
  const auto executable_path =
      workboost::windows::CurrentExecutablePath(&error);
  if (!executable_path) return 5;
  const auto expected_image =
      executable_path->parent_path() / "workboost.exe";
  if (!peer_image || !workboost::windows::PathEqualsInsensitive(
                         *peer_image, expected_image)) {
    return 5;
  }

  const auto request_bytes = connection->ReadMessage(15000, &error);
  if (!request_bytes) return 1;
  std::string protocol_error;
  const auto request =
      workboost::helper::DecodeRequest(*request_bytes, &protocol_error);
  if (!request || request->nonce != *expected_nonce) return 5;

  bool config_ok = false;
  const auto local_app_data =
      workboost::windows::KnownLocalAppDataDirectory(&error);
  workboost::Config config = workboost::Config::Defaults();
  if (local_app_data) {
    config = LoadTrustedConfig(executable_path->parent_path(), *local_app_data,
                               &config_ok);
  }
  workboost::helper::Response response;
  if (!config_ok) {
    response = ConfigurationFailure(*request);
  } else {
    workboost::windows::SystemCollector collector(config);
    workboost::windows::WindowsError collection_error;
    if (!collector.Initialize(&collection_error)) {
      response = ConfigurationFailure(*request);
      response.error_code = static_cast<std::uint32_t>(collection_error.code);
      response.message = "helper protection state could not be collected";
    } else {
      const workboost::SystemSnapshot snapshot =
          collector.Sample(&collection_error);
      if (!snapshot.process_inventory_complete ||
          !snapshot.tcp_inventory_complete) {
        response = ConfigurationFailure(*request);
        response.error_code = ERROR_INVALID_DATA;
        response.message = "helper protection state is incomplete";
      } else {
        const workboost::RuntimeContext context =
            workboost::BuildRuntimeContext(snapshot,
                                           config.remote_debug_ports);
        response =
            workboost::HandleElevatedRequest(*request, config, context);
      }
    }
  }
  const auto response_bytes =
      workboost::helper::Encode(response, &protocol_error);
  if (!response_bytes ||
      !connection->WriteMessage(*response_bytes, 15000, &error)) {
    return 1;
  }
  return 0;
}
