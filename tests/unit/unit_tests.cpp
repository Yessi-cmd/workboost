#include "app/session_manager.h"
#include "core/config/config.h"
#include "core/diagnosis/diagnosis_engine.h"
#include "core/optimization/optimization.h"
#include "core/policy/protection_policy.h"

#include <windows.h>

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using workboost::ActionRisk;
using workboost::ActionExecutor;
using workboost::ActionType;
using workboost::Config;
using workboost::DiagnosisEngine;
using workboost::DiskMedia;
using workboost::DiskSnapshot;
using workboost::OptimizationAction;
using workboost::ProcessClass;
using workboost::ProcessSnapshot;
using workboost::ProtectionLevel;
using workboost::ProtectionPolicy;
using workboost::SafetyValidator;
using workboost::SessionManager;
using workboost::SnapshotHistory;
using workboost::SystemSnapshot;
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

void TestCommitRatio() {
  workboost::MemorySnapshot memory;
  memory.commit_total_bytes = 80;
  memory.commit_limit_bytes = 100;
  Check(std::abs(memory.CommitRatio() - 0.8) < 0.0001,
        "commit ratio should be normalized");
  memory.commit_limit_bytes = 0;
  Check(memory.CommitRatio() == 0.0, "zero commit limit must be safe");
}

void TestClassificationAndUnknownProtection() {
  const Config config = Config::Defaults();
  Check(config.RuleFor("SSH.EXE").process_class == ProcessClass::RemoteTerminal,
        "classification must be case-insensitive");
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
}

void TestTcpProtection() {
  Config config = Config::Defaults();
  ProcessSnapshot process;
  process.pid = 42;
  process.name = "custom-terminal.exe";
  process.classification = ProcessClass::Unknown;
  SystemSnapshot snapshot;
  snapshot.processes.push_back(process);
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
}

void TestDiagnosisRules() {
  Config config = Config::Defaults();
  SnapshotHistory history(10);
  for (int i = 0; i < 6; ++i) {
    SystemSnapshot snapshot;
    snapshot.cpu_percent = 95.0;
    snapshot.memory.physical_total_bytes = 16ULL * 1024 * 1024 * 1024;
    snapshot.memory.physical_available_bytes = 900ULL * 1024 * 1024;
    snapshot.memory.commit_total_bytes = 90;
    snapshot.memory.commit_limit_bytes = 100;
    snapshot.page_reads_per_sec = 100.0;
    snapshot.disks.push_back(DiskSnapshot{"1 D:", "D:", DiskMedia::HDD,
                                          0.98, 70.0, 7.0, 20e6, 15e6});
    ProcessSnapshot defender;
    defender.pid = 20;
    defender.name = "MsMpEng.exe";
    defender.classification = ProcessClass::Security;
    defender.cpu_percent = 20.0;
    defender.read_bytes_per_sec = 12e6;
    snapshot.processes.push_back(defender);
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
}

void TestSessionRoundTrip() {
  SystemSnapshot baseline;
  baseline.cpu_percent = 25.0;
  baseline.memory.physical_available_bytes = 4096;
  baseline.memory.commit_total_bytes = 50;
  baseline.memory.commit_limit_bytes = 100;
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
  executed.action.target_priority = ABOVE_NORMAL_PRIORITY_CLASS;
  executed.action.reason = "test";
  executed.state = workboost::ActionState::Applied;
  executed.original_priority = NORMAL_PRIORITY_CLASS;
  session.actions.push_back(executed);

  std::string error;
  const auto parsed = SessionManager::Deserialize(
      SessionManager::Serialize(session), &error);
  Check(parsed.has_value(), "session should deserialize: " + error);
  Check(parsed->session_id == session.session_id, "session id must round-trip");
  Check(parsed->actions.size() == 1, "action list must round-trip");
  Check(parsed->actions[0].original_priority == NORMAL_PRIORITY_CLASS,
        "rollback priority payload must round-trip");
}

void TestRealPriorityExecuteAndRollback() {
  const DWORD original = GetPriorityClass(GetCurrentProcess());
  if (original != NORMAL_PRIORITY_CLASS &&
      original != BELOW_NORMAL_PRIORITY_CLASS) {
    return;
  }
  FILETIME created{}, exited{}, kernel{}, user{};
  Check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) !=
            FALSE,
        "current process times must be readable");
  ULARGE_INTEGER start{};
  start.LowPart = created.dwLowDateTime;
  start.HighPart = created.dwHighDateTime;

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
  process.start_time_100ns = start.QuadPart;
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

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"commit ratio", TestCommitRatio},
      {"classification and unknown protection", TestClassificationAndUnknownProtection},
      {"TCP protection", TestTcpProtection},
      {"custom remote port protection", TestCustomRemotePortProtection},
      {"capture protection", TestCaptureProtection},
      {"safety validator", TestSafetyValidator},
      {"diagnosis rules", TestDiagnosisRules},
      {"session round trip", TestSessionRoundTrip},
      {"real priority execute and rollback", TestRealPriorityExecuteAndRollback},
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
