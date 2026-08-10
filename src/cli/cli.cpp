#include "cli/cli.h"

#include "app/session_manager.h"
#include "core/benchmark/benchmark.h"
#include "core/config/config.h"
#include "core/diagnosis/diagnosis_engine.h"
#include "core/logging/logger.h"
#include "core/optimization/optimization.h"
#include "core/policy/service_protection_policy.h"
#include "gui/dashboard.h"
#include "platform/windows/serial_port_api.h"
#include "platform/windows/service_api.h"
#include "platform/windows/startup_api.h"
#include "platform/windows/startup_benchmark_api.h"
#include "platform/windows/system_collector.h"
#include "platform/windows/windows_utils.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace workboost {
namespace {

constexpr double kMiB = 1024.0 * 1024.0;
constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
constexpr std::size_t kMaximumCleanupProcesses = 64;

struct CliOptions {
  bool json{};
  bool show_remote_ip{};
  bool dry_run{};
  bool confirm_service_actions{};
  int limit{10};
  int duration_seconds{-1};
  int baseline_seconds{10};
  int diagnostic_interval_ms{-1};
  int benchmark_runs{1};
  bool benchmark_runs_specified{};
  std::string benchmark_label{"observed"};
  bool benchmark_label_specified{};
  std::optional<std::filesystem::path> config_directory;
  std::optional<std::filesystem::path> output_path;
  std::vector<ProcessSelection> cleanup_processes;
};

void PrintUsage() {
  std::cout
      << "WorkBoost - Windows development workstation diagnostics\n\n"
      << "Usage:\n"
      << "  workboost gui\n"
      << "  workboost status [--json]\n"
      << "  workboost top <cpu|mem|io> [--limit N] [--json]\n"
      << "  workboost connections [--show-remote-ip] [--json]\n"
      << "  workboost services [--limit N] [--json]\n"
      << "  workboost serial [--json]\n"
      << "  workboost startup [--json]\n"
      << "  workboost benchmark observe <process.exe>"
         " [--duration SECONDS] [--runs N] [--label LABEL] [--output FILE]"
         " [--json]\n"
      << "  workboost benchmark compare <process.exe>"
         " [--duration SECONDS] [--runs N] [--label LABEL] [--output FILE]"
         " [--json]\n"
      << "  workboost protected [--json]\n"
      << "  workboost diagnose [--duration SECONDS] [--interval 250..500]"
         " [--output FILE] [--json]\n"
      << "  workboost profile show coding [--json]\n"
      << "  workboost coding enter [--dry-run] [--confirm-service-actions]"
         " [--baseline-duration 10..600]"
         " [--close-process PID:START_TIME_100NS ...]\n"
      << "  workboost coding exit\n"
      << "  workboost coding retry-close --close-process "
         "PID:START_TIME_100NS ...\n"
      << "  workboost recovery <status|restore|acknowledge>\n\n"
      << "Global option: --config-dir DIRECTORY\n";
}

bool ParsePositiveInt(const std::string& value, int* output) {
  try {
    std::size_t consumed = 0;
    const int parsed = std::stoi(value, &consumed);
    if (consumed != value.size() || parsed < 0) return false;
    *output = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseProcessSelection(const std::string& value,
                           ProcessSelection* output) {
  const std::size_t separator = value.find(':');
  if (separator == std::string::npos || separator == 0 ||
      separator + 1 >= value.size() ||
      value.find(':', separator + 1) != std::string::npos) {
    return false;
  }
  if (!std::all_of(value.begin(), value.end(), [](char character) {
        return character == ':' || (character >= '0' && character <= '9');
      }) ||
      value[separator] != ':') {
    return false;
  }
  try {
    std::size_t pid_consumed = 0;
    std::size_t time_consumed = 0;
    const auto pid = std::stoull(value.substr(0, separator), &pid_consumed);
    const std::string start_time = value.substr(separator + 1);
    const auto parsed_start_time =
        std::stoull(start_time, &time_consumed);
    if (pid_consumed != separator || time_consumed != start_time.size() ||
        pid == 0 ||
        pid > std::numeric_limits<std::uint32_t>::max() ||
        parsed_start_time == 0) {
      return false;
    }
    output->pid = static_cast<std::uint32_t>(pid);
    output->expected_start_time_100ns = parsed_start_time;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseOptions(const std::vector<std::string>& input,
                  std::vector<std::string>* positional, CliOptions* options,
                  std::string* error) {
  for (std::size_t i = 0; i < input.size(); ++i) {
    const std::string& value = input[i];
    if (value == "--json") {
      options->json = true;
    } else if (value == "--show-remote-ip") {
      options->show_remote_ip = true;
    } else if (value == "--dry-run") {
      options->dry_run = true;
    } else if (value == "--confirm-service-actions") {
      options->confirm_service_actions = true;
    } else if (value == "--close-process") {
      if (++i >= input.size()) {
        *error = value + " requires PID:START_TIME_100NS";
        return false;
      }
      ProcessSelection selection;
      if (!ParseProcessSelection(input[i], &selection)) {
        *error = value + " requires PID:START_TIME_100NS using positive "
                         "decimal integers";
        return false;
      }
      if (std::find(options->cleanup_processes.begin(),
                    options->cleanup_processes.end(),
                    selection) == options->cleanup_processes.end()) {
        if (options->cleanup_processes.size() >=
            kMaximumCleanupProcesses) {
          *error = "at most 64 --close-process selections are allowed";
          return false;
        }
        options->cleanup_processes.push_back(selection);
      }
    } else if (value == "--limit" || value == "--duration" ||
               value == "--interval" ||
               value == "--runs" || value == "--label" ||
               value == "--baseline-duration" || value == "--config-dir" ||
               value == "--output") {
      if (++i >= input.size()) {
        *error = value + " requires a value";
        return false;
      }
      if (value == "--config-dir") {
        options->config_directory = input[i];
      } else if (value == "--output") {
        options->output_path = input[i];
      } else if (value == "--label") {
        options->benchmark_label = input[i];
        options->benchmark_label_specified = true;
      } else {
        int parsed = 0;
        if (!ParsePositiveInt(input[i], &parsed)) {
          *error = value + " requires a non-negative integer";
          return false;
        }
        if (value == "--limit") options->limit = std::max(1, parsed);
        if (value == "--duration") options->duration_seconds = parsed;
        if (value == "--interval") options->diagnostic_interval_ms = parsed;
        if (value == "--runs") {
          options->benchmark_runs = parsed;
          options->benchmark_runs_specified = true;
        }
        if (value == "--baseline-duration")
          options->baseline_seconds = parsed;
      }
    } else if (!value.empty() && value[0] == '-') {
      *error = "unknown option: " + value;
      return false;
    } else {
      positional->push_back(value);
    }
  }
  return true;
}

std::string FormatBytes(double bytes) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(1);
  if (bytes >= kGiB) output << bytes / kGiB << " GB";
  else if (bytes >= kMiB) output << bytes / kMiB << " MB";
  else if (bytes >= 1024.0) output << bytes / 1024.0 << " KB";
  else output << bytes << " B";
  return output.str();
}

std::string JsonEvidenceValue(const EvidenceValue& value) {
  return std::visit(
      [](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return "\"" + windows::JsonEscape(item) + "\"";
        } else if constexpr (std::is_same_v<T, bool>) {
          return item ? "true" : "false";
        } else {
          std::ostringstream output;
          output << std::setprecision(12) << item;
          return output.str();
        }
      },
      value);
}

std::string DiagnosisJson(const std::vector<DiagnosisResult>& diagnoses,
                          const SnapshotHistory& history,
                          int sample_interval_ms) {
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
  output << "{\n  \"schema_version\": 1,\n"
         << "  \"generated_at\": \"" << windows::Iso8601Now() << "\",\n"
         << "  \"sample_count\": " << history.Size() << ",\n"
         << "  \"sample_interval_ms\": " << sample_interval_ms << ",\n"
         << "  \"process_inventory_complete_samples\": "
         << process_complete << ",\n"
         << "  \"tcp_inventory_complete_samples\": " << tcp_complete
         << ",\n"
         << "  \"protection_inventory_complete_samples\": "
         << protection_complete << ",\n"
         << "  \"diagnoses\": [";
  for (std::size_t i = 0; i < diagnoses.size(); ++i) {
    const auto& diagnosis = diagnoses[i];
    output << (i == 0 ? "\n" : ",\n")
           << "    {\n"
           << "      \"type\": \"" << windows::JsonEscape(diagnosis.type)
           << "\",\n"
           << "      \"severity\": \"" << ToString(diagnosis.severity)
           << "\",\n"
           << "      \"confidence\": \"" << ToString(diagnosis.confidence)
           << "\",\n"
           << "      \"summary\": \""
           << windows::JsonEscape(diagnosis.summary) << "\",\n"
           << "      \"evidence\": {";
    std::size_t evidence_index = 0;
    for (const auto& [key, value] : diagnosis.evidence) {
      output << (evidence_index++ == 0 ? "\n" : ",\n")
             << "        \"" << windows::JsonEscape(key) << "\": "
             << JsonEvidenceValue(value);
    }
    if (!diagnosis.evidence.empty()) output << '\n';
    output << "      }\n    }";
  }
  if (!diagnoses.empty()) output << '\n';
  output << "  ]\n}\n";
  return output.str();
}

std::string DiagnosisText(const std::vector<DiagnosisResult>& diagnoses,
                          const SnapshotHistory& history,
                          int sample_interval_ms) {
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
  output << "WORKBOOST DIAGNOSTIC REPORT\n"
         << "Generated: " << windows::Iso8601Now() << "\n"
         << "Samples: " << history.Size() << " at " << sample_interval_ms
         << " ms\n"
         << "Complete inventories: process=" << process_complete
         << ", tcp=" << tcp_complete
         << ", process+tcp=" << protection_complete << "\n\n";
  if (diagnoses.empty()) {
    output << "No sustained bottleneck matched the configured thresholds.\n";
    return output.str();
  }
  for (const auto& diagnosis : diagnoses) {
    output << ToString(diagnosis.severity) << " " << diagnosis.type << " ("
           << ToString(diagnosis.confidence) << " confidence)\n"
           << diagnosis.summary << "\n";
    for (const auto& [key, value] : diagnosis.evidence) {
      output << "  " << key << ": ";
      std::visit([&output](const auto& item) { output << item; }, value);
      output << '\n';
    }
    output << '\n';
  }
  return output.str();
}

Config LoadConfig(const CliOptions& options) {
  Config config = Config::Defaults();
  std::vector<std::string> warnings;
  const auto load = [&config, &warnings](const std::filesystem::path& path) {
    std::string warning;
    config.LoadDirectory(path, &warning);
    if (!warning.empty()) warnings.push_back(std::move(warning));
  };
  if (options.config_directory) {
    load(*options.config_directory);
  } else {
    const auto executable_config = windows::ExecutableDirectory() / "config";
    load(executable_config);
    std::error_code current_path_error;
    const auto current_directory =
        std::filesystem::current_path(current_path_error);
    if (current_path_error) {
      warnings.push_back("cannot inspect current config directory: " +
                         current_path_error.message());
    } else {
      const auto current_config = current_directory / "config";
      if (current_config != executable_config) load(current_config);
    }
    const auto user_config = windows::LocalAppDataDirectory();
    load(user_config);
  }
  if (!warnings.empty()) {
    Logger::Instance().Write(LogLevel::Warn,
                             LogEvent::ConfigurationWarning);
    std::cerr << "Configuration warning: ";
    for (std::size_t i = 0; i < warnings.size(); ++i) {
      if (i != 0) std::cerr << "; ";
      std::cerr << warnings[i];
    }
    std::cerr << '\n';
  }
  return config;
}

std::optional<SystemSnapshot> CaptureOne(const Config& config,
                                         std::string* error) {
  windows::SystemCollector collector(config);
  windows::WindowsError windows_error;
  if (!collector.Initialize(&windows_error)) {
    Logger::Instance().Write(LogLevel::Error, LogEvent::MonitorFailed,
                             windows_error.code);
    *error = windows_error.Describe();
    return std::nullopt;
  }
  std::this_thread::sleep_for(
      std::chrono::milliseconds(config.sample_interval_ms));
  return collector.Sample(&windows_error);
}

bool AttachConfiguredServices(const Config& config, SystemSnapshot* snapshot,
                              std::string* error) {
  snapshot->services.clear();
  snapshot->service_inventory_complete = false;
  std::vector<std::string> names(
      config.coding_profile.allow_service_stop.begin(),
      config.coding_profile.allow_service_stop.end());
  std::sort(names.begin(), names.end());
  for (const auto& name : names) {
    const auto lookup = windows::ServiceApi::Query(name);
    if (!lookup.success) {
      if (error) {
        *error = "cannot query configured service " + name + ": " +
                 lookup.error.Describe();
      }
      return false;
    }
    snapshot->services.push_back(lookup.service);
  }
  snapshot->service_inventory_complete = true;
  return true;
}

std::optional<SystemSnapshot> CaptureActionState(const Config& config,
                                                 std::string* error) {
  auto snapshot = CaptureOne(config, error);
  if (!snapshot || !AttachConfiguredServices(config, &*snapshot, error)) {
    return std::nullopt;
  }
  return snapshot;
}

std::optional<SnapshotHistory> CaptureHistory(const Config& config,
                                              int duration_seconds,
                                              std::string* error) {
  const int samples = std::max(
      1, static_cast<int>(std::ceil(duration_seconds * 1000.0 /
                                    config.sample_interval_ms)));
  SnapshotHistory history(static_cast<std::size_t>(std::max(
      samples, config.history_seconds * 1000 / config.sample_interval_ms)));
  windows::SystemCollector collector(config);
  windows::WindowsError windows_error;
  if (!collector.Initialize(&windows_error)) {
    Logger::Instance().Write(LogLevel::Error, LogEvent::MonitorFailed,
                             windows_error.code);
    *error = windows_error.Describe();
    return std::nullopt;
  }
  for (int i = 0; i < samples; ++i) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config.sample_interval_ms));
    history.Add(collector.Sample(&windows_error));
  }
  return history;
}

