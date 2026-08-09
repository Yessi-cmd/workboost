#include "cli/cli.h"

#include "app/session_manager.h"
#include "core/config/config.h"
#include "core/diagnosis/diagnosis_engine.h"
#include "core/optimization/optimization.h"
#include "platform/windows/system_collector.h"
#include "platform/windows/windows_utils.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace workboost {
namespace {

constexpr double kMiB = 1024.0 * 1024.0;
constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

struct CliOptions {
  bool json{};
  bool show_remote_ip{};
  bool dry_run{};
  int limit{10};
  int duration_seconds{-1};
  int baseline_seconds{10};
  std::optional<std::filesystem::path> config_directory;
  std::optional<std::filesystem::path> output_path;
};

void PrintUsage() {
  std::cout
      << "WorkBoost - Windows development workstation diagnostics\n\n"
      << "Usage:\n"
      << "  workboost status [--json]\n"
      << "  workboost top <cpu|mem|io> [--limit N] [--json]\n"
      << "  workboost connections [--show-remote-ip] [--json]\n"
      << "  workboost protected [--json]\n"
      << "  workboost diagnose [--duration SECONDS] [--output FILE] [--json]\n"
      << "  workboost profile show coding [--json]\n"
      << "  workboost coding enter [--dry-run] [--baseline-duration SECONDS]\n"
      << "  workboost coding exit\n"
      << "  workboost recovery <status|restore>\n\n"
      << "Global option: --config-dir DIRECTORY\n";
}

bool ParsePositiveInt(const std::string& value, int* output) {
  try {
    std::size_t consumed = 0;
    const int parsed = std::stoi(value, &consumed);
    if (consumed != value.size() || parsed < 0) return false;
    *output = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseOptions(const std::vector<std::string>& input,
                  std::vector<std::string>* positional, CliOptions* options,
                  std::string* error) {
  for (std::size_t i = 0; i < input.size(); ++i) {
    const std::string& value = input[i];
    if (value == "--json") {
      options->json = true;
    } else if (value == "--show-remote-ip") {
      options->show_remote_ip = true;
    } else if (value == "--dry-run") {
      options->dry_run = true;
    } else if (value == "--limit" || value == "--duration" ||
               value == "--baseline-duration" || value == "--config-dir" ||
               value == "--output") {
      if (++i >= input.size()) {
        *error = value + " requires a value";
        return false;
      }
      if (value == "--config-dir") {
        options->config_directory = input[i];
      } else if (value == "--output") {
        options->output_path = input[i];
      } else {
        int parsed = 0;
        if (!ParsePositiveInt(input[i], &parsed)) {
          *error = value + " requires a non-negative integer";
          return false;
        }
        if (value == "--limit") options->limit = std::max(1, parsed);
        if (value == "--duration") options->duration_seconds = parsed;
        if (value == "--baseline-duration")
          options->baseline_seconds = parsed;
      }
    } else if (!value.empty() && value[0] == '-') {
      *error = "unknown option: " + value;
      return false;
    } else {
      positional->push_back(value);
    }
  }
  return true;
}

std::string FormatBytes(double bytes) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(1);
  if (bytes >= kGiB) output << bytes / kGiB << " GB";
  else if (bytes >= kMiB) output << bytes / kMiB << " MB";
  else if (bytes >= 1024.0) output << bytes / 1024.0 << " KB";
  else output << bytes << " B";
  return output.str();
}

std::string JsonEvidenceValue(const EvidenceValue& value) {
  return std::visit(
      [](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return "\"" + windows::JsonEscape(item) + "\"";
        } else if constexpr (std::is_same_v<T, bool>) {
          return item ? "true" : "false";
        } else {
          std::ostringstream output;
          output << std::setprecision(12) << item;
          return output.str();
        }
      },
      value);
}

std::string DiagnosisJson(const std::vector<DiagnosisResult>& diagnoses,
                          std::size_t sample_count) {
  std::ostringstream output;
  output << "{\n  \"schema_version\": 1,\n"
         << "  \"generated_at\": \"" << windows::Iso8601Now() << "\",\n"
         << "  \"sample_count\": " << sample_count << ",\n"
         << "  \"diagnoses\": [";
  for (std::size_t i = 0; i < diagnoses.size(); ++i) {
    const auto& diagnosis = diagnoses[i];
    output << (i == 0 ? "\n" : ",\n")
           << "    {\n"
           << "      \"type\": \"" << windows::JsonEscape(diagnosis.type)
           << "\",\n"
           << "      \"severity\": \"" << ToString(diagnosis.severity)
           << "\",\n"
           << "      \"confidence\": \"" << ToString(diagnosis.confidence)
           << "\",\n"
           << "      \"summary\": \""
           << windows::JsonEscape(diagnosis.summary) << "\",\n"
           << "      \"evidence\": {";
    std::size_t evidence_index = 0;
    for (const auto& [key, value] : diagnosis.evidence) {
      output << (evidence_index++ == 0 ? "\n" : ",\n")
             << "        \"" << windows::JsonEscape(key) << "\": "
             << JsonEvidenceValue(value);
    }
    if (!diagnosis.evidence.empty()) output << '\n';
    output << "      }\n    }";
  }
  if (!diagnoses.empty()) output << '\n';
  output << "  ]\n}\n";
  return output.str();
}

std::string DiagnosisText(const std::vector<DiagnosisResult>& diagnoses,
                          std::size_t sample_count) {
  std::ostringstream output;
  output << "WORKBOOST DIAGNOSTIC REPORT\n"
         << "Generated: " << windows::Iso8601Now() << "\n"
         << "Samples: " << sample_count << "\n\n";
  if (diagnoses.empty()) {
    output << "No sustained bottleneck matched the configured thresholds.\n";
    return output.str();
  }
  for (const auto& diagnosis : diagnoses) {
    output << ToString(diagnosis.severity) << " " << diagnosis.type << " ("
           << ToString(diagnosis.confidence) << " confidence)\n"
           << diagnosis.summary << "\n";
    for (const auto& [key, value] : diagnosis.evidence) {
      output << "  " << key << ": ";
      std::visit([&output](const auto& item) { output << item; }, value);
      output << '\n';
    }
    output << '\n';
  }
  return output.str();
}

Config LoadConfig(const CliOptions& options) {
  Config config = Config::Defaults();
  std::string warning;
  if (options.config_directory) {
    config.LoadDirectory(*options.config_directory, &warning);
  } else {
    const auto executable_config = windows::ExecutableDirectory() / "config";
    const auto current_config = std::filesystem::current_path() / "config";
    if (std::filesystem::exists(executable_config))
      config.LoadDirectory(executable_config, &warning);
    if (current_config != executable_config &&
        std::filesystem::exists(current_config))
      config.LoadDirectory(current_config, &warning);
    const auto user_config = windows::LocalAppDataDirectory();
    if (std::filesystem::exists(user_config / "diagnosis.json") ||
        std::filesystem::exists(user_config / "profiles.json") ||
        std::filesystem::exists(user_config / "process_rules.json")) {
      config.LoadDirectory(user_config, &warning);
    }
  }
  if (!warning.empty()) std::cerr << "Configuration warning: " << warning << '\n';
  return config;
}

std::optional<SystemSnapshot> CaptureOne(const Config& config,
                                         std::string* error) {
  windows::SystemCollector collector(config);
  windows::WindowsError windows_error;
  if (!collector.Initialize(&windows_error)) {
    *error = windows_error.Describe();
    return std::nullopt;
  }
  std::this_thread::sleep_for(
      std::chrono::milliseconds(config.sample_interval_ms));
  return collector.Sample(&windows_error);
}

std::optional<SnapshotHistory> CaptureHistory(const Config& config,
                                              int duration_seconds,
                                              std::string* error) {
  const int samples = std::max(
      1, static_cast<int>(std::ceil(duration_seconds * 1000.0 /
                                    config.sample_interval_ms)));
  SnapshotHistory history(static_cast<std::size_t>(std::max(
      samples, config.history_seconds * 1000 / config.sample_interval_ms)));
  windows::SystemCollector collector(config);
  windows::WindowsError windows_error;
  if (!collector.Initialize(&windows_error)) {
    *error = windows_error.Describe();
    return std::nullopt;
  }
  for (int i = 0; i < samples; ++i) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config.sample_interval_ms));
    history.Add(collector.Sample(&windows_error));
  }
  return history;
}

