#include "app/session_manager.h"

#include "platform/windows/windows_utils.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <regex>
#include <sstream>

namespace workboost {
namespace {

std::optional<std::string> StringField(const std::string& json,
                                       const std::string& key) {
  const std::regex pattern("\\\"" + key +
                           "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) return std::nullopt;
  return match[1].str();
}

std::optional<std::uint64_t> IntegerField(const std::string& json,
                                          const std::string& key) {
  const std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*([0-9]+)");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) return std::nullopt;
  try {
    return std::stoull(match[1].str());
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> DoubleField(const std::string& json,
                                  const std::string& key) {
  const std::regex pattern("\\\"" + key +
                           "\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) return std::nullopt;
  try {
    return std::stod(match[1].str());
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> ArrayBody(const std::string& json,
                                     const std::string& key) {
  const auto marker = json.find("\"" + key + "\"");
  if (marker == std::string::npos) return std::nullopt;
  const auto begin = json.find('[', marker);
  if (begin == std::string::npos) return std::nullopt;
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = begin; i < json.size(); ++i) {
    const char ch = json[i];
    if (in_string) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') in_string = false;
      continue;
    }
    if (ch == '"') in_string = true;
    else if (ch == '[') ++depth;
    else if (ch == ']' && --depth == 0)
      return json.substr(begin + 1, i - begin - 1);
  }
  return std::nullopt;
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
  output << "{\n"
         << spacing << "  \"timestamp\": \""
         << windows::JsonEscape(point.timestamp) << "\",\n"
         << spacing << "  \"cpu_percent\": " << point.cpu_percent << ",\n"
         << spacing << "  \"available_memory_bytes\": "
         << point.available_memory_bytes << ",\n"
         << spacing << "  \"commit_ratio\": " << point.commit_ratio << ",\n"
         << spacing << "  \"maximum_disk_active_ratio\": "
         << point.maximum_disk_active_ratio << ",\n"
         << spacing << "  \"maximum_disk_latency_ms\": "
         << point.maximum_disk_latency_ms << "\n"
         << spacing << '}';
  return output.str();
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
  return std::filesystem::exists(ActiveSessionPath());
}

OptimizationSession SessionManager::Create(
    const SystemSnapshot& baseline) const {
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
                              std::filesystem::path* report_path,
                              std::string* error) const {
  session.state = SessionState::Completed;
  const BenchmarkPoint final_point = MakeBenchmarkPoint(after);
  std::ostringstream report;
  std::string serialized = Serialize(session);
  if (!serialized.empty() && serialized.back() == '\n') serialized.pop_back();
  if (!serialized.empty() && serialized.back() == '}') serialized.pop_back();
  report << serialized << ",\n"
         << "  \"optimized\": " << SerializeBenchmark(final_point, 2) << ",\n"
         << "  \"delta\": {\n"
         << "    \"cpu_percent\": "
         << final_point.cpu_percent - session.baseline.cpu_percent << ",\n"
         << "    \"available_memory_bytes\": "
         << static_cast<std::int64_t>(final_point.available_memory_bytes) -
                static_cast<std::int64_t>(session.baseline.available_memory_bytes)
         << ",\n"
         << "    \"commit_ratio\": "
         << final_point.commit_ratio - session.baseline.commit_ratio << ",\n"
         << "    \"maximum_disk_latency_ms\": "
         << final_point.maximum_disk_latency_ms -
                session.baseline.maximum_disk_latency_ms
         << "\n  }\n}\n";

  const auto path = root_directory_ / "reports" /
                    (session.session_id + ".json");
  if (!windows::AtomicWriteUtf8(path, report.str(), error)) return false;
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

std::string SessionManager::Serialize(const OptimizationSession& session) {
  std::ostringstream output;
  output << std::setprecision(12);
  output << "{\n"
         << "  \"schema_version\": 1,\n"
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
           << "      \"target_priority\": "
           << action.action.target_priority << ",\n"
           << "      \"reason\": \""
           << windows::JsonEscape(action.action.reason) << "\",\n"
           << "      \"state\": \"" << ToString(action.state) << "\",\n"
           << "      \"original_priority\": " << action.original_priority
           << ",\n"
           << "      \"error_code\": " << action.error_code << ",\n"
           << "      \"error_message\": \""
           << windows::JsonEscape(action.error_message) << "\"\n"
           << "    }";
  }
  if (!session.actions.empty()) output << '\n';
  output << "  ]\n}\n";
  return output.str();
}

std::optional<OptimizationSession> SessionManager::Deserialize(
    const std::string& json, std::string* error) {
  const auto id = StringField(json, "session_id");
  const auto state = StringField(json, "state");
  const auto start_time = StringField(json, "start_time");
  const auto timestamp = StringField(json, "timestamp");
  const auto cpu = DoubleField(json, "cpu_percent");
  const auto available = IntegerField(json, "available_memory_bytes");
  const auto commit = DoubleField(json, "commit_ratio");
  const auto disk_active = DoubleField(json, "maximum_disk_active_ratio");
  const auto disk_latency = DoubleField(json, "maximum_disk_latency_ms");
  if (!id || !state || !start_time || !timestamp || !cpu || !available ||
      !commit || !disk_active || !disk_latency) {
    if (error) *error = "active session JSON is incomplete or invalid";
    return std::nullopt;
  }
  OptimizationSession session;
  session.session_id = *id;
  session.state = SessionStateFromString(*state);
  session.start_time = *start_time;
  session.baseline = {*timestamp, *cpu, *available, *commit, *disk_active,
                      *disk_latency};

  const auto body = ArrayBody(json, "actions");
  if (!body) {
    if (error) *error = "active session has no actions array";
    return std::nullopt;
  }
  for (const auto& object : Objects(*body)) {
    ExecutedAction action;
    const auto action_id = StringField(object, "id");
    const auto type = StringField(object, "type");
    const auto pid = IntegerField(object, "pid");
    const auto expected = IntegerField(object, "expected_start_time_100ns");
    const auto name = StringField(object, "process_name");
    const auto target = IntegerField(object, "target_priority");
    const auto reason = StringField(object, "reason");
    const auto action_state = StringField(object, "state");
    const auto original = IntegerField(object, "original_priority");
    const auto error_code = IntegerField(object, "error_code");
    const auto error_message = StringField(object, "error_message");
    if (!action_id || !type || !pid || !expected || !name || !target ||
        !reason || !action_state || !original || !error_code ||
        !error_message || *type != "SetPriorityClass") {
      if (error) *error = "active session contains an invalid action";
      return std::nullopt;
    }
    action.action.id = *action_id;
    action.action.type = ActionType::SetPriorityClass;
    action.action.risk = ActionRisk::Safe;
    action.action.pid = static_cast<std::uint32_t>(*pid);
    action.action.expected_start_time_100ns = *expected;
    action.action.process_name = *name;
    action.action.target_priority = static_cast<std::uint32_t>(*target);
    action.action.reason = *reason;
    action.state = ActionStateFromString(*action_state);
    action.original_priority = static_cast<std::uint32_t>(*original);
    action.error_code = static_cast<unsigned long>(*error_code);
    action.error_message = *error_message;
    session.actions.push_back(std::move(action));
  }
  return session;
}

BenchmarkPoint SessionManager::MakeBenchmarkPoint(
    const SystemSnapshot& snapshot) {
  BenchmarkPoint point;
  point.timestamp = windows::Iso8601Now();
  point.cpu_percent = snapshot.cpu_percent;
  point.available_memory_bytes = snapshot.memory.physical_available_bytes;
  point.commit_ratio = snapshot.memory.CommitRatio();
  for (const auto& disk : snapshot.disks) {
    point.maximum_disk_active_ratio =
        std::max(point.maximum_disk_active_ratio, disk.active_ratio);
    point.maximum_disk_latency_ms =
        std::max(point.maximum_disk_latency_ms, disk.average_latency_ms);
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

SessionState SessionStateFromString(const std::string& value) {
  if (value == "Active") return SessionState::Active;
  if (value == "Recovering") return SessionState::Recovering;
  if (value == "Completed") return SessionState::Completed;
  return SessionState::SafeMode;
}

}  // namespace workboost