std::unordered_map<std::uint32_t, std::string> ProcessNames(
    const SystemSnapshot& snapshot) {
  std::unordered_map<std::uint32_t, std::string> result;
  for (const auto& process : snapshot.processes)
    result[process.pid] = process.name;
  return result;
}

std::vector<ProcessSnapshot> SortedProcesses(const SystemSnapshot& snapshot,
                                             const std::string& metric) {
  auto processes = snapshot.processes;
  std::stable_sort(processes.begin(), processes.end(),
                   [&metric](const ProcessSnapshot& left,
                             const ProcessSnapshot& right) {
                     if (metric == "cpu")
                       return left.cpu_percent > right.cpu_percent;
                     if (metric == "mem")
                       return left.private_bytes > right.private_bytes;
                     return left.IoBytesPerSec() > right.IoBytesPerSec();
                   });
  return processes;
}

void PrintTop(const SystemSnapshot& snapshot, const std::string& metric,
              const CliOptions& options) {
  const auto processes = SortedProcesses(snapshot, metric);
  const std::size_t count =
      std::min<std::size_t>(processes.size(), options.limit);
  if (options.json) {
    std::cout << "{\n  \"schema_version\": 1,\n"
              << "  \"metric\": \"" << metric << "\",\n"
              << "  \"process_inventory_complete\": "
              << (snapshot.process_inventory_complete ? "true" : "false")
              << ",\n"
              << "  \"processes\": [";
    for (std::size_t i = 0; i < count; ++i) {
      const auto& process = processes[i];
      std::cout << (i == 0 ? "\n" : ",\n")
                << "    {\"pid\": " << process.pid << ", \"name\": \""
                << windows::JsonEscape(process.name) << "\", \"cpu_percent\": "
                << process.cpu_percent << ", \"private_bytes\": "
                << process.private_bytes << ", \"read_bytes_per_sec\": "
                << process.read_bytes_per_sec
                << ", \"write_bytes_per_sec\": "
                << process.write_bytes_per_sec << '}';
    }
    if (count != 0) std::cout << '\n';
    std::cout << "  ]\n}\n";
    return;
  }
  if (!snapshot.process_inventory_complete) {
    std::cout << "WARNING: process inventory is incomplete; results may be "
                 "partial.\n";
  }
  std::cout << "PID      CPU       PRIVATE       READ/s        WRITE/s       NAME\n";
  for (std::size_t i = 0; i < count; ++i) {
    const auto& process = processes[i];
    std::cout << std::left << std::setw(9) << process.pid << std::right
              << std::setw(6) << std::fixed << std::setprecision(1)
              << process.cpu_percent << "%  " << std::setw(12)
              << FormatBytes(static_cast<double>(process.private_bytes)) << "  "
              << std::setw(12) << FormatBytes(process.read_bytes_per_sec) << "  "
              << std::setw(12) << FormatBytes(process.write_bytes_per_sec) << "  "
              << process.name << '\n';
  }
}

void PrintConnections(const SystemSnapshot& snapshot, const CliOptions& options) {
  const auto names = ProcessNames(snapshot);
  std::vector<TcpSession> sessions;
  for (const auto& session : snapshot.tcp_sessions) {
    if (session.state == TcpState::Established) sessions.push_back(session);
  }
  if (options.json) {
    std::cout << "{\n  \"schema_version\": 1,\n"
              << "  \"process_inventory_complete\": "
              << (snapshot.process_inventory_complete ? "true" : "false")
              << ",\n  \"tcp_inventory_complete\": "
              << (snapshot.tcp_inventory_complete ? "true" : "false")
              << ",\n"
              << "  \"remote_ip_masked\": "
              << (options.show_remote_ip ? "false" : "true")
              << ",\n  \"connections\": [";
    for (std::size_t i = 0; i < sessions.size(); ++i) {
      const auto& session = sessions[i];
      const auto name = names.find(session.pid);
      const std::string remote = options.show_remote_ip
                                     ? session.remote_address
                                     : windows::MaskIpAddress(session.remote_address);
      std::cout << (i == 0 ? "\n" : ",\n")
                << "    {\"pid\": " << session.pid << ", \"process\": \""
                << windows::JsonEscape(name == names.end() ? "unknown" : name->second)
                << "\", \"local_address\": \""
                << windows::JsonEscape(session.local_address)
                << "\", \"local_port\": " << session.local_port
                << ", \"remote_address\": \""
                << windows::JsonEscape(remote) << "\", \"remote_port\": "
                << session.remote_port << ", \"state\": \"ESTABLISHED\"}";
    }
    if (!sessions.empty()) std::cout << '\n';
    std::cout << "  ]\n}\n";
    return;
  }
  if (!snapshot.process_inventory_complete ||
      !snapshot.tcp_inventory_complete) {
    std::cout << "WARNING: process/TCP inventory is incomplete; results may "
                 "be partial.\n";
  }
  std::cout << "PID      PROCESS                 LOCAL                    REMOTE\n";
  for (const auto& session : sessions) {
    const auto name = names.find(session.pid);
    const std::string remote = options.show_remote_ip
                                   ? session.remote_address
                                   : windows::MaskIpAddress(session.remote_address);
    std::ostringstream local_endpoint;
    local_endpoint << session.local_address << ':' << session.local_port;
    std::ostringstream remote_endpoint;
    remote_endpoint << remote << ':' << session.remote_port;
    std::cout << std::left << std::setw(9) << session.pid << std::setw(24)
              << (name == names.end() ? "unknown" : name->second) << std::setw(25)
              << local_endpoint.str() << remote_endpoint.str() << '\n';
  }
}

