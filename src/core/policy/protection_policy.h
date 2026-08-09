#pragma once

#include "core/config/config.h"
#include "core/model/types.h"

namespace workboost {

class ProtectionPolicy {
 public:
  explicit ProtectionPolicy(const Config& config) : config_(config) {}

  [[nodiscard]] ProtectionLevel Evaluate(
      const ProcessSnapshot& process, const RuntimeContext& context) const;
  [[nodiscard]] bool IsProtected(const ProcessSnapshot& process,
                                 const RuntimeContext& context) const;
  [[nodiscard]] bool CanChangePriority(const ProcessSnapshot& process,
                                       const RuntimeContext& context,
                                       bool raise_priority) const;
  [[nodiscard]] bool CanGracefullyClose(const ProcessSnapshot& process,
                                        const RuntimeContext& context) const;

 private:
  const Config& config_;
};

}  // namespace workboost