std::unordered_map<std::uint32_t, std::string> ProcessNames(
    const SystemSnapshot& snapshot) {
  std::unordered_map<std::uint32_t, std::string> result;
  for (const auto& process : snapshot.processes)
    result[process.pid] = process.name;
  return result;
}

std::vector<ProcessSnapshot> SortedProcesses(const SystemSnapshot& snapshot,
                                             const std::string& metric) {
  auto processes = snapshot.processes;
  std::stable_sort(processes.begin(), processes.end(),
                   [&metric](const ProcessSnapshot& left,
                             const ProcessSnapshot& right) {
                     if (metric == "cpu")
                       return left.cpu_percent > right.cpu_percent;
                     if (metric == "mem")
                       return left.private_bytes > right.private_bytes;
                     return left.IoBytesPerSec() > right.IoBytesPerSec();
                   });
  return processes;
}

void PrintTop(const SystemSnapshot& snapshot, const std::string& metric,
              const CliOptions& options) {
  const auto processes = SortedProcesses(snapshot, metric);
  const std::size_t count =
      std::min<std::size_t>(processes.size(), options.limit);
  if (options.json) {
    std::cout << "{\n  \"metric\": \"" << metric << "\",\n"
              << "  \"processes\": [";
    for (std::size_t i = 0; i < count; ++i) {
      const auto& process = processes[i];
      std::cout << (i == 0 ? "\n" : ",\n")
                << "    {\"pid\": " << process.pid << ", \"name\": \""
                << windows::JsonEscape(process.name) << "\", \"cpu_percent\": "
                << process.cpu_percent << ", \"private_bytes\": "
                << process.private_bytes << ", \"read_bytes_per_sec\": "
                << process.read_bytes_per_sec
                << ", \"write_bytes_per_sec\": "
                << process.write_bytes_per_sec << '}';
    }
    if (count != 0) std::cout << '\n';
    std::cout << "  ]\n}\n";
    return;
  }
  std::cout << "PID      CPU       PRIVATE       READ/s        WRITE/s       NAME\n";
  for (std::size_t i = 0; i < count; ++i) {
    const auto& process = processes[i];
    std::cout << std::left << std::setw(9) << process.pid << std::right
              << std::setw(6) << std::fixed << std::setprecision(1)
              << process.cpu_percent << "%  " << std::setw(12)
              << FormatBytes(static_cast<double>(process.private_bytes)) << "  "
              << std::setw(12) << FormatBytes(process.read_bytes_per_sec) << "  "
              << std::setw(12) << FormatBytes(process.write_bytes_per_sec) << "  "
              << process.name << '\n';
  }
}

