#include "platform/windows/system_collector.h"

#include "core/policy/protection_policy.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace workboost::windows {
namespace {

std::uint64_t FileTimeValue(const FILETIME& value) {
  ULARGE_INTEGER integer{};
  integer.LowPart = value.dwLowDateTime;
  integer.HighPart = value.dwHighDateTime;
  return integer.QuadPart;
}

TcpState ConvertTcpState(DWORD state) {
  switch (state) {
    case MIB_TCP_STATE_CLOSED: return TcpState::Closed;
    case MIB_TCP_STATE_LISTEN: return TcpState::Listen;
    case MIB_TCP_STATE_SYN_SENT: return TcpState::SynSent;
    case MIB_TCP_STATE_SYN_RCVD: return TcpState::SynReceived;
    case MIB_TCP_STATE_ESTAB: return TcpState::Established;
    case MIB_TCP_STATE_FIN_WAIT1: return TcpState::FinWait1;
    case MIB_TCP_STATE_FIN_WAIT2: return TcpState::FinWait2;
    case MIB_TCP_STATE_CLOSE_WAIT: return TcpState::CloseWait;
    case MIB_TCP_STATE_CLOSING: return TcpState::Closing;
    case MIB_TCP_STATE_LAST_ACK: return TcpState::LastAck;
    case MIB_TCP_STATE_TIME_WAIT: return TcpState::TimeWait;
    case MIB_TCP_STATE_DELETE_TCB: return TcpState::DeleteTcb;
    default: return TcpState::Unknown;
  }
}

std::string Ipv4ToString(DWORD address) {
  IN_ADDR value{};
  value.S_un.S_addr = address;
  std::array<char, INET_ADDRSTRLEN> buffer{};
  return InetNtopA(AF_INET, &value, buffer.data(),
                   static_cast<DWORD>(buffer.size()))
             ? buffer.data()
             : std::string{};
}

std::string Ipv6ToString(const UCHAR address[16]) {
  IN6_ADDR value{};
  std::memcpy(&value, address, 16);
  std::array<char, INET6_ADDRSTRLEN> buffer{};
  return InetNtopA(AF_INET6, &value, buffer.data(),
                   static_cast<DWORD>(buffer.size()))
             ? buffer.data()
             : std::string{};
}

std::unordered_set<DWORD> VisibleWindowPids() {
  std::unordered_set<DWORD> result;
  EnumWindows(
      [](HWND window, LPARAM context) -> BOOL {
        if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr)
          return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != 0) {
          reinterpret_cast<std::unordered_set<DWORD>*>(context)->insert(pid);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&result));
  return result;
}

std::string QueryImagePath(HANDLE process) {
  std::vector<wchar_t> buffer(32768);
  DWORD length = static_cast<DWORD>(buffer.size());
  if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &length)) return {};
  return WideToUtf8(std::wstring(buffer.data(), length));
}

int PhysicalDriveNumber(const std::wstring& instance) {
  std::size_t length = 0;
  while (length < instance.size() && instance[length] >= L'0' &&
         instance[length] <= L'9') {
    ++length;
  }
  if (length == 0) return -1;
  try {
    return std::stoi(instance.substr(0, length));
  } catch (...) {
    return -1;
  }
}

std::string VolumesFromInstance(const std::wstring& instance) {
  const auto space = instance.find(L' ');
  return space == std::wstring::npos ? std::string{}
                                     : WideToUtf8(instance.substr(space + 1));
}

}  // namespace

struct SystemCollector::Impl {
  struct RawProcessCounters {
    std::uint64_t start{};
    std::uint64_t cpu{};
    std::uint64_t read_bytes{};
    std::uint64_t write_bytes{};
  };

  explicit Impl(const Config& value) : config(value) {}

  ~Impl() {
    if (pdh_query != nullptr) PdhCloseQuery(pdh_query);
    if (winsock_initialized) WSACleanup();
  }

  bool AddCounter(const wchar_t* path, PDH_HCOUNTER* counter) {
    if (pdh_query == nullptr) return false;
    const auto status = PdhAddEnglishCounterW(pdh_query, path, 0, counter);
    if (status != ERROR_SUCCESS) {
      *counter = nullptr;
      return false;
    }
    return true;
  }

