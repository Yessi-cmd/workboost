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
  const auto& latest = snapshots.back();
  const double sample_count = static_cast<double>(snapshots.size());

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
    std::size_t observations{};
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
      ++aggregate.observations;
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
    if (aggregate.observations == 0) continue;
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
          {"queue_length", aggregate.queue_sum / aggregate.observations}};
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
          {"avg_latency_ms", aggregate.latency_sum / aggregate.observations}};
      results.push_back(std::move(result));
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
  for (const auto& snapshot : snapshots) {
    double sample_total_io = 0.0;
    for (const auto& process : snapshot.processes)
      sample_total_io += process.IoBytesPerSec();
    all_process_io += sample_total_io;
    if (const auto* defender = FindProcess(snapshot, "msmpeng.exe")) {
      defender_cpu += defender->cpu_percent;
      defender_io += defender->IoBytesPerSec();
      ++defender_samples;
    }
  }
  if (defender_samples != 0) {
    const double average_defender_cpu = defender_cpu / sample_count;
    const double average_defender_io = defender_io / sample_count;
    const double io_share =
        all_process_io <= 0.0 ? 0.0 : defender_io / all_process_io;
    const bool cpu_impact =
        average_defender_cpu >= config_.thresholds.defender_cpu_ratio * 100.0;
    const bool io_impact =
        average_defender_io >= 1024.0 * 1024.0 &&
        io_share >= config_.thresholds.defender_io_ratio;
    if (cpu_impact || io_impact) {
      DiagnosisResult result;
      result.type = "DefenderImpact";
      result.severity = cpu_impact && io_impact ? Severity::High
                                                : Severity::Medium;
      result.confidence = defender_samples == snapshots.size()
                              ? Confidence::High
                              : Confidence::Medium;
      result.summary =
          "Microsoft Defender accounted for a material share of CPU or I/O; "
          "no security setting was changed.";
      result.evidence = {{"cpu_percent", average_defender_cpu},
                         {"io_bytes_per_sec", average_defender_io},
                         {"io_share", io_share}};
      results.push_back(std::move(result));
    }
  }

  const ProcessSnapshot* top_background = nullptr;
  for (const auto& process : latest.processes) {
    const bool eligible = process.classification == ProcessClass::Updater ||
                          process.classification == ProcessClass::CloudSync ||
                          process.classification == ProcessClass::VendorUtility ||
                          process.classification == ProcessClass::Communication;
    if (!eligible || process.protection_level == ProtectionLevel::Strong ||
        process.protection_level == ProtectionLevel::SystemCritical ||
        process.protection_level == ProtectionLevel::UserExplicit) {
      continue;
    }
    if (top_background == nullptr ||
        process.IoBytesPerSec() > top_background->IoBytesPerSec()) {
      top_background = &process;
    }
  }
  if (top_background != nullptr &&
      top_background->IoBytesPerSec() >=
          config_.thresholds.background_io_bytes_per_sec) {
    DiagnosisResult result;
    result.type = "BackgroundIoImpact";
    result.severity = Severity::Medium;
    result.confidence = Confidence::Medium;
    result.summary = "An unprotected background application generated heavy I/O.";
    result.evidence = {{"process", top_background->name},
                       {"pid", static_cast<std::uint64_t>(top_background->pid)},
                       {"io_bytes_per_sec", top_background->IoBytesPerSec()}};
    results.push_back(std::move(result));
  }

  if (IsMemoryPressure(latest, config_.thresholds)) {
    const auto foreground = std::find_if(
        latest.processes.begin(), latest.processes.end(),
        [](const ProcessSnapshot& process) {
          return process.is_foreground &&
                 process.classification == ProcessClass::Development;
        });
    if (foreground != latest.processes.end()) {
      DiagnosisResult result;
      result.type = "ForegroundAppMemoryPressure";
      result.severity = PressureSeverity(
          static_cast<double>(latest.memory.physical_available_bytes) / kMiB,
          latest.memory.CommitRatio(), config_.thresholds);
      result.confidence = Confidence::Medium;
      result.summary =
          "The active development application was running under system memory "
          "pressure.";
      result.evidence = {
          {"process", foreground->name},
          {"pid", static_cast<std::uint64_t>(foreground->pid)},
          {"working_set_bytes", foreground->working_set_bytes},
          {"available_memory_mb",
           static_cast<double>(latest.memory.physical_available_bytes) / kMiB}};
      results.push_back(std::move(result));
    }
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
