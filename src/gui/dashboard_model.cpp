#include "gui/dashboard_model.h"

#include "core/diagnosis/diagnosis_engine.h"
#include "core/optimization/optimization.h"
#include "core/policy/protection_policy.h"
#include "platform/windows/windows_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace workboost::gui {
namespace {

constexpr double kKiB = 1024.0;
constexpr double kMiB = kKiB * 1024.0;
constexpr double kGiB = kMiB * 1024.0;

std::string FormatBytes(double bytes) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(1);
  if (bytes >= kGiB) {
    output << bytes / kGiB << " GB";
  } else if (bytes >= kMiB) {
    output << bytes / kMiB << " MB";
  } else if (bytes >= kKiB) {
    output << bytes / kKiB << " KB";
  } else {
    output << bytes << " B";
  }
  return output.str();
}

std::string FormatRate(double bytes_per_second) {
  return FormatBytes(bytes_per_second) + "/s";
}

std::vector<std::string> SplitVolumeNames(const std::string& volumes) {
  std::vector<std::string> result;
  std::istringstream input(volumes);
  std::string volume;
  while (input >> volume) {
    if (volume.size() == 2 &&
        std::isalpha(static_cast<unsigned char>(volume[0])) &&
        volume[1] == ':') {
      result.push_back(std::move(volume));
    }
  }
  return result;
}

int DiskSortRank(const std::string& name) {
  if (name.size() == 2 &&
      std::isalpha(static_cast<unsigned char>(name[0])) && name[1] == ':') {
    return std::toupper(static_cast<unsigned char>(name[0]));
  }
  return 256;
}

std::string InventoryState(bool complete) {
  return complete ? "complete" : "partial";
}

std::string EvidenceValueText(const EvidenceValue& value) {
  return std::visit(
      [](const auto& item) {
        using Value = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Value, bool>) {
          return std::string(item ? "true" : "false");
        } else if constexpr (std::is_same_v<Value, std::string>) {
          return item;
        } else {
          std::ostringstream output;
          output << std::setprecision(8) << item;
          return output.str();
        }
      },
      value);
}

std::vector<const ProcessSnapshot*> SortedProcesses(
    const SystemSnapshot& snapshot) {
  std::vector<const ProcessSnapshot*> processes;
  processes.reserve(snapshot.processes.size());
  for (const auto& process : snapshot.processes) {
    processes.push_back(&process);
  }
  std::stable_sort(
      processes.begin(), processes.end(),
      [](const ProcessSnapshot* left, const ProcessSnapshot* right) {
        if (left->cpu_percent != right->cpu_percent) {
          return left->cpu_percent > right->cpu_percent;
        }
        if (left->IoBytesPerSec() != right->IoBytesPerSec()) {
          return left->IoBytesPerSec() > right->IoBytesPerSec();
        }
        return left->pid < right->pid;
      });
  return processes;
}

int ImpactRank(ImpactLevel impact) {
  switch (impact) {
    case ImpactLevel::High: return 2;
    case ImpactLevel::Medium: return 1;
    case ImpactLevel::Low: return 0;
  }
  return 0;
}

struct ImpactBreakdown {
  ImpactLevel cpu{ImpactLevel::Low};
  ImpactLevel memory{ImpactLevel::Low};
  ImpactLevel io{ImpactLevel::Low};
  ImpactLevel overall{ImpactLevel::Low};
};

ImpactLevel ThresholdImpact(double value, double medium, double high) {
  if (value >= high) return ImpactLevel::High;
  if (value >= medium) return ImpactLevel::Medium;
  return ImpactLevel::Low;
}

ImpactBreakdown CalculateImpact(const Config& config, double cpu_percent,
                                std::uint64_t private_bytes,
                                double io_bytes_per_sec) {
  const double high_io =
      std::max(1.0, config.thresholds.background_io_bytes_per_sec * 2.0);
  const double medium_io = high_io * 0.25;
  ImpactBreakdown result;
  result.cpu = ThresholdImpact(cpu_percent, 5.0, 20.0);
  result.memory = ThresholdImpact(static_cast<double>(private_bytes),
                                  512.0 * kMiB, 2.0 * kGiB);
  result.io = ThresholdImpact(io_bytes_per_sec, medium_io, high_io);
  result.overall =
      std::max({result.cpu, result.memory, result.io},
               [](ImpactLevel left, ImpactLevel right) {
                 return ImpactRank(left) < ImpactRank(right);
               });
  return result;
}

ImpactLevel ProcessImpact(const Config& config,
                          const ProcessSnapshot& process) {
  return CalculateImpact(config, process.cpu_percent, process.private_bytes,
                         process.IoBytesPerSec())
      .overall;
}

