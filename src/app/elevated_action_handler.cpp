#include "app/elevated_action_handler.h"

#include "core/policy/service_protection_policy.h"
#include "platform/windows/service_api.h"

#include <windows.h>

#include <string>

namespace workboost {
namespace {

std::string SanitizeMessage(std::string value) {
  for (char& character : value) {
    if (static_cast<unsigned char>(character) < 0x20) character = ' ';
  }
  return value;
}

helper::Response FailureResponse(const helper::Request& request,
                                 helper::Status status,
                                 ServiceState before, ServiceState after,
                                 const windows::WindowsError& error) {
  helper::Response response;
  response.request_id = request.request_id;
  response.status = status;
  response.state_before = before;
  response.state_after = after;
  response.error_code = static_cast<std::uint32_t>(error.code);
  response.nonce = request.nonce;
  response.message = SanitizeMessage(error.Describe());
  return response;
}

}  // namespace

helper::Response HandleElevatedRequest(const helper::Request& request,
                                       const Config& config,
                                       const RuntimeContext& context) {
  const auto lookup = windows::ServiceApi::Query(request.service_name);
  if (!lookup.success) {
    return FailureResponse(request, helper::Status::Failed,
                           ServiceState::Unknown, ServiceState::Unknown,
                           lookup.error);
  }
  const auto& service = lookup.service;
  if (!service.identity_verified ||
      service.identity_token != request.expected_identity_token) {
    return FailureResponse(
        request, helper::Status::Rejected, service.state, service.state,
        windows::LastError("Verify helper service identity",
                           ERROR_REVISION_MISMATCH));
  }

  const ServiceProtectionPolicy policy(config);
  if (request.command == helper::Command::StopServiceTemporary) {
    if (!policy.CanStopTemporary(service, context)) {
      return FailureResponse(
          request, helper::Status::Rejected, service.state, service.state,
          windows::LastError("Validate temporary service stop policy",
                             ERROR_ACCESS_DISABLED_BY_POLICY));
    }
  } else if (request.command == helper::Command::StartServiceRestore) {
    if (service.state == ServiceState::Running) {
      helper::Response response;
      response.request_id = request.request_id;
      response.status = helper::Status::Succeeded;
      response.state_before = service.state;
      response.state_after = service.state;
      response.nonce = request.nonce;
      response.message = "service was already running";
      return response;
    }
    if (!policy.CanRestore(service, context)) {
      return FailureResponse(
          request, helper::Status::Rejected, service.state, service.state,
          windows::LastError("Validate service restore policy",
                             ERROR_ACCESS_DISABLED_BY_POLICY));
    }
  } else {
    return FailureResponse(
        request, helper::Status::Rejected, service.state, service.state,
        windows::LastError("Validate elevated action allowlist",
                           ERROR_NOT_SUPPORTED));
  }

  const windows::ServiceControlResult controlled =
      request.command == helper::Command::StopServiceTemporary
          ? windows::ServiceApi::StopTemporary(
                request.service_name, request.expected_identity_token,
                service.pid, request.timeout_ms)
          : windows::ServiceApi::StartForRestore(
                request.service_name, request.expected_identity_token,
                request.timeout_ms);
  if (!controlled.success) {
    return FailureResponse(
        request,
        controlled.outcome_uncertain ? helper::Status::Uncertain
                                     : helper::Status::Failed,
        controlled.state_before, controlled.state_after, controlled.error);
  }

  helper::Response response;
  response.request_id = request.request_id;
  response.status = helper::Status::Succeeded;
  response.state_before = controlled.state_before;
  response.state_after = controlled.state_after;
  response.nonce = request.nonce;
  response.message = request.command == helper::Command::StopServiceTemporary
                         ? "service stopped temporarily"
                         : "service restored to running";
  return response;
}

}  // namespace workboost
