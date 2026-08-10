#pragma once

#include "gui/dashboard_model.h"

#include <windows.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace workboost::gui {

enum class DashboardUiAction {
  None,
  Navigate,
  Refresh,
  Export,
  CodingModePrimary,
  RecoveryRestore,
  ToggleLanguage,
  FocusProcessSearch,
  ProcessFilterAll,
  ProcessFilterHighImpact,
  ProcessFilterProtected,
  ProcessSortCpu,
  ProcessSortMemory,
  ProcessSortIo,
  ProcessSortImpact,
  SelectProcess,
  ToggleCleanupProcess,
};

struct DashboardUiCommand {
  DashboardUiAction action{DashboardUiAction::None};
  DashboardPage page{DashboardPage::Dashboard};
  std::uint32_t value{};

  [[nodiscard]] bool operator==(const DashboardUiCommand& other) const {
    return action == other.action && page == other.page &&
           value == other.value;
  }
  [[nodiscard]] bool operator!=(const DashboardUiCommand& other) const {
    return !(*this == other);
  }
};

enum class ProcessFilter { All, HighImpact, Protected };
enum class ProcessSort { Impact, Cpu, Memory, DiskIo };

struct ProcessViewOptions {
  ProcessFilter filter{ProcessFilter::All};
  ProcessSort sort{ProcessSort::Impact};
  std::string search;
  std::uint32_t selected_pid{};
  bool search_focused{};
};

struct CodingModeViewOptions {
  std::vector<ProcessSelection> cleanup_processes;
  std::size_t process_scroll_offset{};
  std::size_t pool_scroll_offset{};
};

// Device-independent geometry for the variable-length System Overview list.
// Keeping this calculation separate from GDI drawing makes the responsive
// behavior deterministic and unit-testable.
struct DashboardOverviewLayout {
  int primary_line_height{};
  int metric_row_height{};
  int disk_row_height{};
  std::size_t disk_columns{1};
  std::size_t disk_rows{1};
  int required_height{};
  bool compact{};
  bool fits{};
};

[[nodiscard]] DashboardOverviewLayout CalculateDashboardOverviewLayout(
    int maximum_height, int overview_width, std::size_t disk_count,
    int primary_text_height, int secondary_text_height);

class DashboardRenderer {
 public:
  DashboardRenderer() = default;
  ~DashboardRenderer();

  DashboardRenderer(const DashboardRenderer&) = delete;
  DashboardRenderer& operator=(const DashboardRenderer&) = delete;

  bool Initialize(HWND window);
  void Paint(HDC target, const RECT& client,
             const std::optional<DashboardViewModel>& model,
             DashboardPage current_page,
             const std::optional<DashboardUiCommand>& hovered,
             const std::string& status_message,
             const ProcessViewOptions& process_options = {},
             const CodingModeViewOptions& coding_options = {});
  [[nodiscard]] std::optional<DashboardUiCommand> HitTest(POINT point) const;

  // Recreates fonts after the interface language changed (the active locale
  // is read from Locale::Current()).
  void NotifyLocaleChanged();

 private:
  struct HitTarget {
    RECT bounds{};
    DashboardUiCommand command;
  };

  void RecreateFonts(int dpi);
  void DrawFrame(HDC dc, const RECT& client,
                 const std::optional<DashboardViewModel>& model,
                 DashboardPage current_page,
                 const std::optional<DashboardUiCommand>& hovered,
                 const std::string& status_message,
                 const ProcessViewOptions& process_options,
                 const CodingModeViewOptions& coding_options);
  void DrawDashboard(HDC dc, const RECT& content,
                     const DashboardViewModel& model,
                     const std::optional<DashboardUiCommand>& hovered);
  void DrawProcesses(HDC dc, const RECT& content,
                     const DashboardViewModel& model,
                     const ProcessViewOptions& options,
                     const std::optional<DashboardUiCommand>& hovered);
  void DrawDiagnosis(HDC dc, const RECT& content,
                     const DashboardViewModel& model);
  void DrawProtected(HDC dc, const RECT& content,
                     const DashboardViewModel& model);
  void DrawCodingMode(HDC dc, const RECT& content,
                      const DashboardViewModel& model,
                      const CodingModeViewOptions& options,
                      const std::optional<DashboardUiCommand>& hovered);
  void DrawRecovery(HDC dc, const RECT& content,
                    const DashboardViewModel& model,
                    const std::optional<DashboardUiCommand>& hovered);
  void DrawSettings(HDC dc, const RECT& content,
                    const DashboardViewModel& model,
                    const std::optional<DashboardUiCommand>& hovered);
  void DrawTextPage(HDC dc, const RECT& content, const std::string& text);
  void AddTarget(const RECT& bounds, DashboardUiCommand command);
  [[nodiscard]] int Scale(int value) const;

  HWND window_{};
  int dpi_{96};
  HFONT brand_font_{};
  HFONT page_title_font_{};
  HFONT section_font_{};
  HFONT body_font_{};
  HFONT small_font_{};
  HFONT metric_font_{};
  HFONT mono_font_{};
  int body_line_height_{};
  int small_line_height_{};
  int metric_line_height_{};
  std::vector<HitTarget> hit_targets_;
};

}  // namespace workboost::gui
