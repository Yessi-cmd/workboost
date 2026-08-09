#include "platform/windows/service_api.h"

#include <windows.h>
#include <winsvc.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace workboost::windows {
namespace {

class UniqueServiceHandle {
 public:
  explicit UniqueServiceHandle(SC_HANDLE handle) : handle_(handle) {}
  ~UniqueServiceHandle() {
    if (handle_ != nullptr) CloseServiceHandle(handle_);
  }

  UniqueServiceHandle(const UniqueServiceHandle&) = delete;
  UniqueServiceHandle& operator=(const UniqueServiceHandle&) = delete;

  [[nodiscard]] bool Valid() const { return handle_ != nullptr; }
  [[nodiscard]] SC_HANDLE Get() const { return handle_; }

 private:
  SC_HANDLE handle_{};
};

ServiceState MapServiceState(DWORD value) {
  switch (value) {
    case SERVICE_STOPPED: return ServiceState::Stopped;
    case SERVICE_START_PENDING: return ServiceState::StartPending;
    case SERVICE_STOP_PENDING: return ServiceState::StopPending;
    case SERVICE_RUNNING: return ServiceState::Running;
    case SERVICE_CONTINUE_PENDING: return ServiceState::ContinuePending;
    case SERVICE_PAUSE_PENDING: return ServiceState::PausePending;
    case SERVICE_PAUSED: return ServiceState::Paused;
    default: return ServiceState::Unknown;
  }
}

void HashBytes(std::uint64_t* hash, const void* bytes, std::size_t size) {
  constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
  const auto* current = static_cast<const unsigned char*>(bytes);
  for (std::size_t i = 0; i < size; ++i) {
    *hash ^= current[i];
    *hash *= kFnvPrime;
  }
}

void HashWideString(std::uint64_t* hash, const wchar_t* value) {
  const wchar_t separator = L'\0';
  if (value != nullptr) {
    const std::size_t length = std::wcslen(value);
    HashBytes(hash, value, length * sizeof(wchar_t));
  }
  HashBytes(hash, &separator, sizeof(separator));
}

void HashMultiString(std::uint64_t* hash, const wchar_t* value) {
  if (value == nullptr || *value == L'\0') {
    const wchar_t empty[2]{};
    HashBytes(hash, empty, sizeof(empty));
    return;
  }
  const wchar_t* current = value;
  while (*current != L'\0') current += std::wcslen(current) + 1;
  ++current;
  HashBytes(hash, value,
            static_cast<std::size_t>(current - value) * sizeof(wchar_t));
}

bool PopulateServiceIdentityFromHandle(
    SC_HANDLE service, const wchar_t* service_name, ServiceSnapshot* snapshot,
    WindowsError* error = nullptr) {
  DWORD bytes_needed = 0;
  SetLastError(ERROR_SUCCESS);
  QueryServiceConfigW(service, nullptr, 0, &bytes_needed);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes_needed == 0) {
    if (error) *error = LastError("Size service configuration identity");
    return false;
  }
  std::vector<BYTE> buffer(bytes_needed);
  auto* configuration =
      reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
  if (!QueryServiceConfigW(service, configuration,
                           static_cast<DWORD>(buffer.size()),
                           &bytes_needed)) {
    if (error) *error = LastError("Read service configuration identity");
    return false;
  }

  constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
  std::uint64_t hash = kFnvOffset;
  HashWideString(&hash, service_name);
  HashBytes(&hash, &configuration->dwServiceType,
            sizeof(configuration->dwServiceType));
  HashBytes(&hash, &configuration->dwStartType,
            sizeof(configuration->dwStartType));
  HashBytes(&hash, &configuration->dwErrorControl,
            sizeof(configuration->dwErrorControl));
  HashWideString(&hash, configuration->lpBinaryPathName);
  HashWideString(&hash, configuration->lpLoadOrderGroup);
  HashMultiString(&hash, configuration->lpDependencies);
  HashWideString(&hash, configuration->lpServiceStartName);

  std::ostringstream token;
  token << std::hex << std::setfill('0') << std::setw(16) << hash;
  snapshot->service_type = configuration->dwServiceType;
  snapshot->start_type = configuration->dwStartType;
  snapshot->identity_token = token.str();
  snapshot->identity_verified = true;
  return true;
}

bool PopulateServiceIdentityFromManager(SC_HANDLE manager,
                                        const wchar_t* service_name,
                                        ServiceSnapshot* snapshot) {
  UniqueServiceHandle service(
      OpenServiceW(manager, service_name, SERVICE_QUERY_CONFIG));
  return service.Valid() &&
         PopulateServiceIdentityFromHandle(service.Get(), service_name,
                                           snapshot);
}