std::string WorkloadCategory(ProcessClass value) {
  switch (value) {
    case ProcessClass::RemoteTerminal: return "Remote";
    case ProcessClass::SerialTerminal: return "Serial";
    case ProcessClass::PacketCapture: return "Packet capture";
    case ProcessClass::Development: return "Development";
    case ProcessClass::BuildTool: return "Build";
    case ProcessClass::VersionControl: return "Version control";
    default: return "Protected";
  }
}

std::string ProtectionReason(const Config& config,
                             const ProcessSnapshot& process,
                             const RuntimeContext& context) {
  if (context.remote_session_pids.count(process.pid) != 0) {
    return "Active protected remote session";
  }
  if (context.active_capture_pids.count(process.pid) != 0) {
    return "Packet capture is running";
  }
  if (config.IsAlwaysProtected(process.name)) {
    return "Always Protect profile";
  }
  switch (process.classification) {
    case ProcessClass::RemoteTerminal: return "Known remote terminal";
    case ProcessClass::SerialTerminal: return "Known serial terminal";
    case ProcessClass::PacketCapture: return "Known packet-capture tool";
    case ProcessClass::Development: return "Development workload";
    case ProcessClass::BuildTool: return "Build task";
    case ProcessClass::VersionControl: return "Version-control task";
    default: return "Fail-closed protection policy";
  }
}

std::string DiagnosisTitle(const std::string& type) {
  if (type == "MemoryPressure") return "Memory Pressure";
  if (type == "PagingPressure") return "Paging Pressure";
  if (type == "DiskBottleneck") return "Disk I/O Bottleneck";
  if (type == "HddPagingBottleneck") return "HDD Paging Bottleneck";
  if (type == "SsdSpacePressure") return "SSD Space Pressure";
  if (type == "CpuSaturation") return "CPU Saturation";
  if (type == "DefenderImpact") return "Windows Defender Impact";
  if (type == "BackgroundIoImpact") return "Background I/O Impact";
  if (type == "ForegroundAppMemoryPressure") {
    return "Foreground App Memory Pressure";
  }
  return type;
}

CodingActionViewModel ActionView(const OptimizationAction& action,
                                 const std::string& state) {
  CodingActionViewModel view;
  view.target = !action.process_name.empty() ? action.process_name
                                             : action.service_name;
  if (view.target.empty()) view.target = "System";
  switch (action.type) {
    case ActionType::SetPriorityClass:
      view.action = "Priority";
      view.change = PriorityName(action.source_priority) + " -> " +
                    PriorityName(action.target_priority);
      break;
    case ActionType::GracefulCloseProcess:
      view.action = "Close application";
      view.change = "Request WM_CLOSE; never terminate";
      break;
    case ActionType::StopServiceTemporary:
      view.action = "Temporary service stop";
      view.change = ToString(action.source_service_state) + " -> Stopped";
      break;
    default:
      view.action = ToString(action.type);
      view.change = "Validated application action";
      break;
  }
  view.risk = ToString(action.risk);
  view.state = state;
  view.reason = action.reason;
  return view;
}

std::vector<std::string> SortedStrings(
    const std::unordered_set<std::string>& values) {
  std::vector<std::string> result(values.begin(), values.end());
  std::sort(result.begin(), result.end());
  return result;
}

bool IsDeveloperWorkload(ProcessClass value) {
  return value == ProcessClass::Development ||
         value == ProcessClass::RemoteTerminal ||
         value == ProcessClass::SerialTerminal ||
         value == ProcessClass::PacketCapture ||
         value == ProcessClass::BuildTool ||
         value == ProcessClass::VersionControl;
}

std::string StartupRecommendation(const StartupEntrySnapshot& entry,
                                  const Config& config) {
  if (entry.executable_name.empty()) return "MonitorOnly";
  const ProcessRule rule = config.RuleFor(entry.executable_name);
  if (rule.process_class == ProcessClass::Unknown) return "MonitorOnly";
  if (rule.process_class == ProcessClass::Updater ||
      rule.process_class == ProcessClass::CloudSync ||
      rule.process_class == ProcessClass::VendorUtility) {
    return rule.protection == ProtectionLevel::Optimizable ? "Review"
                                                           : "Keep";
  }
  if (rule.protection == ProtectionLevel::SystemCritical ||
      rule.protection == ProtectionLevel::Strong ||
      rule.protection == ProtectionLevel::UserExplicit) {
    return "Protect";
  }
  return "Keep";
}

