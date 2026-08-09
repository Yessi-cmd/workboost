#include "app/coding_mode_command.h"
#include "app/locale.h"
#include "app/session_manager.h"
#include "app/elevated_action_handler.h"
#include "core/benchmark/benchmark.h"
#include "core/config/config.h"
#include "core/diagnosis/diagnosis_engine.h"
#include "core/helper/helper_protocol.h"
#include "core/logging/logger.h"
#include "core/optimization/optimization.h"
#include "core/policy/protection_policy.h"
#include "core/policy/service_protection_policy.h"
#include "gui/dashboard.h"
#include "gui/dashboard_model.h"
#include "gui/dashboard_renderer.h"
#include "platform/windows/service_api.h"
#include "platform/windows/helper_pipe.h"
#include "platform/windows/serial_port_api.h"
#include "platform/windows/startup_api.h"
#include "platform/windows/startup_benchmark_api.h"
#include "platform/windows/system_collector.h"
#include "platform/windows/system_hardware.h"
#include "platform/windows/windows_utils.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using workboost::ActionRisk;
using workboost::ActionExecutor;
using workboost::ActionState;
using workboost::ActionType;
using workboost::Config;
using workboost::DiagnosisEngine;
using workboost::DiskMedia;
using workboost::DiskSnapshot;
using workboost::OptimizationAction;
using workboost::OptimizationPlanner;
using workboost::ProcessClass;
using workboost::ProcessSnapshot;
using workboost::ProtectionLevel;
using workboost::ProtectionPolicy;
using workboost::SafetyValidator;
using workboost::ServiceSnapshot;
using workboost::SessionManager;
using workboost::SnapshotHistory;
using workboost::SystemSnapshot;
using workboost::RuntimeContext;
using workboost::TcpSession;
using workboost::TcpState;

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

bool HasDiagnosis(const std::vector<workboost::DiagnosisResult>& results,
                  const std::string& type) {
  for (const auto& result : results)
    if (result.type == type) return true;
  return false;
}

std::uint64_t CurrentProcessStartTime() {
  FILETIME created{}, exited{}, kernel{}, user{};
  Check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) !=
            FALSE,
        "current process times must be readable");
  ULARGE_INTEGER start{};
  start.LowPart = created.dwLowDateTime;
  start.HighPart = created.dwHighDateTime;
  return start.QuadPart;
}

void RemoveLineContaining(std::string* text, const std::string& needle) {
  const std::size_t position = text->find(needle);
  Check(position != std::string::npos,
        "expected serialized field is missing: " + needle);
  const std::size_t previous_newline = text->rfind('\n', position);
  const std::size_t line_start =
      previous_newline == std::string::npos ? 0 : previous_newline + 1;
  const std::size_t next_newline = text->find('\n', position);
  const std::size_t line_end =
      next_newline == std::string::npos ? text->size() : next_newline + 1;
  text->erase(line_start, line_end - line_start);
}

void TestCommitRatio() {
  workboost::MemorySnapshot memory;
  memory.commit_total_bytes = 80;
  memory.commit_limit_bytes = 100;
  Check(std::abs(memory.CommitRatio() - 0.8) < 0.0001,
        "commit ratio should be normalized");
  memory.commit_limit_bytes = 0;
  Check(memory.CommitRatio() == 0.0, "zero commit limit must be safe");
}

void TestCpuPercentageNormalization() {
  Check(std::abs(workboost::NormalizeProcessCpuPercent(
                     10'000'000, 1.0, 4) -
                 25.0) < 0.0001,
        "one fully used core on four logical processors should be 25 percent");
  Check(workboost::NormalizeProcessCpuPercent(10'000'000, 0.0, 4) == 0.0 &&
            workboost::NormalizeProcessCpuPercent(10'000'000, 1.0, 0) ==
                0.0,
        "invalid CPU normalization denominators must be safe");
  Check(workboost::NormalizeProcessCpuPercent(80'000'000, 1.0, 4) == 100.0,
        "process CPU percentage must be capped at whole-machine capacity");
}

void TestClassificationAndUnknownProtection() {
  const Config config = Config::Defaults();
  Check(config.RuleFor("SSH.EXE").process_class == ProcessClass::RemoteTerminal,
        "classification must be case-insensitive");
  for (const char* name : {"putty.exe", "xshell.exe", "securecrt.exe",
                           "ttermpro.exe"}) {
    const auto terminal = config.RuleFor(name);
    Check((terminal.process_class == ProcessClass::RemoteTerminal ||
           terminal.process_class == ProcessClass::SerialTerminal) &&
              terminal.protection == ProtectionLevel::Strong,
          std::string("known terminal must receive Strong protection: ") +
              name);
  }
  for (const char* name : {"wireshark.exe", "dumpcap.exe", "tshark.exe"}) {
    const auto capture = config.RuleFor(name);
    Check(capture.process_class == ProcessClass::PacketCapture &&
              capture.protection == ProtectionLevel::Strong,
          std::string("known capture tool must receive Strong protection: ") +
              name);
  }
  const auto unknown = config.RuleFor("PrivateVendorTool.exe");
  Check(unknown.process_class == ProcessClass::Unknown,
        "unlisted process must remain Unknown");
  Check(unknown.protection == ProtectionLevel::Strong,
        "unknown process must default to Strong protection");

  Config overridden = Config::Defaults();
  overridden.process_rules["svchost.exe"] =
      {ProcessClass::Updater, ProtectionLevel::Optimizable};
  const auto critical = overridden.RuleFor("SVCHOST.EXE");
  Check(critical.process_class == ProcessClass::System &&
            critical.protection == ProtectionLevel::SystemCritical,
        "configuration must not downgrade immutable system processes");
  overridden.process_rules["ssh.exe"] =
      {ProcessClass::Updater, ProtectionLevel::Optimizable};
  overridden.coding_profile.always_protect.clear();
  const auto immutable_ssh = overridden.RuleFor("SSH.EXE");
  Check(immutable_ssh.process_class == ProcessClass::RemoteTerminal &&
            immutable_ssh.protection == ProtectionLevel::Strong,
        "configuration must not reclassify or downgrade known workloads");
}

void TestTcpProtection() {
  Config config = Config::Defaults();
  ProcessSnapshot process;
  process.pid = 42;
  process.name = "custom-terminal.exe";
  process.classification = ProcessClass::Unknown;
  SystemSnapshot snapshot;
  snapshot.processes.push_back(process);
  snapshot.process_inventory_complete = true;
  snapshot.tcp_inventory_complete = true;
  snapshot.tcp_sessions.push_back(
      TcpSession{42, "127.0.0.1", 50000, "10.0.0.1", 22,
                 TcpState::Established, false});
  const auto context = workboost::BuildRuntimeContext(snapshot);
  Check(context.remote_session_pids.count(42) == 1,
        "established TCP/22 must map to protected PID");
  Check(ProtectionPolicy(config).Evaluate(process, context) ==
            ProtectionLevel::Strong,
        "SSH session owner must receive Strong protection");
}

void TestCustomRemotePortProtection() {
  SystemSnapshot snapshot;
  snapshot.tcp_sessions.push_back(
      TcpSession{77, "127.0.0.1", 50001, "10.0.0.2", 22222,
                 TcpState::Established, false});
  const std::unordered_set<std::uint16_t> ports{22, 23, 22222};
  const auto context = workboost::BuildRuntimeContext(snapshot, ports);
  Check(context.remote_session_pids.count(77) == 1,
        "configured remote debug port must protect its owning PID");
  snapshot.tcp_sessions.push_back(
      TcpSession{78, "127.0.0.1", 50002, "10.0.0.3", 22,
                 TcpState::Established, false});
  const auto immutable_defaults =
      workboost::BuildRuntimeContext(snapshot, {22222});
  Check(immutable_defaults.remote_session_pids.count(77) == 1 &&
            immutable_defaults.remote_session_pids.count(78) == 1,
        "custom port sets must not remove the built-in SSH/Telnet ports");
}

void TestCaptureProtection() {
  Config config = Config::Defaults();
  ProcessSnapshot dumpcap;
  dumpcap.pid = 7;
  dumpcap.name = "dumpcap.exe";
  dumpcap.classification = ProcessClass::PacketCapture;
  SystemSnapshot snapshot;
  snapshot.processes.push_back(dumpcap);
  const auto context = workboost::BuildRuntimeContext(snapshot);
  Check(context.active_capture_pids.count(7) == 1,
        "dumpcap must mark active packet capture");
}

void TestSafetyValidator() {
  Config config = Config::Defaults();
  SystemSnapshot snapshot;
  ProcessSnapshot ssh;
  ssh.pid = 10;
  ssh.name = "ssh.exe";
  ssh.start_time_100ns = 123;
  ssh.priority_class = NORMAL_PRIORITY_CLASS;
  ssh.classification = ProcessClass::RemoteTerminal;
  snapshot.processes.push_back(ssh);
  OptimizationAction action;
  action.id = "test";
  action.type = ActionType::SetPriorityClass;
  action.risk = ActionRisk::Safe;
  action.pid = ssh.pid;
  action.expected_start_time_100ns = ssh.start_time_100ns;
  action.process_name = ssh.name;
  action.target_priority = BELOW_NORMAL_PRIORITY_CLASS;
  std::string reason;
  Check(!SafetyValidator(config).Validate(action, snapshot, &reason),
        "protected SSH process must reject priority decrease");

  ProcessSnapshot ide;
  ide.pid = 11;
  ide.name = "Code.exe";
  ide.start_time_100ns = 456;
  ide.priority_class = NORMAL_PRIORITY_CLASS;
  ide.classification = ProcessClass::Development;
  snapshot.processes.push_back(ide);
  action.pid = ide.pid;
  action.expected_start_time_100ns = ide.start_time_100ns;
  action.process_name = ide.name;
  action.target_priority = ABOVE_NORMAL_PRIORITY_CLASS;
  Check(SafetyValidator(config).Validate(action, snapshot, &reason),
        "configured IDE priority raise should be safe");
  config.coding_profile.always_protect.insert("code.exe");
  Check(!SafetyValidator(config).Validate(action, snapshot, &reason),
        "user Always Protect must override a configured priority raise");
  config.coding_profile.always_protect.erase("code.exe");
  snapshot.processes.back().start_time_100ns = 0;
  action.expected_start_time_100ns = 0;
  Check(!SafetyValidator(config).Validate(action, snapshot, &reason),
        "missing process start time must reject an otherwise allowed action");
}

