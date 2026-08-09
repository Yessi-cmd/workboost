#pragma once

#include "core/config/config.h"
#include "core/model/types.h"
#include "platform/windows/windows_utils.h"

#include <memory>

namespace workboost::windows {

class SystemCollector {
 public:
  explicit SystemCollector(const Config& config);
  ~SystemCollector();

  SystemCollector(const SystemCollector&) = delete;
  SystemCollector& operator=(const SystemCollector&) = delete;

  bool Initialize(WindowsError* error = nullptr);
  SystemSnapshot Sample(WindowsError* error = nullptr);
  [[nodiscard]] bool Initialized() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace workboost::windows