bool PopulateServiceStatus(SC_HANDLE service, ServiceSnapshot* snapshot,
                           WindowsError* error = nullptr) {
  SERVICE_STATUS_PROCESS status{};
  DWORD bytes_needed = 0;
  if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                            reinterpret_cast<LPBYTE>(&status), sizeof(status),
                            &bytes_needed)) {
    if (error) *error = LastError("Query service status");
    return false;
  }
  snapshot->state = MapServiceState(status.dwCurrentState);
  snapshot->pid = status.dwProcessId;
  snapshot->win32_exit_code = status.dwWin32ExitCode;
  snapshot->service_specific_exit_code = status.dwServiceSpecificExitCode;
  snapshot->accepts_stop =
      (status.dwControlsAccepted & SERVICE_ACCEPT_STOP) != 0;
  return true;
}

bool WaitForServiceStateUntil(SC_HANDLE service, ServiceState desired,
                              ULONGLONG deadline, ServiceState* last_state,
                              WindowsError* error) {
  for (;;) {
    ServiceSnapshot current;
    if (!PopulateServiceStatus(service, &current, error)) return false;
    if (last_state) *last_state = current.state;
    if (current.state == desired) return true;
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) {
      if (error) *error = LastError("Wait for service state", ERROR_TIMEOUT);
      return false;
    }
    const DWORD remaining = static_cast<DWORD>(deadline - now);
    Sleep(std::min<DWORD>(remaining, 100));
  }
}

bool WaitForServiceState(SC_HANDLE service, ServiceState desired,
                         std::uint32_t timeout_ms, ServiceState* last_state,
                         WindowsError* error) {
  return WaitForServiceStateUntil(service, desired,
                                  GetTickCount64() + timeout_ms, last_state,
                                  error);
}

bool HasActiveDependents(SC_HANDLE service, bool* has_dependents,
                         WindowsError* error) {
  *has_dependents = false;
  DWORD bytes_needed = 0;
  DWORD returned = 0;
  SetLastError(ERROR_SUCCESS);
  if (EnumDependentServicesW(service, SERVICE_ACTIVE, nullptr, 0,
                             &bytes_needed, &returned)) {
    return true;
  }
  const DWORD code = GetLastError();
  if (code == ERROR_MORE_DATA && bytes_needed != 0) {
    std::vector<BYTE> buffer(bytes_needed);
    if (!EnumDependentServicesW(
            service, SERVICE_ACTIVE,
            reinterpret_cast<ENUM_SERVICE_STATUSW*>(buffer.data()),
            static_cast<DWORD>(buffer.size()), &bytes_needed, &returned)) {
      if (error) *error = LastError("Enumerate active dependent services");
      return false;
    }
    *has_dependents = returned != 0;
    return true;
  }
  if (code == ERROR_SUCCESS) return true;
  if (error) *error = LastError("Size active dependent services", code);
  return false;
}

std::wstring DisplayName(SC_HANDLE manager, const wchar_t* service_name) {
  DWORD characters = 0;
  GetServiceDisplayNameW(manager, service_name, nullptr, &characters);
  if (characters == 0) return service_name;
  std::vector<wchar_t> buffer(static_cast<std::size_t>(characters) + 1);
  if (!GetServiceDisplayNameW(manager, service_name, buffer.data(),
                              &characters)) {
    return service_name;
  }
  return std::wstring(buffer.data());
}

bool ValidateServiceIdentity(const ServiceSnapshot& service,
                             const std::string& expected,
                             WindowsError* error) {
  if (!service.identity_verified || expected.empty() ||
      service.identity_token != expected) {
    if (error) {
      *error = LastError("Verify service configuration identity",
                         ERROR_REVISION_MISMATCH);
    }
    return false;
  }
  return true;
}

}  // namespace

