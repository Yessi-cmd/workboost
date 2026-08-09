#pragma once

#include "core/model/types.h"
#include "core/optimization/optimization.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace workboost {

enum class SessionState { Active, Recovering, Completed, SafeMode };

struct BenchmarkPoint {
  std::string timestamp;
  double cpu_percent{};
  std::uint64_t available_memory_bytes{};
  double commit_ratio{};
  double maximum_disk_active_ratio{};
  double maximum_disk_latency_ms{};
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

  bool Save(const OptimizationSession& session, std::string* error = nullptr) const;
  std::optional<OptimizationSession> LoadActive(
      std::string* error = nullptr) const;
  bool Complete(OptimizationSession session, const SystemSnapshot& after,
                std::filesystem::path* report_path = nullptr,
                std::string* error = nullptr) const;

  static std::string Serialize(const OptimizationSession& session);
  static std::optional<OptimizationSession> Deserialize(
      const std::string& json, std::string* error = nullptr);
  static BenchmarkPoint MakeBenchmarkPoint(const SystemSnapshot& snapshot);

 private:
  std::filesystem::path root_directory_;
};

std::string ToString(SessionState value);
SessionState SessionStateFromString(const std::string& value);

}  // namespace workboost