void PrintServices(const std::vector<ServiceSnapshot>& services,
                   const Config& config,
                   const CliOptions& options) {
  const ServiceProtectionPolicy policy(config);
  const std::size_t count = std::min<std::size_t>(
      services.size(), static_cast<std::size_t>(options.limit));
  if (options.json) {
    std::cout << "{\n  \"schema_version\": 1,\n"
              << "  \"service_count\": " << services.size() << ",\n"
              << "  \"returned_count\": " << count << ",\n"
              << "  \"services\": [";
    for (std::size_t i = 0; i < count; ++i) {
      const auto& service = services[i];
      const ServiceRule rule = config.ServiceRuleFor(service.name);
      std::cout << (i == 0 ? "\n" : ",\n")
                << "    {\"name\": \""
                << windows::JsonEscape(service.name)
                << "\", \"display_name\": \""
                << windows::JsonEscape(service.display_name)
                << "\", \"state\": \"" << ToString(service.state)
                << "\", \"class\": \"" << ToString(rule.service_class)
                << "\", \"protection\": \""
                << ToString(policy.Evaluate(service))
                << "\", \"pid\": " << service.pid
                << ", \"accepts_stop\": "
                << (service.accepts_stop ? "true" : "false")
                << ", \"win32_exit_code\": " << service.win32_exit_code
                << ", \"service_specific_exit_code\": "
                << service.service_specific_exit_code
                << ", \"identity_verified\": "
                << (service.identity_verified ? "true" : "false") << '}';
    }
    if (count != 0) std::cout << '\n';
    std::cout << "  ]\n}\n";
    return;
  }

  std::cout << "SERVICES " << services.size() << " total, showing " << count
            << '\n';
  for (std::size_t i = 0; i < count; ++i) {
    const auto& service = services[i];
    const ServiceRule rule = config.ServiceRuleFor(service.name);
    std::cout << std::left << std::setw(38) << service.name << std::setw(18)
              << ToString(service.state) << std::setw(16)
              << ToString(rule.service_class) << std::setw(16)
              << ToString(policy.Evaluate(service)) << "PID " << service.pid
              << (service.accepts_stop ? "  accepts-stop" : "") << '\n';
  }
}

void PrintSerialPorts(const std::vector<SerialPortSnapshot>& ports,
                      bool json) {
  if (json) {
    std::cout << "{\n  \"schema_version\": 1,\n"
              << "  \"port_count\": " << ports.size() << ",\n"
              << "  \"ports\": [";
    for (std::size_t i = 0; i < ports.size(); ++i) {
      const auto& port = ports[i];
      std::cout << (i == 0 ? "\n" : ",\n")
                << "    {\"port_name\": \""
                << windows::JsonEscape(port.port_name)
                << "\", \"friendly_name\": \""
                << windows::JsonEscape(port.friendly_name)
                << "\", \"manufacturer\": \""
                << windows::JsonEscape(port.manufacturer) << "\"}";
    }
    if (!ports.empty()) std::cout << '\n';
    std::cout << "  ]\n}\n";
    return;
  }
  std::cout << "SERIAL PORTS " << ports.size() << " present\n";
  for (const auto& port : ports) {
    std::cout << std::left << std::setw(10) << port.port_name
              << (port.friendly_name.empty() ? "Unknown device"
                                             : port.friendly_name);
    if (!port.manufacturer.empty()) {
      std::cout << " (" << port.manufacturer << ')';
    }
    std::cout << '\n';
  }
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

void PrintStartupEntries(const std::vector<StartupEntrySnapshot>& entries,
                         const Config& config, bool json) {
  if (json) {
    std::cout << "{\n  \"schema_version\": 1,\n"
              << "  \"entry_count\": " << entries.size() << ",\n"
              << "  \"contains_command_arguments\": false,\n"
              << "  \"entries\": [";
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const auto& entry = entries[i];
      const ProcessRule rule = config.RuleFor(entry.executable_name);
      std::cout << (i == 0 ? "\n" : ",\n")
                << "    {\"scope\": \"" << ToString(entry.scope)
                << "\", \"source\": \"" << ToString(entry.source)
                << "\", \"location\": \""
                << windows::JsonEscape(entry.location)
                << "\", \"name\": \"" << windows::JsonEscape(entry.name)
                << "\", \"executable_name\": \""
                << windows::JsonEscape(entry.executable_name)
                << "\", \"class\": \"" << ToString(rule.process_class)
                << "\", \"protection\": \""
                << ToString(rule.protection) << "\", \"recommendation\": \""
                << StartupRecommendation(entry, config) << "\"}";
    }
    if (!entries.empty()) std::cout << '\n';
    std::cout << "  ]\n}\n";
    return;
  }
  std::cout << "STARTUP ENTRIES " << entries.size()
            << " (read-only; command arguments omitted)\n";
  for (const auto& entry : entries) {
    const ProcessRule rule = config.RuleFor(entry.executable_name);
    std::cout << std::left << std::setw(20) << entry.name << std::setw(18)
              << entry.executable_name << std::setw(15)
              << ToString(rule.process_class) << std::setw(13)
              << StartupRecommendation(entry, config) << entry.location
              << '\n';
  }
}

std::vector<const ProcessSnapshot*> ProtectedProcesses(
    const SystemSnapshot& snapshot, bool workload_only) {
  std::vector<const ProcessSnapshot*> result;
  for (const auto& process : snapshot.processes) {
    const bool protected_level =
        process.protection_level == ProtectionLevel::Strong ||
        process.protection_level == ProtectionLevel::SystemCritical ||
        process.protection_level == ProtectionLevel::UserExplicit;
    const bool workload = process.classification == ProcessClass::Development ||
                          process.classification == ProcessClass::RemoteTerminal ||
                          process.classification == ProcessClass::SerialTerminal ||
                          process.classification == ProcessClass::PacketCapture ||
                          process.classification == ProcessClass::BuildTool ||
                          process.classification == ProcessClass::VersionControl;
    if (protected_level && (!workload_only || workload)) result.push_back(&process);
  }
  std::stable_sort(result.begin(), result.end(),
                   [](const ProcessSnapshot* left, const ProcessSnapshot* right) {
                     return left->name < right->name;
                   });
  return result;
}

void PrintProtected(const SystemSnapshot& snapshot, const CliOptions& options,
                    bool workload_only = false) {
  const auto processes = ProtectedProcesses(snapshot, workload_only);
  if (options.json) {
    std::cout << "{\n  \"schema_version\": 1,\n"
              << "  \"process_inventory_complete\": "
              << (snapshot.process_inventory_complete ? "true" : "false")
              << ",\n  \"tcp_inventory_complete\": "
              << (snapshot.tcp_inventory_complete ? "true" : "false")
              << ",\n"
              << "  \"processes\": [";
    for (std::size_t i = 0; i < processes.size(); ++i) {
      const auto& process = *processes[i];
      std::cout << (i == 0 ? "\n" : ",\n")
                << "    {\"pid\": " << process.pid << ", \"name\": \""
                << windows::JsonEscape(process.name) << "\", \"class\": \""
                << ToString(process.classification)
                << "\", \"protection\": \""
                << ToString(process.protection_level) << "\"}";
    }
    if (!processes.empty()) std::cout << '\n';
    std::cout << "  ]\n}\n";
    return;
  }
  if (!snapshot.process_inventory_complete ||
      !snapshot.tcp_inventory_complete) {
    std::cout << "WARNING: protection inventory is incomplete; results may "
                 "be partial.\n";
  }
  for (const auto* process : processes) {
    std::cout << std::left << std::setw(28) << process->name << " PID "
              << std::setw(8) << process->pid << ' '
              << std::setw(18) << ToString(process->classification) << ' '
              << ToString(process->protection_level) << '\n';
  }
  if (processes.empty()) std::cout << "No protected process is currently running.\n";
}