void TestGracefulClosePlanningAndProtection() {
  Config config = Config::Defaults();
  config.process_rules["exampleupdater.exe"] =
      {ProcessClass::Updater, ProtectionLevel::Optimizable};
  config.coding_profile.allow_graceful_close.insert("exampleupdater.exe");

  ProcessSnapshot updater;
  updater.pid = 21;
  updater.name = "ExampleUpdater.exe";
  updater.start_time_100ns = 789;
  updater.priority_class = NORMAL_PRIORITY_CLASS;
  updater.classification = ProcessClass::Updater;
  updater.protection_level = ProtectionLevel::Optimizable;
  updater.has_visible_window = true;
  SystemSnapshot snapshot;
  snapshot.processes.push_back(updater);
  snapshot.process_inventory_complete = true;
  snapshot.tcp_inventory_complete = true;

  const auto plan = OptimizationPlanner(config).Create(snapshot);
  Check(plan.actions.size() == 1,
        "explicit updater allowlist should plan one graceful close");
  Check(plan.actions[0].type == ActionType::GracefulCloseProcess &&
            plan.actions[0].risk == ActionRisk::Low,
        "graceful close must remain a typed Low-risk action");
  std::string reason;
  Check(SafetyValidator(config).Validate(plan.actions[0], snapshot, &reason),
        "visible background updater should pass independent validation");

  snapshot.tcp_inventory_complete = false;
  Check(OptimizationPlanner(config).Create(snapshot).actions.empty(),
        "incomplete TCP inventory must suppress graceful close planning");
  Check(!SafetyValidator(config).Validate(plan.actions[0], snapshot, &reason),
        "validator must reject graceful close with incomplete TCP inventory");
  snapshot.tcp_inventory_complete = true;

  snapshot.processes[0].is_foreground = true;
  Check(OptimizationPlanner(config).Create(snapshot).actions.empty(),
        "foreground application must not be auto-closed");
  Check(!SafetyValidator(config).Validate(plan.actions[0], snapshot, &reason),
        "validator must independently reject a foreground target");

  snapshot.processes[0].is_foreground = false;
  snapshot.tcp_sessions.push_back(
      TcpSession{updater.pid, "127.0.0.1", 50002, "10.0.0.3", 22,
                 TcpState::Established, false});
  Check(OptimizationPlanner(config).Create(snapshot).actions.empty(),
        "active remote session must override the close allowlist");
  Check(!SafetyValidator(config).Validate(plan.actions[0], snapshot, &reason),
        "validator must reject a newly protected remote-session PID");

  config.coding_profile.allow_graceful_close.clear();
  config.coding_profile.allow_priority_down.insert("exampleupdater.exe");
  snapshot.tcp_sessions.clear();
  snapshot.tcp_inventory_complete = false;
  SystemSnapshot complete_snapshot = snapshot;
  complete_snapshot.tcp_inventory_complete = true;
  const auto down_plan = OptimizationPlanner(config).Create(complete_snapshot);
  Check(down_plan.actions.size() == 1 &&
            down_plan.actions[0].type == ActionType::SetPriorityClass,
        "explicit priority decrease requires complete protection inventories");
  Check(OptimizationPlanner(config).Create(snapshot).actions.empty(),
        "incomplete TCP inventory must suppress priority decrease planning");
  Check(!SafetyValidator(config).Validate(down_plan.actions[0], snapshot,
                                          &reason),
        "validator must reject priority decrease with incomplete TCP inventory");
}

void TestServiceProtectionPolicy() {
  Config config = Config::Defaults();
  ServiceSnapshot service;
  service.name = "ExampleUpdater";
  service.display_name = "Example updater service";
  service.state = workboost::ServiceState::Running;
  service.accepts_stop = true;
  service.identity_verified = true;
  service.identity_token = "0123456789abcdef";
  config.service_rules["exampleupdater"] =
      {workboost::ServiceClass::Updater, ProtectionLevel::Optimizable};
  config.coding_profile.allow_service_stop.insert("exampleupdater");
  workboost::ServiceProtectionPolicy policy(config);
  Check(policy.CanStopTemporary(service),
        "explicit optimizable updater service should be eligible");

  service.name = "RpcSs";
  service.display_name = "Remote Procedure Call";
  config.service_rules["rpcss"] =
      {workboost::ServiceClass::Updater, ProtectionLevel::Optimizable};
  config.coding_profile.allow_service_stop.insert("rpcss");
  Check(policy.Evaluate(service) == ProtectionLevel::SystemCritical &&
            !policy.CanStopTemporary(service),
        "immutable core service rule must not be downgraded by configuration");

  service.name = "VendorVpnAgent";
  service.display_name = "Vendor VPN agent";
  config.service_rules["vendorvpnagent"] =
      {workboost::ServiceClass::Updater, ProtectionLevel::Optimizable};
  config.coding_profile.allow_service_stop.insert("vendorvpnagent");
  Check(policy.Evaluate(service) == ProtectionLevel::Strong &&
            !policy.CanStopTemporary(service),
        "VPN keyword protection must override an unsafe allowlist entry");

  service.name = "ExampleUpdater";
  service.display_name = "Example updater service";
  service.pid = 404;
  RuntimeContext context;
  context.remote_session_pids.insert(service.pid);
  Check(!policy.CanStopTemporary(service, context),
        "service hosting a protected remote-session PID must not stop");

  service.pid = 0;
  service.identity_verified = false;
  Check(!policy.CanStopTemporary(service),
        "service without a complete identity payload must not stop");
}

void TestServiceActionPlanningAndValidation() {
  Config config = Config::Defaults();
  config.service_rules["exampleupdater"] =
      {workboost::ServiceClass::Updater, ProtectionLevel::Optimizable};
  config.coding_profile.allow_service_stop.insert("exampleupdater");
  ServiceSnapshot service;
  service.name = "ExampleUpdater";
  service.display_name = "Example updater service";
  service.state = workboost::ServiceState::Running;
  service.pid = 4123;
  service.accepts_stop = true;
  service.identity_verified = true;
  service.identity_token = "0123456789abcdef";
  SystemSnapshot snapshot;
  snapshot.process_inventory_complete = true;
  snapshot.tcp_inventory_complete = true;
  snapshot.service_inventory_complete = true;
  snapshot.services.push_back(service);

  const auto plan = OptimizationPlanner(config).Create(snapshot);
  Check(plan.actions.size() == 1 &&
            plan.actions[0].type == ActionType::StopServiceTemporary &&
            plan.actions[0].risk == ActionRisk::Medium,
        "explicit service allowlist should produce one Medium-risk action");
  std::string reason;
  auto action = plan.actions[0];
  Check(!SafetyValidator(config).Validate(action, snapshot, &reason),
        "service action must require independent explicit confirmation");
  action.explicit_confirmation = true;
  Check(SafetyValidator(config).Validate(action, snapshot, &reason),
        "confirmed eligible service action should validate");

  snapshot.tcp_inventory_complete = false;
  Check(!SafetyValidator(config).Validate(action, snapshot, &reason),
        "incomplete connection inventory must fail closed");
  snapshot.tcp_inventory_complete = true;
  snapshot.services[0].identity_token = "fedcba9876543210";
  Check(!SafetyValidator(config).Validate(action, snapshot, &reason),
        "service identity change must invalidate the plan");
  snapshot.services[0] = service;
  snapshot.tcp_sessions.push_back(
      TcpSession{service.pid, "127.0.0.1", 50003, "10.0.0.4", 22,
                 TcpState::Established, false});
  Check(!SafetyValidator(config).Validate(action, snapshot, &reason),
        "service hosting a remote session must fail revalidation");
}

void TestMalformedConfigFailsClosed() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("workboost-config-test-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(GetTickCount64()));
  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  Check(!filesystem_error, "temporary config directory should be created");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{directory};

  std::string error;
  Check(workboost::windows::AtomicWriteUtf8(
            directory / "profiles.json",
            "{\"profiles\":{\"coding\":{\"allow_service_stop\":"
            "[\"ExampleUpdater\"]}}} trailing",
            &error),
        "malformed config fixture should be written: " + error);
  Config malformed = Config::Defaults();
  std::string warning;
  Check(!malformed.LoadDirectory(directory, &warning) &&
            malformed.coding_profile.allow_service_stop.empty() &&
            warning.find("invalid JSON") != std::string::npos,
        "malformed privileged allowlist must be ignored with a warning");

  Check(workboost::windows::AtomicWriteUtf8(
            directory / "profiles.json",
            "{\"profiles\":{\"coding\":{\"allow_service_stop\":"
            "[\"ExampleUpdater\"]}}}",
            &error),
        "valid config fixture should be written: " + error);
  Config valid = Config::Defaults();
  warning.clear();
  Check(valid.LoadDirectory(directory, &warning) && warning.empty() &&
            valid.coding_profile.allow_service_stop.count(
                "exampleupdater") == 1,
        "well-formed explicit service allowlist should still load");

  Check(workboost::windows::AtomicWriteUtf8(
            directory / "diagnosis.json",
            "{\"sample_interval_ms\":99999999,\"history_seconds\":"
            "99999999,\"thresholds\":{\"commit_warning\":-1,"
            "\"ssd_free_space_ratio\":2}}",
            &error),
        "bounded config fixture should be written: " + error);
  Config bounded = Config::Defaults();
  Check(bounded.LoadDirectory(directory, &warning) &&
            bounded.sample_interval_ms == 60000 &&
            bounded.history_seconds == 3600 &&
            bounded.thresholds.commit_warning == 0.01 &&
            bounded.thresholds.ssd_free_space_ratio == 1.0,
        "numeric configuration must stay within safe resource bounds");

  Check(workboost::windows::AtomicWriteUtf8(
            directory / "diagnosis.json",
            "{\"remote_debug_ports\":[22222]}", &error),
        "custom remote-port fixture should be written: " + error);
  Config custom_ports = Config::Defaults();
  Check(custom_ports.LoadDirectory(directory, &warning) &&
            custom_ports.remote_debug_ports.count(22) == 1 &&
            custom_ports.remote_debug_ports.count(23) == 1 &&
            custom_ports.remote_debug_ports.count(22222) == 1,
        "custom remote ports must preserve immutable SSH/Telnet defaults");
  Check(workboost::windows::AtomicWriteUtf8(
            directory / "diagnosis.json", "{\"remote_debug_ports\":[]}",
            &error),
        "empty remote-port fixture should be written: " + error);
  Check(custom_ports.LoadDirectory(directory, &warning) &&
            custom_ports.remote_debug_ports.size() == 2 &&
            custom_ports.remote_debug_ports.count(22) == 1 &&
            custom_ports.remote_debug_ports.count(23) == 1,
        "an empty override may clear custom ports but never SSH/Telnet");

  Check(workboost::windows::AtomicWriteUtf8(
            directory / "diagnosis.json",
            std::string(1024 * 1024 + 1, ' '), &error),
        "oversized config fixture should be written: " + error);
  Config oversized = Config::Defaults();
  warning.clear();
  Check(!oversized.LoadDirectory(directory, &warning) &&
            warning.find("exceeds 1 MiB") != std::string::npos,
        "oversized configuration must fail closed before parsing");
}

void TestPrivacySafeLocalLogger() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("workboost-log-test-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(GetTickCount64()));
  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  Check(!filesystem_error, "temporary log directory should be created");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      workboost::Logger::Instance().Shutdown();
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{directory};

  std::string error;
  Check(workboost::Logger::Instance().Initialize(
            directory, workboost::LogLevel::Info, &error),
        "local logger should initialize: " + error);
  workboost::Logger::Instance().Write(workboost::LogLevel::Trace,
                                      workboost::LogEvent::MonitorFailed, 5);
  workboost::Logger::Instance().Write(
      workboost::LogLevel::Info,
      workboost::LogEvent::ApplicationStarted);
  workboost::Logger::Instance().Shutdown();
  const auto content = workboost::windows::ReadUtf8(
      directory / "logs" / "workboost.log", &error);
  Check(content && workboost::IsValidJsonObjectSyntax(*content) &&
            content->find("\"level\":\"INFO\"") !=
                       std::string::npos &&
            content->find("\"event\":\"ApplicationStarted\"") !=
                std::string::npos &&
            content->find("TRACE") == std::string::npos,
        "local log should be structured and enforce its minimum level");

  const auto log_path = directory / "logs" / "workboost.log";
  const auto backup_path = directory / "logs" / "workboost.1.log";
  Check(workboost::windows::AtomicWriteUtf8(
            log_path, std::string(1024 * 1024 - 16, 'x'), &error),
        "near-limit log fixture should be written: " + error);
  Check(workboost::Logger::Instance().Initialize(
            directory, workboost::LogLevel::Info, &error),
        "logger should reopen a near-limit file: " + error);
  workboost::Logger::Instance().Write(
      workboost::LogLevel::Warn,
      workboost::LogEvent::ConfigurationWarning, ERROR_INVALID_DATA);
  workboost::Logger::Instance().Shutdown();
  const auto rotated = workboost::windows::ReadUtf8(backup_path, &error);
  const auto active = workboost::windows::ReadUtf8(log_path, &error);
  Check(rotated && rotated->size() == 1024 * 1024 - 16 && active &&
            workboost::IsValidJsonObjectSyntax(*active) &&
            active->find("\"level\":\"WARN\"") != std::string::npos &&
            active->find("\"error_code\":13") != std::string::npos,
        "crossing the size limit while running must rotate before writing");
}