std::string BuildDashboard(const SystemSnapshot& snapshot) {
  std::ostringstream output;
  const double used = static_cast<double>(
      snapshot.memory.physical_total_bytes >=
              snapshot.memory.physical_available_bytes
          ? snapshot.memory.physical_total_bytes -
                snapshot.memory.physical_available_bytes
          : 0);
  output << std::fixed << std::setprecision(1)
         << "SYSTEM\r\n"
         << "CPU             " << snapshot.cpu_percent << "%\r\n"
         << "RAM             " << FormatBytes(used) << " / "
         << FormatBytes(static_cast<double>(
                snapshot.memory.physical_total_bytes))
         << "\r\n"
         << "AVAILABLE       "
         << FormatBytes(static_cast<double>(
                snapshot.memory.physical_available_bytes))
         << "\r\n"
         << "COMMIT          " << snapshot.memory.CommitRatio() * 100.0
         << "%\r\n"
         << "PAGE READS      " << snapshot.page_reads_per_sec << "/s\r\n\r\n"
         << "INVENTORY\r\n"
         << "Processes       "
         << InventoryState(snapshot.process_inventory_complete) << "\r\n"
         << "TCP sessions    "
         << InventoryState(snapshot.tcp_inventory_complete) << "\r\n\r\n"
         << "DISKS\r\n";
  if (snapshot.disks.empty()) {
    output << "No physical disk counters available.\r\n";
  }
  for (const auto& disk : snapshot.disks) {
    auto volumes = SplitVolumeNames(disk.volumes);
    if (volumes.empty()) {
      volumes.push_back(disk.volumes.empty() ? disk.instance : disk.volumes);
    }
    for (const auto& volume : volumes) {
      output << std::left << std::setw(14) << volume << std::setw(9)
             << ToString(disk.media) << std::right << std::setw(6)
             << disk.active_ratio * 100.0 << "%  " << std::setw(8)
             << disk.average_latency_ms << " ms  "
             << FormatRate(disk.read_bytes_per_sec +
                           disk.write_bytes_per_sec)
             << "  " << disk.IoOperationsPerSec() << " IOPS";
      if (disk.space_inventory_complete) {
        output << "  free "
               << FormatBytes(static_cast<double>(disk.free_space_bytes))
               << " / "
               << FormatBytes(static_cast<double>(disk.total_space_bytes));
      }
      output << "\r\n";
    }
  }

  const auto processes = SortedProcesses(snapshot);
  output << "\r\nTOP I/O\r\n";
  const std::size_t count = std::min<std::size_t>(5, processes.size());
  for (std::size_t i = 0; i < count; ++i) {
    const auto& process = *processes[i];
    output << std::left << std::setw(26) << process.name << " PID "
           << std::setw(7) << process.pid << ' '
           << FormatRate(process.IoBytesPerSec()) << "\r\n";
  }
  return output.str();
}

std::string BuildProcesses(const SystemSnapshot& snapshot) {
  std::ostringstream output;
  output << "PROCESSES " << snapshot.processes.size()
         << " (sorted by CPU, then I/O)\r\n\r\n"
         << std::left << std::setw(7) << "PID" << std::setw(28) << "NAME"
         << std::right << std::setw(8) << "CPU %" << std::setw(13) << "WORKING"
         << std::setw(13) << "PRIVATE" << std::setw(13) << "I/O" << "  CLASS"
         << " / PROTECTION\r\n";
  const auto processes = SortedProcesses(snapshot);
  const std::size_t count = std::min<std::size_t>(200, processes.size());
  for (std::size_t i = 0; i < count; ++i) {
    const auto& process = *processes[i];
    output << std::left << std::setw(7) << process.pid << std::setw(28)
           << process.name.substr(0, 27) << std::right << std::fixed
           << std::setprecision(1) << std::setw(8) << process.cpu_percent
           << std::setw(13)
           << FormatBytes(static_cast<double>(process.working_set_bytes))
           << std::setw(13)
           << FormatBytes(static_cast<double>(process.private_bytes))
           << std::setw(13) << FormatRate(process.IoBytesPerSec()) << "  "
           << ToString(process.classification) << " / "
           << ToString(process.protection_level) << "\r\n";
  }
  if (processes.size() > count) {
    output << "\r\nShowing first " << count << " processes.\r\n";
  }
  return output.str();
}

std::string BuildDiagnosis(const std::vector<DiagnosisResult>& diagnoses,
                           const SnapshotHistory& history) {
  const auto& snapshots = history.Snapshots();
  const auto process_complete = std::count_if(
      snapshots.begin(), snapshots.end(), [](const auto& snapshot) {
        return snapshot.process_inventory_complete;
      });
  const auto tcp_complete = std::count_if(
      snapshots.begin(), snapshots.end(), [](const auto& snapshot) {
        return snapshot.tcp_inventory_complete;
      });
  const auto protection_complete = std::count_if(
      snapshots.begin(), snapshots.end(), [](const auto& snapshot) {
        return snapshot.process_inventory_complete &&
               snapshot.tcp_inventory_complete;
      });
  std::ostringstream output;
  output << "DIAGNOSIS\r\n"
         << "Window samples: " << history.Size() << "\r\n"
         << "Complete process inventories: " << process_complete << "\r\n"
         << "Complete TCP inventories: " << tcp_complete << "\r\n"
         << "Complete protection inventories: " << protection_complete
         << "\r\n\r\n";
  if (diagnoses.empty()) {
    output << "No configured bottleneck rule is currently sustained.\r\n"
           << "Keep the dashboard open to build a longer evidence window.\r\n";
    return output.str();
  }
  for (const auto& diagnosis : diagnoses) {
    output << ToString(diagnosis.severity) << "  " << diagnosis.type
           << "  confidence=" << ToString(diagnosis.confidence) << "\r\n"
           << diagnosis.summary << "\r\n";
    for (const auto& [name, value] : diagnosis.evidence) {
      output << "  " << name << ": " << EvidenceValueText(value) << "\r\n";
    }
    output << "\r\n";
  }
  return output.str();
}