void PrintConnections(const SystemSnapshot& snapshot, const CliOptions& options) {
  const auto names = ProcessNames(snapshot);
  std::vector<TcpSession> sessions;
  for (const auto& session : snapshot.tcp_sessions) {
    if (session.state == TcpState::Established) sessions.push_back(session);
  }
  if (options.json) {
    std::cout << "{\n  \"remote_ip_masked\": "
              << (options.show_remote_ip ? "false" : "true")
              << ",\n  \"connections\": [";
    for (std::size_t i = 0; i < sessions.size(); ++i) {
      const auto& session = sessions[i];
      const auto name = names.find(session.pid);
      const std::string remote = options.show_remote_ip
                                     ? session.remote_address
                                     : windows::MaskIpAddress(session.remote_address);
      std::cout << (i == 0 ? "\n" : ",\n")
                << "    {\"pid\": " << session.pid << ", \"process\": \""
                << windows::JsonEscape(name == names.end() ? "unknown" : name->second)
                << "\", \"local_address\": \""
                << windows::JsonEscape(session.local_address)
                << "\", \"local_port\": " << session.local_port
                << ", \"remote_address\": \""
                << windows::JsonEscape(remote) << "\", \"remote_port\": "
                << session.remote_port << ", \"state\": \"ESTABLISHED\"}";
    }
    if (!sessions.empty()) std::cout << '\n';
    std::cout << "  ]\n}\n";
    return;
  }
  std::cout << "PID      PROCESS                 LOCAL                    REMOTE\n";
  for (const auto& session : sessions) {
    const auto name = names.find(session.pid);
    const std::string remote = options.show_remote_ip
                                   ? session.remote_address
                                   : windows::MaskIpAddress(session.remote_address);
    std::ostringstream local_endpoint;
    local_endpoint << session.local_address << ':' << session.local_port;
    std::ostringstream remote_endpoint;
    remote_endpoint << remote << ':' << session.remote_port;
    std::cout << std::left << std::setw(9) << session.pid << std::setw(24)
              << (name == names.end() ? "unknown" : name->second) << std::setw(25)
              << local_endpoint.str() << remote_endpoint.str() << '\n';
  }
}