void TestAtomicRelativeWrite() {
  const std::filesystem::path path =
      "workboost-relative-write-test-" +
      std::to_string(GetCurrentProcessId()) + ".json";
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      std::filesystem::remove(path.wstring() + L".tmp", ignored);
    }
  } cleanup{path};
  std::string error;
  Check(workboost::windows::AtomicWriteUtf8(path, "{\"ok\":true}\n",
                                             &error),
        "relative atomic output should succeed: " + error);
  const auto content = workboost::windows::ReadUtf8(path, &error);
  Check(content && *content == "{\"ok\":true}\n",
        "relative atomic output should round-trip: " + error);
}

void TestElevatedHandlerRejectsCriticalService() {
  const auto lookup = workboost::windows::ServiceApi::Query("RpcSs");
  Check(lookup.success && lookup.service.identity_verified,
        "RpcSs identity should be available for a read-only policy test");
  Config config = Config::Defaults();
  config.service_rules["rpcss"] =
      {workboost::ServiceClass::Updater, ProtectionLevel::Optimizable};
  config.coding_profile.allow_service_stop.insert("rpcss");
  workboost::helper::Request request;
  request.request_id = 9001;
  request.command = workboost::helper::Command::StopServiceTemporary;
  request.timeout_ms = 1000;
  request.nonce.fill(0x2a);
  request.service_name = lookup.service.name;
  request.expected_identity_token = lookup.service.identity_token;
  const auto response =
      workboost::HandleElevatedRequest(request, config, RuntimeContext{});
  Check(response.status == workboost::helper::Status::Rejected &&
            response.state_before == lookup.service.state &&
            response.state_after == lookup.service.state,
        "elevated handler must reject immutable RpcSs before service control");
}

void TestCurrentProcessVerificationHelpers() {
  workboost::windows::WindowsError error;
  const auto image = workboost::windows::ProcessImagePath(
      GetCurrentProcessId(), &error);
  Check(image.has_value() && !image->empty(),
        "current process image must be available: " + error.Describe());
  const auto module = workboost::windows::CurrentExecutablePath(&error);
  Check(module &&
            workboost::windows::PathEqualsInsensitive(*image, *module),
        "module path and process image path must identify the same binary");
  bool elevated = false;
  Check(workboost::windows::ProcessIsElevated(GetCurrentProcessId(), &elevated,
                                              &error),
        "current process elevation should be queryable: " + error.Describe());
  Check(workboost::windows::PathEqualsInsensitive(*image, *image),
        "verified process path comparison must be case-insensitive and stable");
}

void TestHelperProtocol() {
  workboost::helper::Request request;
  request.request_id = 42;
  request.command = workboost::helper::Command::StopServiceTemporary;
  request.timeout_ms = 15000;
  request.nonce.fill(0x5a);
  request.service_name = "ExampleUpdater_1";
  request.expected_identity_token = "0123456789abcdef";
  std::string error;
  const auto encoded = workboost::helper::Encode(request, &error);
  Check(encoded.has_value(), "valid helper request should encode: " + error);
  const auto decoded = workboost::helper::DecodeRequest(*encoded, &error);
  Check(decoded.has_value(), "valid helper request should decode: " + error);
  Check(decoded->request_id == request.request_id &&
            decoded->command == request.command &&
            decoded->nonce == request.nonce &&
            decoded->service_name == request.service_name &&
            decoded->expected_identity_token ==
                request.expected_identity_token,
        "helper request fields must round-trip exactly");

  auto invalid_command = request;
  invalid_command.command = static_cast<workboost::helper::Command>(99);
  Check(!workboost::helper::Encode(invalid_command, &error),
        "helper protocol must reject commands outside the fixed allowlist");
  auto invalid_name = request;
  invalid_name.service_name = "..\\RpcSs";
  Check(!workboost::helper::Encode(invalid_name, &error),
        "helper protocol must reject path-like service names");
  auto zero_nonce = request;
  zero_nonce.nonce.fill(0);
  Check(!workboost::helper::Encode(zero_nonce, &error),
        "helper protocol must reject an unauthenticated zero nonce");
  auto trailing = *encoded;
  trailing.push_back(0);
  Check(!workboost::helper::DecodeRequest(trailing, &error),
        "helper protocol must reject trailing or length-mismatched bytes");
  auto wrong_version = *encoded;
  wrong_version[4] = 2;
  Check(!workboost::helper::DecodeRequest(wrong_version, &error),
        "helper protocol must reject an unknown version");

  workboost::helper::Response response;
  response.request_id = request.request_id;
  response.status = workboost::helper::Status::Succeeded;
  response.state_before = workboost::ServiceState::Running;
  response.state_after = workboost::ServiceState::Stopped;
  response.nonce = request.nonce;
  response.message = "service stopped temporarily";
  const auto encoded_response = workboost::helper::Encode(response, &error);
  Check(encoded_response.has_value(),
        "valid helper response should encode: " + error);
  const auto decoded_response =
      workboost::helper::DecodeResponse(*encoded_response, &error);
  Check(decoded_response.has_value() &&
            decoded_response->request_id == response.request_id &&
            decoded_response->nonce == response.nonce &&
            decoded_response->state_after == workboost::ServiceState::Stopped,
        "helper response must round-trip and bind to request identity");
  response.error_code = ERROR_ACCESS_DENIED;
  Check(!workboost::helper::Encode(response, &error),
        "successful helper response must not hide an error code");
}

void TestAuthenticatedHelperPipe() {
  workboost::windows::WindowsError server_error;
  auto server = workboost::windows::HelperPipeServer::Create(&server_error);
  Check(server.has_value(),
        "authenticated helper pipe should be created: " +
            server_error.Describe());
  const std::string nonce_hex = workboost::windows::HexNonce(server->Nonce());
  const auto parsed_nonce = workboost::windows::ParseHexNonce(nonce_hex);
  Check(parsed_nonce && *parsed_nonce == server->Nonce(),
        "helper pipe nonce must round-trip through command-line encoding");

  workboost::helper::Request request;
  request.request_id = 77;
  request.command = workboost::helper::Command::StopServiceTemporary;
  request.timeout_ms = 5000;
  request.nonce = server->Nonce();
  request.service_name = "ExampleUpdater";
  request.expected_identity_token = "0123456789abcdef";
  std::string protocol_error;
  const auto request_bytes =
      workboost::helper::Encode(request, &protocol_error);
  Check(request_bytes.has_value(),
        "pipe test request should encode: " + protocol_error);

  bool client_success = false;
  std::uint32_t client_peer_pid = 0;
  std::string client_failure;
  const std::wstring pipe_name = server->Name();
  std::thread client([&] {
    workboost::windows::WindowsError error;
    auto connection =
        workboost::windows::ConnectToHelperPipe(pipe_name, 5000, &error);
    if (!connection) {
      client_failure = error.Describe();
      return;
    }
    const auto peer = connection->PeerProcessId(&error);
    if (!peer) {
      client_failure = error.Describe();
      return;
    }
    client_peer_pid = *peer;
    const auto bytes = connection->ReadMessage(5000, &error);
    if (!bytes) {
      client_failure = error.Describe();
      return;
    }
    std::string decode_error;
    const auto received =
        workboost::helper::DecodeRequest(*bytes, &decode_error);
    if (!received || received->nonce != request.nonce ||
        received->request_id != request.request_id) {
      client_failure = "request authentication mismatch: " + decode_error;
      return;
    }
    workboost::helper::Response response;
    response.request_id = received->request_id;
    response.status = workboost::helper::Status::Succeeded;
    response.state_before = workboost::ServiceState::Running;
    response.state_after = workboost::ServiceState::Stopped;
    response.nonce = received->nonce;
    response.message = "isolated pipe response";
    const auto encoded = workboost::helper::Encode(response, &decode_error);
    if (!encoded || !connection->WriteMessage(*encoded, 5000, &error)) {
      client_failure = encoded ? error.Describe() : decode_error;
      return;
    }
    client_success = true;
  });

  auto connection = server->Accept(5000, &server_error);
  std::uint32_t server_peer_pid = 0;
  std::optional<workboost::helper::Response> response;
  if (connection) {
    const auto peer = connection->PeerProcessId(&server_error);
    if (peer) server_peer_pid = *peer;
    if (peer && connection->WriteMessage(*request_bytes, 5000, &server_error)) {
      const auto bytes = connection->ReadMessage(5000, &server_error);
      if (bytes) {
        response = workboost::helper::DecodeResponse(*bytes, &protocol_error);
      }
    }
  }
  client.join();

  Check(connection.has_value(),
        "helper pipe client should connect: " + server_error.Describe());
  Check(client_success, "helper pipe client exchange failed: " + client_failure);
  Check(client_peer_pid == GetCurrentProcessId() &&
            server_peer_pid == GetCurrentProcessId(),
        "both pipe endpoints must identify the peer process");
  Check(response && response->request_id == request.request_id &&
            response->nonce == request.nonce &&
            response->status == workboost::helper::Status::Succeeded,
        "authenticated helper pipe response must bind to request and nonce");

  workboost::windows::WindowsError invalid_error;
  Check(!workboost::windows::ConnectToHelperPipe(
            L"\\\\.\\pipe\\OtherProduct", 100, &invalid_error),
        "helper client must reject pipe names outside the fixed namespace");
}

void TestGracefulCloseRecoveryState() {
  workboost::ExecutedAction record;
  record.action.id = "graceful-close-21";
  record.action.type = ActionType::GracefulCloseProcess;
  record.action.risk = ActionRisk::Low;
  record.action.pid = 21;
  record.action.expected_start_time_100ns = 789;
  record.action.process_name = "ExampleUpdater.exe";
  record.state = ActionState::Planned;

  ActionExecutor executor(Config::Defaults());
  Check(!executor.Rollback(&record),
        "a potentially delivered graceful close cannot claim rollback");
  Check(record.state == ActionState::Uncertain,
        "planned one-shot action must become Uncertain during recovery");
  Check(!executor.Rollback(&record) && record.state == ActionState::Uncertain,
        "uncertain one-shot recovery must be idempotent and never replayed");

  record.state = ActionState::Completed;
  Check(executor.Rollback(&record),
        "persisted completed one-shot action needs no rollback");
}

