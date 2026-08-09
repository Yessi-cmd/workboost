#pragma once

#include "core/config/config.h"
#include "core/model/types.h"

#include <vector>

namespace workboost {

class DiagnosisEngine {
 public:
  explicit DiagnosisEngine(const Config& config) : config_(config) {}

  [[nodiscard]] std::vector<DiagnosisResult> Evaluate(
      const SnapshotHistory& history) const;

 private:
  const Config& config_;
};

}  // namespace workboost
