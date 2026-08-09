#pragma once

#include "core/config/config.h"
#include "core/model/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace workboost {

enum class ActionType {
  GracefulCloseProcess,
  TerminateProcess,
  SetPriorityClass,
  StopServiceTemporary,
  StartServiceAction,
  DisableStartupEntry,
  RestoreStartupEntry,
};

enum class ActionRisk { Safe, Low, Medium, High, Forbidden };
enum class ActionState { Planned, Applied, RolledBack, Failed, Rejected };

struct OptimizationAction {
  std::string id;
  ActionType type{ActionType::SetPriorityClass};
  ActionRisk risk{ActionRisk::Safe};
  std::uint32_t pid{};
  std::uint64_t expected_start_time_100ns{};
  std::string process_name;
  std::uint32_t source_priority{};
  std::uint32_t target_priority{};
  std::string reason;
};

struct ExecutedAction {
  OptimizationAction action;
  ActionState state{ActionState::Planned};
  std::uint32_t original_priority{};
  unsigned long error_code{};
  std::string error_message;
};

struct OptimizationPlan {
  std::vector<OptimizationAction> actions;
  std::vector<std::string> rejected;
};

class OptimizationPlanner {
 public:
  explicit OptimizationPlanner(const Config& config) : config_(config) {}
  [[nodiscard]] OptimizationPlan Create(const SystemSnapshot& snapshot) const;

 private:
  const Config& config_;
};

class SafetyValidator {
 public:
  explicit SafetyValidator(const Config& config) : config_(config) {}
  [[nodiscard]] bool Validate(const OptimizationAction& action,
                              const SystemSnapshot& snapshot,
                              std::string* reason = nullptr) const;

 private:
  const Config& config_;
};

class ActionExecutor {
 public:
  explicit ActionExecutor(const Config& config) : config_(config) {}

  ExecutedAction Execute(const OptimizationAction& action,
                         const SystemSnapshot& current_snapshot) const;
  bool Rollback(ExecutedAction* action) const;

 private:
  const Config& config_;
};

std::string ToString(ActionType value);
std::string ToString(ActionRisk value);
std::string ToString(ActionState value);
ActionState ActionStateFromString(const std::string& value);
std::uint32_t PriorityFromName(const std::string& value);
std::string PriorityName(std::uint32_t value);

}  // namespace workboost