std::string BuildCodingMode(const OptimizationPlan& plan,
                            const std::optional<OptimizationSession>& session) {
  std::ostringstream output;
  output << "CODING MODE\r\n"
         << "State: " << (session ? ToString(session->state) : "Inactive")
         << "\r\n"
         << "GUI operations use the same typed application command, policy, "
            "validation, persistence, and rollback path as the CLI.\r\n\r\n"
         << "PLANNED ACTIONS " << plan.actions.size() << "\r\n";
  if (plan.actions.empty()) output << "No eligible actions.\r\n";
  for (const auto& action : plan.actions) {
    output << ToString(action.risk) << "  " << ToString(action.type) << "  ";
    if (!action.process_name.empty()) {
      output << action.process_name << " PID " << action.pid;
    } else {
      output << action.service_name;
    }
    output << "\r\n  " << action.reason << "\r\n";
  }
  output << "\r\nREJECTED / PROTECTED " << plan.rejected.size() << "\r\n";
  const std::size_t rejected_count =
      std::min<std::size_t>(50, plan.rejected.size());
  for (std::size_t i = 0; i < rejected_count; ++i) {
    output << "- " << plan.rejected[i] << "\r\n";
  }
  output << "\r\nCommands:\r\n"
         << "  workboost coding enter --dry-run\r\n"
         << "  workboost coding enter\r\n"
         << "  workboost coding exit\r\n";
  return output.str();
}

std::string BuildProtected(
    const Config& config, const SystemSnapshot& snapshot,
    const std::vector<SerialPortSnapshot>& serial_ports,
    const std::string& serial_error) {
  const RuntimeContext context =
      BuildRuntimeContext(snapshot, config.remote_debug_ports);
  const ProtectionPolicy policy(config);
  std::vector<const ProcessSnapshot*> processes;
  for (const auto& process : snapshot.processes) {
    if (policy.IsProtected(process, context)) processes.push_back(&process);
  }
  std::stable_sort(
      processes.begin(), processes.end(),
      [](const ProcessSnapshot* left, const ProcessSnapshot* right) {
        if (left->classification != right->classification) {
          return left->classification < right->classification;
        }
        return left->name < right->name;
      });

  std::ostringstream output;
  output << "PROTECTED WORKLOAD\r\n\r\n"
         << "PROCESSES " << processes.size() << "\r\n";
  const std::size_t count = std::min<std::size_t>(200, processes.size());
  for (std::size_t i = 0; i < count; ++i) {
    const auto& process = *processes[i];
    output << std::left << std::setw(28) << process.name << " PID "
           << std::setw(7) << process.pid << ToString(process.classification)
           << " / " << ToString(policy.Evaluate(process, context));
    if (context.remote_session_pids.count(process.pid) != 0) {
      output << " / active-remote";
    }
    if (context.active_capture_pids.count(process.pid) != 0) {
      output << " / capture-active";
    }
    output << "\r\n";
  }

  std::unordered_map<std::uint32_t, std::string> process_names;
  for (const auto& process : snapshot.processes) {
    process_names.emplace(process.pid, process.name);
  }
  output << "\r\nPROTECTED REMOTE SESSIONS\r\n";
  bool found_remote = false;
  for (const auto& session : snapshot.tcp_sessions) {
    if (session.state != TcpState::Established ||
        config.remote_debug_ports.count(session.remote_port) == 0) {
      continue;
    }
    found_remote = true;
    const auto name = process_names.find(session.pid);
    output << (name == process_names.end() ? "unknown" : name->second)
           << " PID " << session.pid << " -> "
           << windows::MaskIpAddress(session.remote_address) << ':'
           << session.remote_port << "\r\n";
  }
  if (!found_remote) output << "None detected.\r\n";

  output << "\r\nSERIAL PORTS " << serial_ports.size()
         << " (inventory only; ports are never opened)\r\n";
  if (!serial_error.empty()) output << "Inventory error: " << serial_error << "\r\n";
  for (const auto& port : serial_ports) {
    output << std::left << std::setw(10) << port.port_name
           << (port.friendly_name.empty() ? "Unknown device"
                                          : port.friendly_name);
    if (!port.manufacturer.empty()) output << " (" << port.manufacturer << ')';
    output << "\r\n";
  }
  return output.str();
}

