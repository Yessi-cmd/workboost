#pragma once

#include "app/session_manager.h"
#include "core/config/config.h"
#include "core/model/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace workboost::gui {

enum class DashboardPage : std::size_t {
  Dashboard,
  Processes,
  Diagnosis,
  CodingMode,
  ProtectedWorkload,
  Recovery,
  Settings,
  Count,
};

constexpr std::size_t kDashboardPageCount =
    static_cast<std::size_t>(DashboardPage::Count);

enum class ImpactLevel { Low, Medium, High };

struct SystemOverviewViewModel {
  double cpu_percent{};
  double memory_used_ratio{};
  std::uint64_t memory_used_bytes{};
  std::uint64_t memory_total_bytes{};
  std::uint64_t available_memory_bytes{};
  double commit_ratio{};
  double page_reads_per_sec{};
  std::string cpu_model;
  std::string memory_model;
  bool process_inventory_complete{};
  bool tcp_inventory_complete{};
};

struct DiskViewModel {
  std::string name;
  std::string media;
  double active_percent{};
  double latency_ms{};
  double queue_length{};
  double throughput_bytes_per_sec{};
};

struct DiagnosisViewModel {
  std::string severity;
  std::string confidence;
  std::string type;
  std::string summary;
  std::vector<std::pair<std::string, std::string>> evidence;
};

struct ProcessViewModel {
  std::uint32_t pid{};
  std::uint64_t start_time_100ns{};
  std::string name;
  double cpu_percent{};
  std::uint64_t working_set_bytes{};
  std::uint64_t private_bytes{};
  double read_bytes_per_sec{};
  double write_bytes_per_sec{};
  std::string process_class;
  std::string protection;
  ImpactLevel impact{ImpactLevel::Low};
  bool protected_workload{};
  bool has_visible_window{};
  bool is_foreground{};
  bool cleanup_eligible{};
  bool cleanup_already_planned{};
  std::string cleanup_block_reason;
};

struct TopImpactViewModel {
  std::string name;
  double cpu_percent{};
  std::uint64_t private_bytes{};
  double io_bytes_per_sec{};
  ImpactLevel cpu_impact{ImpactLevel::Low};
  ImpactLevel memory_impact{ImpactLevel::Low};
  ImpactLevel io_impact{ImpactLevel::Low};
  ImpactLevel impact{ImpactLevel::Low};
};

struct ProtectedWorkloadViewModel {
  std::string category;
  std::string name;
  std::string detail;
  std::string reason;
};

struct CodingActionViewModel {
  std::string target;
  std::string action;
  std::string change;
  std::string risk;
  std::string state;
  std::string reason;
};

struct CodingModeViewModel {
  bool active{};
  bool safe_mode{};
  bool operation_in_progress{};
  std::string state;
  std::string started_at;
  std::string operation_status;
  std::size_t active_actions{};
  std::size_t planned_actions{};
  std::size_t rejected_actions{};
  std::size_t protected_workloads{};
  std::vector<CodingActionViewModel> actions;
  std::vector<std::string> protected_processes;
};

struct HistoryReportViewModel {
  std::string session_id;
  std::string started_at;
  std::string measurement_phase;
  std::string primary_diagnosis;
  std::string primary_severity;
  std::size_t action_count{};
  std::uint64_t baseline_available_memory_bytes{};
  std::uint64_t optimized_available_memory_bytes{};
  double baseline_disk_latency_ms{};
  double optimized_disk_latency_ms{};
  bool rollback_complete{};
};

struct RecoveryViewModel {
  bool required{};
  bool can_restore{};
  std::string state;
  std::string session_id;
  std::string started_at;
  std::string error;
  std::vector<CodingActionViewModel> actions;
  std::vector<HistoryReportViewModel> reports;
  std::string report_error;
};

struct StartupEntryViewModel {
  std::string name;
  std::string executable_name;
  std::string scope;
  std::string recommendation;
};

struct SettingsViewModel {
  int sample_interval_ms{};
  int history_seconds{};
  std::size_t process_rule_count{};
  std::size_t service_rule_count{};
  std::vector<std::uint16_t> remote_debug_ports;
  std::vector<std::string> always_protect;
  std::vector<std::string> allow_graceful_close;
  std::vector<std::string> allow_service_stop;
  double commit_warning_percent{};
  double available_memory_mb{};
  double disk_active_percent{};
  double hdd_latency_ms{};
  std::vector<StartupEntryViewModel> startup_entries;
  std::string startup_error;
};

struct DashboardViewModel {
  std::array<std::string, kDashboardPageCount> pages;
  std::string mode;
  std::string updated_at;
  SystemOverviewViewModel system;
  std::vector<DiskViewModel> disks;
  std::vector<DiagnosisViewModel> diagnoses;
  std::vector<ProcessViewModel> processes;
  std::vector<TopImpactViewModel> top_impacts;
  std::vector<ProtectedWorkloadViewModel> protected_workloads;
  CodingModeViewModel coding_mode;
  RecoveryViewModel recovery;
  SettingsViewModel settings;
};

class DashboardPresenter {
 public:
  static const std::array<const char*, kDashboardPageCount>& PageNames();

  static DashboardViewModel Build(
      const Config& config, const SystemSnapshot& snapshot,
      const SnapshotHistory& history,
      const std::vector<SerialPortSnapshot>& serial_ports,
      const std::vector<StartupEntrySnapshot>& startup_entries,
      const std::optional<OptimizationSession>& active_session,
      const std::string& recovery_error,
      const std::string& serial_error = {},
      const std::string& startup_error = {},
      const std::vector<CompletionReportSummary>& reports = {},
      const std::string& report_error = {});
};

// Parses the stable "UNCLOSED_PROCESS pid=... start=... name=..." lines that
// `coding exit` emits for processes still running after WM_CLOSE. The name is
// display-only; the returned selections carry PID + start time for revalidation.
[[nodiscard]] std::vector<ProcessSelection> ParseUnclosedProcessSelections(
    const std::string& output);

}  // namespace workboost::gui
