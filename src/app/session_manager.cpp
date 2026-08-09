#include "app/session_manager.h"

#include "platform/windows/windows_utils.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <type_traits>

namespace workboost {
namespace {

std::optional<std::size_t> FieldValueStart(const std::string& json,
                                           const std::string& key) {
  std::size_t first = 0;
  while (first < json.size() &&
         std::isspace(static_cast<unsigned char>(json[first]))) {
    ++first;
  }
  if (first >= json.size() || json[first] != '{') return std::nullopt;
  int depth = 0;
  for (std::size_t i = first; i < json.size(); ++i) {
    if (json[i] == '{' || json[i] == '[') {
      ++depth;
      continue;
    }
    if (json[i] == '}' || json[i] == ']') {
      --depth;
      continue;
    }
    if (json[i] != '"') continue;
    const std::size_t begin = i + 1;
    bool escaped = false;
    ++i;
    for (; i < json.size(); ++i) {
      if (escaped) {
        escaped = false;
      } else if (json[i] == '\\') {
        escaped = true;
      } else if (json[i] == '"') {
        break;
      }
    }
    if (i >= json.size()) return std::nullopt;
    if (depth != 1 || escaped || i - begin != key.size() ||
        json.compare(begin, key.size(), key) != 0) {
      continue;
    }
    std::size_t value = i + 1;
    while (value < json.size() &&
           std::isspace(static_cast<unsigned char>(json[value]))) {
      ++value;
    }
    if (value >= json.size() || json[value] != ':') continue;
    ++value;
    while (value < json.size() &&
           std::isspace(static_cast<unsigned char>(json[value]))) {
      ++value;
    }
    return value;
  }
  return std::nullopt;
}

std::optional<std::string> DelimitedField(const std::string& json,
                                          const std::string& key, char open,
                                          char close) {
  const auto start = FieldValueStart(json, key);
  if (!start || *start >= json.size() || json[*start] != open) {
    return std::nullopt;
  }
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = *start; i < json.size(); ++i) {
    const char character = json[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_string = false;
      }
      continue;
    }
    if (character == '"') {
      in_string = true;
    } else if (character == open) {
      ++depth;
    } else if (character == close && --depth == 0) {
      return json.substr(*start, i - *start + 1);
    }
  }
  return std::nullopt;
}

int HexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool IsJsonValueDelimiter(char value) {
  return value == ',' || value == '}' || value == ']' ||
         std::isspace(static_cast<unsigned char>(value)) != 0;
}

bool AppendUtf8CodeUnit(unsigned int value, std::string* output) {
  if (value <= 0x7f) {
    output->push_back(static_cast<char>(value));
  } else if (value <= 0x7ff) {
    output->push_back(static_cast<char>(0xc0 | (value >> 6)));
    output->push_back(static_cast<char>(0x80 | (value & 0x3f)));
  } else if (value < 0xd800 || value > 0xdfff) {
    output->push_back(static_cast<char>(0xe0 | (value >> 12)));
    output->push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
    output->push_back(static_cast<char>(0x80 | (value & 0x3f)));
  } else {
    return false;
  }
  return true;
}

