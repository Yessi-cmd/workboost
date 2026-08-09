#include "core/policy/service_protection_policy.h"

#include <array>
#include <string>

namespace workboost {
namespace {

bool ContainsBlockedKeyword(const ServiceSnapshot& service) {
  const std::string text = ToLowerAscii(service.name + " " +
                                        service.display_name);
  static constexpr std::array<const char*, 20> kKeywords{
      "antivirus",    "anyconnect", "carbon black", "crowdstrike",
      "cylance",      "defender",   "endpoint",     "falcon",
      "forti",        "globalprotect", "mcafee",   "npcap",
      "openvpn",      "security",   "sentinel",     "sophos",
      "symantec",     "trend micro", "vpn",         "zscaler"};
  for (const char* keyword : kKeywords) {
    if (text.find(keyword) != std::string::npos) return true;
  }
  return false;
}

bool IsServiceClassNeverStopped(ServiceClass value) {
  return value == ServiceClass::System || value == ServiceClass::Network ||
         value == ServiceClass::Security ||
         value == ServiceClass::RemoteAccess ||
         value == ServiceClass::PacketCapture ||
         value == ServiceClass::Device || value == ServiceClass::Unknown;
}

}  // namespace

ProtectionLevel ServiceProtectionPolicy::Evaluate(
    const ServiceSnapshot& service, const RuntimeContext& context) const {
  const ServiceRule rule = config_.ServiceRuleFor(service.name);
  if (rule.protection == ProtectionLevel::SystemCritical ||
      rule.service_class == ServiceClass::System ||
      rule.service_class == ServiceClass::Network ||
      rule.service_class == ServiceClass::Security) {
    return ProtectionLevel::SystemCritical;
  }
  if (ContainsBlockedKeyword(service) ||
      IsServiceClassNeverStopped(rule.service_class) ||
      context.remote_session_pids.count(service.pid) != 0 ||
      context.active_capture_pids.count(service.pid) != 0) {
    return ProtectionLevel::Strong;
  }
  if (config_.IsAlwaysProtectedService(service.name)) {
    return ProtectionLevel::UserExplicit;
  }
  return rule.protection;
}

bool ServiceProtectionPolicy::CanStopTemporary(
    const ServiceSnapshot& service, const RuntimeContext& context) const {
  const ServiceRule rule = config_.ServiceRuleFor(service.name);
  const bool eligible_class = rule.service_class == ServiceClass::Updater ||
                              rule.service_class == ServiceClass::CloudSync ||
                              rule.service_class == ServiceClass::VendorUtility;
  return service.state == ServiceState::Running && service.accepts_stop &&
         service.identity_verified && !service.identity_token.empty() &&
         eligible_class &&
         Evaluate(service, context) == ProtectionLevel::Optimizable &&
         config_.coding_profile.allow_service_stop.count(
             ToLowerAscii(service.name)) != 0;
}

bool ServiceProtectionPolicy::CanRestore(
    const ServiceSnapshot& service, const RuntimeContext& context) const {
  const ServiceRule rule = config_.ServiceRuleFor(service.name);
  const bool eligible_class = rule.service_class == ServiceClass::Updater ||
                              rule.service_class == ServiceClass::CloudSync ||
                              rule.service_class == ServiceClass::VendorUtility;
  return service.identity_verified && !service.identity_token.empty() &&
         eligible_class &&
         Evaluate(service, context) == ProtectionLevel::Optimizable &&
         config_.coding_profile.allow_service_stop.count(
             ToLowerAscii(service.name)) != 0;
}

}  // namespace workboost
