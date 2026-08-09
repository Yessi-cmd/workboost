#include "core/diagnosis/diagnosis_engine.h"

#include "core/config/config.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <string>

namespace workboost {
namespace {

constexpr double kMiB = 1024.0 * 1024.0;

Confidence ConfidenceFor(double fraction, std::size_t samples) {
  if (samples >= 5 && fraction >= 0.75) return Confidence::High;
  if (samples >= 2 && fraction >= 0.50) return Confidence::Medium;
  return Confidence::Low;
}

Severity PressureSeverity(double available_mb, double commit_ratio,
                          const DiagnosisThresholds& thresholds) {
  return available_mb < thresholds.available_memory_mb * 0.5 &&
                 commit_ratio >= std::max(0.90, thresholds.commit_warning)
             ? Severity::High
             : Severity::Medium;
}

bool IsMemoryPressure(const SystemSnapshot& snapshot,
                      const DiagnosisThresholds& thresholds) {
  return static_cast<double>(snapshot.memory.physical_available_bytes) / kMiB <
             thresholds.available_memory_mb &&
         snapshot.memory.CommitRatio() >= thresholds.commit_warning;
}

const ProcessSnapshot* FindProcess(const SystemSnapshot& snapshot,
                                   const std::string& lower_name) {
  for (const auto& process : snapshot.processes) {
    if (ToLowerAscii(process.name) == lower_name) return &process;
  }
  return nullptr;
}

}  // namespace

std::vector<DiagnosisResult> DiagnosisEngine::Evaluate(
    const SnapshotHistory& history) const {
  std::vector<DiagnosisResult> results;
  if (history.Empty()) return results;
  const auto& snapshots = history.Snapshots();
  if (snapshots.size() < 2) return results;
  const double sample_count = static_cast<double>(snapshots.size());
  const std::size_t minimum_evidence_samples =
      std::max<std::size_t>(2, (snapshots.size() + 1) / 2);
  const std::size_t complete_process_samples =
      static_cast<std::size_t>(std::count_if(
          snapshots.begin(), snapshots.end(), [](const auto& snapshot) {
            return snapshot.process_inventory_complete;
          }));
  const std::size_t complete_protection_samples =
      static_cast<std::size_t>(std::count_if(
          snapshots.begin(), snapshots.end(), [](const auto& snapshot) {
            return snapshot.process_inventory_complete &&
                   snapshot.tcp_inventory_complete;
          }));

  std::size_t memory_hits = 0;
  std::size_t paging_hits = 0;
  std::size_t cpu_hits = 0;
  double average_available_mb = 0.0;
  double average_commit_ratio = 0.0;
  double average_page_reads = 0.0;
  double average_cpu = 0.0;
  for (const auto& snapshot : snapshots) {
    const bool memory_pressure = IsMemoryPressure(snapshot, config_.thresholds);
    memory_hits += memory_pressure ? 1U : 0U;
    paging_hits +=
        memory_pressure && snapshot.page_reads_per_sec >=
                               config_.thresholds.page_reads_per_sec
            ? 1U
            : 0U;
    cpu_hits += snapshot.cpu_percent >=
                        config_.thresholds.cpu_saturation_ratio * 100.0
                    ? 1U
                    : 0U;
    average_available_mb +=
        static_cast<double>(snapshot.memory.physical_available_bytes) / kMiB;
    average_commit_ratio += snapshot.memory.CommitRatio();
    average_page_reads += snapshot.page_reads_per_sec;
    average_cpu += snapshot.cpu_percent;
  }
  average_available_mb /= sample_count;
  average_commit_ratio /= sample_count;
  average_page_reads /= sample_count;
  average_cpu /= sample_count;

  const double memory_fraction = memory_hits / sample_count;
  if (memory_fraction >= 0.5) {
    DiagnosisResult result;
    result.type = "MemoryPressure";
    result.severity = PressureSeverity(average_available_mb,
                                       average_commit_ratio,
                                       config_.thresholds);
    result.confidence = ConfidenceFor(memory_fraction, snapshots.size());
    result.summary =
        "Available physical memory stayed low while commit usage was high.";
    result.evidence = {{"available_memory_mb", average_available_mb},
                       {"commit_ratio", average_commit_ratio},
                       {"matching_samples", static_cast<std::uint64_t>(memory_hits)},
                       {"sample_count",
                        static_cast<std::uint64_t>(snapshots.size())}};
    results.push_back(std::move(result));
  }

  const double paging_fraction = paging_hits / sample_count;
  if (paging_fraction >= 0.5) {
    DiagnosisResult result;
    result.type = "PagingPressure";
    result.severity = average_page_reads >=
                              config_.thresholds.page_reads_per_sec * 3.0
                          ? Severity::High
                          : Severity::Medium;
    result.confidence = ConfidenceFor(paging_fraction, snapshots.size());
    result.summary =
        "Memory pressure coincided with sustained page-read activity.";
    result.evidence = {{"available_memory_mb", average_available_mb},
                       {"commit_ratio", average_commit_ratio},
                       {"page_reads_per_sec", average_page_reads},
                       {"matching_samples", static_cast<std::uint64_t>(paging_hits)}};
    results.push_back(std::move(result));
  }

  struct DiskAggregate {
    DiskMedia media{DiskMedia::Unknown};
    std::string volumes;
    std::size_t bottleneck_hits{};
    std::size_t hdd_paging_hits{};
    double active_sum{};
    double latency_sum{};
    double queue_sum{};
    double operations_sum{};
    std::size_t observations{};
    std::size_t space_observations{};
    std::size_t low_space_hits{};
    std::uint64_t free_space_bytes{};
    std::uint64_t total_space_bytes{};
  };
  std::map<std::string, DiskAggregate> disk_aggregates;
  for (const auto& snapshot : snapshots) {
    const bool memory_pressure = IsMemoryPressure(snapshot, config_.thresholds);
    for (const auto& disk : snapshot.disks) {
      auto& aggregate = disk_aggregates[disk.instance];
      aggregate.media = disk.media;
      aggregate.volumes = disk.volumes;
      aggregate.active_sum += disk.active_ratio;
      aggregate.latency_sum += disk.average_latency_ms;
      aggregate.queue_sum += disk.queue_length;
      aggregate.operations_sum += disk.IoOperationsPerSec();
      ++aggregate.observations;
      if (disk.media == DiskMedia::SSD && disk.space_inventory_complete) {
        ++aggregate.space_observations;
        aggregate.free_space_bytes = disk.free_space_bytes;
        aggregate.total_space_bytes = disk.total_space_bytes;
        aggregate.low_space_hits +=
            disk.FreeSpaceRatio() <=
                    config_.thresholds.ssd_free_space_ratio
                ? 1U
                : 0U;
      }
      const double latency_threshold =
          disk.media == DiskMedia::SSD
              ? std::max(8.0, config_.thresholds.hdd_latency_ms / 3.0)
              : config_.thresholds.hdd_latency_ms;
      const bool bottleneck =
          disk.active_ratio >= config_.thresholds.disk_active_ratio &&
          disk.average_latency_ms >= latency_threshold;
      aggregate.bottleneck_hits += bottleneck ? 1U : 0U;
      aggregate.hdd_paging_hits +=
          bottleneck && disk.media == DiskMedia::HDD && memory_pressure &&
                  snapshot.page_reads_per_sec >=
                      config_.thresholds.page_reads_per_sec
              ? 1U
              : 0U;
    }
  }

  for (const auto& [name, aggregate] : disk_aggregates) {
    if (aggregate.observations < minimum_evidence_samples) continue;
    const double fraction = static_cast<double>(aggregate.bottleneck_hits) /
                            static_cast<double>(aggregate.observations);
    if (fraction >= 0.5) {
      DiagnosisResult result;
      result.type = "DiskBottleneck";
      result.severity = aggregate.latency_sum / aggregate.observations >=
                                config_.thresholds.hdd_latency_ms * 2.0
                            ? Severity::High
                            : Severity::Medium;
      result.confidence = ConfidenceFor(fraction, aggregate.observations);
      result.summary = "Disk active time and transfer latency stayed high.";
      result.evidence = {
          {"disk", name},
          {"volumes", aggregate.volumes},
          {"media", ToString(aggregate.media)},
          {"active_ratio", aggregate.active_sum / aggregate.observations},
          {"avg_latency_ms", aggregate.latency_sum / aggregate.observations},
          {"queue_length", aggregate.queue_sum / aggregate.observations},
          {"operations_per_sec",
           aggregate.operations_sum / aggregate.observations},
          {"matching_samples",
           static_cast<std::uint64_t>(aggregate.bottleneck_hits)},
          {"sample_count",
           static_cast<std::uint64_t>(aggregate.observations)}};
      results.push_back(std::move(result));
    }

    const double hdd_fraction =
        static_cast<double>(aggregate.hdd_paging_hits) /
        static_cast<double>(aggregate.observations);
    if (hdd_fraction >= 0.5) {
      DiagnosisResult result;
      result.type = "HddPagingBottleneck";
      result.severity = Severity::High;
      result.confidence = ConfidenceFor(hdd_fraction, aggregate.observations);
      result.summary =
          "Paging pressure coincided with high-latency activity on an HDD.";
      result.evidence = {
          {"disk", name},
          {"volumes", aggregate.volumes},
          {"available_memory_mb", average_available_mb},
          {"commit_ratio", average_commit_ratio},
          {"page_reads_per_sec", average_page_reads},
          {"active_ratio", aggregate.active_sum / aggregate.observations},
          {"avg_latency_ms", aggregate.latency_sum / aggregate.observations},
          {"matching_samples",
           static_cast<std::uint64_t>(aggregate.hdd_paging_hits)},
          {"sample_count",
           static_cast<std::uint64_t>(aggregate.observations)}};
      results.push_back(std::move(result));
    }

    if (aggregate.space_observations >= minimum_evidence_samples) {
      const double space_fraction =
          static_cast<double>(aggregate.low_space_hits) /
          static_cast<double>(aggregate.space_observations);
      if (space_fraction >= 0.5) {
        const double free_ratio =
            aggregate.total_space_bytes == 0
                ? 0.0
                : static_cast<double>(aggregate.free_space_bytes) /
                      static_cast<double>(aggregate.total_space_bytes);
        DiagnosisResult result;
        result.type = "SsdSpacePressure";
        result.severity =
            free_ratio <=
                    std::min(0.02,
                             config_.thresholds.ssd_free_space_ratio * 0.25)
                ? Severity::High
                : Severity::Medium;
        result.confidence =
            ConfidenceFor(space_fraction, aggregate.space_observations);
        result.summary =
            "Free space stayed low on an SSD; no files were moved or deleted.";
        result.evidence = {
            {"disk", name},
            {"volumes", aggregate.volumes},
            {"free_space_ratio", free_ratio},
            {"free_space_bytes", aggregate.free_space_bytes},
            {"total_space_bytes", aggregate.total_space_bytes},
            {"matching_samples",
             static_cast<std::uint64_t>(aggregate.low_space_hits)},
            {"sample_count",
             static_cast<std::uint64_t>(aggregate.space_observations)}};
        results.push_back(std::move(result));
      }
    }
  }

  const double cpu_fraction = cpu_hits / sample_count;
  if (cpu_fraction >= 0.5) {
    DiagnosisResult result;
    result.type = "CpuSaturation";
    result.severity = average_cpu >= 97.0 ? Severity::High : Severity::Medium;
    result.confidence = ConfidenceFor(cpu_fraction, snapshots.size());
    result.summary = "Total CPU utilization stayed close to saturation.";
    result.evidence = {{"cpu_percent", average_cpu},
                       {"matching_samples", static_cast<std::uint64_t>(cpu_hits)}};
    results.push_back(std::move(result));
  }

  double defender_cpu = 0.0;
  double defender_io = 0.0;
  double all_process_io = 0.0;
  std::size_t defender_samples = 0;
  std::size_t defender_impact_hits = 0;
  std::size_t defender_cpu_hits = 0;
  std::size_t defender_io_hits = 0;
  for (const auto& snapshot : snapshots) {
    if (!snapshot.process_inventory_complete) continue;
    double sample_total_io = 0.0;
    for (const auto& process : snapshot.processes)
      sample_total_io += process.IoBytesPerSec();
    all_process_io += sample_total_io;
    if (const auto* defender = FindProcess(snapshot, "msmpeng.exe")) {
      defender_cpu += defender->cpu_percent;
      defender_io += defender->IoBytesPerSec();
      ++defender_samples;
      const bool cpu_impact =
          defender->cpu_percent >=
          config_.thresholds.defender_cpu_ratio * 100.0;
      const bool io_impact =
          defender->IoBytesPerSec() >= 1024.0 * 1024.0 &&
          sample_total_io > 0.0 &&
          defender->IoBytesPerSec() / sample_total_io >=
              config_.thresholds.defender_io_ratio;
      defender_cpu_hits += cpu_impact ? 1U : 0U;
      defender_io_hits += io_impact ? 1U : 0U;
      defender_impact_hits += cpu_impact || io_impact ? 1U : 0U;
    }
  }
  if (defender_samples != 0 &&
      complete_process_samples >= minimum_evidence_samples) {
    const double process_sample_count =
        static_cast<double>(complete_process_samples);
    const double average_defender_cpu = defender_cpu / process_sample_count;
    const double average_defender_io = defender_io / process_sample_count;
    const double io_share =
        all_process_io <= 0.0 ? 0.0 : defender_io / all_process_io;
    const double impact_fraction = defender_impact_hits / process_sample_count;
    if (impact_fraction >= 0.5) {
      DiagnosisResult result;
      result.type = "DefenderImpact";
      result.severity = defender_cpu_hits / process_sample_count >= 0.5 &&
                                defender_io_hits / process_sample_count >= 0.5
                            ? Severity::High
                            : Severity::Medium;
      result.confidence =
          ConfidenceFor(impact_fraction, complete_process_samples);
      result.summary =
          "Microsoft Defender repeatedly accounted for a material share of "
          "CPU or I/O; no security setting was changed.";
      result.evidence = {{"cpu_percent", average_defender_cpu},
                         {"io_bytes_per_sec", average_defender_io},
                         {"io_share", io_share},
                         {"matching_samples", static_cast<std::uint64_t>(
                                                  defender_impact_hits)},
                         {"sample_count",
                          static_cast<std::uint64_t>(
                              complete_process_samples)}};
      results.push_back(std::move(result));
    }
  }

  struct ProcessImpactAggregate {
    std::string name;
    std::uint32_t pid{};
    std::size_t matching_samples{};
    double io_sum{};
    std::uint64_t working_set_sum{};
  };
  using ProcessIdentity = std::pair<std::uint32_t, std::uint64_t>;
  std::map<ProcessIdentity, ProcessImpactAggregate> background_aggregates;
  std::map<ProcessIdentity, ProcessImpactAggregate> foreground_aggregates;
  for (const auto& snapshot : snapshots) {
    if (!snapshot.process_inventory_complete) continue;
    const bool memory_pressure = IsMemoryPressure(snapshot, config_.thresholds);
    for (const auto& process : snapshot.processes) {
      const ProcessIdentity identity{process.pid, process.start_time_100ns};
      const bool eligible_background =
          process.classification == ProcessClass::Updater ||
          process.classification == ProcessClass::CloudSync ||
          process.classification == ProcessClass::VendorUtility ||
          process.classification == ProcessClass::Communication;
      const bool protected_process =
          process.protection_level == ProtectionLevel::Strong ||
          process.protection_level == ProtectionLevel::SystemCritical ||
          process.protection_level == ProtectionLevel::UserExplicit;
      if (snapshot.tcp_inventory_complete && eligible_background &&
          !protected_process) {
        auto& aggregate = background_aggregates[identity];
        aggregate.name = process.name;
        aggregate.pid = process.pid;
        aggregate.io_sum += process.IoBytesPerSec();
        aggregate.matching_samples +=
            process.IoBytesPerSec() >=
                    config_.thresholds.background_io_bytes_per_sec
                ? 1U
                : 0U;
      }
      if (memory_pressure && process.is_foreground &&
          process.classification == ProcessClass::Development) {
        auto& aggregate = foreground_aggregates[identity];
        aggregate.name = process.name;
        aggregate.pid = process.pid;
        aggregate.working_set_sum += process.working_set_bytes;
        ++aggregate.matching_samples;
      }
    }
  }

  const ProcessImpactAggregate* top_background = nullptr;
  double top_background_average_io = 0.0;
  double top_background_fraction = 0.0;
  for (const auto& [identity, aggregate] : background_aggregates) {
    static_cast<void>(identity);
    const double process_sample_count =
        static_cast<double>(complete_protection_samples);
    const double fraction = complete_protection_samples == 0
                                ? 0.0
                                : aggregate.matching_samples /
                                      process_sample_count;
    const double average_io = complete_protection_samples == 0
                                  ? 0.0
                                  : aggregate.io_sum / process_sample_count;
    if (complete_protection_samples >= minimum_evidence_samples &&
        fraction >= 0.5 &&
        (top_background == nullptr ||
         average_io > top_background_average_io)) {
      top_background = &aggregate;
      top_background_average_io = average_io;
      top_background_fraction = fraction;
    }
  }
  if (top_background != nullptr) {
    DiagnosisResult result;
    result.type = "BackgroundIoImpact";
    result.severity =
        top_background_average_io >=
                config_.thresholds.background_io_bytes_per_sec * 3.0
            ? Severity::High
            : Severity::Medium;
    result.confidence =
        ConfidenceFor(top_background_fraction, complete_protection_samples);
    result.summary =
        "An unprotected background application sustained heavy I/O.";
    result.evidence = {{"process", top_background->name},
                       {"pid", static_cast<std::uint64_t>(top_background->pid)},
                       {"io_bytes_per_sec", top_background_average_io},
                       {"matching_samples", static_cast<std::uint64_t>(
                                                top_background->matching_samples)},
                       {"sample_count",
                        static_cast<std::uint64_t>(
                            complete_protection_samples)}};
    results.push_back(std::move(result));
  }

  const ProcessImpactAggregate* foreground = nullptr;
  double foreground_fraction = 0.0;
  for (const auto& [identity, aggregate] : foreground_aggregates) {
    static_cast<void>(identity);
    const double process_sample_count =
        static_cast<double>(complete_process_samples);
    const double fraction = complete_process_samples == 0
                                ? 0.0
                                : aggregate.matching_samples /
                                      process_sample_count;
    if (complete_process_samples >= minimum_evidence_samples &&
        fraction >= 0.5 &&
        (foreground == nullptr ||
         aggregate.matching_samples > foreground->matching_samples)) {
      foreground = &aggregate;
      foreground_fraction = fraction;
    }
  }
  if (foreground != nullptr) {
    DiagnosisResult result;
    result.type = "ForegroundAppMemoryPressure";
    result.severity = PressureSeverity(average_available_mb,
                                       average_commit_ratio,
                                       config_.thresholds);
    result.confidence =
        ConfidenceFor(foreground_fraction, complete_process_samples);
    result.summary =
        "The active development application repeatedly coincided with system "
        "memory pressure.";
    result.evidence = {
        {"process", foreground->name},
        {"pid", static_cast<std::uint64_t>(foreground->pid)},
        {"working_set_bytes",
         foreground->matching_samples == 0
             ? std::uint64_t{0}
             : foreground->working_set_sum / foreground->matching_samples},
        {"available_memory_mb", average_available_mb},
        {"commit_ratio", average_commit_ratio},
        {"matching_samples",
         static_cast<std::uint64_t>(foreground->matching_samples)},
        {"sample_count",
         static_cast<std::uint64_t>(complete_process_samples)}};
    results.push_back(std::move(result));
  }

  std::stable_sort(results.begin(), results.end(),
                   [](const DiagnosisResult& left,
                      const DiagnosisResult& right) {
                     return static_cast<int>(left.severity) >
                            static_cast<int>(right.severity);
                   });
  return results;
}

}  // namespace workboost
