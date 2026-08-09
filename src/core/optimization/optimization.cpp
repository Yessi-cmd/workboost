#include "core/optimization/optimization.h"

#include "core/policy/protection_policy.h"
#include "platform/windows/windows_utils.h"

#include <windows.h>

#include <algorithm>
#include <sstream>

namespace workboost {
namespace {

const ProcessSnapshot* FindProcess(const SystemSnapshot& snapshot,
                                   std::uint32_t pid) {
  const auto it = std::find_if(snapshot.processes.begin(), snapshot.processes.end(),
                               [pid](const ProcessSnapshot& process) {
                                 return process.pid == pid;
                               });
  return it == snapshot.processes.end() ? nullptr : &*it;
}

int PriorityRank(std::uint32_t priority) {
  switch (priority) {
    case IDLE_PRIORITY_CLASS: return 0;
    case BELOW_NORMAL_PRIORITY_CLASS: return 1;
    case NORMAL_PRIORITY_CLASS: return 2;
    case ABOVE_NORMAL_PRIORITY_CLASS: return 3;
    case HIGH_PRIORITY_CLASS: return 4;
    case REALTIME_PRIORITY_CLASS: return 5;
    default: return 2;
  }
}

bool SameProcess(HANDLE process, std::uint64_t expected_start_time) {
  if (expected_start_time == 0) return true;
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
  ULARGE_INTEGER value{};
  value.LowPart = created.dwLowDateTime;
  value.HighPart = created.dwHighDateTime;
  return value.QuadPart == expected_start_time;
}

}  // namespace

OptimizationPlan OptimizationPlanner::Create(
    const SystemSnapshot& snapshot) const {
  OptimizationPlan plan;
  const RuntimeContext context =
      BuildRuntimeContext(snapshot, config_.remote_debug_ports);
  ProtectionPolicy policy(config_);
  for (const auto& process : snapshot.processes) {
    const std::string name = ToLowerAscii(process.name);
    const auto raise = config_.coding_profile.foreground_priority.find(name);
    const bool interactive_target = name == "codex.exe" ||
                                    process.has_visible_window ||
                                    process.is_foreground;
    if (raise != config_.coding_profile.foreground_priority.end() &&
        interactive_target) {
      const std::uint32_t target = PriorityFromName(raise->second);
      if (target == 0) {
        plan.rejected.push_back(process.name + ": unknown priority value");
      } else if (target != process.priority_class) {
        OptimizationAction action;
        action.id = "priority-up-" + std::to_string(process.pid);
        action.type = ActionType::SetPriorityClass;
        action.risk = ActionRisk::Safe;
        action.pid = process.pid;
        action.expected_start_time_100ns = process.start_time_100ns;
        action.process_name = process.name;
        action.source_priority = process.priority_class;
        action.target_priority = target;
        action.reason = "Coding profile foreground priority";
        if (policy.CanChangePriority(process, context, true)) {
          plan.actions.push_back(std::move(action));
        } else {
          plan.rejected.push_back(process.name +
                                  ": protection policy rejected priority raise");
        }
      }
      continue;
    }

    if (config_.coding_profile.allow_priority_down.count(name) != 0 &&
        process.priority_class != BELOW_NORMAL_PRIORITY_CLASS) {
      OptimizationAction action;
      action.id = "priority-down-" + std::to_string(process.pid);
      action.type = ActionType::SetPriorityClass;
      action.risk = ActionRisk::Safe;
      action.pid = process.pid;
      action.expected_start_time_100ns = process.start_time_100ns;
      action.process_name = process.name;
      action.source_priority = process.priority_class;
      action.target_priority = BELOW_NORMAL_PRIORITY_CLASS;
      action.reason = "Explicit coding profile background priority";
      if (policy.CanChangePriority(process, context, false)) {
        plan.actions.push_back(std::move(action));
      } else {
        plan.rejected.push_back(process.name +
                                ": protection policy rejected priority decrease");
      }
    }
  }
  return plan;
}

bool SafetyValidator::Validate(const OptimizationAction& action,
                               const SystemSnapshot& snapshot,
                               std::string* reason) const {
  if (action.type != ActionType::SetPriorityClass) {
    if (reason) *reason = "V1 only permits typed priority actions";
    return false;
  }
  if (action.risk != ActionRisk::Safe) {
    if (reason) *reason = "automatic execution requires Safe risk";
    return false;
  }
  const ProcessSnapshot* process = FindProcess(snapshot, action.pid);
  if (process == nullptr) {
    if (reason) *reason = "target process is no longer present";
    return false;
  }
  if (process->start_time_100ns != action.expected_start_time_100ns) {
    if (reason) *reason = "PID was reused by another process";
    return false;
  }
  if (action.target_priority == HIGH_PRIORITY_CLASS ||
      action.target_priority == REALTIME_PRIORITY_CLASS ||
      action.target_priority == IDLE_PRIORITY_CLASS) {
    if (reason) *reason = "target priority is outside the safe allowlist";
    return false;
  }
  const bool raise = PriorityRank(action.target_priority) >
                     PriorityRank(process->priority_class);
  ProtectionPolicy policy(config_);
  if (!policy.CanChangePriority(
          *process,
          BuildRuntimeContext(snapshot, config_.remote_debug_ports), raise)) {
    if (reason) *reason = "target is protected by policy";
    return false;
  }
  return true;
}

ExecutedAction ActionExecutor::Execute(
    const OptimizationAction& action,
    const SystemSnapshot& current_snapshot) const {
  ExecutedAction result;
  result.action = action;
  std::string rejection_reason;
  if (!SafetyValidator(config_).Validate(action, current_snapshot,
                                         &rejection_reason)) {
    result.state = ActionState::Rejected;
    result.error_code = ERROR_ACCESS_DISABLED_BY_POLICY;
    result.error_message = rejection_reason;
    return result;
  }
  if (action.type != ActionType::SetPriorityClass) {
    result.state = ActionState::Rejected;
    result.error_code = ERROR_NOT_SUPPORTED;
    result.error_message = "action type is not implemented";
    return result;
  }
  windows::UniqueHandle process(OpenProcess(
      PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE,
      action.pid));
  if (!process.Valid()) {
    const auto error = windows::LastError("OpenProcess");
    result.state = ActionState::Failed;
    result.error_code = error.code;
    result.error_message = error.Describe();
    return result;
  }
  if (!SameProcess(process.Get(), action.expected_start_time_100ns)) {
    result.state = ActionState::Rejected;
    result.error_code = ERROR_INVALID_PARAMETER;
    result.error_message = "PID identity changed before execution";
    return result;
  }
  result.original_priority = GetPriorityClass(process.Get());
  if (result.original_priority == 0) {
    const auto error = windows::LastError("GetPriorityClass");
    result.state = ActionState::Failed;
    result.error_code = error.code;
    result.error_message = error.Describe();
    return result;
  }
  if (!SetPriorityClass(process.Get(), action.target_priority)) {
    const auto error = windows::LastError("SetPriorityClass");
    result.state = ActionState::Failed;
    result.error_code = error.code;
    result.error_message = error.Describe();
    return result;
  }
  result.state = ActionState::Applied;
  return result;
}

bool ActionExecutor::Rollback(ExecutedAction* action) const {
  if (action == nullptr || action->state == ActionState::RolledBack ||
      action->state == ActionState::Rejected ||
      action->state == ActionState::Failed) {
    return true;
  }
  if (action->action.type != ActionType::SetPriorityClass ||
      action->original_priority == 0) {
    action->state = ActionState::Failed;
    action->error_code = ERROR_NOT_SUPPORTED;
    action->error_message = "action has no reversible priority payload";
    return false;
  }
  windows::UniqueHandle process(OpenProcess(
      PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE,
      action->action.pid));
  if (!process.Valid()) {
    const auto error = windows::LastError("OpenProcess during rollback");
    action->state = ActionState::Failed;
    action->error_code = error.code;
    action->error_message = error.Describe();
    return false;
  }
  if (!SameProcess(process.Get(), action->action.expected_start_time_100ns)) {
    // The original process already exited; there is nothing left to restore.
    action->state = ActionState::RolledBack;
    return true;
  }
  if (!SetPriorityClass(process.Get(), action->original_priority)) {
    const auto error = windows::LastError("SetPriorityClass during rollback");
    action->state = ActionState::Failed;
    action->error_code = error.code;
    action->error_message = error.Describe();
    return false;
  }
  action->state = ActionState::RolledBack;
  return true;
}

std::string ToString(ActionType value) {
  switch (value) {
    case ActionType::GracefulCloseProcess: return "GracefulCloseProcess";
    case ActionType::TerminateProcess: return "TerminateProcess";
    case ActionType::SetPriorityClass: return "SetPriorityClass";
    case ActionType::StopServiceTemporary: return "StopServiceTemporary";
    case ActionType::StartServiceAction: return "StartService";
    case ActionType::DisableStartupEntry: return "DisableStartupEntry";
    case ActionType::RestoreStartupEntry: return "RestoreStartupEntry";
  }
  return "Unknown";
}

std::string ToString(ActionRisk value) {
  switch (value) {
    case ActionRisk::Safe: return "Safe";
    case ActionRisk::Low: return "Low";
    case ActionRisk::Medium: return "Medium";
    case ActionRisk::High: return "High";
    case ActionRisk::Forbidden: return "Forbidden";
  }
  return "Forbidden";
}

std::string ToString(ActionState value) {
  switch (value) {
    case ActionState::Planned: return "Planned";
    case ActionState::Applied: return "Applied";
    case ActionState::RolledBack: return "RolledBack";
    case ActionState::Failed: return "Failed";
    case ActionState::Rejected: return "Rejected";
  }
  return "Failed";
}

ActionState ActionStateFromString(const std::string& value) {
  if (value == "Planned") return ActionState::Planned;
  if (value == "Applied") return ActionState::Applied;
  if (value == "RolledBack") return ActionState::RolledBack;
  if (value == "Rejected") return ActionState::Rejected;
  return ActionState::Failed;
}

std::uint32_t PriorityFromName(const std::string& value) {
  const auto lower = ToLowerAscii(value);
  if (lower == "idle") return IDLE_PRIORITY_CLASS;
  if (lower == "below_normal") return BELOW_NORMAL_PRIORITY_CLASS;
  if (lower == "normal") return NORMAL_PRIORITY_CLASS;
  if (lower == "above_normal") return ABOVE_NORMAL_PRIORITY_CLASS;
  if (lower == "high") return HIGH_PRIORITY_CLASS;
  if (lower == "realtime") return REALTIME_PRIORITY_CLASS;
  return 0;
}

std::string PriorityName(std::uint32_t value) {
  switch (value) {
    case IDLE_PRIORITY_CLASS: return "idle";
    case BELOW_NORMAL_PRIORITY_CLASS: return "below_normal";
    case NORMAL_PRIORITY_CLASS: return "normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "above_normal";
    case HIGH_PRIORITY_CLASS: return "high";
    case REALTIME_PRIORITY_CLASS: return "realtime";
    default: return "unknown";
  }
}

}  // namespace workboost