void PrintStatus(const SystemSnapshot& snapshot, const Config& config,
                 const CliOptions& options) {
  if (options.json) {
    SnapshotHistory history(1);
    history.Add(snapshot);
    const auto diagnoses = DiagnosisEngine(config).Evaluate(history);
    std::cout << "{\n  \"schema_version\": 1,\n"
              << "  \"process_inventory_complete\": "
              << (snapshot.process_inventory_complete ? "true" : "false")
              << ",\n  \"tcp_inventory_complete\": "
              << (snapshot.tcp_inventory_complete ? "true" : "false")
              << ",\n  \"diagnosis_window_complete\": false,\n"
              << "  \"diagnosis_sample_count\": 1,\n"
              << "  \"system\": {\"cpu_percent\": "
              << snapshot.cpu_percent << ", \"physical_total_bytes\": "
              << snapshot.memory.physical_total_bytes
              << ", \"physical_available_bytes\": "
              << snapshot.memory.physical_available_bytes
              << ", \"commit_ratio\": " << snapshot.memory.CommitRatio()
              << ", \"page_reads_per_sec\": " << snapshot.page_reads_per_sec
              << "},\n  \"disks\": [";
    for (std::size_t i = 0; i < snapshot.disks.size(); ++i) {
      const auto& disk = snapshot.disks[i];
      std::cout << (i == 0 ? "\n" : ",\n")
                << "    {\"instance\": \""
                << windows::JsonEscape(disk.instance) << "\", \"volumes\": \""
                << windows::JsonEscape(disk.volumes) << "\", \"media\": \""
                << ToString(disk.media) << "\", \"active_ratio\": "
                << disk.active_ratio << ", \"latency_ms\": "
                << disk.average_latency_ms << ", \"queue_length\": "
                << disk.queue_length << ", \"read_bytes_per_sec\": "
                << disk.read_bytes_per_sec
                << ", \"write_bytes_per_sec\": "
                << disk.write_bytes_per_sec
                << ", \"read_operations_per_sec\": "
                << disk.read_operations_per_sec
                << ", \"write_operations_per_sec\": "
                << disk.write_operations_per_sec
                << ", \"space_inventory_complete\": "
                << (disk.space_inventory_complete ? "true" : "false")
                << ", \"total_space_bytes\": " << disk.total_space_bytes
                << ", \"free_space_bytes\": " << disk.free_space_bytes
                << "}";
    }
    if (!snapshot.disks.empty()) std::cout << '\n';
    std::cout << "  ],\n  \"diagnosis_count\": " << diagnoses.size()
              << "\n}\n";
    return;
  }
  if (!snapshot.process_inventory_complete ||
      !snapshot.tcp_inventory_complete) {
    std::cout << "WARNING: protection inventory is incomplete; process and "
                 "connection sections may be partial.\n\n";
  }
  std::cout << "SYSTEM\n"
            << "CPU       " << std::fixed << std::setprecision(1)
            << snapshot.cpu_percent << "%\n"
            << "RAM       "
            << FormatBytes(static_cast<double>(
                   snapshot.memory.physical_total_bytes >=
                           snapshot.memory.physical_available_bytes
                       ? snapshot.memory.physical_total_bytes -
                             snapshot.memory.physical_available_bytes
                       : 0))
            << " / "
            << FormatBytes(static_cast<double>(snapshot.memory.physical_total_bytes))
            << "\nAVAILABLE "
            << FormatBytes(
                   static_cast<double>(snapshot.memory.physical_available_bytes))
            << "\nCOMMIT    " << snapshot.memory.CommitRatio() * 100.0 << "%\n"
            << "PAGE READ " << snapshot.page_reads_per_sec << "/s\n\nDISK\n";
  if (snapshot.disks.empty()) {
    std::cout << "Disk performance counters are unavailable.\n";
  } else {
    for (const auto& disk : snapshot.disks) {
      std::cout << disk.instance << " " << ToString(disk.media) << " Active "
                << disk.active_ratio * 100.0 << "% Latency "
                << disk.average_latency_ms << " ms Queue " << disk.queue_length
                << " IOPS " << disk.IoOperationsPerSec();
      if (disk.space_inventory_complete) {
        std::cout << " Free "
                  << FormatBytes(static_cast<double>(disk.free_space_bytes))
                  << " / "
                  << FormatBytes(static_cast<double>(disk.total_space_bytes));
      }
      std::cout << '\n';
    }
  }
  std::cout << "\nPROTECTED WORKLOAD\n";
  PrintProtected(snapshot, CliOptions{}, true);
  std::cout << "\nTOP IO\n";
  CliOptions top_options;
  top_options.limit = 5;
  PrintTop(snapshot, "io", top_options);
  SnapshotHistory history(1);
  history.Add(snapshot);
  const auto diagnoses = DiagnosisEngine(config).Evaluate(history);
  std::cout << "\nDIAGNOSIS\n";
  if (diagnoses.empty()) {
    std::cout << "No current threshold match; use 'diagnose' for a time-window "
                 "assessment.\n";
  } else {
    for (const auto& diagnosis : diagnoses) {
      std::cout << ToString(diagnosis.severity) << ' ' << diagnosis.type << '\n';
    }
  }
}

void PrintJsonStringSet(const std::string& field,
                        const std::unordered_set<std::string>& values,
                        bool trailing_comma) {
  std::vector<std::string> sorted(values.begin(), values.end());
  std::sort(sorted.begin(), sorted.end());
  std::cout << "  \"" << field << "\": [";
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    std::cout << (i == 0 ? "\n" : ",\n") << "    \""
              << windows::JsonEscape(sorted[i]) << "\"";
  }
  if (!sorted.empty()) std::cout << '\n';
  std::cout << "  ]" << (trailing_comma ? ",\n" : "\n");
}

std::string DescribeAction(const OptimizationAction& action) {
  std::ostringstream description;
  if (action.type == ActionType::SetPriorityClass) {
    description << "set priority to " << PriorityName(action.target_priority);
  } else if (action.type == ActionType::GracefulCloseProcess) {
    description << "request graceful close via WM_CLOSE (timeout "
                << action.timeout_ms << " ms)";
  } else if (action.type == ActionType::StopServiceTemporary) {
    description << "temporarily stop service " << action.service_name
                << " (timeout " << action.timeout_ms << " ms)";
  } else {
    description << ToString(action.type);
  }
  description << ", risk " << ToString(action.risk);
  return description.str();
}

void PrintProfile(const Config& config, bool json) {
  std::vector<std::pair<std::string, std::string>> priorities(
      config.coding_profile.foreground_priority.begin(),
      config.coding_profile.foreground_priority.end());
  std::sort(priorities.begin(), priorities.end(),
            [](const auto& left, const auto& right) {
              return left.first < right.first;
            });
  if (json) {
    std::cout << "{\n  \"schema_version\": 1,\n"
              << "  \"name\": \"coding\",\n"
              << "  \"foreground_priority\": {";
    std::size_t index = 0;
    for (const auto& [name, priority] : priorities) {
      std::cout << (index++ == 0 ? "\n" : ",\n") << "    \""
                << windows::JsonEscape(name) << "\": \""
                << windows::JsonEscape(priority) << "\"";
    }
    if (index != 0) std::cout << '\n';
    std::cout << "  },\n";
    PrintJsonStringSet("always_protect",
                       config.coding_profile.always_protect, true);
    PrintJsonStringSet("allow_priority_down",
                       config.coding_profile.allow_priority_down, true);
    PrintJsonStringSet("allow_graceful_close",
                       config.coding_profile.allow_graceful_close, true);
    PrintJsonStringSet("always_protect_services",
                       config.coding_profile.always_protect_services, true);
    PrintJsonStringSet("allow_service_stop",
                       config.coding_profile.allow_service_stop, true);
    std::cout << "  \"graceful_close_batch_budget_ms\": "
              << config.coding_profile.graceful_close_batch_budget_ms
              << '\n';
    std::cout << "}\n";
    return;
  }
  std::cout << "PROFILE coding\nForeground priority:\n";
  for (const auto& [name, priority] : priorities)
    std::cout << "  " << name << " -> " << priority << '\n';
  std::cout << "Always protect:\n";
  std::vector<std::string> always_protect(
      config.coding_profile.always_protect.begin(),
      config.coding_profile.always_protect.end());
  std::sort(always_protect.begin(), always_protect.end());
  for (const auto& name : always_protect)
    std::cout << "  " << name << '\n';
  std::cout << "Priority down allowed: "
            << config.coding_profile.allow_priority_down.size() << " rule(s)\n"
            << "Graceful close allowed: "
            << config.coding_profile.allow_graceful_close.size() << " rule(s)\n"
            << "Always protected services: "
            << config.coding_profile.always_protect_services.size()
            << " rule(s)\n"
            << "Temporary service stop allowed: "
            << config.coding_profile.allow_service_stop.size() << " rule(s)\n"
            << "Graceful close shared budget: "
            << config.coding_profile.graceful_close_batch_budget_ms
            << " ms\n";
}