void TestRealGracefulCloseRequest() {
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const wchar_t* class_name = L"WorkBoostGracefulCloseTestWindow";
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.hInstance = instance;
  window_class.lpszClassName = class_name;
  Check(RegisterClassW(&window_class) != 0,
        "test window class should register");
  const HWND window = CreateWindowExW(
      WS_EX_NOACTIVATE, class_name, L"WorkBoost graceful close test",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 320, 200, nullptr,
      nullptr, instance, nullptr);
  if (window == nullptr) {
    UnregisterClassW(class_name, instance);
    Check(false, "isolated graceful-close test window should be created");
  }
  ShowWindow(window, SW_SHOWNOACTIVATE);
  UpdateWindow(window);
  const bool was_visible = IsWindowVisible(window) != FALSE;

  Config config = Config::Defaults();
  config.process_rules["workboost_tests.exe"] =
      {ProcessClass::Updater, ProtectionLevel::Optimizable};
  config.coding_profile.allow_graceful_close.insert("workboost_tests.exe");
  ProcessSnapshot process;
  process.pid = GetCurrentProcessId();
  process.name = "workboost_tests.exe";
  process.classification = ProcessClass::Updater;
  process.protection_level = ProtectionLevel::Optimizable;
  process.start_time_100ns = CurrentProcessStartTime();
  process.priority_class = GetPriorityClass(GetCurrentProcess());
  process.has_visible_window = true;
  process.is_foreground = false;
  SystemSnapshot snapshot;
  snapshot.processes.push_back(process);
  snapshot.process_inventory_complete = true;
  snapshot.tcp_inventory_complete = true;

  OptimizationAction action;
  action.id = "self-graceful-close-test";
  action.type = ActionType::GracefulCloseProcess;
  action.risk = ActionRisk::Low;
  action.pid = process.pid;
  action.expected_start_time_100ns = process.start_time_100ns;
  action.process_name = process.name;
  action.timeout_ms = 100;
  action.reason = "isolated integration test";
  const auto executed = ActionExecutor(config).Execute(action, snapshot);
  const bool window_destroyed = IsWindow(window) == FALSE;
  if (!window_destroyed) DestroyWindow(window);
  UnregisterClassW(class_name, instance);

  Check(was_visible, "isolated test window should be visible to EnumWindows");
  Check(executed.state == ActionState::Completed,
        "validated graceful-close request should be delivered");
  Check(window_destroyed,
        "WM_CLOSE should destroy the isolated default test window");

  const wchar_t* partial_class = L"WorkBoostPartialCloseTestWindow";
  WNDCLASSW partial_window_class{};
  partial_window_class.lpfnWndProc = DefWindowProcW;
  partial_window_class.hInstance = instance;
  partial_window_class.lpszClassName = partial_class;
  Check(RegisterClassW(&partial_window_class) != 0,
        "partial-close test window class should register");
  workboost::windows::UniqueHandle ready(
      CreateEventW(nullptr, TRUE, FALSE, nullptr));
  workboost::windows::UniqueHandle stop(
      CreateEventW(nullptr, TRUE, FALSE, nullptr));
  Check(ready.Valid() && stop.Valid(),
        "partial-close synchronization events should be created");
  std::atomic<bool> hung_window_created{false};
  std::thread hung_window_thread([&] {
    const HWND hung = CreateWindowExW(
        WS_EX_NOACTIVATE, partial_class, L"WorkBoost hung close test",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 320, 200, nullptr,
        nullptr, instance, nullptr);
    if (hung != nullptr) {
      ShowWindow(hung, SW_SHOWNOACTIVATE);
      UpdateWindow(hung);
      hung_window_created.store(true);
    }
    SetEvent(ready.Get());
    WaitForSingleObject(stop.Get(), INFINITE);
    if (hung != nullptr && IsWindow(hung)) DestroyWindow(hung);
  });

  const bool worker_ready =
      WaitForSingleObject(ready.Get(), 5000) == WAIT_OBJECT_0;
  HWND responsive = nullptr;
  if (worker_ready && hung_window_created.load()) {
    responsive = CreateWindowExW(
        WS_EX_NOACTIVATE, partial_class, L"WorkBoost responsive close test",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 320, 200, nullptr,
        nullptr, instance, nullptr);
    if (responsive != nullptr) {
      ShowWindow(responsive, SW_SHOWNOACTIVATE);
      UpdateWindow(responsive);
    }
  }
  workboost::ExecutedAction partial_result;
  if (responsive != nullptr) {
    partial_result = ActionExecutor(config).Execute(action, snapshot);
  }
  if (responsive != nullptr && IsWindow(responsive)) DestroyWindow(responsive);
  SetEvent(stop.Get());
  hung_window_thread.join();
  UnregisterClassW(partial_class, instance);

  Check(worker_ready && hung_window_created.load() && responsive != nullptr,
        "responsive and deliberately hung test windows should be created");
  Check(partial_result.state == ActionState::Uncertain,
        "partial WM_CLOSE delivery must remain Uncertain while the process lives");
}

void TestReadOnlyServiceQuery() {
  const auto result = workboost::windows::ServiceApi::QueryAll();
  Check(result.success,
        "read-only SCM enumeration should succeed: " + result.error.Describe());
  Check(!result.services.empty(),
        "Windows service enumeration should return at least one service");
  bool has_verified_identity = false;
  std::string verified_name;
  std::string verified_token;
  for (std::size_t i = 0; i < result.services.size(); ++i) {
    Check(!result.services[i].name.empty(),
          "enumerated service must have a stable service name");
    if (i != 0) {
      Check(result.services[i - 1].name <= result.services[i].name,
            "service query result should be deterministically sorted");
    }
    if (result.services[i].identity_verified) {
      has_verified_identity = true;
      Check(result.services[i].identity_token.size() == 16,
            "verified service identity token should use fixed-width encoding");
      if (verified_name.empty()) {
        verified_name = result.services[i].name;
        verified_token = result.services[i].identity_token;
      }
    }
  }
  Check(has_verified_identity,
        "at least one service configuration identity should be readable");
  const auto lookup = workboost::windows::ServiceApi::Query(verified_name);
  Check(lookup.success && lookup.service.identity_verified &&
            lookup.service.identity_token == verified_token,
        "single-service query must reproduce the planned identity token");
  Check(workboost::ToString(workboost::ServiceState::Running) == "RUNNING",
        "service state should have a stable machine-readable name");
}

void TestReadOnlySerialPortQuery() {
  const auto query = workboost::windows::SerialPortApi::QueryPresent();
  Check(query.success,
        "read-only serial port query should succeed: " +
            query.error.Describe());
  std::uint32_t previous = 0;
  for (const auto& port : query.ports) {
    Check(port.port_name.size() >= 4 &&
              workboost::ToLowerAscii(port.port_name.substr(0, 3)) == "com",
          "serial inventory should contain only COM port names");
    const auto number =
        static_cast<std::uint32_t>(std::stoul(port.port_name.substr(3)));
    Check(number > previous,
          "serial inventory must use stable natural COM-number ordering");
    previous = number;
  }
}

void TestReadOnlyStartupQuery() {
  const auto query = workboost::windows::StartupApi::QueryAll();
  Check(query.success,
        "read-only startup query should succeed: " + query.error.Describe());
  for (const auto& entry : query.entries) {
    Check(entry.executable_name.find('\\') == std::string::npos &&
              entry.executable_name.find('/') == std::string::npos,
          "startup inventory must omit full executable paths");
    Check(entry.location.find('\\') == std::string::npos,
          "startup inventory must expose a logical location, not a path");
  }
}

void TestStartupBenchmarkSummary() {
  std::vector<workboost::StartupObservation> observations(3);
  observations[0].status = workboost::StartupObservationStatus::Succeeded;
  observations[0].visible_window_ms = 80;
  observations[0].responsive_window_ms = 120;
  observations[1].status = workboost::StartupObservationStatus::Succeeded;
  observations[1].visible_window_ms = 100;
  observations[1].responsive_window_ms = 200;
  observations[2].status = workboost::StartupObservationStatus::Succeeded;
  observations[2].visible_window_ms = 90;
  observations[2].responsive_window_ms = 160;
  const auto summary = workboost::BenchmarkManager::Summarize(
      "cold", "Codex.exe", "2026-01-01T00:00:00Z", 3,
      std::move(observations));
  Check(summary.median_visible_window_ms == 90.0 &&
            summary.median_responsive_window_ms == 160.0,
        "startup benchmark should report the median of successful runs");
  const std::string json = workboost::BenchmarkManager::Json(summary);
  Check(json.find("\"schema_version\": 1") != std::string::npos &&
            json.find("\"median_responsive_window_ms\": 160") !=
                std::string::npos,
        "startup benchmark JSON should include stable schema and medians");
}

void TestStartupBenchmarkComparison() {
  std::vector<workboost::StartupObservation> baseline_observations(3);
  std::vector<workboost::StartupObservation> optimized_observations(3);
  const std::uint64_t baseline_visible[] = {120, 100, 110};
  const std::uint64_t baseline_responsive[] = {180, 160, 170};
  const std::uint64_t optimized_visible[] = {90, 80, 85};
  const std::uint64_t optimized_responsive[] = {130, 120, 125};
  for (std::size_t i = 0; i < 3; ++i) {
    baseline_observations[i].status =
        workboost::StartupObservationStatus::Succeeded;
    baseline_observations[i].visible_window_ms = baseline_visible[i];
    baseline_observations[i].responsive_window_ms = baseline_responsive[i];
    optimized_observations[i].status =
        workboost::StartupObservationStatus::Succeeded;
    optimized_observations[i].visible_window_ms = optimized_visible[i];
    optimized_observations[i].responsive_window_ms =
        optimized_responsive[i];
  }
  auto baseline = workboost::BenchmarkManager::Summarize(
      "trial/baseline", "Codex.exe", "2026-01-01T00:00:00Z", 3,
      std::move(baseline_observations));
  auto optimized = workboost::BenchmarkManager::Summarize(
      "trial/optimized", "Codex.exe", "2026-01-01T00:01:00Z", 3,
      std::move(optimized_observations));
  const auto comparison = workboost::BenchmarkManager::Compare(
      "trial", "2026-01-01T00:02:00Z", std::move(baseline),
      std::move(optimized));
  Check(comparison.delta_visible_window_ms == -25.0 &&
            comparison.delta_responsive_window_ms == -45.0,
        "startup comparison should subtract baseline medians from optimized "
        "medians");
  const std::string json =
      workboost::BenchmarkManager::ComparisonJson(comparison);
  Check(workboost::IsValidJsonObjectSyntax(json) &&
            json.find("\"type\": \"StartupResponsivenessComparison\"") !=
                std::string::npos &&
            json.find("\"negative_delta_is_improvement\": true") !=
                std::string::npos &&
            json.find("\"median_responsive_window_ms\": -45") !=
                std::string::npos,
        "startup comparison JSON should be valid and expose delta semantics");
}

void TestPassiveStartupBenchmarkTimeout() {
  const std::string target =
      "workboost-never-" + std::to_string(GetCurrentProcessId()) + ".exe";
  bool ready = false;
  const auto observation =
      workboost::windows::StartupBenchmarkApi::ObserveNewProcess(
          target, 100, [&ready]() { ready = true; });
  Check(observation.status ==
                workboost::StartupObservationStatus::TimedOut &&
            observation.pid == 0 && ready,
        "passive benchmark should announce readiness after its baseline and "
        "time out without launching a process");
  bool invalid_ready = false;
  const auto invalid =
      workboost::windows::StartupBenchmarkApi::ObserveNewProcess(
          "..\\Codex.exe", 100,
          [&invalid_ready]() { invalid_ready = true; });
  Check(invalid.status ==
                workboost::StartupObservationStatus::InvalidTarget &&
            !invalid_ready,
        "passive benchmark must reject path-like targets before readiness");
}

