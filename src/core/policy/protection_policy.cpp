#include "core/policy/protection_policy.h"

namespace workboost {

ProtectionLevel ProtectionPolicy::Evaluate(
    const ProcessSnapshot& process, const RuntimeContext& context) const {
  if (process.pid <= 4) return ProtectionLevel::SystemCritical;
  if (config_.IsAlwaysProtected(process.name)) {
    return ProtectionLevel::UserExplicit;
  }
  if (process.classification == ProcessClass::System ||
      process.classification == ProcessClass::Security) {
    return ProtectionLevel::SystemCritical;
  }
  if (context.remote_session_pids.count(process.pid) != 0 ||
      context.active_capture_pids.count(process.pid) != 0) {
    return ProtectionLevel::Strong;
  }
  switch (process.classification) {
    case ProcessClass::Development:
    case ProcessClass::RemoteTerminal:
    case ProcessClass::SerialTerminal:
    case ProcessClass::PacketCapture:
    case ProcessClass::BuildTool:
    case ProcessClass::VersionControl:
    case ProcessClass::Unknown:
      return ProtectionLevel::Strong;
    default:
      return config_.RuleFor(process.name).protection;
  }
}

bool ProtectionPolicy::IsProtected(const ProcessSnapshot& process,
                                   const RuntimeContext& context) const {
  const auto level = Evaluate(process, context);
  return level == ProtectionLevel::SystemCritical ||
         level == ProtectionLevel::Strong ||
         level == ProtectionLevel::UserExplicit;
}

bool ProtectionPolicy::CanChangePriority(const ProcessSnapshot& process,
                                         const RuntimeContext& context,
                                         bool raise_priority) const {
  const auto level = Evaluate(process, context);
  if (level == ProtectionLevel::SystemCritical) return false;
  if (raise_priority) {
    return process.classification == ProcessClass::Development &&
           config_.coding_profile.foreground_priority.count(
               ToLowerAscii(process.name)) != 0;
  }
  if (level == ProtectionLevel::Strong ||
      level == ProtectionLevel::UserExplicit) {
    return false;
  }
  return config_.coding_profile.allow_priority_down.count(
             ToLowerAscii(process.name)) != 0;
}

bool ProtectionPolicy::CanGracefullyClose(
    const ProcessSnapshot& process, const RuntimeContext& context) const {
  if (IsProtected(process, context)) return false;
  return config_.coding_profile.allow_graceful_close.count(
             ToLowerAscii(process.name)) != 0;
}

}  // namespace workboost