std::vector<const ProcessSnapshot*> ProtectedProcesses(
    const SystemSnapshot& snapshot, bool workload_only) {
  std::vector<const ProcessSnapshot*> result;
  for (const auto& process : snapshot.processes) {
    const bool protected_level =
        process.protection_level == ProtectionLevel::Strong ||
        process.protection_level == ProtectionLevel::SystemCritical ||
        process.protection_level == ProtectionLevel::UserExplicit;
    const bool workload = process.classification == ProcessClass::Development ||
                          process.classification == ProcessClass::RemoteTerminal ||
                          process.classification == ProcessClass::SerialTerminal ||
                          process.classification == ProcessClass::PacketCapture ||
                          process.classification == ProcessClass::BuildTool ||
                          process.classification == ProcessClass::VersionControl;
    if (protected_level && (!workload_only || workload)) result.push_back(&process);
  }
  std::stable_sort(result.begin(), result.end(),
                   [](const ProcessSnapshot* left, const ProcessSnapshot* right) {
                     return left->name < right->name;
                   });
  return result;
}

void PrintProtected(const SystemSnapshot& snapshot, const CliOptions& options,
                    bool workload_only = false) {
  const auto processes = ProtectedProcesses(snapshot, workload_only);
  if (options.json) {
    std::cout << "{\n  \"processes\": [";
    for (std::size_t i = 0; i < processes.size(); ++i) {
      const auto& process = *processes[i];
      std::cout << (i == 0 ? "\n" : ",\n")
                << "    {\"pid\": " << process.pid << ", \"name\": \""
                << windows::JsonEscape(process.name) << "\", \"class\": \""
                << ToString(process.classification)
                << "\", \"protection\": \""
                << ToString(process.protection_level) << "\"}";
    }
    if (!processes.empty()) std::cout << '\n';
    std::cout << "  ]\n}\n";
    return;
  }
  for (const auto* process : processes) {
    std::cout << std::left << std::setw(28) << process->name << " PID "
              << std::setw(8) << process->pid << ' '
              << std::setw(18) << ToString(process->classification) << ' '
              << ToString(process->protection_level) << '\n';
  }
  if (processes.empty()) std::cout << "No protected process is currently running.\n";
}

