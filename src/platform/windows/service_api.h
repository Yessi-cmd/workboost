#pragma once

#include "core/model/types.h"
#include "platform/windows/windows_utils.h"

#include <vector>

namespace workboost::windows {

struct ServiceQueryResult {
  bool success{};
  std::vector<ServiceSnapshot> services;
  WindowsError error;
};

struct ServiceLookupResult {
  bool success{};
  ServiceSnapshot service;
  WindowsError error;
};

struct ServiceControlResult {
  bool success{};
  bool changed{};
  bool outcome_uncertain{};
  ServiceState state_before{ServiceState::Unknown};
  ServiceState state_after{ServiceState::Unknown};
  WindowsError error;
};

class ServiceApi {
 public:
  // Read-only SCM enumeration. This API never opens an individual service for
  // mutation and never sends a service control code.
  static ServiceQueryResult QueryAll();
  static ServiceLookupResult Query(const std::string& service_name);

  // These methods are intended only for the separately elevated helper after
  // the core policy and helper protocol have independently validated a typed
  // request. They never change service startup configuration.
  static ServiceControlResult StopTemporary(
      const std::string& service_name,
      const std::string& expected_identity_token,
      std::uint32_t expected_pid,
      std::uint32_t timeout_ms);
  static ServiceControlResult StartForRestore(
      const std::string& service_name,
      const std::string& expected_identity_token,
      std::uint32_t timeout_ms);
};

}  // namespace workboost::windows
