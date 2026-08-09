#pragma once

#include "core/model/types.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace workboost {

struct DiagnosisThresholds {
  double commit_warning{0.80};
  double available_memory_mb{2048.0};
  double page_reads_per_sec{25.0};
  double disk_active_ratio{0.90};
  double hdd_latency_ms{30.0};
  double cpu_saturation_ratio{0.90};
  double background_io_bytes_per_sec{10.0 * 1024.0 * 1024.0};
  double defender_cpu_ratio{0.10};
  double defender_io_ratio{0.25};
};

struct ProcessRule {
  ProcessClass process_class{ProcessClass::Unknown};
  ProtectionLevel protection{ProtectionLevel::Strong};
};

struct CodingProfile {
  std::unordered_map<std::string, std::string> foreground_priority;
  std::unordered_set<std::string> always_protect;
  std::unordered_set<std::string> allow_graceful_close;
  std::unordered_set<std::string> allow_priority_down;
};

class Config {
 public:
  static Config Defaults();

  // Missing files retain safe built-in defaults. Syntax errors are reported.
  bool LoadDirectory(const std::filesystem::path& directory,
                     std::string* warning = nullptr);

  [[nodiscard]] ProcessRule RuleFor(const std::string& process_name) const;
  [[nodiscard]] bool IsAlwaysProtected(const std::string& process_name) const;

  int sample_interval_ms{1000};
  int history_seconds{60};
  DiagnosisThresholds thresholds;
  CodingProfile coding_profile;
  std::unordered_map<std::string, ProcessRule> process_rules;
  std::unordered_set<std::uint16_t> remote_debug_ports{22, 23};
};

std::string ToLowerAscii(std::string value);

}  // namespace workboost