void PrintStatus(const SystemSnapshot& snapshot, const Config& config,
                 const CliOptions& options) {
  if (options.json) {
    SnapshotHistory history(1);
    history.Add(snapshot);
    const auto diagnoses = DiagnosisEngine(config).Evaluate(history);
    std::cout << "{\n  \"system\": {\"cpu_percent\": "
              << snapshot.cpu_percent << ", \"physical_total_bytes\": "
              << snapshot.memory.physical_total_bytes
              << ", \"physical_available_bytes\": "
              << snapshot.memory.physical_available_bytes
              << ", \"commit_ratio\": " << snapshot.memory.CommitRatio()
              << ", \"page_reads_per_sec\": " << snapshot.page_reads_per_sec
              << "},\n  \"disks\": [";
    for (std::size_t i = 0; i < snapshot.disks.size(); ++i) {
      const auto& disk = snapshot.disks[i];
      std::cout << (i == 0 ? "\n" : ",\n")
                << "    {\"instance\": \""
                << windows::JsonEscape(disk.instance) << "\", \"volumes\": \""
                << windows::JsonEscape(disk.volumes) << "\", \"media\": \""
                << ToString(disk.media) << "\", \"active_ratio\": "
                << disk.active_ratio << ", \"latency_ms\": "
                << disk.average_latency_ms << "}";
    }
    if (!snapshot.disks.empty()) std::cout << '\n';
    std::cout << "  ],\n  \"diagnosis_count\": " << diagnoses.size()
              << "\n}\n";
    return;
  }
  std::cout << "SYSTEM\n"
            << "CPU       " << std::fixed << std::setprecision(1)
            << snapshot.cpu_percent << "%\n"
            << "RAM       "
            << FormatBytes(static_cast<double>(
                   snapshot.memory.physical_total_bytes -
                   snapshot.memory.physical_available_bytes))
            << " / "
            << FormatBytes(static_cast<double>(snapshot.memory.physical_total_bytes))
            << "\nAVAILABLE "
            << FormatBytes(
                   static_cast<double>(snapshot.memory.physical_available_bytes))
            << "\nCOMMIT    " << snapshot.memory.CommitRatio() * 100.0 << "%\n"
            << "PAGE READ " << snapshot.page_reads_per_sec << "/s\n\nDISK\n";
  if (snapshot.disks.empty()) {
    std::cout << "Disk performance counters are unavailable.\n";
  } else {
    for (const auto& disk : snapshot.disks) {
      std::cout << disk.instance << " " << ToString(disk.media) << " Active "
                << disk.active_ratio * 100.0 << "% Latency "
                << disk.average_latency_ms << " ms Queue " << disk.queue_length
                << '\n';
    }
  }
  std::cout << "\nPROTECTED WORKLOAD\n";
  PrintProtected(snapshot, CliOptions{}, true);
  std::cout << "\nTOP IO\n";
  CliOptions top_options;
  top_options.limit = 5;
  PrintTop(snapshot, "io", top_options);
  SnapshotHistory history(1);
  history.Add(snapshot);
  const auto diagnoses = DiagnosisEngine(config).Evaluate(history);
  std::cout << "\nDIAGNOSIS\n";
  if (diagnoses.empty()) {
    std::cout << "No current threshold match; use 'diagnose' for a time-window "
                 "assessment.\n";
  } else {
    for (const auto& diagnosis : diagnoses) {
      std::cout << ToString(diagnosis.severity) << ' ' << diagnosis.type << '\n';
    }
  }
}

void PrintProfile(const Config& config, bool json) {
  if (json) {
    std::cout << "{\n  \"name\": \"coding\",\n"
              << "  \"foreground_priority\": {";
    std::size_t index = 0;
    for (const auto& [name, priority] :
         config.coding_profile.foreground_priority) {
      std::cout << (index++ == 0 ? "\n" : ",\n") << "    \""
                << windows::JsonEscape(name) << "\": \""
                << windows::JsonEscape(priority) << "\"";
    }
    if (index != 0) std::cout << '\n';
    std::cout << "  },\n  \"always_protect\": [";
    index = 0;
    for (const auto& name : config.coding_profile.always_protect) {
      std::cout << (index++ == 0 ? "\n" : ",\n") << "    \""
                << windows::JsonEscape(name) << "\"";
    }
    if (index != 0) std::cout << '\n';
    std::cout << "  ]\n}\n";
    return;
  }
  std::cout << "PROFILE coding\nForeground priority:\n";
  for (const auto& [name, priority] :
       config.coding_profile.foreground_priority)
    std::cout << "  " << name << " -> " << priority << '\n';
  std::cout << "Always protect:\n";
  for (const auto& name : config.coding_profile.always_protect)
    std::cout << "  " << name << '\n';
  std::cout << "Priority down allowed: "
            << config.coding_profile.allow_priority_down.size() << " rule(s)\n"
            << "Graceful close allowed: "
            << config.coding_profile.allow_graceful_close.size() << " rule(s)\n";
}

