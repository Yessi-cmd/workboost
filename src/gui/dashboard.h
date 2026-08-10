#pragma once

#include "core/config/config.h"

#include <cstdint>

namespace workboost::gui {

enum class DashboardCloseRequest { WindowClose, ExplicitExit };
enum class DashboardCloseDisposition { HideToTray, KeepOpen, Exit };

struct DashboardTrayNotification {
  std::uint16_t event_code{};
  std::uint16_t icon_id{};
};

[[nodiscard]] constexpr DashboardTrayNotification
DecodeDashboardTrayNotification(std::uintptr_t callback_lparam,
                                bool version_4) {
  if (!version_4) {
    return DashboardTrayNotification{
        static_cast<std::uint16_t>(callback_lparam), 0};
  }
  return DashboardTrayNotification{
      static_cast<std::uint16_t>(callback_lparam & 0xFFFFU),
      static_cast<std::uint16_t>((callback_lparam >> 16U) & 0xFFFFU)};
}

[[nodiscard]] constexpr DashboardCloseDisposition ResolveDashboardClose(
    DashboardCloseRequest request, bool operation_in_progress) {
  if (request == DashboardCloseRequest::WindowClose) {
    return DashboardCloseDisposition::HideToTray;
  }
  return operation_in_progress ? DashboardCloseDisposition::KeepOpen
                               : DashboardCloseDisposition::Exit;
}

int RunDashboard(const Config& config);

}  // namespace workboost::gui
