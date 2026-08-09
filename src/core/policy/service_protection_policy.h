#pragma once

#include "core/config/config.h"
#include "core/model/types.h"

namespace workboost {

class ServiceProtectionPolicy {
 public:
  explicit ServiceProtectionPolicy(const Config& config) : config_(config) {}

  [[nodiscard]] ProtectionLevel Evaluate(
      const ServiceSnapshot& service,
      const RuntimeContext& context = RuntimeContext{}) const;
  [[nodiscard]] bool CanStopTemporary(
      const ServiceSnapshot& service,
      const RuntimeContext& context = RuntimeContext{}) const;
  [[nodiscard]] bool CanRestore(
      const ServiceSnapshot& service,
      const RuntimeContext& context = RuntimeContext{}) const;

 private:
  const Config& config_;
};

}  // namespace workboost
