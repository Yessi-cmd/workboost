#include "core/benchmark/benchmark.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace workboost {
namespace {

std::optional<double> Median(std::vector<std::uint64_t> values) {
  if (values.empty()) return std::nullopt;
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  if (values.size() % 2 != 0) return static_cast<double>(values[middle]);
  return (static_cast<double>(values[middle - 1]) +
          static_cast<double>(values[middle])) /
         2.0;
}

std::string EscapeJson(const std::string& value) {
  std::ostringstream output;
  for (const unsigned char character : value) {
    switch (character) {
      case '"': output << "\\\""; break;
      case '\\': output << "\\\\"; break;
      case '\b': output << "\\b"; break;
      case '\f': output << "\\f"; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default:
        if (character < 0x20) {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<unsigned int>(character) << std::dec;
        } else {
          output << static_cast<char>(character);
        }
    }
  }
  return output.str();
}

void AppendOptionalJson(std::ostringstream* output,
                        const std::optional<std::uint64_t>& value) {
  if (value) {
    *output << *value;
  } else {
    *output << "null";
  }
}

void AppendOptionalJson(std::ostringstream* output,
                        const std::optional<double>& value) {
  if (value) {
    *output << *value;
  } else {
    *output << "null";
  }
}

std::string IndentAfterNewline(std::string value,
                               const std::string& indentation) {
  if (!value.empty() && value.back() == '\n') value.pop_back();
  std::size_t position = 0;
  while ((position = value.find('\n', position)) != std::string::npos) {
    value.insert(position + 1, indentation);
    position += indentation.size() + 1;
  }
  return value;
}

}  // namespace

StartupBenchmarkSummary BenchmarkManager::Summarize(
    std::string label, std::string process_name, std::string generated_at,
    std::size_t requested_runs,
    std::vector<StartupObservation> observations) {
  StartupBenchmarkSummary summary;
  summary.label = std::move(label);
  summary.process_name = std::move(process_name);
  summary.generated_at = std::move(generated_at);
  summary.requested_runs = requested_runs;
  summary.observations = std::move(observations);
  std::vector<std::uint64_t> visible;
  std::vector<std::uint64_t> responsive;
  for (const auto& observation : summary.observations) {
    if (observation.status != StartupObservationStatus::Succeeded) continue;
    if (observation.visible_window_ms) {
      visible.push_back(*observation.visible_window_ms);
    }
    if (observation.responsive_window_ms) {
      responsive.push_back(*observation.responsive_window_ms);
    }
  }
  summary.median_visible_window_ms = Median(std::move(visible));
  summary.median_responsive_window_ms = Median(std::move(responsive));
  return summary;
}

std::string BenchmarkManager::Json(const StartupBenchmarkSummary& summary) {
  const auto successful_runs = static_cast<std::size_t>(std::count_if(
      summary.observations.begin(), summary.observations.end(),
      [](const StartupObservation& observation) {
        return observation.status == StartupObservationStatus::Succeeded;
      }));
  std::ostringstream output;
  output << std::setprecision(12)
         << "{\n  \"schema_version\": 1,\n"
         << "  \"type\": \"StartupResponsiveness\",\n"
         << "  \"label\": \"" << EscapeJson(summary.label) << "\",\n"
         << "  \"process_name\": \"" << EscapeJson(summary.process_name)
         << "\",\n"
         << "  \"generated_at\": \"" << EscapeJson(summary.generated_at)
         << "\",\n"
         << "  \"requested_runs\": " << summary.requested_runs << ",\n"
         << "  \"attempted_runs\": " << summary.observations.size()
         << ",\n"
         << "  \"successful_runs\": " << successful_runs
         << ",\n"
         << "  \"median_visible_window_ms\": ";
  AppendOptionalJson(&output, summary.median_visible_window_ms);
  output << ",\n  \"median_responsive_window_ms\": ";
  AppendOptionalJson(&output, summary.median_responsive_window_ms);
  output << ",\n  \"observations\": [";
  for (std::size_t i = 0; i < summary.observations.size(); ++i) {
    const auto& observation = summary.observations[i];
    output << (i == 0 ? "\n" : ",\n")
           << "    {\"run\": " << i + 1 << ", \"status\": \""
           << ToString(observation.status) << "\", \"pid\": "
           << observation.pid << ", \"visible_window_ms\": ";
    AppendOptionalJson(&output, observation.visible_window_ms);
    output << ", \"responsive_window_ms\": ";
    AppendOptionalJson(&output, observation.responsive_window_ms);
    output << ", \"error_code\": " << observation.error_code << '}';
  }
  if (!summary.observations.empty()) output << '\n';
  output << "  ]\n}\n";
  return output.str();
}