ServiceQueryResult ServiceApi::QueryAll() {
  ServiceQueryResult result;
  UniqueServiceHandle manager(
      OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE));
  if (!manager.Valid()) {
    result.error = LastError("OpenSCManager for read-only enumeration");
    return result;
  }

  DWORD bytes_needed = 0;
  DWORD services_returned = 0;
  DWORD resume_handle = 0;
  SetLastError(ERROR_SUCCESS);
  const BOOL sized = EnumServicesStatusExW(
      manager.Get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
      nullptr, 0, &bytes_needed, &services_returned, &resume_handle, nullptr);
  const DWORD initial_error = sized ? ERROR_SUCCESS : GetLastError();
  if (initial_error != ERROR_MORE_DATA && initial_error != ERROR_SUCCESS) {
    result.error = LastError("Size SCM service enumeration", initial_error);
    return result;
  }

  std::vector<BYTE> buffer(std::max<DWORD>(bytes_needed, 4096));
  for (int attempt = 0; attempt < 3; ++attempt) {
    bytes_needed = 0;
    services_returned = 0;
    resume_handle = 0;
    SetLastError(ERROR_SUCCESS);
    if (EnumServicesStatusExW(
            manager.Get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
            SERVICE_STATE_ALL, buffer.data(), static_cast<DWORD>(buffer.size()),
            &bytes_needed, &services_returned, &resume_handle, nullptr)) {
      const auto* entries =
          reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
      result.services.reserve(services_returned);
      for (DWORD i = 0; i < services_returned; ++i) {
        const auto& entry = entries[i];
        ServiceSnapshot service;
        service.name = WideToUtf8(entry.lpServiceName);
        service.display_name = WideToUtf8(entry.lpDisplayName);
        service.state = MapServiceState(entry.ServiceStatusProcess.dwCurrentState);
        service.pid = entry.ServiceStatusProcess.dwProcessId;
        service.win32_exit_code = entry.ServiceStatusProcess.dwWin32ExitCode;
        service.service_specific_exit_code =
            entry.ServiceStatusProcess.dwServiceSpecificExitCode;
        service.accepts_stop =
            (entry.ServiceStatusProcess.dwControlsAccepted &
             SERVICE_ACCEPT_STOP) != 0;
        PopulateServiceIdentityFromManager(manager.Get(), entry.lpServiceName,
                                           &service);
        result.services.push_back(std::move(service));
      }
      std::sort(result.services.begin(), result.services.end(),
                [](const ServiceSnapshot& left, const ServiceSnapshot& right) {
                  return left.name < right.name;
                });
      result.success = true;
      return result;
    }

    const DWORD code = GetLastError();
    if (code != ERROR_MORE_DATA || bytes_needed == 0) {
      result.error = LastError("Enumerate Windows services", code);
      return result;
    }
    buffer.resize(buffer.size() + bytes_needed + 4096);
  }

  result.error = LastError("Enumerate Windows services", ERROR_MORE_DATA);
  return result;
}