  bool Initialize(WindowsError* error) {
    if (initialized) return true;
    WSADATA data{};
    const int socket_status = WSAStartup(MAKEWORD(2, 2), &data);
    if (socket_status == 0) {
      winsock_initialized = true;
    }

    processor_count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (processor_count == 0) processor_count = 1;

    const PDH_STATUS pdh_status = PdhOpenQueryW(nullptr, 0, &pdh_query);
    if (pdh_status == ERROR_SUCCESS) {
      AddCounter(L"\\PhysicalDisk(*)\\% Disk Time", &disk_active);
      AddCounter(L"\\PhysicalDisk(*)\\Avg. Disk sec/Transfer", &disk_latency);
      AddCounter(L"\\PhysicalDisk(*)\\Current Disk Queue Length", &disk_queue);
      AddCounter(L"\\PhysicalDisk(*)\\Disk Read Bytes/sec", &disk_read);
      AddCounter(L"\\PhysicalDisk(*)\\Disk Write Bytes/sec", &disk_write);
      AddCounter(L"\\Memory\\Page Reads/sec", &page_reads);
      AddCounter(L"\\Memory\\Pages Input/sec", &pages_input);
      PdhCollectQueryData(pdh_query);
    } else {
      pdh_query = nullptr;
    }

    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) {
      if (error) *error = LastError("GetSystemTimes");
      return false;
    }
    previous_idle = FileTimeValue(idle);
    previous_kernel = FileTimeValue(kernel);
    previous_user = FileTimeValue(user);
    process_sample_time = std::chrono::steady_clock::now();
    CollectProcesses();
    initialized = true;
    return true;
  }

  double CollectCpu() {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0.0;
    const auto idle_now = FileTimeValue(idle);
    const auto kernel_now = FileTimeValue(kernel);
    const auto user_now = FileTimeValue(user);
    const auto idle_delta = idle_now - previous_idle;
    const auto kernel_delta = kernel_now - previous_kernel;
    const auto user_delta = user_now - previous_user;
    previous_idle = idle_now;
    previous_kernel = kernel_now;
    previous_user = user_now;
    const auto total = kernel_delta + user_delta;
    if (total == 0 || idle_delta > total) return 0.0;
    return std::clamp(100.0 * static_cast<double>(total - idle_delta) /
                          static_cast<double>(total),
                      0.0, 100.0);
  }

  MemorySnapshot CollectMemory() const {
    MemorySnapshot result;
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
      result.physical_total_bytes = memory.ullTotalPhys;
      result.physical_available_bytes = memory.ullAvailPhys;
    }
    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (GetPerformanceInfo(&performance, sizeof(performance))) {
      result.commit_total_bytes =
          static_cast<std::uint64_t>(performance.CommitTotal) *
          performance.PageSize;
      result.commit_limit_bytes =
          static_cast<std::uint64_t>(performance.CommitLimit) *
          performance.PageSize;
      result.commit_peak_bytes =
          static_cast<std::uint64_t>(performance.CommitPeak) *
          performance.PageSize;
    }
    return result;
  }

  std::vector<ProcessSnapshot> CollectProcesses() {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::max(
        0.001, std::chrono::duration<double>(now - process_sample_time).count());
    process_sample_time = now;
    std::unordered_map<std::uint32_t, RawProcessCounters> current;
    std::vector<ProcessSnapshot> result;
    const auto visible_pids = VisibleWindowPids();
    DWORD foreground_pid = 0;
    const HWND foreground = GetForegroundWindow();
    if (foreground != nullptr)
      GetWindowThreadProcessId(foreground, &foreground_pid);

    UniqueHandle snapshot(
        CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.Valid()) return result;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.Get(), &entry)) return result;
    do {
      ProcessSnapshot process;
      process.pid = entry.th32ProcessID;
      process.parent_pid = entry.th32ParentProcessID;
      process.name = WideToUtf8(entry.szExeFile);
      process.has_visible_window = visible_pids.count(process.pid) != 0;
      process.is_foreground = process.pid != 0 && process.pid == foreground_pid;
      const auto rule = config.RuleFor(process.name);
      process.classification = rule.process_class;
      process.protection_level = rule.protection;

      RawProcessCounters raw;
      UniqueHandle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
                                          PROCESS_VM_READ,
                                      FALSE, process.pid));
      if (handle.Valid()) {
        process.image_path = QueryImagePath(handle.Get());
        process.priority_class = GetPriorityClass(handle.Get());
        DWORD session_id = 0;
        if (ProcessIdToSessionId(process.pid, &session_id))
          process.session_id = static_cast<std::uint32_t>(session_id);

        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (GetProcessMemoryInfo(
                handle.Get(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                sizeof(memory))) {
          process.working_set_bytes = memory.WorkingSetSize;
          process.private_bytes = memory.PrivateUsage;
        }

        IO_COUNTERS io{};
        if (GetProcessIoCounters(handle.Get(), &io)) {
          raw.read_bytes = io.ReadTransferCount;
          raw.write_bytes = io.WriteTransferCount;
        }

        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(handle.Get(), &created, &exited, &kernel, &user)) {
          raw.start = FileTimeValue(created);
          raw.cpu = FileTimeValue(kernel) + FileTimeValue(user);
          process.start_time_100ns = raw.start;
        }
      }

      const auto previous = previous_processes.find(process.pid);
      if (previous != previous_processes.end() &&
          previous->second.start == raw.start && raw.start != 0) {
        if (raw.cpu >= previous->second.cpu) {
          process.cpu_percent = std::clamp(
              (static_cast<double>(raw.cpu - previous->second.cpu) / 1.0e7) /
                  elapsed / static_cast<double>(processor_count) * 100.0,
              0.0, 100.0);
        }
        if (raw.read_bytes >= previous->second.read_bytes) {
          process.read_bytes_per_sec =
              static_cast<double>(raw.read_bytes - previous->second.read_bytes) /
              elapsed;
        }
        if (raw.write_bytes >= previous->second.write_bytes) {
          process.write_bytes_per_sec =
              static_cast<double>(raw.write_bytes -
                                  previous->second.write_bytes) /
              elapsed;
        }
      }
      current.emplace(process.pid, raw);
      result.push_back(std::move(process));
    } while (Process32NextW(snapshot.Get(), &entry));
    previous_processes = std::move(current);
    return result;
  }

  std::map<std::wstring, double> CounterArray(PDH_HCOUNTER counter) const {
    std::map<std::wstring, double> result;
    if (counter == nullptr) return result;
    DWORD buffer_size = 0;
    DWORD item_count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(
        counter, PDH_FMT_DOUBLE, &buffer_size, &item_count, nullptr);
    if (status != static_cast<PDH_STATUS>(PDH_MORE_DATA) || buffer_size == 0)
      return result;
    std::vector<unsigned char> buffer(buffer_size);
    auto* items =
        reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &buffer_size,
                                          &item_count, items);
    if (status != ERROR_SUCCESS) return result;
    for (DWORD i = 0; i < item_count; ++i) {
      if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
          items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) {
        result[items[i].szName] = items[i].FmtValue.doubleValue;
      }
    }
    return result;
  }

  double CounterValue(PDH_HCOUNTER counter) const {
    if (counter == nullptr) return 0.0;
    PDH_FMT_COUNTERVALUE value{};
    if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) !=
        ERROR_SUCCESS) {
      return 0.0;
    }
    if (value.CStatus != PDH_CSTATUS_VALID_DATA &&
        value.CStatus != PDH_CSTATUS_NEW_DATA) {
      return 0.0;
    }
    return std::max(0.0, value.doubleValue);
  }

  DiskMedia QueryDiskMedia(int drive_number) {
    const auto cached = disk_media.find(drive_number);
    if (cached != disk_media.end()) return cached->second;
    if (drive_number < 0) return DiskMedia::Unknown;
    const std::wstring path =
        L"\\\\.\\PhysicalDrive" + std::to_wstring(drive_number);
    UniqueHandle drive(CreateFileW(path.c_str(), 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr));
    if (!drive.Valid()) {
      disk_media[drive_number] = DiskMedia::Unknown;
      return DiskMedia::Unknown;
    }
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
    DWORD returned = 0;
    if (!DeviceIoControl(drive.Get(), IOCTL_STORAGE_QUERY_PROPERTY, &query,
                         sizeof(query), &descriptor, sizeof(descriptor),
                         &returned, nullptr)) {
      disk_media[drive_number] = DiskMedia::Unknown;
      return DiskMedia::Unknown;
    }
    const auto media = descriptor.IncursSeekPenalty ? DiskMedia::HDD
                                                     : DiskMedia::SSD;
    disk_media[drive_number] = media;
    return media;
  }

  std::vector<DiskSnapshot> CollectDisks() {
    const auto active = CounterArray(disk_active);
    const auto latency = CounterArray(disk_latency);
    const auto queue = CounterArray(disk_queue);
    const auto read = CounterArray(disk_read);
    const auto write = CounterArray(disk_write);
    std::set<std::wstring> instances;
    for (const auto& value : active) instances.insert(value.first);
    for (const auto& value : latency) instances.insert(value.first);
    for (const auto& value : queue) instances.insert(value.first);
    for (const auto& value : read) instances.insert(value.first);
    for (const auto& value : write) instances.insert(value.first);

    std::vector<DiskSnapshot> result;
    for (const auto& instance : instances) {
      if (instance == L"_Total") continue;
      DiskSnapshot disk;
      disk.instance = WideToUtf8(instance);
      disk.volumes = VolumesFromInstance(instance);
      disk.media = QueryDiskMedia(PhysicalDriveNumber(instance));
      if (const auto it = active.find(instance); it != active.end())
        disk.active_ratio = std::clamp(it->second / 100.0, 0.0, 1.0);
      if (const auto it = latency.find(instance); it != latency.end())
        disk.average_latency_ms = std::max(0.0, it->second * 1000.0);
      if (const auto it = queue.find(instance); it != queue.end())
        disk.queue_length = std::max(0.0, it->second);
      if (const auto it = read.find(instance); it != read.end())
        disk.read_bytes_per_sec = std::max(0.0, it->second);
      if (const auto it = write.find(instance); it != write.end())
        disk.write_bytes_per_sec = std::max(0.0, it->second);
      result.push_back(std::move(disk));
    }
    return result;
  }

  std::vector<TcpSession> CollectTcpSessions() const {
    std::vector<TcpSession> result;
    DWORD size = 0;
    if (GetExtendedTcpTable(nullptr, &size, TRUE, AF_INET,
                            TCP_TABLE_OWNER_PID_ALL, 0) ==
            ERROR_INSUFFICIENT_BUFFER &&
        size > 0) {
      std::vector<unsigned char> buffer(size);
      if (GetExtendedTcpTable(buffer.data(), &size, TRUE, AF_INET,
                              TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
        const auto* table =
            reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
          const auto& row = table->table[i];
          result.push_back(TcpSession{
              row.dwOwningPid,
              Ipv4ToString(row.dwLocalAddr),
              ntohs(static_cast<u_short>(row.dwLocalPort)),
              Ipv4ToString(row.dwRemoteAddr),
              ntohs(static_cast<u_short>(row.dwRemotePort)),
              ConvertTcpState(row.dwState),
              false});
        }
      }
    }

    size = 0;
    if (GetExtendedTcpTable(nullptr, &size, TRUE, AF_INET6,
                            TCP_TABLE_OWNER_PID_ALL, 0) ==
            ERROR_INSUFFICIENT_BUFFER &&
        size > 0) {
      std::vector<unsigned char> buffer(size);
      if (GetExtendedTcpTable(buffer.data(), &size, TRUE, AF_INET6,
                              TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
        const auto* table =
            reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
          const auto& row = table->table[i];
          result.push_back(TcpSession{
              row.dwOwningPid,
              Ipv6ToString(row.ucLocalAddr),
              ntohs(static_cast<u_short>(row.dwLocalPort)),
              Ipv6ToString(row.ucRemoteAddr),
              ntohs(static_cast<u_short>(row.dwRemotePort)),
              ConvertTcpState(row.dwState),
              true});
        }
      }
    }
    return result;
  }

  SystemSnapshot Sample(WindowsError* error) {
    SystemSnapshot snapshot;
    if (!initialized && !Initialize(error)) return snapshot;
    snapshot.timestamp = std::chrono::system_clock::now();
    snapshot.cpu_percent = CollectCpu();
    snapshot.memory = CollectMemory();
    snapshot.processes = CollectProcesses();
    snapshot.tcp_sessions = CollectTcpSessions();
    if (pdh_query != nullptr) {
      PdhCollectQueryData(pdh_query);
      snapshot.page_reads_per_sec = CounterValue(page_reads);
      snapshot.pages_input_per_sec = CounterValue(pages_input);
      snapshot.disks = CollectDisks();
    }

    const auto context =
        BuildRuntimeContext(snapshot, config.remote_debug_ports);
    ProtectionPolicy policy(config);
    for (auto& process : snapshot.processes) {
      process.protection_level = policy.Evaluate(process, context);
    }
    return snapshot;
  }

  const Config& config;
  bool initialized{};
  bool winsock_initialized{};
  DWORD processor_count{1};
  std::uint64_t previous_idle{};
  std::uint64_t previous_kernel{};
  std::uint64_t previous_user{};
  std::chrono::steady_clock::time_point process_sample_time{};
  std::unordered_map<std::uint32_t, RawProcessCounters> previous_processes;
  std::unordered_map<int, DiskMedia> disk_media;

  PDH_HQUERY pdh_query{};
  PDH_HCOUNTER disk_active{};
  PDH_HCOUNTER disk_latency{};
  PDH_HCOUNTER disk_queue{};
  PDH_HCOUNTER disk_read{};
  PDH_HCOUNTER disk_write{};
  PDH_HCOUNTER page_reads{};
  PDH_HCOUNTER pages_input{};
};

SystemCollector::SystemCollector(const Config& config)
    : impl_(std::make_unique<Impl>(config)) {}
SystemCollector::~SystemCollector() = default;

bool SystemCollector::Initialize(WindowsError* error) {
  return impl_->Initialize(error);
}

SystemSnapshot SystemCollector::Sample(WindowsError* error) {
  return impl_->Sample(error);
}

bool SystemCollector::Initialized() const { return impl_->initialized; }

}  // namespace workboost::windows