int RestoreSession(const Config& config, SessionManager* manager,
                   bool recovery_command) {
  std::string error;
  auto session = manager->LoadActive(&error);
  if (!session) {
    if (manager->HasActiveSession()) {
      std::cerr << "Safe Mode: " << error << '\n';
      return 2;
    }
    std::cout << "No active Coding Mode session.\n";
    return 0;
  }
  session->state = SessionState::Recovering;
  if (!manager->Save(*session, &error)) {
    std::cerr << "Safe Mode: cannot persist recovery state: " << error << '\n';
    return 2;
  }
  std::optional<SnapshotHistory> optimized;
  std::vector<DiagnosisResult> report_diagnoses;
  bool report_evidence_captured = false;
  if (!recovery_command) {
    std::string capture_error;
    const int comparison_seconds = std::max(1, std::min(5, config.history_seconds));
    std::cerr << "Capturing " << comparison_seconds
              << " second(s) of optimized-state evidence before rollback.\n";
    auto optimized_history =
        CaptureHistory(config, comparison_seconds, &capture_error);
    if (optimized_history && optimized_history->Latest() != nullptr) {
      report_diagnoses = DiagnosisEngine(config).Evaluate(*optimized_history);
      optimized = std::move(*optimized_history);
      report_evidence_captured = true;
    } else {
      std::cerr << "Warning: optimized-state capture failed; rollback will "
                   "continue: "
                << capture_error << '\n';
    }
  }
  ActionExecutor executor(config);
  bool success = true;
  std::string failure_detail;
  for (auto it = session->actions.rbegin(); it != session->actions.rend(); ++it) {
    if (!executor.Rollback(&*it)) {
      success = false;
      if (failure_detail.empty()) failure_detail = it->error_message;
    }
    std::string persist_error;
    if (!manager->Save(*session, &persist_error)) {
      success = false;
      failure_detail = "cannot persist rollback state: " + persist_error;
      break;
    }
  }
  if (!success) {
    session->state = SessionState::SafeMode;
    std::string safe_mode_error;
    if (!manager->Save(*session, &safe_mode_error)) {
      if (!failure_detail.empty()) failure_detail += "; ";
      failure_detail += "cannot persist Safe Mode: " + safe_mode_error;
    }
    std::cerr << "Safe Mode: one or more actions could not be restored";
    if (!failure_detail.empty()) std::cerr << ": " << failure_detail;
    std::cerr << ".\n";
    return 2;
  }
  auto verification = CaptureOne(config, &error);
  if (!verification) {
    session->state = SessionState::SafeMode;
    manager->Save(*session, &error);
    std::cerr << "Safe Mode: rollback succeeded but verification failed: "
              << error << '\n';
    return 2;
  }
  for (const auto& record : session->actions) {
    if (record.action.type != ActionType::StopServiceTemporary ||
        record.original_service_state != ServiceState::Running ||
        record.state != ActionState::RolledBack) {
      continue;
    }
    const auto service =
        windows::ServiceApi::Query(record.action.service_name);
    if (!service.success || service.service.state != ServiceState::Running ||
        !service.service.identity_verified ||
        service.service.identity_token !=
            record.action.expected_service_identity) {
      session->state = SessionState::SafeMode;
      std::string ignored;
      manager->Save(*session, &ignored);
      std::cerr << "Safe Mode: restored service verification failed for "
                << record.action.service_name << ".\n";
      return 2;
    }
  }
  std::filesystem::path report;
  SnapshotHistory report_history(1);
  if (optimized) {
    report_history = std::move(*optimized);
  } else {
    report_history.Add(*verification);
  }
  const auto unclosed =
      SessionManager::CollectUnclosedProcesses(*session, *verification);
  if (!report_evidence_captured) {
    report_diagnoses = DiagnosisEngine(config).Evaluate(report_history);
  }
  const std::string measurement_phase = report_evidence_captured
                                            ? "optimized_before_rollback"
                                            : "recovery_after_rollback";
  if (!manager->Complete(*session, report_history, report_diagnoses,
                         measurement_phase, &report, &error)) {
    std::cerr << "Safe Mode: " << error << '\n';
    return 2;
  }
  Logger::Instance().Write(LogLevel::Info, LogEvent::RecoveryCompleted);
  Logger::Instance().Write(LogLevel::Info, LogEvent::ReportWritten);
  std::cout << (recovery_command ? "Recovery completed.\n"
                                 : "Coding Mode exited.\n")
            << "Report: " << report.string() << '\n';
  if (!unclosed.empty()) {
    for (const auto& process : unclosed) {
      std::cout << "UNCLOSED_PROCESS pid=" << process.pid
                << " start=" << process.start_time_100ns
                << " name=" << process.name << '\n';
    }
    std::cout << unclosed.size()
              << " cleanup process(es) are still running after exit. Retry "
                 "with:\n  workboost coding retry-close";
    for (const auto& process : unclosed) {
      std::cout << " --close-process " << process.pid << ':'
                << process.start_time_100ns;
    }
    std::cout << '\n';
  }
  return 0;
}

int AcknowledgeRecovery(const Config& config, SessionManager* manager) {
  std::string error;
  auto session = manager->LoadActive(&error);
  if (!session) {
    if (manager->HasActiveSession()) {
      std::cerr << "Safe Mode: " << error << '\n';
      return 2;
    }
    std::cout << "No active Coding Mode session.\n";
    return 0;
  }

  bool has_uncertain_one_shot = false;
  for (const auto& record : session->actions) {
    const bool final_reversible_state =
        record.state == ActionState::RolledBack ||
        record.state == ActionState::Rejected ||
        record.state == ActionState::Failed;
    if (IsReversible(record.action.type) && !final_reversible_state) {
      std::cerr << "Safe Mode: reversible action " << record.action.id
                << " is not restored; run 'workboost recovery restore' first.\n";
      return 2;
    }
    if (!IsReversible(record.action.type) &&
        record.state == ActionState::Planned) {
      std::cerr << "Safe Mode: one-shot action " << record.action.id
                << " has not been assessed; run 'workboost recovery restore' "
                   "first.\n";
      return 2;
    }
    if (record.action.type == ActionType::GracefulCloseProcess &&
        record.state == ActionState::Uncertain) {
      has_uncertain_one_shot = true;
    }
  }
  if (!has_uncertain_one_shot) {
    std::cerr << "No uncertain one-shot action is awaiting acknowledgement; "
                 "run 'workboost recovery restore'.\n";
    return 2;
  }

  for (auto& record : session->actions) {
    if (record.action.type == ActionType::GracefulCloseProcess &&
        record.state == ActionState::Uncertain) {
      record.state = ActionState::Completed;
      record.result_message =
          "user acknowledged uncertain graceful-close outcome after recovery";
    }
  }
  session->state = SessionState::Recovering;
  if (!manager->Save(*session, &error)) {
    std::cerr << "Safe Mode: cannot persist acknowledgement: " << error << '\n';
    return 2;
  }
  auto verification = CaptureOne(config, &error);
  if (!verification) {
    session->state = SessionState::SafeMode;
    std::string ignored;
    manager->Save(*session, &ignored);
    std::cerr << "Safe Mode: acknowledgement was recorded but verification "
                 "failed: "
              << error << '\n';
    return 2;
  }
  std::filesystem::path report;
  SnapshotHistory verification_history(1);
  verification_history.Add(*verification);
  const auto diagnoses =
      DiagnosisEngine(config).Evaluate(verification_history);
  if (!manager->Complete(*session, verification_history, diagnoses,
                         "recovery_after_acknowledgement", &report, &error)) {
    std::cerr << "Safe Mode: " << error << '\n';
    return 2;
  }
  Logger::Instance().Write(LogLevel::Info, LogEvent::RecoveryCompleted);
  Logger::Instance().Write(LogLevel::Info, LogEvent::ReportWritten);
  std::cout << "Uncertain one-shot action acknowledged; no action was "
               "replayed.\nReport: "
            << report.string() << '\n';
  return 0;
}

