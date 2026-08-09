#include "gui/dashboard_model.h"

#include "core/diagnosis/diagnosis_engine.h"
#include "core/optimization/optimization.h"
#include "core/policy/protection_policy.h"
#include "platform/windows/windows_utils.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
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
    output << std::left << std::setw(14)
           << (disk.volumes.empty() ? disk.instance : disk.volumes)
           << std::setw(9) << ToString(disk.media) << std::right
           << std::setw(6) << disk.active_ratio * 100.0 << "%  "
           << std::setw(8) << disk.average_latency_ms << " ms  "
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

std::string BuildDiagnosis(const Config& config,
                           const SnapshotHistory& history) {
  const auto diagnoses = DiagnosisEngine(config).Evaluate(history);
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

std::string BuildCodingMode(const Config& config,
                            const SystemSnapshot& snapshot,
                            const std::optional<OptimizationSession>& session) {
  const auto plan = OptimizationPlanner(config).Create(snapshot);
  std::ostringstream output;
  output << "CODING MODE\r\n"
         << "State: " << (session ? ToString(session->state) : "Inactive")
         << "\r\n"
         << "This page is a live, read-only plan preview. System changes still "
            "require the typed CLI action path.\r\n\r\n"
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
    const std::string& recovery_error) {
  std::ostringstream output;
  output << "RECOVERY\r\n\r\n";
  if (!recovery_error.empty()) {
    output << "SAFE MODE\r\n"
           << "The active session could not be validated: "
           << recovery_error << "\r\n"
           << "No new system changes are allowed.\r\n";
    return output.str();
  }
  if (!active_session) {
    output << "Recovery not required. No unfinished Coding Mode session was "
              "found.\r\n";
    return output.str();
  }
  output << "Recovery required\r\n"
         << "Session: " << active_session->session_id << "\r\n"
         << "State: " << ToString(active_session->state) << "\r\n"
         << "Started: " << active_session->start_time << "\r\n"
         << "Recorded actions: " << active_session->actions.size()
         << "\r\n\r\n";
  for (const auto& action : active_session->actions) {
    output << ToString(action.state) << "  " << ToString(action.action.type)
           << "  " << action.action.id << "\r\n";
  }
  output << "\r\nCommands:\r\n"
         << "  workboost recovery status\r\n"
         << "  workboost recovery restore\r\n"
         << "  workboost recovery acknowledge\r\n";
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
      "Protected Workload", "Recovery", "Settings"};
  return names;
}

DashboardViewModel DashboardPresenter::Build(
    const Config& config, const SystemSnapshot& snapshot,
    const SnapshotHistory& history,
    const std::vector<SerialPortSnapshot>& serial_ports,
    const std::vector<StartupEntrySnapshot>& startup_entries,
    const std::optional<OptimizationSession>& active_session,
    const std::string& recovery_error, const std::string& serial_error,
    const std::string& startup_error) {
  DashboardViewModel model;
  model.mode = !recovery_error.empty()
                   ? "Safe Mode"
                   : active_session ? ToString(active_session->state)
                                    : "Monitor Mode";
  model.updated_at = windows::Iso8601Now();
  model.pages[static_cast<std::size_t>(DashboardPage::Dashboard)] =
      BuildDashboard(snapshot);
  model.pages[static_cast<std::size_t>(DashboardPage::Processes)] =
      BuildProcesses(snapshot);
  model.pages[static_cast<std::size_t>(DashboardPage::Diagnosis)] =
      BuildDiagnosis(config, history);
  model.pages[static_cast<std::size_t>(DashboardPage::CodingMode)] =
      BuildCodingMode(config, snapshot, active_session);
  model.pages[static_cast<std::size_t>(DashboardPage::ProtectedWorkload)] =
      BuildProtected(config, snapshot, serial_ports, serial_error);
  model.pages[static_cast<std::size_t>(DashboardPage::Recovery)] =
      BuildRecovery(active_session, recovery_error);
  model.pages[static_cast<std::size_t>(DashboardPage::Settings)] =
      BuildSettings(config, startup_entries, startup_error);
  return model;
}

}  // namespace workboost::gui