std::string BuildRecovery(
    const std::optional<OptimizationSession>& active_session,
    const std::string& recovery_error,
    const std::vector<CompletionReportSummary>& reports,
    const std::string& report_error) {
  std::ostringstream output;
  output << "RECOVERY\r\n\r\n";
  if (!recovery_error.empty()) {
    output << "SAFE MODE\r\n"
           << "The active session could not be validated: "
           << recovery_error << "\r\n"
           << "No new system changes are allowed.\r\n";
  } else if (!active_session) {
    output << "Recovery not required. No unfinished Coding Mode session was "
              "found.\r\n";
  } else {
    output << "Recovery required\r\n"
           << "Session: " << active_session->session_id << "\r\n"
           << "State: " << ToString(active_session->state) << "\r\n"
           << "Started: " << active_session->start_time << "\r\n"
           << "Recorded actions: " << active_session->actions.size()
           << "\r\n\r\n";
    for (const auto& action : active_session->actions) {
      output << ToString(action.state) << "  "
             << ToString(action.action.type) << "  " << action.action.id
             << "\r\n";
    }
    output << "\r\nCommands:\r\n"
           << "  workboost recovery status\r\n"
           << "  workboost recovery restore\r\n"
           << "  workboost recovery acknowledge\r\n";
  }
  output << "\r\nHISTORY " << reports.size() << "\r\n";
  if (!report_error.empty()) output << "History warning: " << report_error << "\r\n";
  for (const auto& report : reports) {
    output << report.start_time << "  " << report.session_id << "  "
           << report.measurement_phase << "  actions=" << report.action_count;
    if (!report.primary_diagnosis.empty()) {
      output << "  diagnosis=" << report.primary_diagnosis;
    }
    output << "\r\n";
  }
  return output.str();
}

std::string BuildSettings(
    const Config& config,
    const std::vector<StartupEntrySnapshot>& startup_entries,
    const std::string& startup_error) {
  std::ostringstream output;
  output << "SETTINGS (effective, read-only)\r\n\r\n"
         << "Sample interval       " << config.sample_interval_ms << " ms\r\n"
         << "History window        " << config.history_seconds << " s\r\n"
         << "Process rules         " << config.process_rules.size() << "\r\n"
         << "Service rules         " << config.service_rules.size() << "\r\n"
         << "Remote debug ports    ";
  std::vector<std::uint16_t> ports(config.remote_debug_ports.begin(),
                                   config.remote_debug_ports.end());
  std::sort(ports.begin(), ports.end());
  for (std::size_t i = 0; i < ports.size(); ++i) {
    if (i != 0) output << ", ";
    output << ports[i];
  }
  output << "\r\n"
         << "Graceful-close allow  "
         << config.coding_profile.allow_graceful_close.size() << "\r\n"
         << "Service-stop allow    "
         << config.coding_profile.allow_service_stop.size() << "\r\n\r\n"
         << "STARTUP INVENTORY " << startup_entries.size()
         << " (read-only; command arguments omitted)\r\n";
  if (!startup_error.empty()) {
    output << "Inventory error: " << startup_error << "\r\n";
  }
  for (const auto& entry : startup_entries) {
    const ProcessRule rule = config.RuleFor(entry.executable_name);
    output << std::left << std::setw(24) << entry.name.substr(0, 23)
           << std::setw(22) << entry.executable_name.substr(0, 21)
           << std::setw(16) << ToString(rule.process_class)
           << std::setw(13) << StartupRecommendation(entry, config)
           << entry.location << "\r\n";
  }
  return output.str();
}

}  // namespace

const std::array<const char*, kDashboardPageCount>&
DashboardPresenter::PageNames() {
  static const std::array<const char*, kDashboardPageCount> names{
      "Dashboard", "Processes", "Diagnosis", "Coding Mode",
      "Protected Workload", "Recovery & History", "Settings"};
  return names;
}