void TestDashboardPresenter() {
  Config config = Config::Defaults();
  SystemSnapshot snapshot;
  snapshot.timestamp = std::chrono::system_clock::now();
  snapshot.memory.physical_total_bytes = 16ULL * 1024 * 1024 * 1024;
  snapshot.memory.physical_available_bytes = 4ULL * 1024 * 1024 * 1024;
  snapshot.memory.commit_total_bytes = 10;
  snapshot.memory.commit_limit_bytes = 20;
  snapshot.process_inventory_complete = true;
  snapshot.tcp_inventory_complete = true;
  ProcessSnapshot process;
  process.pid = 42;
  process.name = "ssh.exe";
  process.classification = ProcessClass::RemoteTerminal;
  process.protection_level = ProtectionLevel::Strong;
  snapshot.processes.push_back(process);
  snapshot.tcp_sessions.push_back(workboost::TcpSession{
      42, "0.0.0.0", 51000, "192.168.12.34", 22,
      workboost::TcpState::Established, false});
  snapshot.disks.push_back(workboost::DiskSnapshot{
      "0 C: D:", "C: D:", workboost::DiskMedia::SSD, 0.25, 0.004, 0.0,
      0.0, 0.0, 0.0, 0.0, 0, 0, false});
  snapshot.disks.push_back(workboost::DiskSnapshot{
      "1 E:", "E:", workboost::DiskMedia::HDD, 0.5, 0.012, 0.0, 0.0,
      0.0, 0.0, 0.0, 0, 0, false});
  snapshot.disks.push_back(workboost::DiskSnapshot{
      "0 F:", "F:", workboost::DiskMedia::HDD, 0.1, 0.008, 0.0, 0.0,
      0.0, 0.0, 0.0, 0, 0, false});
  SnapshotHistory history(4);
  history.Add(snapshot);
  const auto model = workboost::gui::DashboardPresenter::Build(
      config, snapshot, history, {}, {}, std::nullopt, "");
  Check(workboost::gui::DashboardPresenter::PageNames().size() == 7,
        "dashboard must expose all seven design pages");
  Check(model.system.memory_used_bytes == 12ULL * 1024 * 1024 * 1024 &&
            model.system.memory_total_bytes == 16ULL * 1024 * 1024 * 1024 &&
            model.processes.size() == 1 &&
            model.disks.size() == 4 && model.disks[0].name == "C:" &&
            model.disks[1].name == "D:" && model.disks[2].name == "E:" &&
            model.disks[3].name == "F:" &&
            model.protected_workloads.size() == 1 &&
            model.protected_workloads[0].reason ==
                "Active protected remote session" &&
            model.coding_mode.protected_workloads == 1,
        "dashboard must expose typed real-core data for native rendering");
  const auto& protected_page =
      model.pages[static_cast<std::size_t>(
          workboost::gui::DashboardPage::ProtectedWorkload)];
  Check(protected_page.find("192.168.12.34") == std::string::npos &&
            protected_page.find("192.168.12.x") != std::string::npos,
        "dashboard must mask remote IP addresses by default");
}

void TestDashboardTopImpactMetrics() {
  Config config = Config::Defaults();
  SystemSnapshot snapshot;
  snapshot.timestamp = std::chrono::system_clock::now();
  snapshot.process_inventory_complete = true;
  snapshot.tcp_inventory_complete = true;

  ProcessSnapshot io_first;
  io_first.pid = 101;
  io_first.name = "io-worker.exe";
  io_first.read_bytes_per_sec = 3.0 * 1024 * 1024;
  snapshot.processes.push_back(io_first);
  ProcessSnapshot io_second = io_first;
  io_second.pid = 102;
  snapshot.processes.push_back(io_second);

  ProcessSnapshot terminal_first;
  terminal_first.pid = 201;
  terminal_first.name = "terminal.exe";
  terminal_first.cpu_percent = 3.0;
  terminal_first.private_bytes = 300ULL * 1024 * 1024;
  snapshot.processes.push_back(terminal_first);
  ProcessSnapshot terminal_second = terminal_first;
  terminal_second.pid = 202;
  snapshot.processes.push_back(terminal_second);

  SnapshotHistory history(4);
  history.Add(snapshot);
  const auto model = workboost::gui::DashboardPresenter::Build(
      config, snapshot, history, {}, {}, std::nullopt, "");
  Check(model.top_impacts.size() == 2,
        "Top Impact must aggregate processes with the same executable name");
  const auto& io = model.top_impacts[0];
  const auto& terminal = model.top_impacts[1];
  Check(io.name == "io-worker.exe" &&
            io.io_bytes_per_sec == 6.0 * 1024 * 1024 &&
            io.cpu_impact == workboost::gui::ImpactLevel::Low &&
            io.memory_impact == workboost::gui::ImpactLevel::Low &&
            io.io_impact == workboost::gui::ImpactLevel::Medium &&
            io.impact == workboost::gui::ImpactLevel::Medium,
        "aggregated I/O must determine the displayed impact level");
  Check(terminal.name == "terminal.exe" && terminal.cpu_percent == 6.0 &&
            terminal.private_bytes == 600ULL * 1024 * 1024 &&
            terminal.io_bytes_per_sec == 0.0 &&
            terminal.cpu_impact == workboost::gui::ImpactLevel::Medium &&
            terminal.memory_impact == workboost::gui::ImpactLevel::Medium &&
            terminal.io_impact == workboost::gui::ImpactLevel::Low &&
            terminal.impact == workboost::gui::ImpactLevel::Medium,
        "a zero-I/O Medium row must expose its CPU and memory triggers");
}

void TestDashboardOverviewLayout() {
  const auto regular = workboost::gui::CalculateDashboardOverviewLayout(
      424, 538, 4, 18, 15);
  Check(regular.fits && !regular.compact && regular.disk_columns == 1 &&
            regular.primary_line_height == 20 &&
            regular.metric_row_height == 38 && regular.disk_rows == 4 &&
            regular.required_height == 318,
        "four disks should stay in one aligned column when height permits");

  const auto constrained = workboost::gui::CalculateDashboardOverviewLayout(
      274, 389, 8, 18, 15);
  Check(constrained.fits && constrained.compact &&
            constrained.primary_line_height == 19 &&
            constrained.metric_row_height == 35 &&
            constrained.disk_columns == 3 && constrained.disk_rows == 3 &&
            constrained.required_height == 262,
        "a constrained overview should compact and flow disks into columns");

  const auto many_disks = workboost::gui::CalculateDashboardOverviewLayout(
      424, 538, 26, 18, 15);
  Check(many_disks.fits && many_disks.compact &&
            many_disks.disk_columns == 4 && many_disks.disk_rows == 7 &&
            many_disks.required_height == 402,
        "the initial dashboard size should adapt to every drive letter");

  const auto empty = workboost::gui::CalculateDashboardOverviewLayout(
      220, 300, 0, 18, 15);
  Check(empty.fits && empty.disk_columns == 1 && empty.disk_rows == 1,
        "an empty disk inventory should reserve one status row");
}

void TestDashboardCloseBehavior() {
  using workboost::gui::DashboardCloseDisposition;
  using workboost::gui::DashboardCloseRequest;
  using workboost::gui::ResolveDashboardClose;

  Check(ResolveDashboardClose(DashboardCloseRequest::WindowClose, false) ==
                DashboardCloseDisposition::HideToTray &&
            ResolveDashboardClose(DashboardCloseRequest::WindowClose, true) ==
                DashboardCloseDisposition::HideToTray,
        "the title-bar close button must always hide the dashboard to tray");
  Check(ResolveDashboardClose(DashboardCloseRequest::ExplicitExit, true) ==
                DashboardCloseDisposition::KeepOpen &&
            ResolveDashboardClose(DashboardCloseRequest::ExplicitExit,
                                  false) ==
                DashboardCloseDisposition::Exit,
        "only an idle explicit tray command may exit WorkBoost");

  const auto left_click = workboost::gui::DecodeDashboardTrayNotification(
      static_cast<std::uintptr_t>(MAKELPARAM(WM_LBUTTONUP, 1)), true);
  const auto right_click = workboost::gui::DecodeDashboardTrayNotification(
      static_cast<std::uintptr_t>(MAKELPARAM(WM_RBUTTONUP, 1)), true);
  Check(left_click.event_code == WM_LBUTTONUP && left_click.icon_id == 1 &&
            right_click.event_code == WM_RBUTTONUP &&
            right_click.icon_id == 1,
        "version 4 tray callbacks must decode their packed event and icon ID");
}

void TestDashboardRendererHitTargets() {
  HWND window = CreateWindowExW(0, L"STATIC", L"WorkBoost renderer test",
                                WS_POPUP, 0, 0, 32, 32, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
  Check(window != nullptr, "renderer test window should be created");
  struct Cleanup {
    HWND window;
    ~Cleanup() {
      if (window != nullptr) DestroyWindow(window);
    }
  } cleanup{window};

  workboost::gui::DashboardRenderer renderer;
  Check(renderer.Initialize(window), "native dashboard renderer should initialize");
  HDC window_dc = GetDC(window);
  Check(window_dc != nullptr,
        "renderer test device context should be available");
  const int dpi = std::max(96, GetDeviceCaps(window_dc, LOGPIXELSX));
  const RECT bounds{0, 0, MulDiv(1280, dpi, 96), MulDiv(800, dpi, 96)};
  HDC dc = CreateCompatibleDC(window_dc);
  HBITMAP bitmap =
      CreateCompatibleBitmap(window_dc, bounds.right, bounds.bottom);
  Check(dc != nullptr && bitmap != nullptr,
        "renderer test back buffer should be created");
  const HGDIOBJ previous_bitmap = SelectObject(dc, bitmap);
  workboost::gui::DashboardViewModel model;
  model.mode = "Monitor Mode";
  model.system.cpu_percent = 25.0;
  model.system.memory_total_bytes = 1;
  const std::optional<workboost::gui::DashboardViewModel> optional_model{model};
  renderer.Paint(dc, bounds, optional_model,
                 workboost::gui::DashboardPage::Dashboard, std::nullopt, "");

  const auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };
  const int cpu_row_top = scale(72) + scale(24) + scale(44);
  const int cpu_bar_left = scale(200) + scale(24) + scale(16) + scale(72) +
                           scale(12) + scale(170) + scale(12);
  const int aligned_track_y =
      cpu_row_top + (scale(20) - scale(5)) / 2 + scale(2);
  const int former_track_y = cpu_row_top + scale(15) + scale(2);
  const int track_x = cpu_bar_left + scale(10);
  const COLORREF aligned_color = GetPixel(dc, track_x, aligned_track_y);
  const COLORREF former_color = GetPixel(dc, track_x, former_track_y);

  SelectObject(dc, previous_bitmap);
  DeleteObject(bitmap);
  DeleteDC(dc);
  ReleaseDC(window, window_dc);
  Check(aligned_color == RGB(48, 104, 189) &&
            former_color == RGB(255, 255, 255),
        "CPU progress must share the primary text row instead of sitting "
        "below it");

  const POINT process_navigation{MulDiv(50, dpi, 96),
                                 MulDiv(150, dpi, 96)};
  const auto command = renderer.HitTest(process_navigation);
  Check(command &&
            command->action == workboost::gui::DashboardUiAction::Navigate &&
            command->page == workboost::gui::DashboardPage::Processes,
        "custom sidebar must expose a stable Processes hit target");
}

