#include "core/optimization/optimization.h"

#include "core/policy/protection_policy.h"
#include "core/policy/service_protection_policy.h"
#include "platform/windows/elevated_helper_client.h"
#include "platform/windows/process_action_api.h"

#include <windows.h>

#include <algorithm>

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

const ServiceSnapshot* FindService(const SystemSnapshot& snapshot,
                                   const std::string& service_name) {
  const std::string expected = ToLowerAscii(service_name);
  const auto it = std::find_if(
      snapshot.services.begin(), snapshot.services.end(),
      [&expected](const ServiceSnapshot& service) {
        return ToLowerAscii(service.name) == expected;
      });
  return it == snapshot.services.end() ? nullptr : &*it;
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
      } else if (process.start_time_100ns == 0 ||
                 process.priority_class == 0) {
        plan.rejected.push_back(
            process.name + ": process identity or priority is unavailable");
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

    if (config_.coding_profile.allow_graceful_close.count(name) != 0) {
      if (!snapshot.process_inventory_complete ||
          !snapshot.tcp_inventory_complete) {
        plan.rejected.push_back(
            process.name + ": protection inventory is incomplete");
      } else if (process.start_time_100ns == 0) {
        plan.rejected.push_back(process.name +
                                ": process identity is unavailable");
      } else if (process.is_foreground) {
        plan.rejected.push_back(process.name +
                                ": foreground process is not auto-closed");
      } else if (!process.has_visible_window) {
        plan.rejected.push_back(process.name +
                                ": no visible top-level window to close");
      } else {
        OptimizationAction action;
        action.id = "graceful-close-" + std::to_string(process.pid);
        action.type = ActionType::GracefulCloseProcess;
        action.risk = ActionRisk::Low;
        action.pid = process.pid;
        action.expected_start_time_100ns = process.start_time_100ns;
        action.process_name = process.name;
        action.timeout_ms = 2000;
        action.reason = "Explicit coding profile graceful close";
        if (policy.CanGracefullyClose(process, context)) {
          plan.actions.push_back(std::move(action));
        } else {
          plan.rejected.push_back(
              process.name + ": protection policy rejected graceful close");
        }
      }
      continue;
    }

    if (config_.coding_profile.allow_priority_down.count(name) != 0 &&
        process.priority_class != BELOW_NORMAL_PRIORITY_CLASS) {
      if (!snapshot.process_inventory_complete ||
          !snapshot.tcp_inventory_complete) {
        plan.rejected.push_back(
            process.name + ": protection inventory is incomplete");
        continue;
      }
      if (process.start_time_100ns == 0 || process.priority_class == 0) {
        plan.rejected.push_back(
            process.name + ": process identity or priority is unavailable");
        continue;
      }
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
  const ServiceProtectionPolicy service_policy(config_);
  for (const auto& service : snapshot.services) {
    const std::string name = ToLowerAscii(service.name);
    if (config_.coding_profile.allow_service_stop.count(name) == 0) continue;
    OptimizationAction action;
    action.id = "service-stop-" + name;
    action.type = ActionType::StopServiceTemporary;
    action.risk = ActionRisk::Medium;
    action.service_name = service.name;
    action.expected_service_identity = service.identity_token;
    action.source_service_state = service.state;
    action.timeout_ms = 15000;
    action.reason = "Explicit coding profile temporary service stop";
    if (service_policy.CanStopTemporary(service, context)) {
      plan.actions.push_back(std::move(action));
    } else {
      plan.rejected.push_back(service.name +
                              ": protection policy rejected temporary stop");
    }
  }
  return plan;
}

bool SafetyValidator::Validate(const OptimizationAction& action,
                               const SystemSnapshot& snapshot,
                               std::string* reason) const {
  if (action.type == ActionType::StopServiceTemporary) {
    if (action.risk != ActionRisk::Medium) {
      if (reason) *reason = "temporary service stop requires Medium risk";
      return false;
    }
    if (!action.explicit_confirmation) {
      if (reason) *reason = "temporary service stop requires confirmation";
      return false;
    }
    if (action.timeout_ms < 1000 || action.timeout_ms > 30000) {
      if (reason) *reason = "service timeout is outside the allowlist";
      return false;
    }
    if (!snapshot.process_inventory_complete ||
        !snapshot.tcp_inventory_complete ||
        !snapshot.service_inventory_complete) {
      if (reason) *reason = "service protection inventory is incomplete";
      return false;
    }
    const ServiceSnapshot* service =
        FindService(snapshot, action.service_name);
    if (service == nullptr) {
      if (reason) *reason = "target service is no longer present";
      return false;
    }
    if (!service->identity_verified ||
        service->identity_token != action.expected_service_identity) {
      if (reason) *reason = "target service identity changed after planning";
      return false;
    }
    const auto context =
        BuildRuntimeContext(snapshot, config_.remote_debug_ports);
    if (!ServiceProtectionPolicy(config_).CanStopTemporary(*service, context)) {
      if (reason) *reason = "target service is protected by policy";
      return false;
    }
    return true;
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
  if (ToLowerAscii(process->name) != ToLowerAscii(action.process_name)) {
    if (reason) *reason = "target process name does not match the plan";
    return false;
  }
  if (action.expected_start_time_100ns == 0 ||
      process->start_time_100ns == 0) {
    if (reason) *reason = "target process identity is unavailable";
    return false;
  }
  ProtectionPolicy policy(config_);
  const auto context =
      BuildRuntimeContext(snapshot, config_.remote_debug_ports);

  if (action.type == ActionType::SetPriorityClass) {
    if (action.risk != ActionRisk::Safe) {
      if (reason) *reason = "priority action requires Safe risk";
      return false;
    }
    if (action.target_priority == HIGH_PRIORITY_CLASS ||
        action.target_priority == REALTIME_PRIORITY_CLASS ||
        action.target_priority == IDLE_PRIORITY_CLASS ||
        action.target_priority == 0) {
      if (reason) *reason = "target priority is outside the safe allowlist";
      return false;
    }
    const bool raise = PriorityRank(action.target_priority) >
                       PriorityRank(process->priority_class);
    if (!raise && (!snapshot.process_inventory_complete ||
                   !snapshot.tcp_inventory_complete)) {
      if (reason) *reason = "process protection inventory is incomplete";
      return false;
    }
    if (!policy.CanChangePriority(*process, context, raise)) {
      if (reason) *reason = "target is protected by policy";
      return false;
    }
    return true;
  }

  if (action.type == ActionType::GracefulCloseProcess) {
    if (action.risk != ActionRisk::Low) {
      if (reason) *reason = "graceful close requires Low risk";
      return false;
    }
    if (action.timeout_ms < 100 || action.timeout_ms > 5000) {
      if (reason) *reason = "graceful close timeout is outside the allowlist";
      return false;
    }
    if (!snapshot.process_inventory_complete ||
        !snapshot.tcp_inventory_complete) {
      if (reason) *reason = "process protection inventory is incomplete";
      return false;
    }
    if (!process->has_visible_window || process->is_foreground) {
      if (reason) *reason = "target must be visible and outside the foreground";
      return false;
    }
    if (!policy.CanGracefullyClose(*process, context)) {
      if (reason) *reason = "target is protected by policy";
      return false;
    }
    return true;
  }

  if (reason) *reason = "action type is not executable in this version";
  return false;
}

ExecutedAction ActionExecutor::Execute(
    const OptimizationAction& action,
    const SystemSnapshot& current_snapshot) const {
  ExecutedAction result;
  result.action = action;
  if (action.type == ActionType::StopServiceTemporary) {
    result.original_service_state = action.source_service_state;
  }
  std::string rejection_reason;
  if (!SafetyValidator(config_).Validate(action, current_snapshot,
                                         &rejection_reason)) {
    result.state = ActionState::Rejected;
    result.error_code = ERROR_ACCESS_DISABLED_BY_POLICY;
    result.error_message = rejection_reason;
    return result;
  }
  if (action.type == ActionType::SetPriorityClass) {
    const auto changed = windows::ProcessActionApi::SetPriority(
        action.pid, action.expected_start_time_100ns, action.target_priority);
    result.original_priority = changed.original_priority;
    if (!changed.success) {
      result.state = ActionState::Failed;
      result.error_code = changed.error.code;
      result.error_message = changed.error.Describe();
      return result;
    }
    result.state = ActionState::Applied;
    return result;
  }

  if (action.type == ActionType::GracefulCloseProcess) {
    const auto closed = windows::ProcessActionApi::RequestGracefulClose(
        action.pid, action.expected_start_time_100ns, action.timeout_ms);
    if (!closed.success) {
      result.state = closed.delivery_uncertain ? ActionState::Uncertain
                                               : ActionState::Failed;
      result.error_code = closed.error.code;
      result.error_message = closed.error.Describe();
      if (closed.delivery_uncertain) {
        result.error_message +=
            "; WM_CLOSE delivery could not be determined and will not be "
            "replayed automatically";
      }
      return result;
    }
    result.state = ActionState::Completed;
    result.result_message =
        closed.process_exited
            ? "graceful close completed and process exited"
            : "WM_CLOSE delivered; process may be awaiting user confirmation";
    return result;
  }

  if (action.type == ActionType::StopServiceTemporary) {
    const auto elevated = windows::ElevatedHelperClient::Execute(
        helper::Command::StopServiceTemporary, action.service_name,
        action.expected_service_identity, action.timeout_ms);
    if (!elevated.transport_success) {
      result.state = elevated.request_sent ? ActionState::Uncertain
                                           : ActionState::Failed;
      result.error_code = elevated.error.code;
      result.error_message = elevated.user_cancelled
                                 ? "administrator confirmation was cancelled"
                                 : elevated.error.Describe();
      return result;
    }
    result.error_code = elevated.response.error_code;
    result.error_message = elevated.response.message;
    if (elevated.response.status == helper::Status::Succeeded &&
        elevated.response.state_after == ServiceState::Stopped) {
      result.state = ActionState::Applied;
      result.result_message = elevated.response.message;
    } else if (elevated.response.status == helper::Status::Rejected) {
      result.state = ActionState::Rejected;
    } else if (elevated.response.status == helper::Status::Uncertain ||
               elevated.response.status == helper::Status::Succeeded) {
      result.state = ActionState::Uncertain;
    } else {
      result.state = ActionState::Failed;
    }
    return result;
  }

  result.state = ActionState::Rejected;
  result.error_code = ERROR_NOT_SUPPORTED;
  result.error_message = "action type is not implemented";
  return result;
}

bool ActionExecutor::Rollback(ExecutedAction* action) const {
  if (action == nullptr || action->state == ActionState::RolledBack ||
      action->state == ActionState::Rejected ||
      action->state == ActionState::Failed) {
    return true;
  }
  if (action->state == ActionState::Completed &&
      !IsReversible(action->action.type)) {
    return true;
  }
  if (action->action.type == ActionType::GracefulCloseProcess) {
    action->state = ActionState::Uncertain;
    action->error_code = ERROR_NOT_SUPPORTED;
    action->error_message =
        "graceful close may have been delivered and cannot be rolled back";
    return false;
  }
  if (action->action.type == ActionType::StopServiceTemporary) {
    if (action->original_service_state != ServiceState::Running ||
        action->action.service_name.empty() ||
        action->action.expected_service_identity.empty()) {
      action->state = ActionState::Uncertain;
      action->error_code = ERROR_INVALID_DATA;
      action->error_message = "service action has no valid restore payload";
      return false;
    }
    const auto elevated = windows::ElevatedHelperClient::Execute(
        helper::Command::StartServiceRestore, action->action.service_name,
        action->action.expected_service_identity, action->action.timeout_ms);
    if (!elevated.transport_success) {
      action->state = ActionState::Uncertain;
      action->error_code = elevated.error.code;
      action->error_message = elevated.user_cancelled
                                  ? "administrator confirmation was cancelled"
                                  : elevated.error.Describe();
      return false;
    }
    if (elevated.response.status != helper::Status::Succeeded ||
        elevated.response.state_after != ServiceState::Running) {
      action->state = ActionState::Uncertain;
      action->error_code = elevated.response.error_code;
      action->error_message = elevated.response.message;
      return false;
    }
    action->state = ActionState::RolledBack;
    action->error_code = 0;
    action->error_message.clear();
    action->result_message = "service restored to its original running state";
    return true;
  }
  if (action->action.type != ActionType::SetPriorityClass ||
      action->original_priority == 0) {
    action->state = ActionState::Uncertain;
    action->error_code = ERROR_NOT_SUPPORTED;
    action->error_message = "action has no reversible priority payload";
    return false;
  }
  windows::WindowsError error;
  if (!windows::ProcessActionApi::RestorePriority(
          action->action.pid, action->action.expected_start_time_100ns,
          action->original_priority, &error)) {
    action->state = ActionState::Uncertain;
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
    case ActionState::Completed: return "Completed";
    case ActionState::RolledBack: return "RolledBack";
    case ActionState::Uncertain: return "Uncertain";
    case ActionState::Failed: return "Failed";
    case ActionState::Rejected: return "Rejected";
  }
  return "Failed";
}

std::optional<ActionType> ActionTypeFromString(const std::string& value) {
  if (value == "GracefulCloseProcess")
    return ActionType::GracefulCloseProcess;
  if (value == "TerminateProcess") return ActionType::TerminateProcess;
  if (value == "SetPriorityClass") return ActionType::SetPriorityClass;
  if (value == "StopServiceTemporary")
    return ActionType::StopServiceTemporary;
  if (value == "StartService") return ActionType::StartServiceAction;
  if (value == "DisableStartupEntry")
    return ActionType::DisableStartupEntry;
  if (value == "RestoreStartupEntry") return ActionType::RestoreStartupEntry;
  return std::nullopt;
}

std::optional<ActionRisk> ActionRiskFromString(const std::string& value) {
  if (value == "Safe") return ActionRisk::Safe;
  if (value == "Low") return ActionRisk::Low;
  if (value == "Medium") return ActionRisk::Medium;
  if (value == "High") return ActionRisk::High;
  if (value == "Forbidden") return ActionRisk::Forbidden;
  return std::nullopt;
}

std::optional<ActionState> ActionStateFromString(const std::string& value) {
  if (value == "Planned") return ActionState::Planned;
  if (value == "Applied") return ActionState::Applied;
  if (value == "Completed") return ActionState::Completed;
  if (value == "RolledBack") return ActionState::RolledBack;
  if (value == "Uncertain") return ActionState::Uncertain;
  if (value == "Rejected") return ActionState::Rejected;
  if (value == "Failed") return ActionState::Failed;
  return std::nullopt;
}

bool IsReversible(ActionType value) {
  return value == ActionType::SetPriorityClass ||
         value == ActionType::StopServiceTemporary ||
         value == ActionType::DisableStartupEntry;
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
