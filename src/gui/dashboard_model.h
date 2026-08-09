#pragma once

#include "app/session_manager.h"
#include "core/config/config.h"
#include "core/model/types.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace workboost::gui {

enum class DashboardPage : std::size_t {
  Dashboard,
  Processes,
  Diagnosis,
  CodingMode,
  ProtectedWorkload,
  Recovery,
  Settings,
  Count,
};

constexpr std::size_t kDashboardPageCount =
    static_cast<std::size_t>(DashboardPage::Count);

struct DashboardViewModel {
  std::array<std::string, kDashboardPageCount> pages;
  std::string mode;
  std::string updated_at;
};

class DashboardPresenter {
 public:
  static const std::array<const char*, kDashboardPageCount>& PageNames();

  static DashboardViewModel Build(
      const Config& config, const SystemSnapshot& snapshot,
      const SnapshotHistory& history,
      const std::vector<SerialPortSnapshot>& serial_ports,
      const std::vector<StartupEntrySnapshot>& startup_entries,
      const std::optional<OptimizationSession>& active_session,
      const std::string& recovery_error,
      const std::string& serial_error = {},
      const std::string& startup_error = {});
};

}  // namespace workboost::gui
