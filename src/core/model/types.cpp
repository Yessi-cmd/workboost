#include "core/model/types.h"

#include <algorithm>
#include <cctype>

namespace workboost {

double NormalizeProcessCpuPercent(std::uint64_t process_cpu_delta_100ns,
                                  double elapsed_seconds,
                                  std::uint32_t logical_processor_count) {
  if (elapsed_seconds <= 0.0 || logical_processor_count == 0) return 0.0;
  const double cpu_seconds =
      static_cast<double>(process_cpu_delta_100ns) / 1.0e7;
  return std::clamp(cpu_seconds / elapsed_seconds /
                            static_cast<double>(logical_processor_count) *
                        100.0,
                    0.0, 100.0);
}

double MemorySnapshot::CommitRatio() const {
  return commit_limit_bytes == 0
             ? 0.0
             : static_cast<double>(commit_total_bytes) /
                   static_cast<double>(commit_limit_bytes);
}

SnapshotHistory::SnapshotHistory(std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity)) {}

void SnapshotHistory::Add(SystemSnapshot snapshot) {
  if (snapshots_.size() == capacity_) {
    snapshots_.pop_front();
  }
  snapshots_.push_back(std::move(snapshot));
}

const std::deque<SystemSnapshot>& SnapshotHistory::Snapshots() const {
  return snapshots_;
}

const SystemSnapshot* SnapshotHistory::Latest() const {
  return snapshots_.empty() ? nullptr : &snapshots_.back();
}

bool SnapshotHistory::Empty() const { return snapshots_.empty(); }
std::size_t SnapshotHistory::Size() const { return snapshots_.size(); }

std::string ToString(ProcessClass value) {
  switch (value) {
    case ProcessClass::System: return "System";
    case ProcessClass::Security: return "Security";
    case ProcessClass::Development: return "Development";
    case ProcessClass::RemoteTerminal: return "RemoteTerminal";
    case ProcessClass::SerialTerminal: return "SerialTerminal";
    case ProcessClass::PacketCapture: return "PacketCapture";
    case ProcessClass::BuildTool: return "BuildTool";
    case ProcessClass::VersionControl: return "VersionControl";
    case ProcessClass::Browser: return "Browser";
    case ProcessClass::Communication: return "Communication";
    case ProcessClass::Office: return "Office";
    case ProcessClass::Updater: return "Updater";
    case ProcessClass::CloudSync: return "CloudSync";
    case ProcessClass::VendorUtility: return "VendorUtility";
    case ProcessClass::Unknown: return "Unknown";
  }
  return "Unknown";
}

std::string ToString(ProtectionLevel value) {
  switch (value) {
    case ProtectionLevel::SystemCritical: return "SystemCritical";
    case ProtectionLevel::Strong: return "Strong";
    case ProtectionLevel::Normal: return "Normal";
    case ProtectionLevel::Optimizable: return "Optimizable";
    case ProtectionLevel::UserExplicit: return "UserExplicit";
  }
  return "Normal";
}

std::string ToString(DiskMedia value) {
  switch (value) {
    case DiskMedia::SSD: return "SSD";
    case DiskMedia::HDD: return "HDD";
    case DiskMedia::Unknown: return "Unknown";
  }
  return "Unknown";
}

std::string ToString(Severity value) {
  switch (value) {
    case Severity::Low: return "low";
    case Severity::Medium: return "medium";
    case Severity::High: return "high";
  }
  return "low";
}

std::string ToString(Confidence value) {
  switch (value) {
    case Confidence::Low: return "low";
    case Confidence::Medium: return "medium";
    case Confidence::High: return "high";
  }
  return "low";
}

