#pragma once

#include "core/model/types.h"
#include "platform/windows/windows_utils.h"

#include <vector>

namespace workboost::windows {

struct SerialPortQueryResult {
  bool success{};
  std::vector<SerialPortSnapshot> ports;
  WindowsError error;
};

class SerialPortApi {
 public:
  // Read-only enumeration of currently present COM port devices.
  static SerialPortQueryResult QueryPresent();
};

}  // namespace workboost::windows