void TestDashboardLanguageToggleTarget() {
  HWND window = CreateWindowExW(0, L"STATIC", L"WorkBoost renderer test",
                                WS_POPUP, 0, 0, 32, 32, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
  Check(window != nullptr, "renderer test window should be created");
  struct Cleanup {
    HWND window;
    ~Cleanup() {
      if (window != nullptr) DestroyWindow(window);
    }
  } cleanup{window};

  workboost::gui::DashboardRenderer renderer;
  Check(renderer.Initialize(window),
        "native dashboard renderer should initialize");
  HDC dc = GetDC(window);
  Check(dc != nullptr, "renderer test device context should be available");
  const int dpi = std::max(96, GetDeviceCaps(dc, LOGPIXELSX));
  const RECT bounds{0, 0, MulDiv(1280, dpi, 96), MulDiv(800, dpi, 96)};
  workboost::gui::DashboardViewModel model;
  model.mode = "Monitor Mode";
  const std::optional<workboost::gui::DashboardViewModel> optional_model{model};
  renderer.Paint(dc, bounds, optional_model,
                 workboost::gui::DashboardPage::Settings, std::nullopt, "");
  ReleaseDC(window, dc);

  // Language button sits at the top-right of the Settings summary card:
  // x = content.right - 126s .. - 20s, y = summary.top + 17s .. + 49s.
  const POINT language_button{MulDiv(1183, dpi, 96), MulDiv(129, dpi, 96)};
  const auto command = renderer.HitTest(language_button);
  Check(command &&
            command->action == workboost::gui::DashboardUiAction::ToggleLanguage,
        "settings page must expose a stable language toggle hit target");
}

void TestCodingModeCommandValidation() {
  const auto below_minimum = workboost::CodingModeCommandClient::Execute(
      workboost::CodingModeCommand::Enter, 9);
  Check(!below_minimum.launched &&
            below_minimum.error.code == ERROR_INVALID_PARAMETER,
        "invalid Coding Mode baseline must fail before launching a process");

  const auto above_maximum = workboost::CodingModeCommandClient::Execute(
      workboost::CodingModeCommand::Enter, 601);
  Check(!above_maximum.launched &&
            above_maximum.error.code == ERROR_INVALID_PARAMETER,
        "oversized Coding Mode baseline must fail before launching a process");
}

void TestCompletionReport() {
  workboost::OptimizationSession session;
  session.session_id = "report-test";
  session.state = workboost::SessionState::Recovering;
  session.start_time = "2026-01-01T00:00:00Z";
  session.baseline.cpu_percent = 40.0;
  session.baseline.available_memory_bytes = 1024;
  session.baseline.commit_ratio = 0.8;
  session.baseline.maximum_disk_active_ratio = 0.7;
  session.baseline.maximum_disk_latency_ms = 20.0;
  workboost::ExecutedAction action;
  action.action.id = "priority-up-42";
  action.action.type = ActionType::SetPriorityClass;
  action.action.risk = ActionRisk::Safe;
  action.action.pid = 42;
  action.action.expected_start_time_100ns = 123;
  action.action.process_name = "Codex.exe";
  action.action.source_priority = NORMAL_PRIORITY_CLASS;
  action.action.target_priority = ABOVE_NORMAL_PRIORITY_CLASS;
  action.state = ActionState::RolledBack;
  action.original_priority = NORMAL_PRIORITY_CLASS;
  session.actions.push_back(action);

  SystemSnapshot after;
  after.process_inventory_complete = true;
  after.tcp_inventory_complete = true;
  after.cpu_percent = 25.0;
  after.memory.physical_available_bytes = 2048;
  after.memory.commit_total_bytes = 7;
  after.memory.commit_limit_bytes = 10;
  after.disks.push_back(
      DiskSnapshot{"0 C:", "C:", DiskMedia::SSD, 0.4, 8.0, 1.0, 0, 0});
  ProcessSnapshot protected_process;
  protected_process.pid = 42;
  protected_process.name = "Codex.exe";
  protected_process.image_path = "D:\\private\\workspace\\Codex.exe";
  protected_process.classification = ProcessClass::Development;
  protected_process.protection_level = ProtectionLevel::Strong;
  after.processes.push_back(protected_process);

  workboost::DiagnosisResult diagnosis;
  diagnosis.type = "CpuSaturation";
  diagnosis.severity = workboost::Severity::Medium;
  diagnosis.confidence = workboost::Confidence::High;
  diagnosis.summary = "Sustained CPU pressure";
  diagnosis.evidence["average_cpu_percent"] = 92.5;
  const std::string report = SessionManager::CompletionReport(
      session, after, {diagnosis}, "optimized_before_rollback");
  Check(workboost::IsValidJsonObjectSyntax(report),
        "completion report must be valid JSON");
  Check(report.find("\"report_schema_version\": 1") !=
                std::string::npos &&
            report.find("\"protected_workload\"") != std::string::npos &&
            report.find("\"protected_workload_inventory_complete\": true") !=
                std::string::npos &&
            report.find("\"diagnoses\"") != std::string::npos &&
            report.find("\"rollback\"") != std::string::npos,
        "completion report must include all V1 evidence sections");
  Check(report.find("D:\\private\\workspace") == std::string::npos,
        "completion report must not persist process image paths");
}

void TestDiagnosisRules() {
  Config config = Config::Defaults();
  SnapshotHistory single_sample_history(1);
  SystemSnapshot single_sample;
  single_sample.cpu_percent = 100.0;
  single_sample_history.Add(std::move(single_sample));
  Check(DiagnosisEngine(config).Evaluate(single_sample_history).empty(),
        "a single snapshot must never produce a time-window diagnosis");

  SnapshotHistory history(10);
  for (int i = 0; i < 6; ++i) {
    SystemSnapshot snapshot;
    snapshot.process_inventory_complete = true;
    snapshot.tcp_inventory_complete = true;
    snapshot.cpu_percent = 95.0;
    snapshot.memory.physical_total_bytes = 16ULL * 1024 * 1024 * 1024;
    snapshot.memory.physical_available_bytes = 900ULL * 1024 * 1024;
    snapshot.memory.commit_total_bytes = 90;
    snapshot.memory.commit_limit_bytes = 100;
    snapshot.page_reads_per_sec = 100.0;
    snapshot.disks.push_back(DiskSnapshot{"1 D:", "D:", DiskMedia::HDD,
                                          0.98, 70.0, 7.0, 20e6, 15e6});
    DiskSnapshot ssd;
    ssd.instance = "0 C:";
    ssd.volumes = "C:";
    ssd.media = DiskMedia::SSD;
    ssd.total_space_bytes = 1000;
    ssd.free_space_bytes = 50;
    ssd.space_inventory_complete = true;
    snapshot.disks.push_back(ssd);
    ProcessSnapshot defender;
    defender.pid = 20;
    defender.name = "MsMpEng.exe";
    defender.classification = ProcessClass::Security;
    defender.cpu_percent = 20.0;
    defender.read_bytes_per_sec = 12e6;
    snapshot.processes.push_back(defender);
    ProcessSnapshot updater;
    updater.pid = 21;
    updater.name = "ExampleUpdater.exe";
    updater.classification = ProcessClass::Updater;
    updater.protection_level = ProtectionLevel::Optimizable;
    updater.write_bytes_per_sec = 13e6;
    snapshot.processes.push_back(updater);
    ProcessSnapshot foreground;
    foreground.pid = 22;
    foreground.name = "Codex.exe";
    foreground.classification = ProcessClass::Development;
    foreground.protection_level = ProtectionLevel::Strong;
    foreground.is_foreground = true;
    foreground.working_set_bytes = 1024;
    snapshot.processes.push_back(foreground);
    history.Add(std::move(snapshot));
  }
  const auto results = DiagnosisEngine(config).Evaluate(history);
  Check(HasDiagnosis(results, "MemoryPressure"),
        "memory pressure rule should trigger");
  Check(HasDiagnosis(results, "PagingPressure"),
        "paging pressure rule should trigger");
  Check(HasDiagnosis(results, "DiskBottleneck"),
        "disk bottleneck rule should trigger");
  Check(HasDiagnosis(results, "HddPagingBottleneck"),
        "HDD paging rule should trigger");
  Check(HasDiagnosis(results, "CpuSaturation"),
        "CPU saturation rule should trigger");
  Check(HasDiagnosis(results, "DefenderImpact"),
        "Defender impact rule should trigger without planning an action");
  Check(HasDiagnosis(results, "BackgroundIoImpact"),
        "eligible unprotected background I/O should be diagnosed");
  Check(HasDiagnosis(results, "ForegroundAppMemoryPressure"),
        "active development process should be associated with memory pressure");
  Check(HasDiagnosis(results, "SsdSpacePressure"),
        "low SSD free space should produce a read-only warning");

  SnapshotHistory incomplete_process_history(10);
  SnapshotHistory incomplete_tcp_history(10);
  for (const auto& complete : history.Snapshots()) {
    SystemSnapshot incomplete_process = complete;
    incomplete_process.process_inventory_complete = false;
    incomplete_process_history.Add(std::move(incomplete_process));
    SystemSnapshot incomplete_tcp = complete;
    incomplete_tcp.tcp_inventory_complete = false;
    incomplete_tcp_history.Add(std::move(incomplete_tcp));
  }
  const auto incomplete_process_results =
      DiagnosisEngine(config).Evaluate(incomplete_process_history);
  Check(!HasDiagnosis(incomplete_process_results, "DefenderImpact") &&
            !HasDiagnosis(incomplete_process_results, "BackgroundIoImpact") &&
            !HasDiagnosis(incomplete_process_results,
                          "ForegroundAppMemoryPressure"),
        "incomplete process inventories must suppress process attribution");
  Check(!HasDiagnosis(DiagnosisEngine(config).Evaluate(incomplete_tcp_history),
                      "BackgroundIoImpact"),
        "incomplete TCP inventories must suppress unprotected background "
        "attribution");

  SnapshotHistory sparse_inventory_history(10);
  SnapshotHistory sparse_disk_history(10);
  for (std::size_t i = 0; i < history.Snapshots().size(); ++i) {
    SystemSnapshot sparse_inventory = history.Snapshots()[i];
    sparse_inventory.process_inventory_complete = i < 2;
    sparse_inventory.tcp_inventory_complete = i < 2;
    sparse_inventory_history.Add(std::move(sparse_inventory));
    SystemSnapshot sparse_disk = history.Snapshots()[i];
    if (i >= 2) sparse_disk.disks.clear();
    sparse_disk_history.Add(std::move(sparse_disk));
  }
  const auto sparse_inventory_results =
      DiagnosisEngine(config).Evaluate(sparse_inventory_history);
  Check(!HasDiagnosis(sparse_inventory_results, "DefenderImpact") &&
            !HasDiagnosis(sparse_inventory_results, "BackgroundIoImpact") &&
            !HasDiagnosis(sparse_inventory_results,
                          "ForegroundAppMemoryPressure"),
        "process attribution requires complete coverage of half the window");
  const auto sparse_disk_results =
      DiagnosisEngine(config).Evaluate(sparse_disk_history);
  Check(!HasDiagnosis(sparse_disk_results, "DiskBottleneck") &&
            !HasDiagnosis(sparse_disk_results, "HddPagingBottleneck") &&
            !HasDiagnosis(sparse_disk_results, "SsdSpacePressure"),
        "disk diagnoses require instance coverage of half the window");

  SnapshotHistory incomplete_space_history(10);
  for (int i = 0; i < 6; ++i) {
    SystemSnapshot snapshot;
    snapshot.process_inventory_complete = true;
    snapshot.tcp_inventory_complete = true;
    DiskSnapshot ssd;
    ssd.instance = "0 C:";
    ssd.volumes = "C:";
    ssd.media = DiskMedia::SSD;
    ssd.total_space_bytes = 1000;
    ssd.free_space_bytes = 1;
    ssd.space_inventory_complete = false;
    snapshot.disks.push_back(ssd);
    incomplete_space_history.Add(std::move(snapshot));
  }
  Check(!HasDiagnosis(DiagnosisEngine(config).Evaluate(
                          incomplete_space_history),
                      "SsdSpacePressure"),
        "incomplete volume inventory must not produce an SSD space warning");

  SnapshotHistory transient_history(10);
  for (int i = 0; i < 6; ++i) {
    SystemSnapshot snapshot;
    snapshot.process_inventory_complete = true;
    snapshot.tcp_inventory_complete = true;
    snapshot.memory.physical_available_bytes =
        (i == 5 ? 900ULL : 4096ULL) * 1024 * 1024;
    snapshot.memory.commit_total_bytes = i == 5 ? 90 : 50;
    snapshot.memory.commit_limit_bytes = 100;
    if (i == 5) {
      ProcessSnapshot updater;
      updater.pid = 31;
      updater.name = "TransientUpdater.exe";
      updater.classification = ProcessClass::Updater;
      updater.protection_level = ProtectionLevel::Optimizable;
      updater.write_bytes_per_sec = 50e6;
      snapshot.processes.push_back(updater);
      ProcessSnapshot foreground;
      foreground.pid = 32;
      foreground.name = "Codex.exe";
      foreground.classification = ProcessClass::Development;
      foreground.protection_level = ProtectionLevel::Strong;
      foreground.is_foreground = true;
      snapshot.processes.push_back(foreground);
      ProcessSnapshot defender;
      defender.pid = 33;
      defender.name = "MsMpEng.exe";
      defender.classification = ProcessClass::Security;
      defender.protection_level = ProtectionLevel::SystemCritical;
      defender.cpu_percent = 100.0;
      defender.read_bytes_per_sec = 100e6;
      snapshot.processes.push_back(defender);
    }
    transient_history.Add(std::move(snapshot));
  }
  const auto transient_results =
      DiagnosisEngine(config).Evaluate(transient_history);
  Check(!HasDiagnosis(transient_results, "BackgroundIoImpact") &&
            !HasDiagnosis(transient_results,
                          "ForegroundAppMemoryPressure") &&
            !HasDiagnosis(transient_results, "DefenderImpact"),
        "a single I/O, foreground-memory, or Defender spike must not become "
        "a diagnosis");
}

void TestBenchmarkWindowAggregation() {
  SnapshotHistory history(3);
  for (int i = 1; i <= 3; ++i) {
    SystemSnapshot snapshot;
    snapshot.process_inventory_complete = true;
    snapshot.tcp_inventory_complete = true;
    snapshot.timestamp = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(i * 1000));
    snapshot.cpu_percent = i * 10.0;
    snapshot.memory.physical_available_bytes =
        static_cast<std::uint64_t>(i * 100);
    snapshot.memory.commit_total_bytes =
        static_cast<std::uint64_t>(i * 10);
    snapshot.memory.commit_limit_bytes = 100;
    snapshot.page_reads_per_sec = i * 2.0;
    DiskSnapshot disk;
    disk.media = DiskMedia::SSD;
    disk.active_ratio = i * 0.1;
    disk.average_latency_ms = i * 2.0;
    disk.queue_length = i;
    snapshot.disks.push_back(disk);
    ProcessSnapshot development;
    development.name = "Codex.exe";
    development.classification = ProcessClass::Development;
    development.protection_level = ProtectionLevel::Strong;
    development.cpu_percent = i * 2.0;
    development.working_set_bytes =
        static_cast<std::uint64_t>(i * 100);
    snapshot.processes.push_back(development);
    ProcessSnapshot updater;
    updater.name = "ExampleUpdater.exe";
    updater.classification = ProcessClass::Updater;
    updater.protection_level = ProtectionLevel::Optimizable;
    updater.read_bytes_per_sec = i * 10.0;
    snapshot.processes.push_back(updater);
    history.Add(std::move(snapshot));
  }
  const auto point = SessionManager::MakeBenchmarkPoint(history);
  Check(point.sample_count == 3 && point.observed_span_ms == 2000 &&
            point.process_inventory_complete_samples == 3 &&
            point.tcp_inventory_complete_samples == 3 &&
            point.protection_inventory_complete_samples == 3 &&
            point.cpu_percent == 20.0 &&
            point.available_memory_bytes == 200 &&
            std::abs(point.commit_ratio - 0.2) < 0.0001 &&
            point.page_reads_per_sec == 4.0 &&
            std::abs(point.maximum_disk_active_ratio - 0.2) < 0.0001 &&
            point.maximum_disk_latency_ms == 4.0 &&
            point.maximum_disk_queue_length == 2.0 &&
            point.development_cpu_percent == 4.0 &&
            point.development_working_set_bytes == 200 &&
            point.top_background_io_process == "ExampleUpdater.exe" &&
            point.top_background_io_bytes_per_sec == 20.0,
        "benchmark points must aggregate the full observation window");

  SnapshotHistory degraded_history(3);
  for (std::size_t i = 0; i < history.Snapshots().size(); ++i) {
    SystemSnapshot degraded = history.Snapshots()[i];
    degraded.process_inventory_complete = i == 0;
    degraded.tcp_inventory_complete = true;
    if (i == 0) degraded.tcp_inventory_complete = false;
    degraded_history.Add(std::move(degraded));
  }
  const auto degraded = SessionManager::MakeBenchmarkPoint(degraded_history);
  Check(degraded.process_inventory_complete_samples == 1 &&
            degraded.tcp_inventory_complete_samples == 2 &&
            degraded.protection_inventory_complete_samples == 0 &&
            degraded.development_cpu_percent == 2.0 &&
            degraded.development_working_set_bytes == 100 &&
            degraded.top_background_io_process.empty() &&
            degraded.top_background_io_bytes_per_sec == 0.0,
        "benchmark process metrics must use only complete inventory samples");
}