std::string ToString(TcpState value) {
  switch (value) {
    case TcpState::Closed: return "CLOSED";
    case TcpState::Listen: return "LISTEN";
    case TcpState::SynSent: return "SYN_SENT";
    case TcpState::SynReceived: return "SYN_RECEIVED";
    case TcpState::Established: return "ESTABLISHED";
    case TcpState::FinWait1: return "FIN_WAIT_1";
    case TcpState::FinWait2: return "FIN_WAIT_2";
    case TcpState::CloseWait: return "CLOSE_WAIT";
    case TcpState::Closing: return "CLOSING";
    case TcpState::LastAck: return "LAST_ACK";
    case TcpState::TimeWait: return "TIME_WAIT";
    case TcpState::DeleteTcb: return "DELETE_TCB";
    case TcpState::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

std::string ToString(ServiceState value) {
  switch (value) {
    case ServiceState::Stopped: return "STOPPED";
    case ServiceState::StartPending: return "START_PENDING";
    case ServiceState::StopPending: return "STOP_PENDING";
    case ServiceState::Running: return "RUNNING";
    case ServiceState::ContinuePending: return "CONTINUE_PENDING";
    case ServiceState::PausePending: return "PAUSE_PENDING";
    case ServiceState::Paused: return "PAUSED";
    case ServiceState::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

std::optional<ServiceState> ServiceStateFromString(const std::string& value) {
  if (value == "STOPPED") return ServiceState::Stopped;
  if (value == "START_PENDING") return ServiceState::StartPending;
  if (value == "STOP_PENDING") return ServiceState::StopPending;
  if (value == "RUNNING") return ServiceState::Running;
  if (value == "CONTINUE_PENDING") return ServiceState::ContinuePending;
  if (value == "PAUSE_PENDING") return ServiceState::PausePending;
  if (value == "PAUSED") return ServiceState::Paused;
  if (value == "UNKNOWN") return ServiceState::Unknown;
  return std::nullopt;
}

std::string ToString(ServiceClass value) {
  switch (value) {
    case ServiceClass::System: return "System";
    case ServiceClass::Network: return "Network";
    case ServiceClass::Security: return "Security";
    case ServiceClass::RemoteAccess: return "RemoteAccess";
    case ServiceClass::PacketCapture: return "PacketCapture";
    case ServiceClass::Device: return "Device";
    case ServiceClass::Updater: return "Updater";
    case ServiceClass::CloudSync: return "CloudSync";
    case ServiceClass::VendorUtility: return "VendorUtility";
    case ServiceClass::Unknown: return "Unknown";
  }
  return "Unknown";
}

std::string ToString(StartupScope value) {
  switch (value) {
    case StartupScope::CurrentUser: return "CurrentUser";
    case StartupScope::LocalMachine: return "LocalMachine";
    case StartupScope::AllUsers: return "AllUsers";
  }
  return "CurrentUser";
}

std::string ToString(StartupSource value) {
  switch (value) {
    case StartupSource::RegistryRun: return "RegistryRun";
    case StartupSource::StartupFolder: return "StartupFolder";
  }
  return "RegistryRun";
}

ProcessClass ProcessClassFromString(const std::string& value) {
  if (value == "System") return ProcessClass::System;
  if (value == "Security") return ProcessClass::Security;
  if (value == "Development") return ProcessClass::Development;
  if (value == "RemoteTerminal") return ProcessClass::RemoteTerminal;
  if (value == "SerialTerminal") return ProcessClass::SerialTerminal;
  if (value == "PacketCapture") return ProcessClass::PacketCapture;
  if (value == "BuildTool") return ProcessClass::BuildTool;
  if (value == "VersionControl") return ProcessClass::VersionControl;
  if (value == "Browser") return ProcessClass::Browser;
  if (value == "Communication") return ProcessClass::Communication;
  if (value == "Office") return ProcessClass::Office;
  if (value == "Updater") return ProcessClass::Updater;
  if (value == "CloudSync") return ProcessClass::CloudSync;
  if (value == "VendorUtility") return ProcessClass::VendorUtility;
  return ProcessClass::Unknown;
}

ProtectionLevel ProtectionLevelFromString(const std::string& value) {
  if (value == "SystemCritical") return ProtectionLevel::SystemCritical;
  if (value == "Strong") return ProtectionLevel::Strong;
  if (value == "Optimizable") return ProtectionLevel::Optimizable;
  if (value == "UserExplicit") return ProtectionLevel::UserExplicit;
  return ProtectionLevel::Normal;
}

ServiceClass ServiceClassFromString(const std::string& value) {
  if (value == "System") return ServiceClass::System;
  if (value == "Network") return ServiceClass::Network;
  if (value == "Security") return ServiceClass::Security;
  if (value == "RemoteAccess") return ServiceClass::RemoteAccess;
  if (value == "PacketCapture") return ServiceClass::PacketCapture;
  if (value == "Device") return ServiceClass::Device;
  if (value == "Updater") return ServiceClass::Updater;
  if (value == "CloudSync") return ServiceClass::CloudSync;
  if (value == "VendorUtility") return ServiceClass::VendorUtility;
  return ServiceClass::Unknown;
}

RuntimeContext BuildRuntimeContext(
    const SystemSnapshot& snapshot,
    const std::unordered_set<std::uint16_t>& protected_remote_ports) {
  RuntimeContext context;
  bool capture_active = false;
  for (const auto& process : snapshot.processes) {
    if (process.is_foreground) context.foreground_pid = process.pid;
    std::string lower_name = process.name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                   [](unsigned char ch) {
                     return static_cast<char>(std::tolower(ch));
                   });
    if (lower_name == "dumpcap.exe") {
      capture_active = true;
    }
  }
  for (const auto& session : snapshot.tcp_sessions) {
    if (session.state == TcpState::Established &&
        (session.remote_port == 22 || session.remote_port == 23 ||
         protected_remote_ports.count(session.remote_port) != 0)) {
      context.remote_session_pids.insert(session.pid);
    }
  }
  if (capture_active) {
    for (const auto& process : snapshot.processes) {
      if (process.classification == ProcessClass::PacketCapture) {
        context.active_capture_pids.insert(process.pid);
      }
    }
  }
  return context;
}

RuntimeContext BuildRuntimeContext(const SystemSnapshot& snapshot) {
  static const std::unordered_set<std::uint16_t> kDefaultRemotePorts{22, 23};
  return BuildRuntimeContext(snapshot, kDefaultRemotePorts);
}

WindowRuntimeContext BuildWindowRuntimeContext(
    const SnapshotHistory& history,
    const std::unordered_set<std::uint16_t>& protected_remote_ports) {
  WindowRuntimeContext window;
  for (const auto& snapshot : history.Snapshots()) {
    ++window.total_samples;
    if (snapshot.process_inventory_complete) {
      ++window.complete_process_samples;
      bool capture_active = false;
      for (const auto& process : snapshot.processes) {
        if (process.is_foreground) {
          window.context.foreground_pid = process.pid;
        }
        std::string lower_name = process.name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                       [](unsigned char ch) {
                         return static_cast<char>(std::tolower(ch));
                       });
        if (lower_name == "dumpcap.exe") capture_active = true;
      }
      if (capture_active) {
        for (const auto& process : snapshot.processes) {
          if (process.classification == ProcessClass::PacketCapture) {
            window.context.active_capture_pids.insert(process.pid);
          }
        }
      }
    }
    if (snapshot.tcp_inventory_complete) {
      ++window.complete_tcp_samples;
      for (const auto& session : snapshot.tcp_sessions) {
        if (session.state == TcpState::Established &&
            (session.remote_port == 22 || session.remote_port == 23 ||
             protected_remote_ports.count(session.remote_port) != 0)) {
          window.context.remote_session_pids.insert(session.pid);
        }
      }
    }
  }
  return window;
}

}  // namespace workboost