int EnterCodingMode(const Config& config, const CliOptions& options,
                    SessionManager* manager) {
  if (options.baseline_seconds < 10 || options.baseline_seconds > 600) {
    std::cerr << "Coding Mode --baseline-duration must be between 10 and 600 "
                 "seconds.\n";
    return 64;
  }
  if (manager->HasActiveSession()) {
    std::cerr << "An unfinished session exists. Run 'workboost recovery status' "
                 "or 'workboost recovery restore'.\n";
    return 2;
  }
  std::string error;
  std::cout << "Capturing baseline for " << options.baseline_seconds
            << " second(s)...\n";
  auto history = CaptureHistory(config, options.baseline_seconds, &error);
  if (!history || history->Latest() == nullptr) {
    std::cerr << "Baseline failed: " << error << '\n';
    return 1;
  }
  const auto window_context =
      BuildWindowRuntimeContext(*history, config.remote_debug_ports);
  std::cout << "Baseline window: " << window_context.total_samples
            << " sample(s), " << window_context.complete_process_samples
            << " complete process sample(s), "
            << window_context.complete_tcp_samples
            << " complete TCP sample(s).\n";
  const auto& window_snapshots = history->Snapshots();
  SnapshotHistory planning_history(history->Size());
  for (std::size_t index = 0; index + 1 < window_snapshots.size(); ++index) {
    planning_history.Add(window_snapshots[index]);
  }
  SystemSnapshot planning_latest = window_snapshots.back();
  if (!AttachConfiguredServices(config, &planning_latest, &error)) {
    std::cerr << "Cannot build service protection inventory: " << error
              << '\n';
    return 1;
  }
  planning_history.Add(std::move(planning_latest));
  auto plan = OptimizationPlanner(config).Create(
      planning_history, options.cleanup_processes);
  const std::size_t safe_actions = static_cast<std::size_t>(std::count_if(
      plan.actions.begin(), plan.actions.end(), [](const auto& action) {
        return action.risk == ActionRisk::Safe;
      }));
  const std::size_t low_risk_actions = static_cast<std::size_t>(std::count_if(
      plan.actions.begin(), plan.actions.end(), [](const auto& action) {
        return action.risk == ActionRisk::Low;
      }));
  const std::size_t medium_risk_actions =
      plan.actions.size() - safe_actions - low_risk_actions;
  std::cout << "Plan: " << safe_actions << " safe, " << low_risk_actions
            << " explicitly allowed low-risk, and " << medium_risk_actions
            << " medium-risk action(s), "
            << plan.rejected.size() << " rejected candidate(s).\n";
  for (const auto& action : plan.actions) {
    const std::string target = action.type == ActionType::StopServiceTemporary
                                   ? action.service_name
                                   : action.process_name + " PID " +
                                         std::to_string(action.pid);
    std::cout << "  " << target << ": " << DescribeAction(action) << '\n';
  }
  if (options.dry_run) return 0;
  if (medium_risk_actions != 0 && !options.confirm_service_actions) {
    std::cerr << "Medium-risk service actions require the explicit "
                 "--confirm-service-actions option; no action was executed.\n";
    return 64;
  }
  for (auto& action : plan.actions) {
    if (action.type == ActionType::StopServiceTemporary) {
      action.explicit_confirmation = true;
    }
  }

  auto execution_snapshot = CaptureActionState(config, &error);
  if (!execution_snapshot) {
    std::cerr << "Cannot refresh protection state before execution: " << error
              << '\n';
    return 1;
  }

  OptimizationSession session = manager->Create(*history);
  if (!manager->Save(session, &error)) {
    std::cerr << "Cannot persist session before execution: " << error << '\n';
    return 2;
  }
  Logger::Instance().Write(LogLevel::Info, LogEvent::CodingModeStarted);
  ActionExecutor executor(config);
  const auto refresh_execution_snapshot = [&]() -> bool {
    execution_snapshot = CaptureActionState(config, &error);
    if (execution_snapshot) return true;
    session.state = SessionState::SafeMode;
    std::string persist_error;
    manager->Save(session, &persist_error);
    std::cerr << "Safe Mode: cannot refresh protection state before "
                 "the next action: "
              << error << '\n';
    return false;
  };
  const auto persist_and_report = [&](const ExecutedAction& result) -> int {
    if (!result.result_message.empty()) {
      std::cout << "  " << result.action.id << ": " << result.result_message
                << '\n';
    }
    if (result.state == ActionState::Failed ||
        result.state == ActionState::Uncertain ||
        result.state == ActionState::Rejected) {
      std::cerr << "  " << result.action.id << ": " << result.error_message
                << '\n';
      Logger::Instance().Write(
          result.state == ActionState::Rejected ? LogLevel::Warn
                                                : LogLevel::Error,
          result.state == ActionState::Rejected ? LogEvent::ActionRejected
                                                : LogEvent::ActionFailed,
          static_cast<std::uint32_t>(result.error_code));
    }
    if (result.state != ActionState::Uncertain) return 0;
    session.state = SessionState::SafeMode;
    std::string persist_error;
    if (!manager->Save(session, &persist_error)) {
      std::cerr << "Safe Mode persistence failed: " << persist_error << '\n';
    }
    std::cerr << "Safe Mode: an action outcome is uncertain; no further "
                 "actions will run.\n";
    return 2;
  };
  std::size_t action_index = 0;
  bool first_action = true;
  while (action_index < plan.actions.size()) {
    if (plan.actions[action_index].type !=
        ActionType::GracefulCloseProcess) {
      if (!first_action && !refresh_execution_snapshot()) return 2;
      first_action = false;
      const auto& action = plan.actions[action_index];
      ++action_index;
      ExecutedAction record;
      record.action = action;
      record.state = ActionState::Planned;
      // Reversible actions persist their restore value before the system
      // call. A planned one-shot action is instead recovered as Uncertain
      // and is never replayed automatically.
      if (action.type == ActionType::SetPriorityClass) {
        record.original_priority = action.source_priority;
        for (const auto& process : execution_snapshot->processes) {
          if (process.pid == action.pid &&
              process.start_time_100ns == action.expected_start_time_100ns) {
            record.original_priority = process.priority_class;
            break;
          }
        }
      } else if (action.type == ActionType::StopServiceTemporary) {
        record.original_service_state = action.source_service_state;
      }
      std::string preflight_reason;
      if (!SafetyValidator(config).Validate(action, *execution_snapshot,
                                            &preflight_reason)) {
        record.state = ActionState::Rejected;
        record.error_code = ERROR_ACCESS_DISABLED_BY_POLICY;
        record.error_message = preflight_reason;
        Logger::Instance().Write(LogLevel::Warn, LogEvent::ActionRejected,
                                 record.error_code);
        session.actions.push_back(record);
        if (!manager->Save(session, &error)) {
          std::cerr << "Safe Mode: cannot persist rejected action: " << error
                    << '\n';
          return 2;
        }
        std::cerr << "  " << record.action.id << ": " << record.error_message
                  << '\n';
        continue;
      }
      session.actions.push_back(record);
      if (!manager->Save(session, &error)) {
        std::cerr << "Safe Mode: cannot persist planned action: " << error
                  << '\n';
        return 2;
      }
      session.actions.back() = executor.Execute(action, *execution_snapshot);
      if (!manager->Save(session, &error)) {
        std::cerr << "Safe Mode: action state persistence failed: " << error
                  << '\n';
        return 2;
      }
      const auto result_status =
          persist_and_report(session.actions.back());
      if (result_status != 0) return result_status;
      continue;
    }

    // Contiguous graceful-close actions share one deadline and are validated
    // against a single freshly captured protection snapshot.
    std::vector<OptimizationAction> close_run;
    while (action_index < plan.actions.size() &&
           plan.actions[action_index].type ==
               ActionType::GracefulCloseProcess) {
      close_run.push_back(plan.actions[action_index]);
      ++action_index;
    }
    if (!first_action && !refresh_execution_snapshot()) return 2;
    first_action = false;
    std::vector<std::size_t> planned_indices;
    std::vector<OptimizationAction> validated_close_run;
    for (const auto& action : close_run) {
      ExecutedAction record;
      record.action = action;
      record.state = ActionState::Planned;
      std::string preflight_reason;
      if (!SafetyValidator(config).Validate(action, *execution_snapshot,
                                            &preflight_reason)) {
        record.state = ActionState::Rejected;
        record.error_code = ERROR_ACCESS_DISABLED_BY_POLICY;
        record.error_message = preflight_reason;
        Logger::Instance().Write(LogLevel::Warn, LogEvent::ActionRejected,
                                 record.error_code);
        session.actions.push_back(record);
        if (!manager->Save(session, &error)) {
          std::cerr << "Safe Mode: cannot persist rejected action: " << error
                    << '\n';
          return 2;
        }
        std::cerr << "  " << record.action.id << ": " << record.error_message
                  << '\n';
        continue;
      }
      planned_indices.push_back(session.actions.size());
      session.actions.push_back(record);
      if (!manager->Save(session, &error)) {
        std::cerr << "Safe Mode: cannot persist planned action: " << error
                  << '\n';
        return 2;
      }
      validated_close_run.push_back(action);
    }
    if (validated_close_run.empty()) continue;
    const auto batch_results = executor.ExecuteGracefulCloseBatch(
        validated_close_run, *execution_snapshot,
        config.coding_profile.graceful_close_batch_budget_ms);
    for (std::size_t batch_index = 0;
         batch_index < batch_results.size(); ++batch_index) {
      session.actions[planned_indices[batch_index]] =
          batch_results[batch_index];
      if (!manager->Save(session, &error)) {
        std::cerr << "Safe Mode: action state persistence failed: " << error
                  << '\n';
        return 2;
      }
      const auto result_status =
          persist_and_report(session.actions[planned_indices[batch_index]]);
      if (result_status != 0) return result_status;
    }
  }
  std::size_t applied = 0;
  std::size_t completed = 0;
  for (const auto& action : session.actions) {
    applied += action.state == ActionState::Applied ? 1U : 0U;
    completed += action.state == ActionState::Completed ? 1U : 0U;
  }
  std::cout << "Coding Mode active; " << applied
            << " reversible action(s) applied, " << completed
            << " one-shot action(s) completed.\n"
            << "Exit with: workboost coding exit\n";
  return 0;
}

