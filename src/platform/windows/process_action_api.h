#pragma once

#include "platform/windows/windows_utils.h"

#include <cstdint>

namespace workboost::windows {

struct PriorityChangeResult {
  bool success{};
  std::uint32_t original_priority{};
  WindowsError error;
};

struct GracefulCloseResult {
  bool success{};
  bool delivery_uncertain{};
  std::uint32_t windows_signaled{};
  bool process_exited{};
  WindowsError error;
};

class ProcessActionApi {
 public:
  static PriorityChangeResult SetPriority(
      std::uint32_t pid, std::uint64_t expected_start_time_100ns,
      std::uint32_t target_priority);

  static bool RestorePriority(std::uint32_t pid,
                              std::uint64_t expected_start_time_100ns,
                              std::uint32_t original_priority,
                              WindowsError* error = nullptr);

  // Sends WM_CLOSE only. It never terminates, suspends, or injects into the
  // target process. A successful request may leave the process running while
  // it displays an unsaved-work prompt.
  static GracefulCloseResult RequestGracefulClose(
      std::uint32_t pid, std::uint64_t expected_start_time_100ns,
      std::uint32_t timeout_ms);
};

}  // namespace workboost::windows
