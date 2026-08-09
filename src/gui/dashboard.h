#pragma once

#include "core/config/config.h"

namespace workboost::gui {

enum class DashboardCloseRequest { WindowClose, ExplicitExit };
enum class DashboardCloseDisposition { HideToTray, KeepOpen, Exit };

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
