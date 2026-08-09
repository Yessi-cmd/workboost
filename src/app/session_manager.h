#pragma once

#include "core/model/types.h"
#include "core/optimization/optimization.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace workboost {

enum class SessionState { Active, Recovering, Completed, SafeMode };

struct BenchmarkPoint {
  std::string timestamp;
  std::size_t sample_count{1};
  std::uint64_t observed_span_ms{};
  std::size_t process_inventory_complete_samples{};
  std::size_t tcp_inventory_complete_samples{};
  std::size_t protection_inventory_complete_samples{};
  double cpu_percent{};
  std::uint64_t available_memory_bytes{};
  double commit_ratio{};
  double maximum_disk_active_ratio{};
  double maximum_disk_latency_ms{};
  double maximum_disk_queue_length{};
  double maximum_ssd_active_ratio{};
  double maximum_ssd_latency_ms{};
  double maximum_hdd_active_ratio{};
  double maximum_hdd_latency_ms{};
  double page_reads_per_sec{};
  double development_cpu_percent{};
  std::uint64_t development_working_set_bytes{};
  std::string top_background_io_process;
  double top_background_io_bytes_per_sec{};
};

struct OptimizationSession {
  std::string session_id;
  SessionState state{SessionState::Active};
  std::string start_time;
  BenchmarkPoint baseline;
  std::vector<ExecutedAction> actions;
};

class SessionManager {
 public:
  explicit SessionManager(std::filesystem::path root_directory);

  [[nodiscard]] const std::filesystem::path& RootDirectory() const;
  [[nodiscard]] std::filesystem::path ActiveSessionPath() const;
  [[nodiscard]] bool HasActiveSession() const;
  [[nodiscard]] OptimizationSession Create(const SystemSnapshot& baseline) const;
  [[nodiscard]] OptimizationSession Create(
      const SnapshotHistory& baseline) const;

  bool Save(const OptimizationSession& session, std::string* error = nullptr) const;
  std::optional<OptimizationSession> LoadActive(
      std::string* error = nullptr) const;
  bool Complete(OptimizationSession session, const SystemSnapshot& after,
                const std::vector<DiagnosisResult>& diagnoses,
                const std::string& measurement_phase,
                std::filesystem::path* report_path = nullptr,
                std::string* error = nullptr) const;
  bool Complete(OptimizationSession session, const SnapshotHistory& after,
                const std::vector<DiagnosisResult>& diagnoses,
                const std::string& measurement_phase,
                std::filesystem::path* report_path = nullptr,
                std::string* error = nullptr) const;

  static std::string Serialize(const OptimizationSession& session);
  static std::string CompletionReport(
      OptimizationSession session, const SystemSnapshot& after,
      const std::vector<DiagnosisResult>& diagnoses,
      const std::string& measurement_phase);
  static std::string CompletionReport(
      OptimizationSession session, const SnapshotHistory& after,
      const std::vector<DiagnosisResult>& diagnoses,
      const std::string& measurement_phase);
  static std::optional<OptimizationSession> Deserialize(
      const std::string& json, std::string* error = nullptr);
  static BenchmarkPoint MakeBenchmarkPoint(const SystemSnapshot& snapshot);
  static BenchmarkPoint MakeBenchmarkPoint(const SnapshotHistory& history);

 private:
  std::filesystem::path root_directory_;
};

std::string ToString(SessionState value);
std::optional<SessionState> SessionStateFromString(const std::string& value);

}  // namespace workboost