void TestSessionRoundTrip() {
  SystemSnapshot baseline;
  baseline.process_inventory_complete = true;
  baseline.tcp_inventory_complete = true;
  baseline.cpu_percent = 25.0;
  baseline.memory.physical_available_bytes = 4096;
  baseline.memory.commit_total_bytes = 50;
  baseline.memory.commit_limit_bytes = 100;
  baseline.page_reads_per_sec = 12.5;
  DiskSnapshot baseline_disk;
  baseline_disk.media = DiskMedia::SSD;
  baseline_disk.active_ratio = 0.5;
  baseline_disk.average_latency_ms = 4.0;
  baseline_disk.queue_length = 2.0;
  baseline.disks.push_back(baseline_disk);
  ProcessSnapshot development;
  development.name = "Code.exe";
  development.classification = ProcessClass::Development;
  development.protection_level = ProtectionLevel::Strong;
  development.cpu_percent = 8.0;
  development.working_set_bytes = 1024;
  baseline.processes.push_back(development);
  ProcessSnapshot background;
  background.name = "ExampleUpdater.exe";
  background.classification = ProcessClass::Updater;
  background.protection_level = ProtectionLevel::Optimizable;
  background.read_bytes_per_sec = 4096;
  baseline.processes.push_back(background);
  auto session = workboost::OptimizationSession{};
  session.session_id = "test-session";
  session.state = workboost::SessionState::Active;
  session.start_time = "2026-01-01T00:00:00Z";
  session.baseline = SessionManager::MakeBenchmarkPoint(baseline);
  workboost::ExecutedAction executed;
  executed.action.id = "priority-up-1";
  executed.action.type = ActionType::SetPriorityClass;
  executed.action.risk = ActionRisk::Safe;
  executed.action.pid = 1;
  executed.action.expected_start_time_100ns = 99;
  executed.action.process_name = "Code.exe";
  executed.action.source_priority = NORMAL_PRIORITY_CLASS;
  executed.action.target_priority = ABOVE_NORMAL_PRIORITY_CLASS;
  executed.action.reason = "test \"quoted\" \\ path";
  executed.state = workboost::ActionState::Applied;
  executed.original_priority = NORMAL_PRIORITY_CLASS;
  executed.error_message = "recover \"exactly\"";
  session.actions.push_back(executed);

  workboost::ExecutedAction close;
  close.action.id = "graceful-close-2";
  close.action.type = ActionType::GracefulCloseProcess;
  close.action.risk = ActionRisk::Low;
  close.action.pid = 2;
  close.action.expected_start_time_100ns = 100;
  close.action.process_name = "ExampleUpdater.exe";
  close.action.timeout_ms = 1500;
  close.action.reason = "explicit close test";
  close.state = ActionState::Uncertain;
  close.error_code = ERROR_NOT_SUPPORTED;
  close.error_message = "outcome cannot be rolled back";
  close.result_message = "awaiting acknowledgement";
  session.actions.push_back(close);

  workboost::ExecutedAction service;
  service.action.id = "service-stop-exampleupdater";
  service.action.type = ActionType::StopServiceTemporary;
  service.action.risk = ActionRisk::Medium;
  service.action.timeout_ms = 15000;
  service.action.service_name = "ExampleUpdater";
  service.action.expected_service_identity = "0123456789abcdef";
  service.action.source_service_state = workboost::ServiceState::Running;
  service.action.explicit_confirmation = true;
  service.action.reason = "explicit service stop test";
  service.state = ActionState::Applied;
  service.original_service_state = workboost::ServiceState::Running;
  service.result_message = "service stopped temporarily";
  session.actions.push_back(service);

  std::string error;
  const std::string serialized = SessionManager::Serialize(session);
  Check(serialized.find("\"schema_version\": 3") != std::string::npos,
        "service recovery semantics must use session schema v3");
  const auto parsed = SessionManager::Deserialize(serialized, &error);
  Check(parsed.has_value(), "session should deserialize: " + error);
  Check(parsed->session_id == session.session_id, "session id must round-trip");
  Check(parsed->baseline.maximum_disk_queue_length == 2.0 &&
            parsed->baseline.sample_count == 1 &&
            parsed->baseline.observed_span_ms == 0 &&
            parsed->baseline.process_inventory_complete_samples == 1 &&
            parsed->baseline.tcp_inventory_complete_samples == 1 &&
            parsed->baseline.protection_inventory_complete_samples == 1 &&
            parsed->baseline.maximum_ssd_active_ratio == 0.5 &&
            parsed->baseline.page_reads_per_sec == 12.5 &&
            parsed->baseline.development_cpu_percent == 8.0 &&
            parsed->baseline.development_working_set_bytes == 1024 &&
            parsed->baseline.top_background_io_process ==
                "ExampleUpdater.exe" &&
            parsed->baseline.top_background_io_bytes_per_sec == 4096.0,
        "complete benchmark metrics must round-trip");
  Check(parsed->actions.size() == 3, "action list must round-trip");
  Check(parsed->actions[0].original_priority == NORMAL_PRIORITY_CLASS,
        "rollback priority payload must round-trip");
  Check(parsed->actions[0].action.reason == executed.action.reason &&
            parsed->actions[0].error_message == executed.error_message,
        "escaped action strings must round-trip");
  Check(parsed->actions[1].action.type == ActionType::GracefulCloseProcess &&
            parsed->actions[1].action.risk == ActionRisk::Low &&
            parsed->actions[1].action.timeout_ms == 1500 &&
            parsed->actions[1].state == ActionState::Uncertain &&
            parsed->actions[1].result_message == close.result_message,
        "one-shot recovery metadata must round-trip");
  Check(parsed->actions[2].action.type ==
                ActionType::StopServiceTemporary &&
            parsed->actions[2].action.service_name ==
                service.action.service_name &&
            parsed->actions[2].action.expected_service_identity ==
                service.action.expected_service_identity &&
            parsed->actions[2].action.explicit_confirmation &&
            parsed->actions[2].original_service_state ==
                workboost::ServiceState::Running,
        "service identity and original-state restore payload must round-trip");

  std::string unsupported = serialized;
  const auto type_position = unsupported.find("SetPriorityClass");
  Check(type_position != std::string::npos,
        "serialized priority type should be present");
  unsupported.replace(type_position, std::string("SetPriorityClass").size(),
                      "TerminateProcess");
  Check(!SessionManager::Deserialize(unsupported, &error),
        "active session parser must reject unsupported executable actions");
  Check(!SessionManager::Deserialize(serialized + " trailing", &error),
        "active session parser must reject trailing non-JSON content");
  std::string invalid_coverage = serialized;
  const std::string valid_coverage =
      "\"process_inventory_complete_samples\": 1";
  const auto coverage_position = invalid_coverage.find(valid_coverage);
  Check(coverage_position != std::string::npos,
        "serialized inventory coverage should be present");
  invalid_coverage.replace(coverage_position, valid_coverage.size(),
                           "\"process_inventory_complete_samples\": 2");
  Check(!SessionManager::Deserialize(invalid_coverage, &error),
        "inventory coverage cannot exceed the benchmark sample count");
  std::string duplicate_schema = serialized;
  duplicate_schema.insert(duplicate_schema.find('{') + 1,
                          "\n  \"schema_version\": 3,");
  Check(!SessionManager::Deserialize(duplicate_schema, &error),
        "active session parser must reject ambiguous duplicate fields");
  std::string nested_schema = serialized;
  RemoveLineContaining(&nested_schema, "\"schema_version\"");
  const auto baseline_object = nested_schema.find("\"baseline\": {");
  Check(baseline_object != std::string::npos,
        "baseline object should be present in nested-field fixture");
  nested_schema.insert(nested_schema.find('{', baseline_object) + 1,
                       "\n    \"schema_version\": 3,");
  Check(!SessionManager::Deserialize(nested_schema, &error),
        "nested fields must not satisfy required top-level session fields");

  auto legacy_session = session;
  legacy_session.actions.resize(1);
  legacy_session.actions[0].error_message.clear();
  std::string legacy = SessionManager::Serialize(legacy_session);
  const auto schema_position = legacy.find("\"schema_version\": 3");
  Check(schema_position != std::string::npos,
        "current schema marker should be present");
  legacy.replace(schema_position, std::string("\"schema_version\": 3").size(),
                 "\"schema_version\": 1");
  for (const char* field :
       {"\"sample_count\"", "\"observed_span_ms\"",
        "\"process_inventory_complete_samples\"",
        "\"tcp_inventory_complete_samples\"",
        "\"protection_inventory_complete_samples\"",
        "\"maximum_disk_queue_length\"",
        "\"maximum_ssd_active_ratio\"", "\"maximum_ssd_latency_ms\"",
        "\"maximum_hdd_active_ratio\"", "\"maximum_hdd_latency_ms\"",
        "\"page_reads_per_sec\"", "\"development_cpu_percent\"",
        "\"development_working_set_bytes\"",
        "\"top_background_io_process\"",
        "\"top_background_io_bytes_per_sec\""}) {
    RemoveLineContaining(&legacy, field);
  }
  const auto legacy_latency = legacy.find("\"maximum_disk_latency_ms\"");
  const auto legacy_latency_comma = legacy.find(',', legacy_latency);
  Check(legacy_latency != std::string::npos &&
            legacy_latency_comma != std::string::npos,
        "legacy fixture should contain a benchmark field separator");
  legacy.erase(legacy_latency_comma, 1);
  RemoveLineContaining(&legacy, "\"source_priority\"");
  RemoveLineContaining(&legacy, "\"timeout_ms\"");
  RemoveLineContaining(&legacy, "\"service_name\"");
  RemoveLineContaining(&legacy, "\"expected_service_identity\"");
  RemoveLineContaining(&legacy, "\"source_service_state\"");
  RemoveLineContaining(&legacy, "\"explicit_confirmation\"");
  RemoveLineContaining(&legacy, "\"reversible\"");
  RemoveLineContaining(&legacy, "\"original_service_state\"");
  RemoveLineContaining(&legacy, "\"result_message\"");
  const std::string error_with_comma = "      \"error_message\": \"\",\n";
  const auto comma_position = legacy.find(error_with_comma);
  Check(comma_position != std::string::npos,
        "legacy fixture should contain the final v2 field separator");
  legacy.replace(comma_position, error_with_comma.size(),
                 "      \"error_message\": \"\"\n");
  const auto legacy_parsed = SessionManager::Deserialize(legacy, &error);
  Check(legacy_parsed.has_value(),
        "schema v1 priority session must remain readable: " + error);
  Check(legacy_parsed->actions[0].action.source_priority ==
            NORMAL_PRIORITY_CLASS &&
            legacy_parsed->actions[0].action.timeout_ms == 2000 &&
            legacy_parsed->baseline.sample_count == 1 &&
            legacy_parsed->baseline.observed_span_ms == 0 &&
            legacy_parsed->baseline.process_inventory_complete_samples == 0 &&
            legacy_parsed->baseline.tcp_inventory_complete_samples == 0 &&
            legacy_parsed->baseline.protection_inventory_complete_samples ==
                0 &&
            legacy_parsed->baseline.maximum_disk_queue_length == 0.0 &&
            legacy_parsed->baseline.page_reads_per_sec == 0.0 &&
            legacy_parsed->baseline.top_background_io_process.empty(),
        "missing v1 fields must receive safe compatibility defaults");

  auto version_two_session = session;
  version_two_session.actions.resize(2);
  std::string version_two = SessionManager::Serialize(version_two_session);
  const auto version_two_position =
      version_two.find("\"schema_version\": 3");
  Check(version_two_position != std::string::npos,
        "schema v3 marker should be present in compatibility fixture");
  version_two.replace(version_two_position,
                      std::string("\"schema_version\": 3").size(),
                      "\"schema_version\": 2");
  for (const char* field :
       {"\"service_name\"", "\"expected_service_identity\"",
        "\"source_service_state\"", "\"explicit_confirmation\"",
        "\"original_service_state\""}) {
    RemoveLineContaining(&version_two, field);
    RemoveLineContaining(&version_two, field);
  }
  const auto version_two_parsed =
      SessionManager::Deserialize(version_two, &error);
  Check(version_two_parsed && version_two_parsed->actions.size() == 2,
        "schema v2 process-action sessions must remain readable: " + error);
}

