#pragma once

#include "core/config/config.h"
#include "core/model/types.h"

#include <cstdint>
#include <optional>
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
enum class ActionState {
  Planned,
  Applied,
  Completed,
  RolledBack,
  Uncertain,
  Failed,
  Rejected,
};

struct OptimizationAction {
  std::string id;
  ActionType type{ActionType::SetPriorityClass};
  ActionRisk risk{ActionRisk::Safe};
  std::uint32_t pid{};
  std::uint64_t expected_start_time_100ns{};
  std::string process_name;
  std::uint32_t source_priority{};
  std::uint32_t target_priority{};
  std::uint32_t timeout_ms{2000};
  std::string service_name;
  std::string expected_service_identity;
  ServiceState source_service_state{ServiceState::Unknown};
  bool explicit_confirmation{};
  std::string reason;
};

struct ExecutedAction {
  OptimizationAction action;
  ActionState state{ActionState::Planned};
  std::uint32_t original_priority{};
  ServiceState original_service_state{ServiceState::Unknown};
  unsigned long error_code{};
  std::string error_message;
  std::string result_message;
};

struct ProcessSelection {
  std::uint32_t pid{};
  std::uint64_t expected_start_time_100ns{};

  [[nodiscard]] bool operator==(const ProcessSelection& other) const {
    return pid == other.pid &&
           expected_start_time_100ns == other.expected_start_time_100ns;
  }
};

struct OptimizationPlan {
  std::vector<OptimizationAction> actions;
  std::vector<std::string> rejected;
};

class OptimizationPlanner {
 public:
  explicit OptimizationPlanner(const Config& config) : config_(config) {}
  [[nodiscard]] OptimizationPlan Create(
      const SystemSnapshot& snapshot,
      const std::vector<ProcessSelection>& explicit_close = {}) const;
  [[nodiscard]] OptimizationPlan Create(
      const SnapshotHistory& history,
      const std::vector<ProcessSelection>& explicit_close = {}) const;

 private:
  [[nodiscard]] OptimizationPlan CreateFrom(
      const SystemSnapshot& snapshot, const RuntimeContext& context,
      bool window_coverage_ok,
      const std::vector<ProcessSelection>& explicit_close) const;

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
  // Validates and sends WM_CLOSE for every action in one shared deadline.
  // Results preserve input order; rejected actions stay Rejected and are not
  // sent. Only GracefulCloseProcess actions are accepted.
  std::vector<ExecutedAction> ExecuteGracefulCloseBatch(
      const std::vector<OptimizationAction>& actions,
      const SystemSnapshot& current_snapshot,
      std::uint32_t shared_timeout_ms) const;
  bool Rollback(ExecutedAction* action) const;

 private:
  const Config& config_;
};

std::string ToString(ActionType value);
std::string ToString(ActionRisk value);
std::string ToString(ActionState value);
std::optional<ActionType> ActionTypeFromString(const std::string& value);
std::optional<ActionRisk> ActionRiskFromString(const std::string& value);
std::optional<ActionState> ActionStateFromString(const std::string& value);
bool IsReversible(ActionType value);
std::uint32_t PriorityFromName(const std::string& value);
std::string PriorityName(std::uint32_t value);

}  // namespace workboost