int RetryCloseProcesses(const Config& config, const CliOptions& options,
                        SessionManager* manager) {
  if (options.cleanup_processes.empty()) {
    std::cerr << "retry-close requires at least one --close-process "
                 "PID:START_TIME_100NS.\n";
    return 64;
  }
  if (manager->HasActiveSession()) {
    std::cerr << "An unfinished Coding Mode session exists; exit or restore "
                 "it before retrying cleanup.\n";
    return 2;
  }
  std::string error;
  auto execution_snapshot = CaptureActionState(config, &error);
  if (!execution_snapshot) {
    std::cerr << "Cannot refresh protection state before retry: " << error
              << '\n';
    return 1;
  }
  SnapshotHistory baseline(1);
  baseline.Add(*execution_snapshot);
  auto plan = OptimizationPlanner(config).Create(baseline,
                                                 options.cleanup_processes);
  if (plan.actions.empty()) {
    for (const auto& rejected : plan.rejected) {
      std::cerr << "  rejected: " << rejected << '\n';
    }
    std::cerr << "No valid retry target; nothing was executed.\n";
    return 1;
  }

  OptimizationSession session = manager->Create(baseline);
  if (!manager->Save(session, &error)) {
    std::cerr << "Cannot persist retry session: " << error << '\n';
    return 2;
  }
  Logger::Instance().Write(LogLevel::Info, LogEvent::CodingModeStarted);
  ActionExecutor executor(config);
  std::vector<std::size_t> planned_indices;
  std::vector<OptimizationAction> validated;
  for (const auto& action : plan.actions) {
    ExecutedAction record;
    record.action = action;
    record.state = ActionState::Planned;
    std::string preflight_reason;
    if (!SafetyValidator(config).Validate(action, *execution_snapshot,
                                          &preflight_reason)) {
      record.state = ActionState::Rejected;
      record.error_code = ERROR_ACCESS_DISABLED_BY_POLICY;
      record.error_message = preflight_reason;
      Logger::Instance().Write(LogLevel::Warn, LogEvent::ActionRejected,
                               record.error_code);
      session.actions.push_back(record);
      if (!manager->Save(session, &error)) {
        std::cerr << "Safe Mode: cannot persist rejected retry: " << error
                  << '\n';
        return 2;
      }
      std::cerr << "  " << record.action.id << ": " << record.error_message
                << '\n';
      continue;
    }
    planned_indices.push_back(session.actions.size());
    session.actions.push_back(record);
    if (!manager->Save(session, &error)) {
      std::cerr << "Safe Mode: cannot persist planned retry: " << error
                << '\n';
      return 2;
    }
    validated.push_back(action);
  }

  bool uncertain = false;
  bool failed = false;
  if (!validated.empty()) {
    const auto batch_results = executor.ExecuteGracefulCloseBatch(
        validated, *execution_snapshot,
        config.coding_profile.graceful_close_batch_budget_ms);
    for (std::size_t index = 0; index < batch_results.size(); ++index) {
      session.actions[planned_indices[index]] = batch_results[index];
      if (!manager->Save(session, &error)) {
        std::cerr << "Safe Mode: retry state persistence failed: " << error
                  << '\n';
        return 2;
      }
      const auto& result = session.actions[planned_indices[index]];
      if (!result.result_message.empty()) {
        std::cout << "  " << result.action.id << ": "
                  << result.result_message << '\n';
      }
      if (result.state == ActionState::Failed ||
          result.state == ActionState::Uncertain ||
          result.state == ActionState::Rejected) {
        std::cerr << "  " << result.action.id << ": "
                  << result.error_message << '\n';
        Logger::Instance().Write(
            result.state == ActionState::Rejected ? LogLevel::Warn
                                                  : LogLevel::Error,
            result.state == ActionState::Rejected
                ? LogEvent::ActionRejected
                : LogEvent::ActionFailed,
            static_cast<std::uint32_t>(result.error_code));
      }
      if (result.state == ActionState::Uncertain) uncertain = true;
      if (result.state == ActionState::Failed) failed = true;
    }
  }
  if (uncertain) {
    session.state = SessionState::SafeMode;
    std::string persist_error;
    if (!manager->Save(session, &persist_error)) {
      std::cerr << "Safe Mode persistence failed: " << persist_error << '\n';
    }
    std::cerr << "Safe Mode: a retry outcome is uncertain; run 'workboost "
                 "recovery acknowledge' after reviewing the processes.\n";
    return 2;
  }

  auto verification = CaptureActionState(config, &error);
  if (!verification) {
    session.state = SessionState::SafeMode;
    manager->Save(session, &error);
    std::cerr << "Safe Mode: retry completed but verification failed: "
              << error << '\n';
    return 2;
  }
  SnapshotHistory report_history(1);
  report_history.Add(*verification);
  const auto diagnoses =
      DiagnosisEngine(config).Evaluate(report_history);
  std::filesystem::path report;
  if (!manager->Complete(session, report_history, diagnoses, "retry_close",
                         &report, &error)) {
    std::cerr << "Safe Mode: " << error << '\n';
    return 2;
  }
  Logger::Instance().Write(LogLevel::Info, LogEvent::RecoveryCompleted);
  Logger::Instance().Write(LogLevel::Info, LogEvent::ReportWritten);
  std::cout << (failed ? "Retry completed with failures.\n"
                       : "Retry completed.\n")
            << "Report: " << report.string() << '\n';
  return failed ? 1 : 0;
}

int RunDiagnose(const Config& config, const CliOptions& options) {
  Config recording_config = config;
  if (options.diagnostic_interval_ms >= 0) {
    if (options.diagnostic_interval_ms < 250 ||
        options.diagnostic_interval_ms > 500) {
      std::cerr << "Diagnostic --interval must be between 250 and 500 ms.\n";
      return 64;
    }
    recording_config.sample_interval_ms = options.diagnostic_interval_ms;
  }
  const int duration = options.duration_seconds < 0
                           ? recording_config.history_seconds
                           : options.duration_seconds;
  if (duration < 1 || duration > 3600) {
    std::cerr << "Diagnostic --duration must be between 1 and 3600 seconds.\n";
    return 64;
  }
  std::string error;
  auto history = CaptureHistory(recording_config, duration, &error);
  if (!history) {
    std::cerr << "Diagnostic capture failed: " << error << '\n';
    return 1;
  }
  const auto diagnoses = DiagnosisEngine(recording_config).Evaluate(*history);
  const std::string report = options.json
                                 ? DiagnosisJson(
                                       diagnoses, *history,
                                       recording_config.sample_interval_ms)
                                 : DiagnosisText(
                                       diagnoses, *history,
                                       recording_config.sample_interval_ms);
  if (options.output_path) {
    if (!windows::AtomicWriteUtf8(*options.output_path, report, &error)) {
      std::cerr << "Cannot write report: " << error << '\n';
      return 1;
    }
    std::cout << "Report: " << options.output_path->string() << '\n';
  } else {
    std::cout << report;
  }
  return 0;
}

bool ValidBenchmarkLabel(const std::string& value) {
  return !value.empty() && value.size() <= 64 &&
         std::none_of(value.begin(), value.end(), [](unsigned char character) {
           return character < 0x20;
         });
}

bool AllBenchmarkRunsSucceeded(const StartupBenchmarkSummary& summary) {
  return summary.observations.size() == summary.requested_runs &&
         std::all_of(summary.observations.begin(), summary.observations.end(),
                     [](const StartupObservation& observation) {
                       return observation.status ==
                              StartupObservationStatus::Succeeded;
                     });
}

StartupBenchmarkSummary ObserveStartupBenchmarkPhase(
    const std::string& process_name, const std::string& phase_label,
    int duration_seconds, int runs) {
  std::vector<StartupObservation> observations;
  observations.reserve(static_cast<std::size_t>(runs));
  for (int run = 1; run <= runs; ++run) {
    const auto observation = windows::StartupBenchmarkApi::ObserveNewProcess(
        process_name,
        static_cast<std::uint32_t>(duration_seconds * 1000),
        [&]() {
          std::cerr << "Benchmark " << phase_label << " run " << run << '/'
                    << runs << ": start a new " << process_name
                    << " process within " << duration_seconds
                    << " seconds. Existing instances are ignored.\n";
        });
    observations.push_back(observation);
    if (observation.status != StartupObservationStatus::Succeeded) break;
  }
  return BenchmarkManager::Summarize(
      phase_label, process_name, windows::Iso8601Now(),
      static_cast<std::size_t>(runs), std::move(observations));
}

int EmitBenchmarkReport(const CliOptions& options, const std::string& report) {
  if (options.output_path) {
    std::string error;
    if (!windows::AtomicWriteUtf8(*options.output_path, report, &error)) {
      std::cerr << "Cannot write benchmark report: " << error << '\n';
      return 1;
    }
    std::cout << "Report: " << options.output_path->string() << '\n';
  } else {
    std::cout << report;
  }
  return 0;
}

int RunStartupBenchmark(const Config& config, const CliOptions& options,
                        const std::string& process_name) {
  const ProcessRule rule = config.RuleFor(process_name);
  if (rule.process_class != ProcessClass::Development) {
    std::cerr << "Startup benchmark target must be a configured Development "
                 "process.\n";
    return 64;
  }
  const int duration =
      options.duration_seconds < 0 ? 60 : options.duration_seconds;
  if (duration < 1 || duration > 600 || options.benchmark_runs < 1 ||
      options.benchmark_runs > 5 ||
      !ValidBenchmarkLabel(options.benchmark_label)) {
    std::cerr << "Benchmark requires duration 1..600 seconds, runs 1..5, and "
                 "a non-empty label of at most 64 characters.\n";
    return 64;
  }

  const auto summary = ObserveStartupBenchmarkPhase(
      process_name, options.benchmark_label, duration,
      options.benchmark_runs);
  const std::string report =
      options.json ? BenchmarkManager::Json(summary)
                   : BenchmarkManager::Text(summary);
  if (EmitBenchmarkReport(options, report) != 0) return 1;
  return AllBenchmarkRunsSucceeded(summary) ? 0 : 1;
}