void TestRealPriorityExecuteAndRollback() {
  const DWORD original = GetPriorityClass(GetCurrentProcess());
  if (original != NORMAL_PRIORITY_CLASS &&
      original != BELOW_NORMAL_PRIORITY_CLASS) {
    return;
  }
  Config config = Config::Defaults();
  config.process_rules["workboost_tests.exe"] =
      {ProcessClass::Development, ProtectionLevel::Strong};
  const DWORD target = original == NORMAL_PRIORITY_CLASS
                           ? ABOVE_NORMAL_PRIORITY_CLASS
                           : NORMAL_PRIORITY_CLASS;
  config.coding_profile.foreground_priority["workboost_tests.exe"] =
      workboost::PriorityName(target);

  ProcessSnapshot process;
  process.pid = GetCurrentProcessId();
  process.name = "workboost_tests.exe";
  process.classification = ProcessClass::Development;
  process.protection_level = ProtectionLevel::Strong;
  process.start_time_100ns = CurrentProcessStartTime();
  process.priority_class = original;
  SystemSnapshot snapshot;
  snapshot.processes.push_back(process);

  OptimizationAction action;
  action.id = "self-priority-test";
  action.type = ActionType::SetPriorityClass;
  action.risk = ActionRisk::Safe;
  action.pid = process.pid;
  action.expected_start_time_100ns = process.start_time_100ns;
  action.process_name = process.name;
  action.source_priority = original;
  action.target_priority = target;
  action.reason = "integration test";

  ActionExecutor executor(config);
  auto executed = executor.Execute(action, snapshot);
  Check(executed.state == workboost::ActionState::Applied,
        "validated self priority action should execute");
  Check(GetPriorityClass(GetCurrentProcess()) == target,
        "target priority should be applied");
  Check(executor.Rollback(&executed), "priority rollback should succeed");
  Check(GetPriorityClass(GetCurrentProcess()) == original,
        "original priority must be restored");
}

void TestLocale() {
  using workboost::Locale;
  using workboost::LocaleId;
  const LocaleId original = Locale::Current();
  Locale::Set(LocaleId::English);
  Check(Locale::Get("Dashboard") == "Dashboard",
        "English locale must return the key unchanged");
  Check(Locale::Get("not-a-translated-key") == "not-a-translated-key",
        "untranslated keys must fall back to English");
  Check(Locale::Format("{0} planned changes", {"5"}) == "5 planned changes",
        "English format must substitute without translating");
  Locale::Set(LocaleId::Chinese);
  Check(Locale::Get("Dashboard") == "仪表盘",
        "Chinese locale must translate known keys");
  Check(Locale::Get("Commit") == "提交内存",
        "Chinese locale must clarify the committed-memory metric");
  Check(!Locale::Get("Settings").empty(),
        "Chinese settings label must be non-empty");
  Check(Locale::Format("{0} planned changes", {"5"}) == "5 项计划变更",
        "Chinese format must substitute into the translation");
  Locale::Set(original);
}

void TestConfigLanguage() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("workboost-lang-test-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(GetTickCount64()));
  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  Check(!filesystem_error, "temporary language directory should be created");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{directory};

  std::string error;
  Check(workboost::windows::AtomicWriteUtf8(
            directory / "settings.json",
            "{\n  \"language\": \"zh\"\n}\n", &error),
        "settings.json fixture should be written: " + error);
  workboost::Config config = workboost::Config::Defaults();
  std::string warning;
  Check(config.LoadDirectory(directory, &warning) && warning.empty() &&
            config.language == "zh",
        "settings.json language must be loaded and win over the default");
  const workboost::Config defaults = workboost::Config::Defaults();
  Check(defaults.language == "en" || defaults.language == "zh",
        "default language must be a known value");
}

void TestHardwareInfo() {
  std::string cpu_model;
  std::string error;
  Check(workboost::windows::QueryCpuModel(&cpu_model, &error) &&
            !cpu_model.empty(),
        "CPU model query must return a non-empty name: " + error);
  std::string memory_model;
  if (workboost::windows::QueryMemoryModel(&memory_model, &error)) {
    Check(!memory_model.empty(), "memory model must be non-empty when read");
  }

  const workboost::Config config = workboost::Config::Defaults();
  workboost::windows::SystemCollector collector(config);
  workboost::windows::WindowsError collector_error;
  Check(collector.Initialize(&collector_error),
        "system collector must initialize: " + collector_error.Describe());
  const auto snapshot = collector.Sample();
  Check(!snapshot.cpu_model.empty(),
        "sample must carry the collected CPU model");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"commit ratio", TestCommitRatio},
      {"CPU percentage normalization", TestCpuPercentageNormalization},
      {"classification and unknown protection", TestClassificationAndUnknownProtection},
      {"TCP protection", TestTcpProtection},
      {"custom remote port protection", TestCustomRemotePortProtection},
      {"capture protection", TestCaptureProtection},
      {"safety validator", TestSafetyValidator},
      {"graceful close planning and protection",
       TestGracefulClosePlanningAndProtection},
      {"service protection policy", TestServiceProtectionPolicy},
      {"service action planning and validation",
       TestServiceActionPlanningAndValidation},
      {"malformed config fails closed", TestMalformedConfigFailsClosed},
      {"privacy-safe local logger", TestPrivacySafeLocalLogger},
      {"relative atomic output", TestAtomicRelativeWrite},
      {"elevated helper protocol", TestHelperProtocol},
      {"authenticated helper pipe", TestAuthenticatedHelperPipe},
      {"elevated handler rejects critical service",
       TestElevatedHandlerRejectsCriticalService},
      {"current process verification helpers",
       TestCurrentProcessVerificationHelpers},
      {"graceful close recovery state", TestGracefulCloseRecoveryState},
      {"real graceful close request", TestRealGracefulCloseRequest},
      {"read-only service query", TestReadOnlyServiceQuery},
      {"read-only serial port query", TestReadOnlySerialPortQuery},
      {"read-only startup query", TestReadOnlyStartupQuery},
      {"startup benchmark summary", TestStartupBenchmarkSummary},
      {"startup benchmark comparison", TestStartupBenchmarkComparison},
      {"passive startup benchmark timeout",
       TestPassiveStartupBenchmarkTimeout},
      {"dashboard presenter", TestDashboardPresenter},
      {"dashboard Top Impact metrics", TestDashboardTopImpactMetrics},
      {"dashboard overview responsive layout", TestDashboardOverviewLayout},
      {"dashboard close behavior", TestDashboardCloseBehavior},
      {"dashboard renderer hit targets", TestDashboardRendererHitTargets},
      {"dashboard language toggle target",
       TestDashboardLanguageToggleTarget},
      {"Coding Mode command validation", TestCodingModeCommandValidation},
      {"completion report", TestCompletionReport},
      {"diagnosis rules", TestDiagnosisRules},
      {"benchmark window aggregation", TestBenchmarkWindowAggregation},
      {"session round trip", TestSessionRoundTrip},
      {"real priority execute and rollback", TestRealPriorityExecuteAndRollback},
      {"locale translation", TestLocale},
      {"config language setting", TestConfigLanguage},
      {"hardware model queries", TestHardwareInfo},
  };
  std::size_t failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "[FAIL] " << name << ": " << exception.what() << '\n';
    }
  }
  std::cout << tests.size() - failures << '/' << tests.size()
            << " tests passed\n";
  return failures == 0 ? 0 : 1;
}
