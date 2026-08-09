#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace workboost {

enum class StartupObservationStatus {
  Succeeded,
  TimedOut,
  ProcessExited,
  WindowNotFound,
  WindowUnresponsive,
  QueryFailed,
  InvalidTarget,
};

struct StartupObservation {
  StartupObservationStatus status{StartupObservationStatus::TimedOut};
  std::uint32_t pid{};
  std::uint64_t process_start_time_100ns{};
  std::optional<std::uint64_t> visible_window_ms;
  std::optional<std::uint64_t> responsive_window_ms;
  unsigned long error_code{};
};

struct StartupBenchmarkSummary {
  std::string label;
  std::string process_name;
  std::string generated_at;
  std::size_t requested_runs{};
  std::vector<StartupObservation> observations;
  std::optional<double> median_visible_window_ms;
  std::optional<double> median_responsive_window_ms;
};

struct StartupBenchmarkComparison {
  std::string label;
  std::string generated_at;
  StartupBenchmarkSummary baseline;
  StartupBenchmarkSummary optimized;
  std::optional<double> delta_visible_window_ms;
  std::optional<double> delta_responsive_window_ms;
};

class BenchmarkManager {
 public:
  static StartupBenchmarkSummary Summarize(
      std::string label, std::string process_name, std::string generated_at,
      std::size_t requested_runs,
      std::vector<StartupObservation> observations);
  static std::string Json(const StartupBenchmarkSummary& summary);
  static std::string Text(const StartupBenchmarkSummary& summary);
  static StartupBenchmarkComparison Compare(
      std::string label, std::string generated_at,
      StartupBenchmarkSummary baseline, StartupBenchmarkSummary optimized);
  static std::string ComparisonJson(
      const StartupBenchmarkComparison& comparison);
  static std::string ComparisonText(
      const StartupBenchmarkComparison& comparison);
};

std::string ToString(StartupObservationStatus value);

}  // namespace workboost