int RunStartupBenchmarkComparison(const Config& config,
                                  const CliOptions& options,
                                  const std::string& process_name) {
  const ProcessRule rule = config.RuleFor(process_name);
  if (rule.process_class != ProcessClass::Development) {
    std::cerr << "Startup benchmark target must be a configured Development "
                 "process.\n";
    return 64;
  }
  const int duration =
      options.duration_seconds < 0 ? 60 : options.duration_seconds;
  const int runs =
      options.benchmark_runs_specified ? options.benchmark_runs : 3;
  const std::string label = options.benchmark_label_specified
                                ? options.benchmark_label
                                : "comparison";
  if (duration < 1 || duration > 600 || runs < 1 || runs > 5 ||
      !ValidBenchmarkLabel(label)) {
    std::cerr << "Benchmark requires duration 1..600 seconds, runs 1..5, and "
                 "a non-empty label of at most 64 characters.\n";
    return 64;
  }

  auto baseline = ObserveStartupBenchmarkPhase(
      process_name, label + "/baseline", duration, runs);
  StartupBenchmarkSummary optimized = BenchmarkManager::Summarize(
      label + "/optimized", process_name, windows::Iso8601Now(),
      static_cast<std::size_t>(runs), {});
  if (AllBenchmarkRunsSucceeded(baseline)) {
    std::cerr
        << "Baseline complete. Apply the environment change you want to test, "
           "then press Enter to record the optimized phase. WorkBoost will "
           "not change or launch the target.\n";
    std::string ready;
    if (std::getline(std::cin, ready)) {
      optimized = ObserveStartupBenchmarkPhase(
          process_name, label + "/optimized", duration, runs);
    } else {
      std::cerr << "Optimized phase was not confirmed; comparison is "
                   "incomplete.\n";
    }
  }

  const auto comparison = BenchmarkManager::Compare(
      label, windows::Iso8601Now(), std::move(baseline),
      std::move(optimized));
  const std::string report =
      options.json ? BenchmarkManager::ComparisonJson(comparison)
                   : BenchmarkManager::ComparisonText(comparison);
  if (EmitBenchmarkReport(options, report) != 0) return 1;
  return AllBenchmarkRunsSucceeded(comparison.baseline) &&
                 AllBenchmarkRunsSucceeded(comparison.optimized) &&
                 comparison.delta_visible_window_ms &&
                 comparison.delta_responsive_window_ms
             ? 0
             : 1;
}

}  // namespace

int RunCli(int argc, char* argv[]) {
  std::vector<std::string> raw;
  for (int i = 1; i < argc; ++i) raw.emplace_back(argv[i]);
  CliOptions options;
  std::vector<std::string> positional;
  std::string error;
  if (!ParseOptions(raw, &positional, &options, &error)) {
    std::cerr << error << "\n\n";
    PrintUsage();
    return 64;
  }
  const bool cleanup_command =
      positional.size() == 2 && positional[0] == "coding" &&
      (positional[1] == "enter" || positional[1] == "retry-close");
  if (!options.cleanup_processes.empty() && !cleanup_command) {
    std::cerr << "--close-process is only valid with 'coding enter' or "
                 "'coding retry-close'.\n";
    return 64;
  }
  // The dashboard detaches from the console so it keeps running without a
  // terminal window; one-shot CLI commands keep the console for their output.
  const bool dashboard_invocation =
      positional.empty() || positional[0] == "gui";
  if (dashboard_invocation) FreeConsole();
  const auto data_directory = windows::LocalAppDataDirectory();
  std::string logger_error;
  if (!Logger::Instance().Initialize(data_directory, LogLevel::Info,
                                     &logger_error)) {
    std::cerr << "Warning: local event logging is unavailable.\n";
  }
  Logger::Instance().Write(LogLevel::Info, LogEvent::ApplicationStarted);
  struct ApplicationLogScope {
    ~ApplicationLogScope() {
      Logger::Instance().Write(LogLevel::Info,
                               LogEvent::ApplicationStopped);
    }
  } application_log_scope;
  if (positional.empty()) {
    return gui::RunDashboard(LoadConfig(options));
  }
  if (positional[0] == "help") {
    PrintUsage();
    return 0;
  }

  Config config = LoadConfig(options);
  SessionManager sessions(data_directory);
  const std::string& command = positional[0];

  const bool recovery_path =
      command == "recovery" ||
      (command == "coding" && positional.size() == 2 &&
       positional[1] == "exit");
  if (sessions.HasActiveSession() && !recovery_path) {
    Logger::Instance().Write(LogLevel::Warn, LogEvent::SafeModeEntered);
    std::cerr << "Safe Mode: an unfinished Coding Mode session exists; "
                 "monitoring remains available, but new actions are blocked.\n";
  }

  if (command == "profile") {
    if (positional.size() == 3 && positional[1] == "show" &&
        positional[2] == "coding") {
      PrintProfile(config, options.json);
      return 0;
    }
    std::cerr << "Usage: workboost profile show coding\n";
    return 64;
  }
  if (command == "recovery") {
    if (positional.size() != 2) {
      std::cerr << "Usage: workboost recovery <status|restore|acknowledge>\n";
      return 64;
    }
    if (positional[1] == "status") {
      if (!sessions.HasActiveSession()) {
        std::cout << "Recovery not required.\n";
        return 0;
      }
      auto session = sessions.LoadActive(&error);
      if (!session) {
        std::cerr << "Safe Mode: " << error << '\n';
        return 2;
      }
      std::cout << "Recovery required for session " << session->session_id
                << " (" << ToString(session->state) << ", "
                << session->actions.size() << " recorded action(s)).\n";
      for (const auto& record : session->actions) {
        if (record.state == ActionState::Uncertain) {
          std::cout << "  Uncertain: " << record.action.id << " ("
                    << ToString(record.action.type) << ")\n";
        }
      }
      return 2;
    }
    if (positional[1] == "restore")
      return RestoreSession(config, &sessions, true);
    if (positional[1] == "acknowledge")
      return AcknowledgeRecovery(config, &sessions);
    std::cerr << "Usage: workboost recovery <status|restore|acknowledge>\n";
    return 64;
  }
  if (command == "coding") {
    if (positional.size() != 2 ||
        (positional[1] != "enter" && positional[1] != "exit" &&
         positional[1] != "retry-close")) {
      std::cerr << "Usage: workboost coding <enter|exit|retry-close>\n";
      return 64;
    }
    if (positional[1] == "enter")
      return EnterCodingMode(config, options, &sessions);
    if (positional[1] == "retry-close")
      return RetryCloseProcesses(config, options, &sessions);
    return RestoreSession(config, &sessions, false);
  }
  if (command == "gui") {
    if (positional.size() != 1) {
      std::cerr << "Usage: workboost gui\n";
      return 64;
    }
    return gui::RunDashboard(config);
  }
  if (command == "diagnose") {
    if (positional.size() != 1) {
      std::cerr << "Usage: workboost diagnose [--duration SECONDS] "
                   "[--interval 250..500] [--output FILE] [--json]\n";
      return 64;
    }
    return RunDiagnose(config, options);
  }
  if (command == "benchmark") {
    if (positional.size() != 3 ||
        (positional[1] != "observe" && positional[1] != "compare")) {
      std::cerr << "Usage: workboost benchmark <observe|compare> "
                   "<process.exe> [--duration SECONDS] [--runs N] "
                   "[--label LABEL] [--output FILE] [--json]\n";
      return 64;
    }
    if (positional[1] == "compare") {
      return RunStartupBenchmarkComparison(config, options, positional[2]);
    }
    return RunStartupBenchmark(config, options, positional[2]);
  }
  if (command == "services") {
    if (positional.size() != 1) {
      std::cerr << "Usage: workboost services [--limit N] [--json]\n";
      return 64;
    }
    const auto services = windows::ServiceApi::QueryAll();
    if (!services.success) {
      std::cerr << "Service enumeration failed: "
                << services.error.Describe() << '\n';
      return 1;
    }
    PrintServices(services.services, config, options);
    return 0;
  }
  if (command == "serial") {
    if (positional.size() != 1) {
      std::cerr << "Usage: workboost serial [--json]\n";
      return 64;
    }
    const auto ports = windows::SerialPortApi::QueryPresent();
    if (!ports.success) {
      std::cerr << "Serial port enumeration failed: "
                << ports.error.Describe() << '\n';
      return 1;
    }
    PrintSerialPorts(ports.ports, options.json);
    return 0;
  }
  if (command == "startup") {
    if (positional.size() != 1) {
      std::cerr << "Usage: workboost startup [--json]\n";
      return 64;
    }
    const auto startup = windows::StartupApi::QueryAll();
    if (!startup.success) {
      std::cerr << "Startup enumeration failed: "
                << startup.error.Describe() << '\n';
      return 1;
    }
    PrintStartupEntries(startup.entries, config, options.json);
    return 0;
  }

  if ((command == "status" || command == "connections" ||
       command == "protected") &&
      positional.size() != 1) {
    std::cerr << "Usage: workboost " << command << " [options]\n";
    return 64;
  }
  if (command == "top" &&
      (positional.size() != 2 ||
       (positional[1] != "cpu" && positional[1] != "mem" &&
        positional[1] != "io"))) {
    std::cerr << "Usage: workboost top <cpu|mem|io>\n";
    return 64;
  }
  if (command != "status" && command != "connections" &&
      command != "protected" && command != "top") {
    std::cerr << "Unknown command: " << command << "\n\n";
    PrintUsage();
    return 64;
  }

  auto snapshot = CaptureOne(config, &error);
  if (!snapshot) {
    std::cerr << "System sampling failed: " << error << '\n';
    return 1;
  }
  if (command == "status") {
    PrintStatus(*snapshot, config, options);
    return 0;
  }
  if (command == "connections") {
    PrintConnections(*snapshot, options);
    return 0;
  }
  if (command == "protected") {
    PrintProtected(*snapshot, options);
    return 0;
  }
  if (command == "top") {
    PrintTop(*snapshot, positional[1], options);
    return 0;
  }
  return 0;
}

}  // namespace workboost
