#pragma once

#include "core/model/types.h"
#include "platform/windows/windows_utils.h"

#include <vector>

namespace workboost::windows {

struct StartupQueryResult {
  bool success{};
  std::vector<StartupEntrySnapshot> entries;
  WindowsError error;
};

class StartupApi {
 public:
  // Read-only enumeration. Command arguments and full paths are never returned.
  static StartupQueryResult QueryAll();
};

}  // namespace workboost::windows