int RestoreSession(const Config& config, SessionManager* manager,
                   bool recovery_command) {
  std::string error;
  auto session = manager->LoadActive(&error);
  if (!session) {
    if (manager->HasActiveSession()) {
      std::cerr << "Safe Mode: " << error << '\n';
      return 2;
    }
    std::cout << "No active Coding Mode session.\n";
    return 0;
  }
  session->state = SessionState::Recovering;
  manager->Save(*session, &error);
  std::optional<SystemSnapshot> optimized;
  if (!recovery_command) {
    std::string capture_error;
    optimized = CaptureOne(config, &capture_error);
    if (!optimized) {
      std::cerr << "Warning: optimized-state capture failed; rollback will "
                   "continue: "
                << capture_error << '\n';
    }
  }
  ActionExecutor executor(config);
  bool success = true;
  for (auto it = session->actions.rbegin(); it != session->actions.rend(); ++it) {
    if (!executor.Rollback(&*it)) success = false;
    if (!manager->Save(*session, &error)) success = false;
  }
  if (!success) {
    session->state = SessionState::SafeMode;
    manager->Save(*session, &error);
    std::cerr << "Safe Mode: one or more actions could not be restored. "
              << error << '\n';
    return 2;
  }
  auto verification = CaptureOne(config, &error);
  if (!verification) {
    session->state = SessionState::SafeMode;
    manager->Save(*session, &error);
    std::cerr << "Safe Mode: rollback succeeded but verification failed: "
              << error << '\n';
    return 2;
  }
  std::filesystem::path report;
  const SystemSnapshot& report_point = optimized ? *optimized : *verification;
  if (!manager->Complete(*session, report_point, &report, &error)) {
    std::cerr << "Safe Mode: " << error << '\n';
    return 2;
  }
  std::cout << (recovery_command ? "Recovery completed.\n"
                                 : "Coding Mode exited.\n")
            << "Report: " << report.string() << '\n';
  return 0;
}

int EnterCodingMode(const Config& config, const CliOptions& options,
                    SessionManager* manager) {
  if (manager->HasActiveSession()) {
    std::cerr << "An unfinished session exists. Run 'workboost recovery status' "
                 "or 'workboost recovery restore'.\n";
    return 2;
  }
  std::string error;
  std::cout << "Capturing baseline for " << options.baseline_seconds
            << " second(s)...\n";
  auto history = CaptureHistory(config, options.baseline_seconds, &error);
  if (!history || history->Latest() == nullptr) {
    std::cerr << "Baseline failed: " << error << '\n';
    return 1;
  }
  const SystemSnapshot& baseline = *history->Latest();
  const auto plan = OptimizationPlanner(config).Create(baseline);
  std::cout << "Plan: " << plan.actions.size() << " safe action(s), "
            << plan.rejected.size() << " rejected candidate(s).\n";
  for (const auto& action : plan.actions) {
    std::cout << "  " << action.process_name << " PID " << action.pid << ": "
              << PriorityName(action.target_priority) << '\n';
  }
  if (options.dry_run) return 0;

  auto execution_snapshot = CaptureOne(config, &error);
  if (!execution_snapshot) {
    std::cerr << "Cannot refresh protection state before execution: " << error
              << '\n';
    return 1;
  }

  OptimizationSession session = manager->Create(baseline);
  if (!manager->Save(session, &error)) {
    std::cerr << "Cannot persist session before execution: " << error << '\n';
    return 2;
  }
  ActionExecutor executor(config);
  for (const auto& action : plan.actions) {
    ExecutedAction record;
    record.action = action;
    record.state = ActionState::Planned;
    // Persist the restore value before making the system call. Recovery treats
    // Planned as an uncertain state and idempotently writes this value back.
    record.original_priority = action.source_priority;
    for (const auto& process : execution_snapshot->processes) {
      if (process.pid == action.pid &&
          process.start_time_100ns == action.expected_start_time_100ns) {
        record.original_priority = process.priority_class;
        break;
      }
    }
    session.actions.push_back(record);
    if (!manager->Save(session, &error)) {
      std::cerr << "Safe Mode: cannot persist planned action: " << error << '\n';
      return 2;
    }
    session.actions.back() = executor.Execute(action, *execution_snapshot);
    if (!manager->Save(session, &error)) {
      std::cerr << "Safe Mode: action state persistence failed: " << error
                << '\n';
      return 2;
    }
  }
  std::size_t applied = 0;
  for (const auto& action : session.actions)
    applied += action.state == ActionState::Applied ? 1U : 0U;
  std::cout << "Coding Mode active; " << applied << " action(s) applied.\n"
            << "Exit with: workboost coding exit\n";
  return 0;
}