DashboardViewModel DashboardPresenter::Build(
    const Config& config, const SystemSnapshot& snapshot,
    const SnapshotHistory& history,
    const std::vector<SerialPortSnapshot>& serial_ports,
    const std::vector<StartupEntrySnapshot>& startup_entries,
    const std::optional<OptimizationSession>& active_session,
    const std::string& recovery_error, const std::string& serial_error,
    const std::string& startup_error,
    const std::vector<CompletionReportSummary>& reports,
    const std::string& report_error) {
  DashboardViewModel model;
  const auto diagnoses = DiagnosisEngine(config).Evaluate(history);
  const auto plan = OptimizationPlanner(config).Create(snapshot);
  const RuntimeContext context =
      BuildRuntimeContext(snapshot, config.remote_debug_ports);
  const ProtectionPolicy policy(config);
  model.mode = !recovery_error.empty()
                   ? "Safe Mode"
                   : active_session ? ToString(active_session->state)
                                    : "Monitor Mode";
  model.updated_at = windows::Iso8601Now();
  model.system.cpu_percent = snapshot.cpu_percent;
  model.system.memory_total_bytes = snapshot.memory.physical_total_bytes;
  model.system.available_memory_bytes =
      snapshot.memory.physical_available_bytes;
  model.system.memory_used_bytes =
      snapshot.memory.physical_total_bytes >=
              snapshot.memory.physical_available_bytes
          ? snapshot.memory.physical_total_bytes -
                snapshot.memory.physical_available_bytes
          : 0;
  model.system.memory_used_ratio =
      snapshot.memory.physical_total_bytes == 0
          ? 0.0
          : static_cast<double>(model.system.memory_used_bytes) /
                static_cast<double>(snapshot.memory.physical_total_bytes);
  model.system.commit_ratio = snapshot.memory.CommitRatio();
  model.system.page_reads_per_sec = snapshot.page_reads_per_sec;
  model.system.cpu_model = snapshot.cpu_model;
  model.system.memory_model = snapshot.memory_model;
  model.system.process_inventory_complete =
      snapshot.process_inventory_complete;
  model.system.tcp_inventory_complete = snapshot.tcp_inventory_complete;
  for (const auto& disk : snapshot.disks) {
    auto volumes = SplitVolumeNames(disk.volumes);
    if (volumes.empty()) {
      volumes.push_back(disk.volumes.empty() ? disk.instance : disk.volumes);
    }
    for (const auto& volume : volumes) {
      DiskViewModel view;
      view.name = volume;
      view.media = ToString(disk.media);
      view.active_percent = disk.active_ratio * 100.0;
      view.latency_ms = disk.average_latency_ms;
      view.queue_length = disk.queue_length;
      view.throughput_bytes_per_sec =
          disk.read_bytes_per_sec + disk.write_bytes_per_sec;
      model.disks.push_back(std::move(view));
    }
  }
  std::stable_sort(
      model.disks.begin(), model.disks.end(),
      [](const DiskViewModel& left, const DiskViewModel& right) {
        const int left_rank = DiskSortRank(left.name);
        const int right_rank = DiskSortRank(right.name);
        if (left_rank != right_rank) return left_rank < right_rank;
        return left.name < right.name;
      });
  for (const auto& diagnosis : diagnoses) {
    DiagnosisViewModel view;
    view.severity = ToString(diagnosis.severity);
    view.confidence = ToString(diagnosis.confidence);
    view.type = DiagnosisTitle(diagnosis.type);
    view.summary = diagnosis.summary;
    for (const auto& [name, value] : diagnosis.evidence) {
      view.evidence.emplace_back(name, EvidenceValueText(value));
    }
    model.diagnoses.push_back(std::move(view));
  }
  std::unordered_map<std::string, std::size_t> top_impact_by_name;
  for (const auto& process : snapshot.processes) {
    const auto [position, inserted] =
        top_impact_by_name.emplace(process.name, model.top_impacts.size());
    if (inserted) {
      TopImpactViewModel view;
      view.name = process.name;
      model.top_impacts.push_back(std::move(view));
    }
    auto& aggregate = model.top_impacts[position->second];
    aggregate.cpu_percent += process.cpu_percent;
    aggregate.private_bytes += process.private_bytes;
    aggregate.io_bytes_per_sec += process.IoBytesPerSec();
  }
  for (auto& process : model.top_impacts) {
    const auto impact = CalculateImpact(config, process.cpu_percent,
                                        process.private_bytes,
                                        process.io_bytes_per_sec);
    process.cpu_impact = impact.cpu;
    process.memory_impact = impact.memory;
    process.io_impact = impact.io;
    process.impact = impact.overall;
  }
  std::stable_sort(
      model.top_impacts.begin(), model.top_impacts.end(),
      [](const TopImpactViewModel& left, const TopImpactViewModel& right) {
        if (ImpactRank(left.impact) != ImpactRank(right.impact)) {
          return ImpactRank(left.impact) > ImpactRank(right.impact);
        }
        if (left.io_bytes_per_sec != right.io_bytes_per_sec) {
          return left.io_bytes_per_sec > right.io_bytes_per_sec;
        }
        if (left.cpu_percent != right.cpu_percent) {
          return left.cpu_percent > right.cpu_percent;
        }
        if (left.private_bytes != right.private_bytes) {
          return left.private_bytes > right.private_bytes;
        }
        return left.name < right.name;
      });
  const auto sorted_processes = SortedProcesses(snapshot);
  std::unordered_set<std::uint32_t> planned_close_pids;
  for (const auto& action : plan.actions) {
    if (action.type == ActionType::GracefulCloseProcess) {
      planned_close_pids.insert(action.pid);
    }
  }
  const std::size_t process_count = sorted_processes.size();
  for (std::size_t i = 0; i < process_count; ++i) {
    const auto& process = *sorted_processes[i];
    ProcessViewModel view;
    view.pid = process.pid;
    view.start_time_100ns = process.start_time_100ns;
    view.name = process.name;
    view.cpu_percent = process.cpu_percent;
    view.working_set_bytes = process.working_set_bytes;
    view.private_bytes = process.private_bytes;
    view.read_bytes_per_sec = process.read_bytes_per_sec;
    view.write_bytes_per_sec = process.write_bytes_per_sec;
    view.process_class = ToString(process.classification);
    view.protection = ToString(policy.Evaluate(process, context));
    view.impact = ProcessImpact(config, process);
    view.protected_workload = policy.IsProtected(process, context);
    view.has_visible_window = process.has_visible_window;
    view.is_foreground = process.is_foreground;
    view.cleanup_already_planned =
        planned_close_pids.count(process.pid) != 0;
    if (!snapshot.process_inventory_complete ||
        !snapshot.tcp_inventory_complete) {
      view.cleanup_block_reason = "Protection inventory is incomplete";
    } else if (process.start_time_100ns == 0) {
      view.cleanup_block_reason = "Process identity is unavailable";
    } else if (view.protected_workload) {
      view.cleanup_block_reason = "Protected by policy";
    } else if (process.is_foreground) {
      view.cleanup_block_reason = "Foreground process";
    } else if (!process.has_visible_window) {
      view.cleanup_block_reason = "No visible window";
    } else if (view.cleanup_already_planned) {
      view.cleanup_block_reason = "Already included in the plan";
    } else {
      view.cleanup_eligible = true;
      view.cleanup_block_reason = "Ready to add";
    }
    model.processes.push_back(std::move(view));

    if (!IsDeveloperWorkload(process.classification) &&
        context.remote_session_pids.count(process.pid) == 0 &&
        context.active_capture_pids.count(process.pid) == 0 &&
        !config.IsAlwaysProtected(process.name)) {
      continue;
    }
    ProtectedWorkloadViewModel workload;
    workload.category = WorkloadCategory(process.classification);
    workload.name = process.name;
    workload.detail = "PID " + std::to_string(process.pid);
    for (const auto& session : snapshot.tcp_sessions) {
      if (session.pid == process.pid && session.state == TcpState::Established &&
          config.remote_debug_ports.count(session.remote_port) != 0) {
        workload.detail += "  " + windows::MaskIpAddress(session.remote_address) +
                           ":" + std::to_string(session.remote_port);
        break;
      }
    }
    workload.reason = ProtectionReason(config, process, context);
    model.protected_workloads.push_back(std::move(workload));
  }
  for (const auto& workload : model.protected_workloads) {
    model.coding_mode.protected_processes.push_back(workload.name);
  }
  std::sort(model.coding_mode.protected_processes.begin(),
            model.coding_mode.protected_processes.end());
  model.coding_mode.protected_processes.erase(
      std::unique(model.coding_mode.protected_processes.begin(),
                  model.coding_mode.protected_processes.end()),
      model.coding_mode.protected_processes.end());
  model.coding_mode.active = active_session.has_value();
  model.coding_mode.safe_mode = !recovery_error.empty();
  model.coding_mode.state = model.mode;
  model.coding_mode.started_at =
      active_session ? active_session->start_time : std::string{};
  model.coding_mode.planned_actions = plan.actions.size();
  model.coding_mode.rejected_actions = plan.rejected.size();
  model.coding_mode.protected_workloads = model.protected_workloads.size();
  if (active_session) {
    model.coding_mode.active_actions =
        static_cast<std::size_t>(std::count_if(
            active_session->actions.begin(), active_session->actions.end(),
            [](const ExecutedAction& action) {
              return action.state == ActionState::Applied ||
                     action.state == ActionState::Completed;
            }));
    for (const auto& action : active_session->actions) {
      model.coding_mode.actions.push_back(
          ActionView(action.action, ToString(action.state)));
    }
  } else {
    for (const auto& action : plan.actions) {
      model.coding_mode.actions.push_back(ActionView(action, "Planned"));
    }
  }

  model.recovery.required =
      !recovery_error.empty() || active_session.has_value();
  model.recovery.can_restore = active_session.has_value();
  model.recovery.state = model.mode;
  model.recovery.error = recovery_error;
  model.recovery.report_error = report_error;
  if (active_session) {
    model.recovery.session_id = active_session->session_id;
    model.recovery.started_at = active_session->start_time;
    for (const auto& action : active_session->actions) {
      model.recovery.actions.push_back(
          ActionView(action.action, ToString(action.state)));
    }
  }
  for (const auto& report : reports) {
    HistoryReportViewModel view;
    view.session_id = report.session_id;
    view.started_at = report.start_time;
    view.measurement_phase = report.measurement_phase;
    view.primary_diagnosis = DiagnosisTitle(report.primary_diagnosis);
    view.primary_severity = report.primary_severity;
    view.action_count = report.action_count;
    view.baseline_available_memory_bytes =
        report.baseline_available_memory_bytes;
    view.optimized_available_memory_bytes =
        report.optimized_available_memory_bytes;
    view.baseline_disk_latency_ms = report.baseline_disk_latency_ms;
    view.optimized_disk_latency_ms = report.optimized_disk_latency_ms;
    view.rollback_complete = report.rollback_complete;
    model.recovery.reports.push_back(std::move(view));
  }

  model.settings.sample_interval_ms = config.sample_interval_ms;
  model.settings.history_seconds = config.history_seconds;
  model.settings.process_rule_count = config.process_rules.size();
  model.settings.service_rule_count = config.service_rules.size();
  model.settings.remote_debug_ports.assign(config.remote_debug_ports.begin(),
                                           config.remote_debug_ports.end());
  std::sort(model.settings.remote_debug_ports.begin(),
            model.settings.remote_debug_ports.end());
  model.settings.always_protect =
      SortedStrings(config.coding_profile.always_protect);
  model.settings.allow_graceful_close =
      SortedStrings(config.coding_profile.allow_graceful_close);
  model.settings.allow_service_stop =
      SortedStrings(config.coding_profile.allow_service_stop);
  model.settings.commit_warning_percent =
      config.thresholds.commit_warning * 100.0;
  model.settings.available_memory_mb =
      config.thresholds.available_memory_mb;
  model.settings.disk_active_percent =
      config.thresholds.disk_active_ratio * 100.0;
  model.settings.hdd_latency_ms = config.thresholds.hdd_latency_ms;
  model.settings.startup_error = startup_error;
  for (const auto& entry : startup_entries) {
    StartupEntryViewModel view;
    view.name = entry.name;
    view.executable_name = entry.executable_name;
    view.scope = ToString(entry.scope);
    view.recommendation = StartupRecommendation(entry, config);
    model.settings.startup_entries.push_back(std::move(view));
  }
  model.pages[static_cast<std::size_t>(DashboardPage::Dashboard)] =
      BuildDashboard(snapshot);
  model.pages[static_cast<std::size_t>(DashboardPage::Processes)] =
      BuildProcesses(snapshot);
  model.pages[static_cast<std::size_t>(DashboardPage::Diagnosis)] =
      BuildDiagnosis(diagnoses, history);
  model.pages[static_cast<std::size_t>(DashboardPage::CodingMode)] =
      BuildCodingMode(plan, active_session);
  model.pages[static_cast<std::size_t>(DashboardPage::ProtectedWorkload)] =
      BuildProtected(config, snapshot, serial_ports, serial_error);
  model.pages[static_cast<std::size_t>(DashboardPage::Recovery)] =
      BuildRecovery(active_session, recovery_error, reports, report_error);
  model.pages[static_cast<std::size_t>(DashboardPage::Settings)] =
      BuildSettings(config, startup_entries, startup_error);
  return model;
}