std::optional<std::string> StringField(const std::string& json,
                                       const std::string& key) {
  const auto start = FieldValueStart(json, key);
  if (!start || *start >= json.size() || json[*start] != '"') {
    return std::nullopt;
  }
  std::string result;
  for (std::size_t i = *start + 1; i < json.size(); ++i) {
    const char value = json[i];
    if (value == '"') return result;
    if (value != '\\') {
      if (static_cast<unsigned char>(value) < 0x20) return std::nullopt;
      result.push_back(value);
      continue;
    }
    if (++i >= json.size()) return std::nullopt;
    switch (json[i]) {
      case '"': result.push_back('"'); break;
      case '\\': result.push_back('\\'); break;
      case '/': result.push_back('/'); break;
      case 'b': result.push_back('\b'); break;
      case 'f': result.push_back('\f'); break;
      case 'n': result.push_back('\n'); break;
      case 'r': result.push_back('\r'); break;
      case 't': result.push_back('\t'); break;
      case 'u': {
        if (i + 4 >= json.size()) return std::nullopt;
        unsigned int code_unit = 0;
        for (int digit = 0; digit < 4; ++digit) {
          const int hex = HexValue(json[++i]);
          if (hex < 0) return std::nullopt;
          code_unit = (code_unit << 4) | static_cast<unsigned int>(hex);
        }
        if (!AppendUtf8CodeUnit(code_unit, &result)) return std::nullopt;
        break;
      }
      default: return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<std::uint64_t> IntegerField(const std::string& json,
                                          const std::string& key) {
  const auto start = FieldValueStart(json, key);
  if (!start || *start >= json.size() || json[*start] < '0' ||
      json[*start] > '9') {
    return std::nullopt;
  }
  std::size_t end = *start;
  while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
  if (end < json.size() && !IsJsonValueDelimiter(json[end])) {
    return std::nullopt;
  }
  try {
    return std::stoull(json.substr(*start, end - *start));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<bool> BoolField(const std::string& json,
                              const std::string& key) {
  const auto start = FieldValueStart(json, key);
  if (!start) return std::nullopt;
  if (json.compare(*start, 4, "true") == 0 &&
      (*start + 4 == json.size() ||
       IsJsonValueDelimiter(json[*start + 4]))) {
    return true;
  }
  if (json.compare(*start, 5, "false") == 0 &&
      (*start + 5 == json.size() ||
       IsJsonValueDelimiter(json[*start + 5]))) {
    return false;
  }
  return std::nullopt;
}

std::optional<double> DoubleField(const std::string& json,
                                  const std::string& key) {
  const auto start = FieldValueStart(json, key);
  if (!start || *start >= json.size()) return std::nullopt;
  try {
    std::size_t consumed = 0;
    const double result = std::stod(json.substr(*start), &consumed);
    const std::size_t end = *start + consumed;
    if (consumed == 0 || !std::isfinite(result) ||
        (end < json.size() && !IsJsonValueDelimiter(json[end]))) {
      return std::nullopt;
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> ArrayBody(const std::string& json,
                                     const std::string& key) {
  const auto value = DelimitedField(json, key, '[', ']');
  if (!value) return std::nullopt;
  return value->substr(1, value->size() - 2);
}

std::vector<std::string> Objects(const std::string& body) {
  std::vector<std::string> result;
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  std::size_t start = 0;
  for (std::size_t i = 0; i < body.size(); ++i) {
    const char ch = body[i];
    if (in_string) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') in_string = false;
      continue;
    }
    if (ch == '"') in_string = true;
    else if (ch == '{') {
      if (depth++ == 0) start = i;
    } else if (ch == '}' && --depth == 0) {
      result.push_back(body.substr(start, i - start + 1));
    }
  }
  return result;
}

std::string SessionId() {
  FILETIME time{};
  GetSystemTimeAsFileTime(&time);
  ULARGE_INTEGER value{};
  value.LowPart = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  std::ostringstream output;
  output << std::hex << value.QuadPart << '-' << GetCurrentProcessId();
  return output.str();
}

std::string SerializeBenchmark(const BenchmarkPoint& point, int indent) {
  const std::string spacing(static_cast<std::size_t>(indent), ' ');
  std::ostringstream output;
  output << std::setprecision(12) << "{\n"
         << spacing << "  \"timestamp\": \""
         << windows::JsonEscape(point.timestamp) << "\",\n"
         << spacing << "  \"sample_count\": " << point.sample_count
         << ",\n"
         << spacing << "  \"observed_span_ms\": "
         << point.observed_span_ms << ",\n"
         << spacing << "  \"process_inventory_complete_samples\": "
         << point.process_inventory_complete_samples << ",\n"
         << spacing << "  \"tcp_inventory_complete_samples\": "
         << point.tcp_inventory_complete_samples << ",\n"
         << spacing << "  \"protection_inventory_complete_samples\": "
         << point.protection_inventory_complete_samples << ",\n"
         << spacing << "  \"cpu_percent\": " << point.cpu_percent << ",\n"
         << spacing << "  \"available_memory_bytes\": "
         << point.available_memory_bytes << ",\n"
         << spacing << "  \"commit_ratio\": " << point.commit_ratio << ",\n"
         << spacing << "  \"maximum_disk_active_ratio\": "
         << point.maximum_disk_active_ratio << ",\n"
         << spacing << "  \"maximum_disk_latency_ms\": "
         << point.maximum_disk_latency_ms << ",\n"
         << spacing << "  \"maximum_disk_queue_length\": "
         << point.maximum_disk_queue_length << ",\n"
         << spacing << "  \"maximum_ssd_active_ratio\": "
         << point.maximum_ssd_active_ratio << ",\n"
         << spacing << "  \"maximum_ssd_latency_ms\": "
         << point.maximum_ssd_latency_ms << ",\n"
         << spacing << "  \"maximum_hdd_active_ratio\": "
         << point.maximum_hdd_active_ratio << ",\n"
         << spacing << "  \"maximum_hdd_latency_ms\": "
         << point.maximum_hdd_latency_ms << ",\n"
         << spacing << "  \"page_reads_per_sec\": "
         << point.page_reads_per_sec << ",\n"
         << spacing << "  \"development_cpu_percent\": "
         << point.development_cpu_percent << ",\n"
         << spacing << "  \"development_working_set_bytes\": "
         << point.development_working_set_bytes << ",\n"
         << spacing << "  \"top_background_io_process\": \""
         << windows::JsonEscape(point.top_background_io_process) << "\",\n"
         << spacing << "  \"top_background_io_bytes_per_sec\": "
         << point.top_background_io_bytes_per_sec << "\n"
         << spacing << '}';
  return output.str();
}

std::string SerializeEvidenceValue(const EvidenceValue& value) {
  return std::visit(
      [](const auto& item) {
        using Value = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Value, std::string>) {
          return std::string("\"") + windows::JsonEscape(item) + '"';
        } else if constexpr (std::is_same_v<Value, bool>) {
          return std::string(item ? "true" : "false");
        } else {
          std::ostringstream output;
          output << std::setprecision(12) << item;
          return output.str();
        }
      },
      value);
}

bool IsWorkloadClass(ProcessClass value) {
  return value == ProcessClass::Development ||
         value == ProcessClass::RemoteTerminal ||
         value == ProcessClass::SerialTerminal ||
         value == ProcessClass::PacketCapture ||
         value == ProcessClass::BuildTool ||
         value == ProcessClass::VersionControl;
}

bool IsProtectedLevel(ProtectionLevel value) {
  return value == ProtectionLevel::SystemCritical ||
         value == ProtectionLevel::Strong ||
         value == ProtectionLevel::UserExplicit;
}

struct ProtectedWorkloadSummary {
  std::string process_name;
  ProcessClass process_class{ProcessClass::Unknown};
  ProtectionLevel protection{ProtectionLevel::Strong};
  std::size_t instances{};
};

std::vector<ProtectedWorkloadSummary> ProtectedWorkloads(
    const SystemSnapshot& snapshot) {
  std::map<std::string, ProtectedWorkloadSummary> grouped;
  for (const auto& process : snapshot.processes) {
    if (!IsWorkloadClass(process.classification) ||
        !IsProtectedLevel(process.protection_level)) {
      continue;
    }
    const std::string key = ToLowerAscii(process.name) + '|' +
                            ToString(process.classification) + '|' +
                            ToString(process.protection_level);
    auto& summary = grouped[key];
    summary.process_name = process.name;
    summary.process_class = process.classification;
    summary.protection = process.protection_level;
    ++summary.instances;
  }
  std::vector<ProtectedWorkloadSummary> result;
  result.reserve(grouped.size());
  for (auto& [key, summary] : grouped) {
    (void)key;
    result.push_back(std::move(summary));
  }
  return result;
}

bool ValidServiceName(const std::string& value) {
  if (value.empty() || value.size() > 256) return false;
  return std::none_of(value.begin(), value.end(), [](unsigned char character) {
    return character < 0x20 || character == '\\' || character == '/';
  });
}

bool ValidServiceIdentity(const std::string& value) {
  return value.size() == 16 &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

}  // namespace

SessionManager::SessionManager(std::filesystem::path root_directory)
    : root_directory_(std::move(root_directory)) {}

const std::filesystem::path& SessionManager::RootDirectory() const {
  return root_directory_;
}

std::filesystem::path SessionManager::ActiveSessionPath() const {
  return root_directory_ / "state" / "active_session.json";
}

bool SessionManager::HasActiveSession() const {
  std::error_code error;
  const bool exists = std::filesystem::exists(ActiveSessionPath(), error);
  return error ? true : exists;
}

OptimizationSession SessionManager::Create(
    const SystemSnapshot& baseline) const {
  SnapshotHistory history(1);
  history.Add(baseline);
  return Create(history);
}

OptimizationSession SessionManager::Create(
    const SnapshotHistory& baseline) const {
  OptimizationSession session;
  session.session_id = SessionId();
  session.state = SessionState::Active;
  session.start_time = windows::Iso8601Now();
  session.baseline = MakeBenchmarkPoint(baseline);
  return session;
}

bool SessionManager::Save(const OptimizationSession& session,
                          std::string* error) const {
  return windows::AtomicWriteUtf8(ActiveSessionPath(), Serialize(session), error);
}

std::optional<OptimizationSession> SessionManager::LoadActive(
    std::string* error) const {
  const auto content = windows::ReadUtf8(ActiveSessionPath(), error);
  if (!content) return std::nullopt;
  return Deserialize(*content, error);
}

bool SessionManager::Complete(OptimizationSession session,
                              const SystemSnapshot& after,
                              const std::vector<DiagnosisResult>& diagnoses,
                              const std::string& measurement_phase,
                              std::filesystem::path* report_path,
                              std::string* error) const {
  SnapshotHistory history(1);
  history.Add(after);
  return Complete(std::move(session), history, diagnoses, measurement_phase,
                  report_path, error);
}

bool SessionManager::Complete(OptimizationSession session,
                              const SnapshotHistory& after,
                              const std::vector<DiagnosisResult>& diagnoses,
                              const std::string& measurement_phase,
                              std::filesystem::path* report_path,
                              std::string* error) const {
  const std::string session_id = session.session_id;
  const std::string report = CompletionReport(
      std::move(session), after, diagnoses, measurement_phase);
  const auto path = root_directory_ / "reports" / (session_id + ".json");
  if (!windows::AtomicWriteUtf8(path, report, error)) return false;
  std::error_code ec;
  std::filesystem::remove(ActiveSessionPath(), ec);
  if (ec) {
    if (error) *error = "report written but active session could not be removed: " +
                        ec.message();
    return false;
  }
  if (report_path) *report_path = path;
  return true;
}

std::string SessionManager::CompletionReport(
    OptimizationSession session, const SystemSnapshot& after,
    const std::vector<DiagnosisResult>& diagnoses,
    const std::string& measurement_phase) {
  SnapshotHistory history(1);
  history.Add(after);
  return CompletionReport(std::move(session), history, diagnoses,
                          measurement_phase);
}

std::string SessionManager::CompletionReport(
    OptimizationSession session, const SnapshotHistory& after,
    const std::vector<DiagnosisResult>& diagnoses,
    const std::string& measurement_phase) {
  session.state = SessionState::Completed;
  const BenchmarkPoint final_point = MakeBenchmarkPoint(after);
  const SystemSnapshot empty_snapshot;
  const SystemSnapshot& latest =
      after.Latest() == nullptr ? empty_snapshot : *after.Latest();
  const bool protected_workload_inventory_complete =
      latest.process_inventory_complete && latest.tcp_inventory_complete;
  const auto protected_workloads = ProtectedWorkloads(latest);
  std::size_t planned = 0;
  std::size_t applied = 0;
  std::size_t completed = 0;
  std::size_t rolled_back = 0;
  std::size_t uncertain = 0;
  std::size_t failed = 0;
  std::size_t rejected = 0;
  bool rollback_complete = true;
  for (const auto& action : session.actions) {
    switch (action.state) {
      case ActionState::Planned: ++planned; break;
      case ActionState::Applied: ++applied; break;
      case ActionState::Completed: ++completed; break;
      case ActionState::RolledBack: ++rolled_back; break;
      case ActionState::Uncertain: ++uncertain; break;
      case ActionState::Failed: ++failed; break;
      case ActionState::Rejected: ++rejected; break;
    }
    if ((IsReversible(action.action.type) &&
         action.state != ActionState::RolledBack &&
         action.state != ActionState::Rejected &&
         action.state != ActionState::Failed) ||
        (!IsReversible(action.action.type) &&
         (action.state == ActionState::Planned ||
          action.state == ActionState::Uncertain))) {
      rollback_complete = false;
    }
  }

  std::string serialized = Serialize(session);
  if (!serialized.empty() && serialized.back() == '\n') serialized.pop_back();
  if (!serialized.empty() && serialized.back() == '}') serialized.pop_back();
  std::ostringstream report;
  report << std::setprecision(12) << serialized << ",\n"
         << "  \"report_schema_version\": 1,\n"
         << "  \"measurement_phase\": \""
         << windows::JsonEscape(measurement_phase) << "\",\n"
         << "  \"optimized\": " << SerializeBenchmark(final_point, 2)
         << ",\n"
         << "  \"delta\": {\n"
         << "    \"cpu_percent\": "
         << final_point.cpu_percent - session.baseline.cpu_percent << ",\n"
         << "    \"available_memory_bytes\": "
         << static_cast<std::int64_t>(final_point.available_memory_bytes) -
                static_cast<std::int64_t>(session.baseline.available_memory_bytes)
         << ",\n"
         << "    \"commit_ratio\": "
         << final_point.commit_ratio - session.baseline.commit_ratio << ",\n"
         << "    \"maximum_disk_active_ratio\": "
         << final_point.maximum_disk_active_ratio -
                session.baseline.maximum_disk_active_ratio
         << ",\n"
         << "    \"maximum_disk_latency_ms\": "
         << final_point.maximum_disk_latency_ms -
                session.baseline.maximum_disk_latency_ms
         << ",\n"
         << "    \"maximum_disk_queue_length\": "
         << final_point.maximum_disk_queue_length -
                session.baseline.maximum_disk_queue_length
         << ",\n"
         << "    \"maximum_ssd_active_ratio\": "
         << final_point.maximum_ssd_active_ratio -
                session.baseline.maximum_ssd_active_ratio
         << ",\n"
         << "    \"maximum_ssd_latency_ms\": "
         << final_point.maximum_ssd_latency_ms -
                session.baseline.maximum_ssd_latency_ms
         << ",\n"
         << "    \"maximum_hdd_active_ratio\": "
         << final_point.maximum_hdd_active_ratio -
                session.baseline.maximum_hdd_active_ratio
         << ",\n"
         << "    \"maximum_hdd_latency_ms\": "
         << final_point.maximum_hdd_latency_ms -
                session.baseline.maximum_hdd_latency_ms
         << ",\n"
         << "    \"page_reads_per_sec\": "
         << final_point.page_reads_per_sec -
                session.baseline.page_reads_per_sec
         << ",\n"
         << "    \"development_cpu_percent\": "
         << final_point.development_cpu_percent -
                session.baseline.development_cpu_percent
         << ",\n"
         << "    \"development_working_set_bytes\": "
         << static_cast<std::int64_t>(
                final_point.development_working_set_bytes) -
                static_cast<std::int64_t>(
                    session.baseline.development_working_set_bytes)
         << ",\n"
         << "    \"top_background_io_bytes_per_sec\": "
         << final_point.top_background_io_bytes_per_sec -
                session.baseline.top_background_io_bytes_per_sec
         << "\n  },\n"
         << "  \"protected_workload_inventory_complete\": "
         << (protected_workload_inventory_complete ? "true" : "false")
         << ",\n"
         << "  \"protected_workload\": [";
  for (std::size_t i = 0; i < protected_workloads.size(); ++i) {
    const auto& workload = protected_workloads[i];
    report << (i == 0 ? "\n" : ",\n")
           << "    {\"process_name\": \""
           << windows::JsonEscape(workload.process_name)
           << "\", \"class\": \"" << ToString(workload.process_class)
           << "\", \"protection\": \"" << ToString(workload.protection)
           << "\", \"instances\": " << workload.instances << '}';
  }
  if (!protected_workloads.empty()) report << '\n';
  report << "  ],\n  \"diagnoses\": [";
  for (std::size_t i = 0; i < diagnoses.size(); ++i) {
    const auto& diagnosis = diagnoses[i];
    report << (i == 0 ? "\n" : ",\n")
           << "    {\"type\": \"" << windows::JsonEscape(diagnosis.type)
           << "\", \"severity\": \"" << ToString(diagnosis.severity)
           << "\", \"confidence\": \"" << ToString(diagnosis.confidence)
           << "\", \"summary\": \""
           << windows::JsonEscape(diagnosis.summary)
           << "\", \"evidence\": {";
    std::size_t evidence_index = 0;
    for (const auto& [name, value] : diagnosis.evidence) {
      report << (evidence_index++ == 0 ? "" : ", ") << '"'
             << windows::JsonEscape(name) << "\": "
             << SerializeEvidenceValue(value);
    }
    report << "}}";
  }
  if (!diagnoses.empty()) report << '\n';
  report << "  ],\n  \"failed_actions\": [";
  std::size_t failure_index = 0;
  for (const auto& action : session.actions) {
    if (action.state != ActionState::Failed &&
        action.state != ActionState::Rejected &&
        action.state != ActionState::Uncertain) {
      continue;
    }
    report << (failure_index++ == 0 ? "\n" : ",\n")
           << "    {\"id\": \"" << windows::JsonEscape(action.action.id)
           << "\", \"type\": \"" << ToString(action.action.type)
           << "\", \"state\": \"" << ToString(action.state)
           << "\", \"error_code\": " << action.error_code << '}';
  }
  if (failure_index != 0) report << '\n';
  report << "  ],\n"
         << "  \"rollback\": {\n"
         << "    \"complete\": "
         << (rollback_complete ? "true" : "false") << ",\n"
         << "    \"planned\": " << planned << ",\n"
         << "    \"applied\": " << applied << ",\n"
         << "    \"completed_one_shot\": " << completed << ",\n"
         << "    \"rolled_back\": " << rolled_back << ",\n"
         << "    \"uncertain\": " << uncertain << ",\n"
         << "    \"failed\": " << failed << ",\n"
         << "    \"rejected\": " << rejected << "\n"
         << "  }\n}\n";
  return report.str();
}

std::string SessionManager::Serialize(const OptimizationSession& session) {
  std::ostringstream output;
  output << std::setprecision(12);
  output << "{\n"
         << "  \"schema_version\": 3,\n"
         << "  \"session_id\": \""
         << windows::JsonEscape(session.session_id) << "\",\n"
         << "  \"state\": \"" << ToString(session.state) << "\",\n"
         << "  \"start_time\": \""
         << windows::JsonEscape(session.start_time) << "\",\n"
         << "  \"baseline\": " << SerializeBenchmark(session.baseline, 2)
         << ",\n"
         << "  \"actions\": [";
  for (std::size_t i = 0; i < session.actions.size(); ++i) {
    const auto& action = session.actions[i];
    output << (i == 0 ? "\n" : ",\n")
           << "    {\n"
           << "      \"id\": \""
           << windows::JsonEscape(action.action.id) << "\",\n"
           << "      \"type\": \"" << ToString(action.action.type) << "\",\n"
           << "      \"risk\": \"" << ToString(action.action.risk) << "\",\n"
           << "      \"pid\": " << action.action.pid << ",\n"
           << "      \"expected_start_time_100ns\": "
           << action.action.expected_start_time_100ns << ",\n"
           << "      \"process_name\": \""
           << windows::JsonEscape(action.action.process_name) << "\",\n"
           << "      \"source_priority\": "
           << action.action.source_priority << ",\n"
           << "      \"target_priority\": "
           << action.action.target_priority << ",\n"
           << "      \"timeout_ms\": " << action.action.timeout_ms << ",\n"
           << "      \"service_name\": \""
           << windows::JsonEscape(action.action.service_name) << "\",\n"
           << "      \"expected_service_identity\": \""
           << windows::JsonEscape(action.action.expected_service_identity)
           << "\",\n"
           << "      \"source_service_state\": \""
           << ToString(action.action.source_service_state) << "\",\n"
           << "      \"explicit_confirmation\": "
           << (action.action.explicit_confirmation ? "true" : "false")
           << ",\n"
           << "      \"reversible\": "
           << (IsReversible(action.action.type) ? "true" : "false") << ",\n"
           << "      \"reason\": \""
           << windows::JsonEscape(action.action.reason) << "\",\n"
           << "      \"state\": \"" << ToString(action.state) << "\",\n"
           << "      \"original_priority\": " << action.original_priority
           << ",\n"
           << "      \"original_service_state\": \""
           << ToString(action.original_service_state) << "\",\n"
           << "      \"error_code\": " << action.error_code << ",\n"
           << "      \"error_message\": \""
           << windows::JsonEscape(action.error_message) << "\",\n"
           << "      \"result_message\": \""
           << windows::JsonEscape(action.result_message) << "\"\n"
           << "    }";
  }
  if (!session.actions.empty()) output << '\n';
  output << "  ]\n}\n";
  return output.str();
}

std::optional<OptimizationSession> SessionManager::Deserialize(
    const std::string& json, std::string* error) {
  if (!IsValidJsonObjectSyntax(json)) {
    if (error) *error = "active session JSON syntax is invalid";
    return std::nullopt;
  }
  const auto schema = IntegerField(json, "schema_version");
  const auto id = StringField(json, "session_id");
  const auto state = StringField(json, "state");
  const auto start_time = StringField(json, "start_time");
  const auto baseline = DelimitedField(json, "baseline", '{', '}');
  const auto timestamp =
      baseline ? StringField(*baseline, "timestamp") : std::nullopt;
  const auto benchmark_sample_count =
      baseline ? IntegerField(*baseline, "sample_count") : std::nullopt;
  const auto observed_span =
      baseline ? IntegerField(*baseline, "observed_span_ms") : std::nullopt;
  const auto process_inventory_complete_samples =
      baseline ? IntegerField(*baseline,
                              "process_inventory_complete_samples")
               : std::nullopt;
  const auto tcp_inventory_complete_samples =
      baseline ? IntegerField(*baseline, "tcp_inventory_complete_samples")
               : std::nullopt;
  const auto protection_inventory_complete_samples =
      baseline ? IntegerField(*baseline,
                              "protection_inventory_complete_samples")
               : std::nullopt;
  const auto cpu =
      baseline ? DoubleField(*baseline, "cpu_percent") : std::nullopt;
  const auto available =
      baseline ? IntegerField(*baseline, "available_memory_bytes")
               : std::nullopt;
  const auto commit =
      baseline ? DoubleField(*baseline, "commit_ratio") : std::nullopt;
  const auto disk_active =
      baseline ? DoubleField(*baseline, "maximum_disk_active_ratio")
               : std::nullopt;
  const auto disk_latency =
      baseline ? DoubleField(*baseline, "maximum_disk_latency_ms")
               : std::nullopt;
  const auto disk_queue =
      baseline ? DoubleField(*baseline, "maximum_disk_queue_length")
               : std::nullopt;
  const auto ssd_active =
      baseline ? DoubleField(*baseline, "maximum_ssd_active_ratio")
               : std::nullopt;
  const auto ssd_latency =
      baseline ? DoubleField(*baseline, "maximum_ssd_latency_ms")
               : std::nullopt;
  const auto hdd_active =
      baseline ? DoubleField(*baseline, "maximum_hdd_active_ratio")
               : std::nullopt;
  const auto hdd_latency =
      baseline ? DoubleField(*baseline, "maximum_hdd_latency_ms")
               : std::nullopt;
  const auto page_reads =
      baseline ? DoubleField(*baseline, "page_reads_per_sec")
               : std::nullopt;
  const auto development_cpu =
      baseline ? DoubleField(*baseline, "development_cpu_percent")
               : std::nullopt;
  const auto development_working_set =
      baseline ? IntegerField(*baseline, "development_working_set_bytes")
               : std::nullopt;
  const auto background_process =
      baseline ? StringField(*baseline, "top_background_io_process")
               : std::nullopt;
  const auto background_io =
      baseline ? DoubleField(*baseline, "top_background_io_bytes_per_sec")
               : std::nullopt;
  const auto parsed_session_state =
      state ? SessionStateFromString(*state) : std::nullopt;
  const std::uint64_t effective_sample_count =
      benchmark_sample_count.value_or(1);
  const auto valid_inventory_coverage =
      [effective_sample_count](const auto& value) {
        return !value || *value <= effective_sample_count;
      };
  if (!schema || (*schema != 1 && *schema != 2 && *schema != 3) || !id ||
      !parsed_session_state || !start_time || !timestamp || !cpu ||
      !available || !commit || !disk_active || !disk_latency ||
      (benchmark_sample_count &&
       (*benchmark_sample_count == 0 ||
        *benchmark_sample_count >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()))) ||
      !valid_inventory_coverage(process_inventory_complete_samples) ||
      !valid_inventory_coverage(tcp_inventory_complete_samples) ||
      !valid_inventory_coverage(protection_inventory_complete_samples) ||
      (protection_inventory_complete_samples &&
       ((*protection_inventory_complete_samples >
         process_inventory_complete_samples.value_or(0)) ||
        (*protection_inventory_complete_samples >
         tcp_inventory_complete_samples.value_or(0))))) {
    if (error) *error = "active session JSON is incomplete or invalid";
    return std::nullopt;
  }
  OptimizationSession session;
  session.session_id = *id;
  session.state = *parsed_session_state;
  session.start_time = *start_time;
  session.baseline.timestamp = *timestamp;
  session.baseline.sample_count = static_cast<std::size_t>(
      benchmark_sample_count.value_or(1));
  session.baseline.observed_span_ms = observed_span.value_or(0);
  session.baseline.process_inventory_complete_samples =
      static_cast<std::size_t>(
          process_inventory_complete_samples.value_or(0));
  session.baseline.tcp_inventory_complete_samples =
      static_cast<std::size_t>(tcp_inventory_complete_samples.value_or(0));
  session.baseline.protection_inventory_complete_samples =
      static_cast<std::size_t>(
          protection_inventory_complete_samples.value_or(0));
  session.baseline.cpu_percent = *cpu;
  session.baseline.available_memory_bytes = *available;
  session.baseline.commit_ratio = *commit;
  session.baseline.maximum_disk_active_ratio = *disk_active;
  session.baseline.maximum_disk_latency_ms = *disk_latency;
  session.baseline.maximum_disk_queue_length = disk_queue.value_or(0.0);
  session.baseline.maximum_ssd_active_ratio = ssd_active.value_or(0.0);
  session.baseline.maximum_ssd_latency_ms = ssd_latency.value_or(0.0);
  session.baseline.maximum_hdd_active_ratio = hdd_active.value_or(0.0);
  session.baseline.maximum_hdd_latency_ms = hdd_latency.value_or(0.0);
  session.baseline.page_reads_per_sec = page_reads.value_or(0.0);
  session.baseline.development_cpu_percent =
      development_cpu.value_or(0.0);
  session.baseline.development_working_set_bytes =
      development_working_set.value_or(0);
  session.baseline.top_background_io_process =
      background_process.value_or("");
  session.baseline.top_background_io_bytes_per_sec =
      background_io.value_or(0.0);

  const auto body = ArrayBody(json, "actions");
  if (!body) {
    if (error) *error = "active session has no actions array";
    return std::nullopt;
  }
  for (const auto& object : Objects(*body)) {
    ExecutedAction action;
    const auto action_id = StringField(object, "id");
    const auto type = StringField(object, "type");
    const auto risk = StringField(object, "risk");
    const auto pid = IntegerField(object, "pid");
    const auto expected = IntegerField(object, "expected_start_time_100ns");
    const auto name = StringField(object, "process_name");
    const auto source = IntegerField(object, "source_priority");
    const auto target = IntegerField(object, "target_priority");
    const auto timeout = IntegerField(object, "timeout_ms");
    const auto service_name = StringField(object, "service_name");
    const auto expected_service_identity =
        StringField(object, "expected_service_identity");
    const auto source_service_state =
        StringField(object, "source_service_state");
    const auto explicit_confirmation =
        BoolField(object, "explicit_confirmation");
    const auto reason = StringField(object, "reason");
    const auto action_state = StringField(object, "state");
    const auto original = IntegerField(object, "original_priority");
    const auto original_service_state =
        StringField(object, "original_service_state");
    const auto error_code = IntegerField(object, "error_code");
    const auto error_message = StringField(object, "error_message");
    const auto result_message = StringField(object, "result_message");
    const auto parsed_type = type ? ActionTypeFromString(*type) : std::nullopt;
    const auto parsed_risk = risk ? ActionRiskFromString(*risk) : std::nullopt;
    const auto parsed_state =
        action_state ? ActionStateFromString(*action_state) : std::nullopt;
    const auto parsed_source_service_state =
        source_service_state ? ServiceStateFromString(*source_service_state)
                             : std::nullopt;
    const auto parsed_original_service_state =
        original_service_state
            ? ServiceStateFromString(*original_service_state)
            : std::nullopt;
    const bool current_fields_present = *schema == 1 ||
                                        (source && timeout && result_message);
    const bool schema_three_fields_present =
        *schema != 3 ||
        (service_name && expected_service_identity &&
         parsed_source_service_state && explicit_confirmation &&
         parsed_original_service_state);
    const bool common_numeric_range_valid =
        pid && *pid <= std::numeric_limits<std::uint32_t>::max() && expected &&
        target && *target <= std::numeric_limits<std::uint32_t>::max() &&
        original &&
        *original <= std::numeric_limits<std::uint32_t>::max() && error_code &&
        *error_code <= std::numeric_limits<unsigned long>::max() &&
        (!source || *source <= std::numeric_limits<std::uint32_t>::max()) &&
        (!timeout || *timeout <= std::numeric_limits<std::uint32_t>::max());
    if (!action_id || !type || !risk || !name || !reason || !action_state ||
        !error_message || !current_fields_present ||
        !schema_three_fields_present || !parsed_type || !parsed_risk ||
        !parsed_state || !common_numeric_range_valid) {
      if (error) *error = "active session contains an invalid action";
      return std::nullopt;
    }
    const bool priority_action =
        *parsed_type == ActionType::SetPriorityClass && *pid > 0 &&
        *expected > 0 && *parsed_risk == ActionRisk::Safe;
    const bool graceful_action =
        *schema >= 2 && *parsed_type == ActionType::GracefulCloseProcess &&
        *pid > 0 && *expected > 0 && *parsed_risk == ActionRisk::Low &&
        timeout && *timeout >= 100 && *timeout <= 5000 &&
        *parsed_state != ActionState::Applied &&
        *parsed_state != ActionState::RolledBack;
    const bool service_action =
        *schema == 3 && *parsed_type == ActionType::StopServiceTemporary &&
        *pid == 0 && *expected == 0 && *parsed_risk == ActionRisk::Medium &&
        timeout && *timeout >= 1000 && *timeout <= 30000 && service_name &&
        ValidServiceName(*service_name) && expected_service_identity &&
        ValidServiceIdentity(*expected_service_identity) &&
        parsed_source_service_state &&
        *parsed_source_service_state == ServiceState::Running &&
        explicit_confirmation && *explicit_confirmation &&
        parsed_original_service_state &&
        *parsed_original_service_state == ServiceState::Running &&
        *parsed_state != ActionState::Completed;
    if ((!priority_action && !graceful_action && !service_action) ||
        (*schema == 1 && !priority_action)) {
      if (error) *error = "active session contains an invalid action";
      return std::nullopt;
    }
    action.action.id = *action_id;
    action.action.type = *parsed_type;
    action.action.risk = *parsed_risk;
    action.action.pid = static_cast<std::uint32_t>(*pid);
    action.action.expected_start_time_100ns = *expected;
    action.action.process_name = *name;
    action.action.source_priority = static_cast<std::uint32_t>(
        source.value_or(*original));
    action.action.target_priority = static_cast<std::uint32_t>(*target);
    action.action.timeout_ms =
        static_cast<std::uint32_t>(timeout.value_or(2000));
    action.action.service_name = service_name.value_or("");
    action.action.expected_service_identity =
        expected_service_identity.value_or("");
    action.action.source_service_state =
        parsed_source_service_state.value_or(ServiceState::Unknown);
    action.action.explicit_confirmation = explicit_confirmation.value_or(false);
    action.action.reason = *reason;
    action.state = *parsed_state;
    action.original_priority = static_cast<std::uint32_t>(*original);
    action.original_service_state =
        parsed_original_service_state.value_or(ServiceState::Unknown);
    action.error_code = static_cast<unsigned long>(*error_code);
    action.error_message = *error_message;
    action.result_message = result_message.value_or("");
    session.actions.push_back(std::move(action));
  }
  return session;
}

BenchmarkPoint SessionManager::MakeBenchmarkPoint(
    const SystemSnapshot& snapshot) {
  SnapshotHistory history(1);
  history.Add(snapshot);
  return MakeBenchmarkPoint(history);
}

BenchmarkPoint SessionManager::MakeBenchmarkPoint(
    const SnapshotHistory& history) {
  BenchmarkPoint point;
  point.timestamp = windows::Iso8601Now();
  point.sample_count = history.Size();
  if (history.Empty()) return point;

  const auto& snapshots = history.Snapshots();
  if (snapshots.size() >= 2) {
    const auto span = std::chrono::duration_cast<std::chrono::milliseconds>(
                          snapshots.back().timestamp -
                          snapshots.front().timestamp)
                          .count();
    if (span > 0) point.observed_span_ms = static_cast<std::uint64_t>(span);
  }

  long double available_memory_sum = 0.0;
  long double development_working_set_sum = 0.0;
  double disk_active_sum = 0.0;
  double disk_latency_sum = 0.0;
  double disk_queue_sum = 0.0;
  double ssd_active_sum = 0.0;
  double ssd_latency_sum = 0.0;
  double hdd_active_sum = 0.0;
  double hdd_latency_sum = 0.0;
  std::size_t disk_samples = 0;
  std::size_t ssd_samples = 0;
  std::size_t hdd_samples = 0;
  struct BackgroundAggregate {
    std::string display_name;
    double io_sum{};
  };
  std::map<std::string, BackgroundAggregate> background_io;

  for (const auto& snapshot : snapshots) {
    point.cpu_percent += snapshot.cpu_percent;
    available_memory_sum += snapshot.memory.physical_available_bytes;
    point.commit_ratio += snapshot.memory.CommitRatio();
    point.page_reads_per_sec += snapshot.page_reads_per_sec;

    double sample_disk_active = 0.0;
    double sample_disk_latency = 0.0;
    double sample_disk_queue = 0.0;
    double sample_ssd_active = 0.0;
    double sample_ssd_latency = 0.0;
    double sample_hdd_active = 0.0;
    double sample_hdd_latency = 0.0;
    bool has_ssd = false;
    bool has_hdd = false;
    for (const auto& disk : snapshot.disks) {
      sample_disk_active = std::max(sample_disk_active, disk.active_ratio);
      sample_disk_latency =
          std::max(sample_disk_latency, disk.average_latency_ms);
      sample_disk_queue = std::max(sample_disk_queue, disk.queue_length);
      if (disk.media == DiskMedia::SSD) {
        has_ssd = true;
        sample_ssd_active = std::max(sample_ssd_active, disk.active_ratio);
        sample_ssd_latency =
            std::max(sample_ssd_latency, disk.average_latency_ms);
      } else if (disk.media == DiskMedia::HDD) {
        has_hdd = true;
        sample_hdd_active = std::max(sample_hdd_active, disk.active_ratio);
        sample_hdd_latency =
            std::max(sample_hdd_latency, disk.average_latency_ms);
      }
    }
    if (!snapshot.disks.empty()) {
      disk_active_sum += sample_disk_active;
      disk_latency_sum += sample_disk_latency;
      disk_queue_sum += sample_disk_queue;
      ++disk_samples;
    }
    if (has_ssd) {
      ssd_active_sum += sample_ssd_active;
      ssd_latency_sum += sample_ssd_latency;
      ++ssd_samples;
    }
    if (has_hdd) {
      hdd_active_sum += sample_hdd_active;
      hdd_latency_sum += sample_hdd_latency;
      ++hdd_samples;
    }

    point.process_inventory_complete_samples +=
        snapshot.process_inventory_complete ? 1U : 0U;
    point.tcp_inventory_complete_samples +=
        snapshot.tcp_inventory_complete ? 1U : 0U;
    const bool protection_inventory_complete =
        snapshot.process_inventory_complete &&
        snapshot.tcp_inventory_complete;
    point.protection_inventory_complete_samples +=
        protection_inventory_complete ? 1U : 0U;
    if (snapshot.process_inventory_complete) {
      for (const auto& process : snapshot.processes) {
        if (process.classification == ProcessClass::Development) {
          point.development_cpu_percent += process.cpu_percent;
          development_working_set_sum += process.working_set_bytes;
        }
        const bool background =
            process.classification == ProcessClass::Updater ||
            process.classification == ProcessClass::CloudSync ||
            process.classification == ProcessClass::VendorUtility ||
            process.classification == ProcessClass::Communication;
        const bool protected_process =
            process.protection_level == ProtectionLevel::SystemCritical ||
            process.protection_level == ProtectionLevel::Strong ||
            process.protection_level == ProtectionLevel::UserExplicit;
        if (protection_inventory_complete && background &&
            !protected_process) {
          auto& aggregate = background_io[ToLowerAscii(process.name)];
          aggregate.display_name = process.name;
          aggregate.io_sum += process.IoBytesPerSec();
        }
      }
    }
  }

  const double sample_count = static_cast<double>(snapshots.size());
  point.cpu_percent /= sample_count;
  point.available_memory_bytes = static_cast<std::uint64_t>(
      available_memory_sum / static_cast<long double>(snapshots.size()));
  point.commit_ratio /= sample_count;
  point.page_reads_per_sec /= sample_count;
  if (point.process_inventory_complete_samples != 0) {
    point.development_cpu_percent /=
        static_cast<double>(point.process_inventory_complete_samples);
    point.development_working_set_bytes = static_cast<std::uint64_t>(
        development_working_set_sum /
        static_cast<long double>(
            point.process_inventory_complete_samples));
  }
  if (disk_samples != 0) {
    point.maximum_disk_active_ratio = disk_active_sum / disk_samples;
    point.maximum_disk_latency_ms = disk_latency_sum / disk_samples;
    point.maximum_disk_queue_length = disk_queue_sum / disk_samples;
  }
  if (ssd_samples != 0) {
    point.maximum_ssd_active_ratio = ssd_active_sum / ssd_samples;
    point.maximum_ssd_latency_ms = ssd_latency_sum / ssd_samples;
  }
  if (hdd_samples != 0) {
    point.maximum_hdd_active_ratio = hdd_active_sum / hdd_samples;
    point.maximum_hdd_latency_ms = hdd_latency_sum / hdd_samples;
  }
  for (const auto& [name, aggregate] : background_io) {
    static_cast<void>(name);
    const double average_io =
        aggregate.io_sum /
        static_cast<double>(point.protection_inventory_complete_samples);
    if (average_io > point.top_background_io_bytes_per_sec) {
      point.top_background_io_process = aggregate.display_name;
      point.top_background_io_bytes_per_sec = average_io;
    }
  }
  return point;
}

std::string ToString(SessionState value) {
  switch (value) {
    case SessionState::Active: return "Active";
    case SessionState::Recovering: return "Recovering";
    case SessionState::Completed: return "Completed";
    case SessionState::SafeMode: return "SafeMode";
  }
  return "SafeMode";
}

std::optional<SessionState> SessionStateFromString(const std::string& value) {
  if (value == "Active") return SessionState::Active;
  if (value == "Recovering") return SessionState::Recovering;
  if (value == "Completed") return SessionState::Completed;
  if (value == "SafeMode") return SessionState::SafeMode;
  return std::nullopt;
}

}  // namespace workboost
