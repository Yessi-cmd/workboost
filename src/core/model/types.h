#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace workboost {

enum class ProcessClass {
  System,
  Security,
  Development,
  RemoteTerminal,
  SerialTerminal,
  PacketCapture,
  BuildTool,
  VersionControl,
  Browser,
  Communication,
  Office,
  Updater,
  CloudSync,
  VendorUtility,
  Unknown,
};

enum class ProtectionLevel {
  SystemCritical,
  Strong,
  Normal,
  Optimizable,
  UserExplicit,
};

enum class DiskMedia { Unknown, SSD, HDD };
enum class Severity { Low, Medium, High };
enum class Confidence { Low, Medium, High };

enum class TcpState {
  Unknown,
  Closed,
  Listen,
  SynSent,
  SynReceived,
  Established,
  FinWait1,
  FinWait2,
  CloseWait,
  Closing,
  LastAck,
  TimeWait,
  DeleteTcb,
};

struct MemorySnapshot {
  std::uint64_t physical_total_bytes{};
  std::uint64_t physical_available_bytes{};
  std::uint64_t commit_total_bytes{};
  std::uint64_t commit_limit_bytes{};
  std::uint64_t commit_peak_bytes{};

  [[nodiscard]] double CommitRatio() const;
};

struct DiskSnapshot {
  std::string instance;
  std::string volumes;
  DiskMedia media{DiskMedia::Unknown};
  double active_ratio{};
  double average_latency_ms{};
  double queue_length{};
  double read_bytes_per_sec{};
  double write_bytes_per_sec{};
};

struct ProcessSnapshot {
  std::uint32_t pid{};
  std::uint32_t parent_pid{};
  std::string name;
  std::string image_path;
  double cpu_percent{};
  std::uint64_t working_set_bytes{};
  std::uint64_t private_bytes{};
  double read_bytes_per_sec{};
  double write_bytes_per_sec{};
  std::uint32_t priority_class{};
  std::uint32_t session_id{};
  bool has_visible_window{};
  bool is_foreground{};
  std::uint64_t start_time_100ns{};
  ProcessClass classification{ProcessClass::Unknown};
  ProtectionLevel protection_level{ProtectionLevel::Normal};

  [[nodiscard]] double IoBytesPerSec() const {
    return read_bytes_per_sec + write_bytes_per_sec;
  }
};

struct TcpSession {
  std::uint32_t pid{};
  std::string local_address;
  std::uint16_t local_port{};
  std::string remote_address;
  std::uint16_t remote_port{};
  TcpState state{TcpState::Unknown};
  bool ipv6{};
};

struct SystemSnapshot {
  std::chrono::system_clock::time_point timestamp{};
  double cpu_percent{};
  MemorySnapshot memory;
  double page_reads_per_sec{};
  double pages_input_per_sec{};
  std::vector<DiskSnapshot> disks;
  std::vector<ProcessSnapshot> processes;
  std::vector<TcpSession> tcp_sessions;
};

using EvidenceValue =
    std::variant<std::int64_t, std::uint64_t, double, bool, std::string>;

struct DiagnosisResult {
  std::string type;
  Severity severity{Severity::Low};
  Confidence confidence{Confidence::Low};
  std::string summary;
  std::map<std::string, EvidenceValue> evidence;
};

class SnapshotHistory {
 public:
  explicit SnapshotHistory(std::size_t capacity);

  void Add(SystemSnapshot snapshot);
  [[nodiscard]] const std::deque<SystemSnapshot>& Snapshots() const;
  [[nodiscard]] const SystemSnapshot* Latest() const;
  [[nodiscard]] bool Empty() const;
  [[nodiscard]] std::size_t Size() const;

 private:
  std::size_t capacity_;
  std::deque<SystemSnapshot> snapshots_;
};

struct RuntimeContext {
  std::unordered_set<std::uint32_t> remote_session_pids;
  std::unordered_set<std::uint32_t> active_capture_pids;
  std::uint32_t foreground_pid{};
};

std::string ToString(ProcessClass value);
std::string ToString(ProtectionLevel value);
std::string ToString(DiskMedia value);
std::string ToString(Severity value);
std::string ToString(Confidence value);
std::string ToString(TcpState value);

ProcessClass ProcessClassFromString(const std::string& value);
ProtectionLevel ProtectionLevelFromString(const std::string& value);
RuntimeContext BuildRuntimeContext(const SystemSnapshot& snapshot);
RuntimeContext BuildRuntimeContext(
    const SystemSnapshot& snapshot,
    const std::unordered_set<std::uint16_t>& protected_remote_ports);

}  // namespace workboost