ServiceLookupResult ServiceApi::Query(const std::string& service_name) {
  ServiceLookupResult result;
  const std::wstring name = Utf8ToWide(service_name);
  if (name.empty()) {
    result.error = LastError("Validate service name", ERROR_INVALID_NAME);
    return result;
  }
  UniqueServiceHandle manager(
      OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!manager.Valid()) {
    result.error = LastError("OpenSCManager for service query");
    return result;
  }
  UniqueServiceHandle service(OpenServiceW(
      manager.Get(), name.c_str(), SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
  if (!service.Valid()) {
    result.error = LastError("OpenService for read-only query");
    return result;
  }
  result.service.name = service_name;
  result.service.display_name = WideToUtf8(DisplayName(manager.Get(), name.c_str()));
  if (!PopulateServiceStatus(service.Get(), &result.service, &result.error) ||
      !PopulateServiceIdentityFromHandle(
          service.Get(), name.c_str(), &result.service, &result.error)) {
    return result;
  }
  result.success = true;
  return result;
}

ServiceControlResult ServiceApi::StopTemporary(
    const std::string& service_name,
    const std::string& expected_identity_token, std::uint32_t expected_pid,
    std::uint32_t timeout_ms) {
  ServiceControlResult result;
  const std::wstring name = Utf8ToWide(service_name);
  if (name.empty()) {
    result.error = LastError("Validate service stop name", ERROR_INVALID_NAME);
    return result;
  }
  UniqueServiceHandle manager(
      OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!manager.Valid()) {
    result.error = LastError("OpenSCManager for temporary service stop");
    return result;
  }
  UniqueServiceHandle service(OpenServiceW(
      manager.Get(), name.c_str(),
      SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | SERVICE_STOP |
          SERVICE_ENUMERATE_DEPENDENTS));
  if (!service.Valid()) {
    result.error = LastError("OpenService for temporary stop");
    return result;
  }
  ServiceSnapshot snapshot;
  snapshot.name = service_name;
  if (!PopulateServiceStatus(service.Get(), &snapshot, &result.error) ||
      !PopulateServiceIdentityFromHandle(service.Get(), name.c_str(), &snapshot,
                                         &result.error) ||
      !ValidateServiceIdentity(snapshot, expected_identity_token,
                               &result.error)) {
    return result;
  }
  result.state_before = snapshot.state;
  result.state_after = snapshot.state;
  if (expected_pid == 0 || snapshot.pid != expected_pid) {
    result.error = LastError("Verify service process identity",
                             ERROR_REVISION_MISMATCH);
    return result;
  }
  if (snapshot.state != ServiceState::Running) {
    result.error = LastError("Require running service for temporary stop",
                             ERROR_SERVICE_NOT_ACTIVE);
    return result;
  }
  if (!snapshot.accepts_stop) {
    result.error = LastError("Require stoppable service",
                             ERROR_SERVICE_CANNOT_ACCEPT_CTRL);
    return result;
  }
  bool has_dependents = false;
  if (!HasActiveDependents(service.Get(), &has_dependents, &result.error)) {
    return result;
  }
  if (has_dependents) {
    result.error = LastError("Reject service with active dependents",
                             ERROR_DEPENDENT_SERVICES_RUNNING);
    return result;
  }

  ServiceSnapshot final_status;
  if (!PopulateServiceStatus(service.Get(), &final_status, &result.error)) {
    return result;
  }
  if (final_status.state != ServiceState::Running ||
      final_status.pid != expected_pid || !final_status.accepts_stop) {
    result.state_after = final_status.state;
    result.error = LastError("Revalidate service process before control",
                             ERROR_REVISION_MISMATCH);
    return result;
  }

  SERVICE_STATUS status{};
  if (!ControlService(service.Get(), SERVICE_CONTROL_STOP, &status)) {
    result.error = LastError("ControlService temporary stop");
    return result;
  }
  result.changed = true;
  if (!WaitForServiceState(service.Get(), ServiceState::Stopped, timeout_ms,
                           &result.state_after, &result.error)) {
    result.outcome_uncertain = true;
    return result;
  }
  result.success = true;
  return result;
}

ServiceControlResult ServiceApi::StartForRestore(
    const std::string& service_name,
    const std::string& expected_identity_token, std::uint32_t timeout_ms) {
  ServiceControlResult result;
  const std::wstring name = Utf8ToWide(service_name);
  if (name.empty()) {
    result.error = LastError("Validate service restore name", ERROR_INVALID_NAME);
    return result;
  }
  UniqueServiceHandle manager(
      OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!manager.Valid()) {
    result.error = LastError("OpenSCManager for service restore");
    return result;
  }
  UniqueServiceHandle service(OpenServiceW(
      manager.Get(), name.c_str(),
      SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | SERVICE_START));
  if (!service.Valid()) {
    result.error = LastError("OpenService for restore");
    return result;
  }
  ServiceSnapshot snapshot;
  snapshot.name = service_name;
  if (!PopulateServiceStatus(service.Get(), &snapshot, &result.error) ||
      !PopulateServiceIdentityFromHandle(service.Get(), name.c_str(), &snapshot,
                                         &result.error) ||
      !ValidateServiceIdentity(snapshot, expected_identity_token,
                               &result.error)) {
    return result;
  }
  result.state_before = snapshot.state;
  result.state_after = snapshot.state;
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  if (snapshot.state == ServiceState::Running) {
    result.success = true;
    return result;
  }
  if (snapshot.state == ServiceState::StartPending) {
    if (!WaitForServiceStateUntil(service.Get(), ServiceState::Running,
                                  deadline, &result.state_after,
                                  &result.error)) {
      return result;
    }
    result.success = true;
    return result;
  }
  if (snapshot.state == ServiceState::StopPending) {
    if (!WaitForServiceStateUntil(service.Get(), ServiceState::Stopped,
                                  deadline, &result.state_after,
                                  &result.error)) {
      return result;
    }
  } else if (snapshot.state != ServiceState::Stopped) {
    result.error = LastError("Require stopped service for restore",
                             ERROR_SERVICE_CANNOT_ACCEPT_CTRL);
    return result;
  }

  if (!StartServiceW(service.Get(), 0, nullptr)) {
    const DWORD code = GetLastError();
    if (code != ERROR_SERVICE_ALREADY_RUNNING) {
      result.error = LastError("StartService during restore", code);
      return result;
    }
  } else {
    result.changed = true;
  }
  if (!WaitForServiceStateUntil(service.Get(), ServiceState::Running, deadline,
                                &result.state_after, &result.error)) {
    result.outcome_uncertain = result.changed;
    return result;
  }
  result.success = true;
  return result;
}

}  // namespace workboost::windows
