#pragma once

#include "core/benchmark/benchmark.h"

#include <cstdint>
#include <functional>
#include <string>

namespace workboost::windows {

class StartupBenchmarkApi {
 public:
  // Passively observes a process created after this call. It never launches,
  // terminates, or modifies the target process. on_ready is invoked only
  // after the existing-process baseline has been captured.
  static StartupObservation ObserveNewProcess(const std::string& process_name,
                                               std::uint32_t timeout_ms,
                                               const std::function<void()>&
                                                   on_ready = {});
};

}  // namespace workboost::windows