std::string BenchmarkManager::Text(const StartupBenchmarkSummary& summary) {
  const auto successful_runs = static_cast<std::size_t>(std::count_if(
      summary.observations.begin(), summary.observations.end(),
      [](const StartupObservation& observation) {
        return observation.status == StartupObservationStatus::Succeeded;
      }));
  std::ostringstream output;
  output << "STARTUP BENCHMARK " << summary.label << " ("
         << summary.process_name << ")\n"
         << "Successful runs: " << successful_runs << '/'
         << summary.requested_runs << '\n';
  for (std::size_t i = 0; i < summary.observations.size(); ++i) {
    const auto& observation = summary.observations[i];
    output << "Run " << i + 1 << ": " << ToString(observation.status);
    if (observation.visible_window_ms) {
      output << ", visible " << *observation.visible_window_ms << " ms";
    }
    if (observation.responsive_window_ms) {
      output << ", responsive " << *observation.responsive_window_ms << " ms";
    }
    if (observation.error_code != 0) {
      output << ", Windows error " << observation.error_code;
    }
    output << '\n';
  }
  output << "Median visible: ";
  if (summary.median_visible_window_ms) {
    output << *summary.median_visible_window_ms << " ms\n";
  } else {
    output << "unavailable\n";
  }
  output << "Median responsive: ";
  if (summary.median_responsive_window_ms) {
    output << *summary.median_responsive_window_ms << " ms\n";
  } else {
    output << "unavailable\n";
  }
  return output.str();
}

StartupBenchmarkComparison BenchmarkManager::Compare(
    std::string label, std::string generated_at,
    StartupBenchmarkSummary baseline, StartupBenchmarkSummary optimized) {
  StartupBenchmarkComparison comparison;
  comparison.label = std::move(label);
  comparison.generated_at = std::move(generated_at);
  comparison.baseline = std::move(baseline);
  comparison.optimized = std::move(optimized);
  if (comparison.baseline.median_visible_window_ms &&
      comparison.optimized.median_visible_window_ms) {
    comparison.delta_visible_window_ms =
        *comparison.optimized.median_visible_window_ms -
        *comparison.baseline.median_visible_window_ms;
  }
  if (comparison.baseline.median_responsive_window_ms &&
      comparison.optimized.median_responsive_window_ms) {
    comparison.delta_responsive_window_ms =
        *comparison.optimized.median_responsive_window_ms -
        *comparison.baseline.median_responsive_window_ms;
  }
  return comparison;
}

std::string BenchmarkManager::ComparisonJson(
    const StartupBenchmarkComparison& comparison) {
  std::ostringstream output;
  output << std::setprecision(12)
         << "{\n  \"schema_version\": 1,\n"
         << "  \"type\": \"StartupResponsivenessComparison\",\n"
         << "  \"label\": \"" << EscapeJson(comparison.label) << "\",\n"
         << "  \"generated_at\": \""
         << EscapeJson(comparison.generated_at) << "\",\n"
         << "  \"negative_delta_is_improvement\": true,\n"
         << "  \"baseline\": "
         << IndentAfterNewline(Json(comparison.baseline), "  ") << ",\n"
         << "  \"optimized\": "
         << IndentAfterNewline(Json(comparison.optimized), "  ") << ",\n"
         << "  \"delta\": {\n"
         << "    \"median_visible_window_ms\": ";
  AppendOptionalJson(&output, comparison.delta_visible_window_ms);
  output << ",\n    \"median_responsive_window_ms\": ";
  AppendOptionalJson(&output, comparison.delta_responsive_window_ms);
  output << "\n  }\n}\n";
  return output.str();
}

std::string BenchmarkManager::ComparisonText(
    const StartupBenchmarkComparison& comparison) {
  std::ostringstream output;
  output << "STARTUP BENCHMARK COMPARISON " << comparison.label << "\n\n"
         << "BASELINE\n"
         << Text(comparison.baseline) << "\nOPTIMIZED\n"
         << Text(comparison.optimized) << "\nDELTA (negative is improvement)\n"
         << "Median visible: ";
  if (comparison.delta_visible_window_ms) {
    output << *comparison.delta_visible_window_ms << " ms\n";
  } else {
    output << "unavailable\n";
  }
  output << "Median responsive: ";
  if (comparison.delta_responsive_window_ms) {
    output << *comparison.delta_responsive_window_ms << " ms\n";
  } else {
    output << "unavailable\n";
  }
  return output.str();
}

std::string ToString(StartupObservationStatus value) {
  switch (value) {
    case StartupObservationStatus::Succeeded: return "Succeeded";
    case StartupObservationStatus::TimedOut: return "TimedOut";
    case StartupObservationStatus::ProcessExited: return "ProcessExited";
    case StartupObservationStatus::WindowNotFound: return "WindowNotFound";
    case StartupObservationStatus::WindowUnresponsive:
      return "WindowUnresponsive";
    case StartupObservationStatus::QueryFailed: return "QueryFailed";
    case StartupObservationStatus::InvalidTarget: return "InvalidTarget";
  }
  return "QueryFailed";
}

}  // namespace workboost
