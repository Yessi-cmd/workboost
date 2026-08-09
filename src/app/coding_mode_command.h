#pragma once

#include "platform/windows/windows_utils.h"

#include <cstdint>
#include <string>

namespace workboost {

enum class CodingModeCommand { Enter, Exit, Restore };

struct CodingModeCommandResult {
  bool launched{};
  std::uint32_t exit_code{};
  std::string output;
  windows::WindowsError error;

  [[nodiscard]] bool Succeeded() const {
    return launched && exit_code == 0 && error.code == 0;
  }
};

// Strongly typed application boundary used by the GUI. The implementation
// launches only the current WorkBoost executable with fixed Coding Mode or
// recovery arguments; it never accepts a shell command or arbitrary payload.
class CodingModeCommandClient {
 public:
  static CodingModeCommandResult Execute(CodingModeCommand command,
                                         int baseline_seconds = 10);
};

}  // namespace workboost