int RunDiagnose(const Config& config, const CliOptions& options) {
  const int duration = options.duration_seconds < 0
                           ? config.history_seconds
                           : options.duration_seconds;
  std::string error;
  auto history = CaptureHistory(config, duration, &error);
  if (!history) {
    std::cerr << "Diagnostic capture failed: " << error << '\n';
    return 1;
  }
  const auto diagnoses = DiagnosisEngine(config).Evaluate(*history);
  const std::string report = options.json
                                 ? DiagnosisJson(diagnoses, history->Size())
                                 : DiagnosisText(diagnoses, history->Size());
  if (options.output_path) {
    if (!windows::AtomicWriteUtf8(*options.output_path, report, &error)) {
      std::cerr << "Cannot write report: " << error << '\n';
      return 1;
    }
    std::cout << "Report: " << options.output_path->string() << '\n';
  } else {
    std::cout << report;
  }
  return 0;
}

}  // namespace

int RunCli(int argc, char* argv[]) {
  std::vector<std::string> raw;
  for (int i = 1; i < argc; ++i) raw.emplace_back(argv[i]);
  CliOptions options;
  std::vector<std::string> positional;
  std::string error;
  if (!ParseOptions(raw, &positional, &options, &error)) {
    std::cerr << error << "\n\n";
    PrintUsage();
    return 64;
  }
  if (positional.empty() || positional[0] == "help") {
    PrintUsage();
    return positional.empty() ? 64 : 0;
  }

  Config config = LoadConfig(options);
  SessionManager sessions(windows::LocalAppDataDirectory());
  const std::string& command = positional[0];

  const bool recovery_path =
      command == "recovery" ||
      (command == "coding" && positional.size() == 2 &&
       positional[1] == "exit");
  if (sessions.HasActiveSession() && !recovery_path) {
    std::cerr << "Safe Mode: an unfinished Coding Mode session exists; "
                 "monitoring remains available, but new actions are blocked.\n";
  }

  if (command == "profile") {
    if (positional.size() == 3 && positional[1] == "show" &&
        positional[2] == "coding") {
      PrintProfile(config, options.json);
      return 0;
    }
    std::cerr << "Usage: workboost profile show coding\n";
    return 64;
  }
  if (command == "recovery") {
    if (positional.size() != 2) {
      std::cerr << "Usage: workboost recovery <status|restore>\n";
      return 64;
    }
    if (positional[1] == "status") {
      if (!sessions.HasActiveSession()) {
        std::cout << "Recovery not required.\n";
        return 0;
      }
      auto session = sessions.LoadActive(&error);
      if (!session) {
        std::cerr << "Safe Mode: " << error << '\n';
        return 2;
      }
      std::cout << "Recovery required for session " << session->session_id
                << " (" << ToString(session->state) << ", "
                << session->actions.size() << " recorded action(s)).\n";
      return 2;
    }
    if (positional[1] == "restore")
      return RestoreSession(config, &sessions, true);
    std::cerr << "Usage: workboost recovery <status|restore>\n";
    return 64;
  }
  if (command == "coding") {
    if (positional.size() != 2 ||
        (positional[1] != "enter" && positional[1] != "exit")) {
      std::cerr << "Usage: workboost coding <enter|exit>\n";
      return 64;
    }
    if (positional[1] == "enter")
      return EnterCodingMode(config, options, &sessions);
    return RestoreSession(config, &sessions, false);
  }
  if (command == "diagnose") return RunDiagnose(config, options);

  auto snapshot = CaptureOne(config, &error);
  if (!snapshot) {
    std::cerr << "System sampling failed: " << error << '\n';
    return 1;
  }
  if (command == "status") {
    PrintStatus(*snapshot, config, options);
    return 0;
  }
  if (command == "connections") {
    PrintConnections(*snapshot, options);
    return 0;
  }
  if (command == "protected") {
    PrintProtected(*snapshot, options);
    return 0;
  }
  if (command == "top") {
    if (positional.size() != 2 ||
        (positional[1] != "cpu" && positional[1] != "mem" &&
         positional[1] != "io")) {
      std::cerr << "Usage: workboost top <cpu|mem|io>\n";
      return 64;
    }
    PrintTop(*snapshot, positional[1], options);
    return 0;
  }
  std::cerr << "Unknown command: " << command << "\n\n";
  PrintUsage();
  return 64;
}

}  // namespace workboost