std::vector<ProcessSelection> ParseUnclosedProcessSelections(
    const std::string& output) {
  constexpr const char* kPrefix = "UNCLOSED_PROCESS ";
  std::vector<ProcessSelection> selections;
  std::istringstream stream(output);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.rfind(kPrefix, 0) != 0) continue;
    const auto parse_number = [&line](const std::string& key,
                                      std::uint64_t* value) {
      const std::size_t key_position = line.find(key);
      if (key_position == std::string::npos) return false;
      const std::size_t start = key_position + key.size();
      std::size_t end = start;
      while (end < line.size() && line[end] >= '0' && line[end] <= '9') {
        ++end;
      }
      if (end == start) return false;
      try {
        *value = std::stoull(line.substr(start, end - start));
      } catch (...) {
        return false;
      }
      return true;
    };
    std::uint64_t pid = 0;
    std::uint64_t start_time = 0;
    if (!parse_number("pid=", &pid) || !parse_number("start=", &start_time) ||
        pid == 0 || pid > UINT32_MAX || start_time == 0) {
      continue;
    }
    const ProcessSelection selection{
        static_cast<std::uint32_t>(pid), start_time};
    if (std::find(selections.begin(), selections.end(), selection) ==
        selections.end()) {
      selections.push_back(selection);
    }
  }
  return selections;
}

}  // namespace workboost::gui
