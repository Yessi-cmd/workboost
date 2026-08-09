#pragma once

#include "core/helper/helper_protocol.h"
#include "platform/windows/windows_utils.h"

#include <cstdint>
#include <string>

namespace workboost::windows {

struct ElevatedHelperResult {
  bool transport_success{};
  bool request_sent{};
  bool user_cancelled{};
  helper::Response response;
  WindowsError error;
};

class ElevatedHelperClient {
 public:
  static ElevatedHelperResult Execute(
      helper::Command command, const std::string& service_name,
      const std::string& expected_identity_token, std::uint32_t timeout_ms);
};

}  // namespace workboost::windows
