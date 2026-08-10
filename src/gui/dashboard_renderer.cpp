#include "gui/dashboard_renderer.h"

#include "app/locale.h"
#include "platform/windows/windows_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace workboost::gui {
namespace {

constexpr COLORREF kWindowBackground = RGB(247, 248, 250);
constexpr COLORREF kSidebarBackground = RGB(243, 245, 248);
constexpr COLORREF kSurface = RGB(255, 255, 255);
constexpr COLORREF kPrimaryText = RGB(30, 32, 36);
constexpr COLORREF kSecondaryText = RGB(102, 110, 122);
constexpr COLORREF kMutedText = RGB(137, 145, 157);
constexpr COLORREF kBorder = RGB(226, 229, 234);
constexpr COLORREF kAccent = RGB(48, 104, 189);
constexpr COLORREF kAccentHover = RGB(39, 88, 164);
constexpr COLORREF kAccentSoft = RGB(236, 243, 253);
constexpr COLORREF kTrack = RGB(232, 235, 240);
constexpr COLORREF kSuccess = RGB(64, 126, 91);
constexpr COLORREF kWarning = RGB(186, 116, 42);
constexpr COLORREF kCritical = RGB(185, 76, 70);
constexpr COLORREF kInfo = RGB(90, 111, 139);

class SelectScope {
 public:
  SelectScope(HDC dc, HGDIOBJ object) : dc_(dc), previous_(SelectObject(dc, object)) {}
  ~SelectScope() {
    if (previous_ != nullptr) SelectObject(dc_, previous_);
  }

  SelectScope(const SelectScope&) = delete;
  SelectScope& operator=(const SelectScope&) = delete;

 private:
  HDC dc_{};
  HGDIOBJ previous_{};
};

RECT MakeRect(int left, int top, int right, int bottom) {
  return RECT{left, top, right, bottom};
}

RECT Inset(RECT value, int horizontal, int vertical) {
  value.left += horizontal;
  value.right -= horizontal;
  value.top += vertical;
  value.bottom -= vertical;
  return value;
}

int Width(const RECT& value) {
  return static_cast<int>(std::max(0L, value.right - value.left));
}

int Height(const RECT& value) {
  return static_cast<int>(std::max(0L, value.bottom - value.top));
}

void Fill(HDC dc, const RECT& bounds, COLORREF color) {
  SetDCBrushColor(dc, color);
  FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color,
          int width = 1) {
  const HPEN pen = CreatePen(PS_SOLID, width, color);
  if (pen == nullptr) return;
  {
    SelectScope select(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
  }
  DeleteObject(pen);
}

void RoundedRectangle(HDC dc, const RECT& bounds, COLORREF fill,
                      COLORREF border, int radius) {
  SetDCBrushColor(dc, fill);
  SetDCPenColor(dc, border);
  SelectScope brush(dc, GetStockObject(DC_BRUSH));
  SelectScope pen(dc, GetStockObject(DC_PEN));
  RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, radius,
            radius);
}

void EllipseFill(HDC dc, const RECT& bounds, COLORREF fill) {
  SetDCBrushColor(dc, fill);
  SelectScope brush(dc, GetStockObject(DC_BRUSH));
  SelectScope pen(dc, GetStockObject(NULL_PEN));
  Ellipse(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
}

void DrawWide(HDC dc, HFONT font, COLORREF color, const std::wstring& text,
              RECT bounds, UINT format) {
  SelectScope select(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &bounds, format);
}

void DrawUtf8(HDC dc, HFONT font, COLORREF color, const std::string& text,
              RECT bounds, UINT format) {
  DrawWide(dc, font, color, windows::Utf8ToWide(text), bounds, format);
}

int MeasureFontHeight(HDC dc, HFONT font) {
  if (dc == nullptr || font == nullptr) return 0;
  SelectScope select(dc, font);
  TEXTMETRICW metrics{};
  return GetTextMetricsW(dc, &metrics) ? metrics.tmHeight : 0;
}

std::string FormatBytes(double bytes) {
  constexpr double kKiB = 1024.0;
  constexpr double kMiB = kKiB * 1024.0;
  constexpr double kGiB = kMiB * 1024.0;
  std::ostringstream output;
  output << std::fixed << std::setprecision(1);
  if (bytes >= kGiB) {
    output << bytes / kGiB << " GB";
  } else if (bytes >= kMiB) {
    output << bytes / kMiB << " MB";
  } else if (bytes >= kKiB) {
    output << bytes / kKiB << " KB";
  } else {
    output << bytes << " B";
  }
  return output.str();
}

std::string FormatRate(double bytes) { return FormatBytes(bytes) + "/s"; }

std::string FormatPercent(double value) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(value >= 10.0 ? 0 : 1) << value
         << '%';
  return output.str();
}

std::string FormatLatency(double value) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(value >= 10.0 ? 0 : 1) << value
         << " ms";
  return output.str();
}

double ClampRatio(double value) {
  if (!std::isfinite(value)) return 0.0;
  return std::clamp(value, 0.0, 1.0);
}

COLORREF SeverityColor(const std::string& severity) {
  if (severity == "Critical" || severity == "High" || severity == "HIGH" ||
      severity == "CRITICAL") {
    return kCritical;
  }
  if (severity == "Medium" || severity == "MEDIUM") return kWarning;
  return kInfo;
}

const char* ImpactText(ImpactLevel impact) {
  switch (impact) {
    case ImpactLevel::High: return "High";
    case ImpactLevel::Medium: return "Medium";
    case ImpactLevel::Low: return "Low";
  }
  return "Low";
}

COLORREF ImpactColor(ImpactLevel impact) {
  switch (impact) {
    case ImpactLevel::High: return kCritical;
    case ImpactLevel::Medium: return kWarning;
    case ImpactLevel::Low: return kSecondaryText;
  }
  return kSecondaryText;
}

int ImpactRank(ImpactLevel impact) {
  switch (impact) {
    case ImpactLevel::High: return 2;
    case ImpactLevel::Medium: return 1;
    case ImpactLevel::Low: return 0;
  }
  return 0;
}

bool IsHovered(const std::optional<DashboardUiCommand>& hovered,
               const DashboardUiCommand& command) {
  return hovered && *hovered == command;
}

}  // namespace

DashboardOverviewLayout CalculateDashboardOverviewLayout(
    int maximum_height, int overview_width, std::size_t disk_count,
    int primary_text_height, int secondary_text_height) {
  constexpr int kHeaderHeight = 44;
  constexpr int kBottomPadding = 8;
  constexpr int kDiskColumnGap = 12;
  const std::size_t item_count = std::max<std::size_t>(1, disk_count);
  const int primary_height = std::max(1, primary_text_height);
  const int secondary_height = std::max(1, secondary_text_height);

  const auto calculate = [&](int primary_line_height, int metric_row_height,
                             int disk_row_height,
                             int minimum_column_width,
                             std::size_t column_limit, bool compact) {
    DashboardOverviewLayout layout;
    layout.primary_line_height = primary_line_height;
    layout.metric_row_height = metric_row_height;
    layout.disk_row_height = disk_row_height;
    layout.compact = compact;

    const int width_capacity = std::max(
        1, (std::max(0, overview_width) + kDiskColumnGap) /
               (minimum_column_width + kDiskColumnGap));
    const std::size_t maximum_columns = std::max<std::size_t>(
        1, std::min<std::size_t>(column_limit, width_capacity));
    const int fixed_height =
        kHeaderHeight + 3 * metric_row_height + kBottomPadding;
    const int available_disk_rows = std::max(
        1, (std::max(0, maximum_height - fixed_height)) / disk_row_height);
    const std::size_t requested_columns =
        (item_count + static_cast<std::size_t>(available_disk_rows) - 1) /
        static_cast<std::size_t>(available_disk_rows);
    layout.disk_columns =
        std::clamp(requested_columns, std::size_t{1}, maximum_columns);
    layout.disk_rows =
        (item_count + layout.disk_columns - 1) / layout.disk_columns;
    layout.required_height =
        fixed_height + static_cast<int>(layout.disk_rows) * disk_row_height;
    layout.fits = layout.required_height <= maximum_height;
    return layout;
  };

  const int regular_primary_height = primary_height + 2;
  const int regular_row_height =
      regular_primary_height + secondary_height + 3;
  auto layout = calculate(regular_primary_height, regular_row_height,
                          regular_row_height, 152, 3, false);
  if (layout.fits) return layout;

  const int compact_primary_height = primary_height + 1;
  const int compact_row_height =
      compact_primary_height + secondary_height + 1;
  auto compact_layout = calculate(compact_primary_height, compact_row_height,
                                  compact_row_height, 112, 4, true);
  if (compact_layout.fits ||
      compact_layout.required_height < layout.required_height) {
    return compact_layout;
  }
  return layout;
}

DashboardRenderer::~DashboardRenderer() {
  for (HFONT font : {brand_font_, page_title_font_, section_font_, body_font_,
                     small_font_, metric_font_, mono_font_}) {
    if (font != nullptr) DeleteObject(font);
  }
}

bool DashboardRenderer::Initialize(HWND window) {
  window_ = window;
  HDC dc = GetDC(window_);
  if (dc == nullptr) return false;
  dpi_ = std::max(96, GetDeviceCaps(dc, LOGPIXELSX));
  ReleaseDC(window_, dc);
  RecreateFonts(dpi_);
  return brand_font_ != nullptr && page_title_font_ != nullptr &&
         section_font_ != nullptr && body_font_ != nullptr &&
         small_font_ != nullptr && metric_font_ != nullptr &&
         mono_font_ != nullptr;
}

void DashboardRenderer::NotifyLocaleChanged() { RecreateFonts(dpi_); }

void DashboardRenderer::RecreateFonts(int dpi) {
  for (HFONT* font : {&brand_font_, &page_title_font_, &section_font_,
                      &body_font_, &small_font_, &metric_font_, &mono_font_}) {
    if (*font != nullptr) {
      DeleteObject(*font);
      *font = nullptr;
    }
  }
  const auto create = [dpi](int points, int weight, const wchar_t* family) {
    return CreateFontW(-MulDiv(points, dpi, 72), 0, 0, 0, weight, FALSE,
                       FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                       family);
  };
  // Segoe UI has no CJK glyphs; use Microsoft YaHei UI for the text fonts when
  // the interface language is Chinese. Metric/mono stay Consolas (numbers and
  // the English text pages).
  const wchar_t* text_family =
      Locale::IsChinese() ? L"Microsoft YaHei UI" : L"Segoe UI";
  brand_font_ = create(17, FW_SEMIBOLD, text_family);
  page_title_font_ = create(21, FW_SEMIBOLD, text_family);
  section_font_ = create(12, FW_SEMIBOLD, text_family);
  body_font_ = create(10, FW_NORMAL, text_family);
  small_font_ = create(9, FW_NORMAL, text_family);
  metric_font_ = create(11, FW_SEMIBOLD, L"Consolas");
  mono_font_ = create(9, FW_NORMAL, L"Consolas");

  HDC metrics_dc = GetDC(window_);
  body_line_height_ = MeasureFontHeight(metrics_dc, body_font_);
  small_line_height_ = MeasureFontHeight(metrics_dc, small_font_);
  metric_line_height_ = MeasureFontHeight(metrics_dc, metric_font_);
  if (metrics_dc != nullptr) ReleaseDC(window_, metrics_dc);
  body_line_height_ =
      std::max(body_line_height_, std::max(1, MulDiv(14, dpi, 96)));
  small_line_height_ =
      std::max(small_line_height_, std::max(1, MulDiv(13, dpi, 96)));
  metric_line_height_ =
      std::max(metric_line_height_, std::max(1, MulDiv(16, dpi, 96)));
}

int DashboardRenderer::Scale(int value) const {
  return MulDiv(value, dpi_, 96);
}

void DashboardRenderer::AddTarget(const RECT& bounds,
                                  DashboardUiCommand command) {
  hit_targets_.push_back(HitTarget{bounds, command});
}

std::optional<DashboardUiCommand> DashboardRenderer::HitTest(
    POINT point) const {
  for (auto it = hit_targets_.rbegin(); it != hit_targets_.rend(); ++it) {
    if (PtInRect(&it->bounds, point)) return it->command;
  }
  return std::nullopt;
}

void DashboardRenderer::Paint(
    HDC target, const RECT& client,
    const std::optional<DashboardViewModel>& model, DashboardPage current_page,
    const std::optional<DashboardUiCommand>& hovered,
    const std::string& status_message,
    const ProcessViewOptions& process_options,
    const CodingModeViewOptions& coding_options) {
  hit_targets_.clear();
  const int width = Width(client);
  const int height = Height(client);
  if (width <= 0 || height <= 0) return;
  HDC buffer = CreateCompatibleDC(target);
  HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
  if (buffer == nullptr || bitmap == nullptr) {
    if (bitmap != nullptr) DeleteObject(bitmap);
    if (buffer != nullptr) DeleteDC(buffer);
    DrawFrame(target, client, model, current_page, hovered, status_message,
              process_options, coding_options);
    return;
  }
  {
    SelectScope select(buffer, bitmap);
    DrawFrame(buffer, client, model, current_page, hovered, status_message,
              process_options, coding_options);
    BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
  }
  DeleteObject(bitmap);
  DeleteDC(buffer);
}

void DashboardRenderer::DrawFrame(
    HDC dc, const RECT& client,
    const std::optional<DashboardViewModel>& model, DashboardPage current_page,
    const std::optional<DashboardUiCommand>& hovered,
    const std::string& status_message,
    const ProcessViewOptions& process_options,
    const CodingModeViewOptions& coding_options) {
  Fill(dc, client, kWindowBackground);
  const int sidebar_width = Scale(200);
  const int header_height = Scale(72);
  const int margin = Scale(24);
  const RECT sidebar = MakeRect(0, 0, sidebar_width, client.bottom);
  const RECT header =
      MakeRect(sidebar_width, 0, client.right, header_height);
  Fill(dc, sidebar, kSidebarBackground);
  Fill(dc, header, kSurface);
  Line(dc, sidebar_width - 1, 0, sidebar_width - 1, client.bottom, kBorder);
  Line(dc, sidebar_width, header_height - 1, client.right,
       header_height - 1, kBorder);

  DrawUtf8(dc, brand_font_, kPrimaryText, "WorkBoost",
           MakeRect(Scale(24), Scale(18), sidebar_width - Scale(16),
                    Scale(48)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawUtf8(dc, small_font_, kSecondaryText, Locale::Get("Developer performance"),
           MakeRect(Scale(24), Scale(45), sidebar_width - Scale(16),
                    Scale(66)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);

  const auto& names = DashboardPresenter::PageNames();
  const std::array<DashboardPage, kDashboardPageCount> pages{
      DashboardPage::Dashboard,         DashboardPage::Processes,
      DashboardPage::Diagnosis,         DashboardPage::CodingMode,
      DashboardPage::ProtectedWorkload, DashboardPage::Recovery,
      DashboardPage::Settings};
  int navigation_y = Scale(84);
  for (std::size_t index = 0; index < pages.size(); ++index) {
    if (pages[index] == DashboardPage::Settings) {
      navigation_y =
          std::max(navigation_y, static_cast<int>(client.bottom) - Scale(62));
    }
    const RECT item = MakeRect(Scale(12), navigation_y,
                               sidebar_width - Scale(12),
                               navigation_y + Scale(42));
    const DashboardUiCommand command{DashboardUiAction::Navigate,
                                     pages[index]};
    if (pages[index] == current_page) {
      RoundedRectangle(dc, item, kSurface, kBorder, Scale(6));
      Fill(dc, MakeRect(item.left, item.top + Scale(8),
                        item.left + Scale(3), item.bottom - Scale(8)),
           kAccent);
    } else if (IsHovered(hovered, command)) {
      RoundedRectangle(dc, item, RGB(238, 241, 245), RGB(238, 241, 245),
                       Scale(6));
    }
    const int icon_x = item.left + Scale(16);
    const int icon_y = item.top + Scale(21);
    SetDCPenColor(dc, pages[index] == current_page ? kAccent : kSecondaryText);
    SelectScope pen(dc, GetStockObject(DC_PEN));
    SelectScope brush(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, icon_x - Scale(6), icon_y - Scale(6), icon_x + Scale(6),
              icon_y + Scale(6));
    if (index % 2 == 0) {
      Line(dc, icon_x - Scale(3), icon_y, icon_x + Scale(3), icon_y,
           pages[index] == current_page ? kAccent : kSecondaryText);
    }
    DrawUtf8(dc, body_font_,
             pages[index] == current_page ? kPrimaryText : kSecondaryText,
             Locale::Get(names[index]),
             MakeRect(item.left + Scale(38), item.top, item.right - Scale(8),
                      item.bottom),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    AddTarget(item, command);
    navigation_y += Scale(46);
  }

  const std::size_t page_index = static_cast<std::size_t>(current_page);
  DrawUtf8(dc, page_title_font_, kPrimaryText,
           Locale::Get(names[page_index]),
           MakeRect(sidebar_width + margin, Scale(16),
                    sidebar_width + margin + Scale(300), header_height),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);

  const std::string mode = model ? model->mode : "Initializing";
  const int pill_width = Scale(mode == "Safe Mode" ? 112 : 126);
  const RECT mode_pill =
      MakeRect(client.right - margin - pill_width, Scale(19),
               client.right - margin, Scale(53));
  const COLORREF mode_color =
      mode == "Safe Mode" ? kCritical
                           : mode == "Monitor Mode" ? kInfo : kSuccess;
  RoundedRectangle(dc, mode_pill, kSurface, kBorder, Scale(17));
  EllipseFill(dc,
              MakeRect(mode_pill.left + Scale(12), mode_pill.top + Scale(13),
                       mode_pill.left + Scale(20), mode_pill.top + Scale(21)),
              mode_color);
  DrawUtf8(dc, small_font_, kPrimaryText, Locale::Get(mode),
           MakeRect(mode_pill.left + Scale(27), mode_pill.top,
                    mode_pill.right - Scale(8), mode_pill.bottom),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  const DashboardUiCommand refresh_command{DashboardUiAction::Refresh,
                                            current_page};
  const DashboardUiCommand export_command{DashboardUiAction::Export,
                                           current_page};
  const int compact_width = Scale(76);
  const RECT export_button =
      MakeRect(mode_pill.left - Scale(12) - compact_width, Scale(19),
               mode_pill.left - Scale(12), Scale(53));
  const RECT refresh_button =
      MakeRect(export_button.left - Scale(8) - compact_width, Scale(19),
               export_button.left - Scale(8), Scale(53));
  for (const auto& [bounds, command, label] :
       std::array<std::tuple<RECT, DashboardUiCommand, const char*>, 2>{
           std::make_tuple(refresh_button, refresh_command, "Refresh"),
           std::make_tuple(export_button, export_command, "Export")}) {
    RoundedRectangle(dc, bounds,
                     IsHovered(hovered, command) ? RGB(245, 247, 250)
                                                 : kSurface,
                     kBorder, Scale(5));
    DrawUtf8(dc, small_font_, kPrimaryText, Locale::Get(label), bounds,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddTarget(bounds, command);
  }

  RECT content = MakeRect(sidebar_width + margin, header_height + margin,
                          client.right - margin, client.bottom - margin);
  const int maximum_content_width = Scale(1500);
  if (Width(content) > maximum_content_width) {
    content.right = content.left + maximum_content_width;
  }
  if (!model) {
    DrawUtf8(dc, section_font_, kPrimaryText,
             Locale::Get("Collecting system metrics..."),
             MakeRect(content.left, content.top, content.right,
                      content.top + Scale(28)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawUtf8(dc, body_font_, kSecondaryText,
             status_message.empty()
                 ? Locale::Get(
                       "The first sample normally takes about one second.")
                 : status_message,
             MakeRect(content.left, content.top + Scale(36), content.right,
                      content.top + Scale(68)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return;
  }

  switch (current_page) {
    case DashboardPage::Dashboard:
      DrawDashboard(dc, content, *model, hovered);
      break;
    case DashboardPage::Processes:
      DrawProcesses(dc, content, *model, process_options, hovered);
      break;
    case DashboardPage::Diagnosis: DrawDiagnosis(dc, content, *model); break;
    case DashboardPage::ProtectedWorkload:
      DrawProtected(dc, content, *model);
      break;
    case DashboardPage::CodingMode:
      DrawCodingMode(dc, content, *model, coding_options, hovered);
      break;
    case DashboardPage::Recovery:
      DrawRecovery(dc, content, *model, hovered);
      break;
    case DashboardPage::Settings:
      DrawSettings(dc, content, *model, hovered);
      break;
    case DashboardPage::Count: break;
  }
}

void DashboardRenderer::DrawDashboard(
    HDC dc, const RECT& content, const DashboardViewModel& model,
    const std::optional<DashboardUiCommand>& hovered) {
  const int gap = Scale(16);
  const int coding_height = Scale(92);
  const int available =
      std::max(Scale(320), Height(content) - coding_height - gap * 2);
  const int panel_margin = Scale(16);
  const int left_width = static_cast<int>(Width(content) * 0.56);
  const int overview_width =
      std::max(0, left_width - gap / 2 - panel_margin * 2);
  const int base_top_height =
      std::clamp(static_cast<int>(available * 0.58), Scale(220), Scale(300));
  const int preferred_top_height =
      std::max(Scale(220), available - Scale(132));
  const int maximum_top_height =
      std::max(preferred_top_height, available - Scale(82));
  const auto to_logical = [this](int value) {
    return MulDiv(value, 96, dpi_);
  };
  const auto to_logical_ceil = [this](int value) {
    return std::max(1, (value * 96 + dpi_ - 1) / dpi_);
  };
  const int primary_text_height =
      to_logical_ceil(std::max(body_line_height_, metric_line_height_));
  const int secondary_text_height = to_logical_ceil(small_line_height_);
  auto overview_layout = CalculateDashboardOverviewLayout(
      to_logical(preferred_top_height), to_logical(overview_width),
      model.disks.size(), primary_text_height, secondary_text_height);
  if (!overview_layout.fits && maximum_top_height > preferred_top_height) {
    const auto expanded_layout = CalculateDashboardOverviewLayout(
        to_logical(maximum_top_height), to_logical(overview_width),
        model.disks.size(), primary_text_height, secondary_text_height);
    if (expanded_layout.fits ||
        expanded_layout.required_height < overview_layout.required_height) {
      overview_layout = expanded_layout;
    }
  }
  const int row_height = Scale(overview_layout.metric_row_height);
  const int disk_row_height = Scale(overview_layout.disk_row_height);
  const int required_top_height = Scale(overview_layout.required_height);
  const int top_height = std::clamp(
      std::max(base_top_height, required_top_height), Scale(220),
      maximum_top_height);
  const int bottom_height =
      std::clamp(available - top_height, Scale(82), Scale(180));
  const RECT system_panel =
      MakeRect(content.left, content.top, content.left + left_width - gap / 2,
               content.top + top_height);
  const RECT diagnosis_panel =
      MakeRect(system_panel.right + gap, content.top, content.right,
               content.top + top_height);
  const RECT impact_panel =
      MakeRect(content.left, system_panel.bottom + gap, system_panel.right,
               system_panel.bottom + gap + bottom_height);
  const RECT protected_panel =
      MakeRect(diagnosis_panel.left, diagnosis_panel.bottom + gap,
               content.right, diagnosis_panel.bottom + gap + bottom_height);
  const RECT coding_panel =
      MakeRect(content.left, impact_panel.bottom + gap, content.right,
               std::min(static_cast<int>(content.bottom),
                        static_cast<int>(impact_panel.bottom) + gap +
                            coding_height));
  for (const RECT& panel :
       {system_panel, diagnosis_panel, impact_panel, protected_panel}) {
    RoundedRectangle(dc, panel, kSurface, kBorder, Scale(7));
  }

  DrawUtf8(dc, section_font_, kPrimaryText, Locale::Get("System Overview"),
           MakeRect(system_panel.left + panel_margin,
                    system_panel.top + Scale(10), system_panel.right,
                    system_panel.top + Scale(38)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  const int content_left = system_panel.left + panel_margin;
  const int content_right = system_panel.right - panel_margin;
  int row_y = system_panel.top + Scale(44);
  auto draw_metric = [&](const std::string& label, double ratio,
                         const std::string& value, const std::string& detail,
                         const std::string& subtitle, const RECT& bounds,
                         int preferred_label_width,
                         int preferred_value_width, int metric_height,
                         bool dense) {
    const int column_gap = Scale(dense ? 6 : 12);
    const int minimum_bar_width = Scale(dense ? 20 : 32);
    const int minimum_value_width = Scale(dense ? 36 : 84);
    const int maximum_label_width =
        std::max(Scale(36), Width(bounds) - column_gap * 2 -
                                minimum_value_width - minimum_bar_width);
    const int label_width =
        std::clamp(preferred_label_width, Scale(36), maximum_label_width);
    const int available_value_width =
        std::max(0, Width(bounds) - label_width - column_gap * 2 -
                        minimum_bar_width);
    const int value_width =
        std::min(preferred_value_width, available_value_width);
    const int value_left = bounds.left + label_width + column_gap;
    const int bar_left = value_left + value_width + column_gap;
    const int primary_height = std::min(
        metric_height, Scale(overview_layout.primary_line_height));
    const int track_height = std::max(1, Scale(5));
    const int track_y =
        bounds.top + std::max(0, (primary_height - track_height) / 2);
    const RECT primary =
        MakeRect(bounds.left, bounds.top, bounds.right,
                 bounds.top + primary_height);
    DrawUtf8(dc, body_font_, kPrimaryText, Locale::Get(label),
             MakeRect(primary.left, primary.top,
                      primary.left + label_width, primary.bottom),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (bar_left < bounds.right) {
      const RECT track =
          MakeRect(bar_left, track_y, bounds.right, track_y + track_height);
      RoundedRectangle(dc, track, kTrack, kTrack, Scale(3));
      RECT progress = track;
      progress.right = progress.left +
                       static_cast<int>(Width(track) * ClampRatio(ratio));
      if (progress.right > progress.left) {
        RoundedRectangle(dc, progress,
                         ratio >= 0.9 ? kCritical
                                      : ratio >= 0.75 ? kWarning : kAccent,
                         ratio >= 0.9 ? kCritical
                                      : ratio >= 0.75 ? kWarning : kAccent,
                         Scale(3));
      }
    }
    DrawUtf8(dc, metric_font_, kPrimaryText, value,
             MakeRect(value_left, primary.top, bar_left - column_gap,
                      primary.bottom),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const std::string& secondary = detail.empty() ? subtitle : detail;
    if (!secondary.empty()) {
      const int secondary_left = detail.empty() ? bounds.left : value_left;
      const int secondary_right =
          detail.empty() ? bounds.right : bar_left - column_gap;
      DrawUtf8(dc, small_font_, kSecondaryText, secondary,
               MakeRect(secondary_left, primary.bottom, secondary_right,
                        bounds.top + metric_height),
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
  };

  const int main_label_width = Scale(72);
  const int main_value_width = Scale(170);
  auto draw_main_metric = [&](const std::string& label, double ratio,
                              const std::string& value,
                              const std::string& detail,
                              const std::string& subtitle) {
    draw_metric(label, ratio, value, detail, subtitle,
                MakeRect(content_left, row_y, content_right,
                         row_y + row_height),
                main_label_width, main_value_width, row_height,
                false);
    row_y += row_height;
  };

  draw_main_metric("CPU", model.system.cpu_percent / 100.0,
                   FormatPercent(model.system.cpu_percent), std::string{},
                   model.system.cpu_model);
  draw_main_metric(
      "Memory", model.system.memory_used_ratio,
      FormatBytes(static_cast<double>(model.system.memory_used_bytes)) +
          " / " +
          FormatBytes(static_cast<double>(model.system.memory_total_bytes)),
      std::string{}, model.system.memory_model);
  draw_main_metric(
      "Commit", model.system.commit_ratio,
      FormatPercent(model.system.commit_ratio * 100.0),
      Locale::Format(
          "{0} page reads/s",
          {std::to_string(
              static_cast<int>(model.system.page_reads_per_sec))}),
      std::string{});

  const int disk_column_gap = Scale(12);
  const int disk_columns =
      std::max(1, static_cast<int>(overview_layout.disk_columns));
  const int disk_width =
      std::max(1, (content_right - content_left -
                   disk_column_gap * (disk_columns - 1)) /
                      disk_columns);
  for (std::size_t index = 0; index < model.disks.size(); ++index) {
    const auto& disk = model.disks[index];
    const int column = static_cast<int>(index % overview_layout.disk_columns);
    const int row = static_cast<int>(index / overview_layout.disk_columns);
    const int left = content_left + column * (disk_width + disk_column_gap);
    const int right = column + 1 == disk_columns ? content_right
                                                 : left + disk_width;
    const int top = row_y + row * disk_row_height;
    const bool dense = disk_columns > 1;
    const int disk_label_width =
        dense ? std::clamp(disk_width / 3, Scale(40), Scale(64))
              : main_label_width;
    draw_metric(disk.media + " " + disk.name,
                disk.active_percent / 100.0,
                FormatPercent(disk.active_percent),
                FormatLatency(disk.latency_ms), std::string{},
                MakeRect(left, top, right, top + disk_row_height),
                disk_label_width,
                dense ? Scale(56) : main_value_width, disk_row_height, dense);
  }
  if (model.disks.empty()) {
    DrawUtf8(dc, small_font_, kMutedText,
             Locale::Get("No physical disk counters available"),
             MakeRect(system_panel.left + panel_margin, row_y,
                      system_panel.right - panel_margin,
                      row_y + disk_row_height),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }

  DrawUtf8(dc, section_font_, kPrimaryText, Locale::Get("Diagnosis"),
           MakeRect(diagnosis_panel.left + panel_margin,
                    diagnosis_panel.top + Scale(10), diagnosis_panel.right,
                    diagnosis_panel.top + Scale(38)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  if (model.diagnoses.empty()) {
    EllipseFill(dc,
                MakeRect(diagnosis_panel.left + panel_margin,
                         diagnosis_panel.top + Scale(56),
                         diagnosis_panel.left + panel_margin + Scale(10),
                         diagnosis_panel.top + Scale(66)),
                kInfo);
    DrawUtf8(dc, body_font_, kPrimaryText,
             Locale::Get("No significant bottleneck detected."),
             MakeRect(diagnosis_panel.left + panel_margin + Scale(20),
                      diagnosis_panel.top + Scale(46),
                      diagnosis_panel.right - panel_margin,
                      diagnosis_panel.top + Scale(78)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, small_font_, kSecondaryText,
             Locale::Get(
                 "Keep this window open to build a longer evidence window."),
             MakeRect(diagnosis_panel.left + panel_margin,
                      diagnosis_panel.top + Scale(86),
                      diagnosis_panel.right - panel_margin,
                      diagnosis_panel.top + Scale(122)),
             DT_LEFT | DT_TOP | DT_WORDBREAK);
  } else {
    int diagnosis_y = diagnosis_panel.top + Scale(48);
    const std::size_t count = std::min<std::size_t>(2, model.diagnoses.size());
    for (std::size_t i = 0; i < count; ++i) {
      const auto& diagnosis = model.diagnoses[i];
      const COLORREF color = SeverityColor(diagnosis.severity);
      Fill(dc,
           MakeRect(diagnosis_panel.left + panel_margin, diagnosis_y,
                    diagnosis_panel.left + panel_margin + Scale(3),
                    diagnosis_y + Scale(76)),
           color);
      DrawUtf8(dc, small_font_, color, Locale::Get(diagnosis.severity),
               MakeRect(diagnosis_panel.left + panel_margin + Scale(12),
                        diagnosis_y - Scale(2),
                        diagnosis_panel.right - panel_margin,
                        diagnosis_y + Scale(18)),
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      DrawUtf8(dc, section_font_, kPrimaryText, Locale::Get(diagnosis.type),
               MakeRect(diagnosis_panel.left + panel_margin + Scale(12),
                        diagnosis_y + Scale(18),
                        diagnosis_panel.right - panel_margin,
                        diagnosis_y + Scale(42)),
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      DrawUtf8(dc, small_font_, kSecondaryText, diagnosis.summary,
               MakeRect(diagnosis_panel.left + panel_margin + Scale(12),
                        diagnosis_y + Scale(43),
                        diagnosis_panel.right - panel_margin,
                        diagnosis_y + Scale(72)),
               DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
      diagnosis_y += Scale(88);
    }
  }

  DrawUtf8(dc, section_font_, kPrimaryText, Locale::Get("Top Impact"),
           MakeRect(impact_panel.left + panel_margin,
                    impact_panel.top + Scale(9), impact_panel.right,
                    impact_panel.top + Scale(36)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  const int impact_left = impact_panel.left + panel_margin;
  const int impact_right = impact_panel.right - panel_margin;
  const int impact_width = std::max(0, impact_right - impact_left);
  const int name_right = impact_left + impact_width * 38 / 100;
  const int cpu_right = impact_left + impact_width * 51 / 100;
  const int memory_right = impact_left + impact_width * 68 / 100;
  const int io_right = impact_left + impact_width * 86 / 100;
  const int header_top = impact_panel.top + Scale(34);
  const int header_bottom = header_top + Scale(16);
  const int column_gap = Scale(5);
  DrawUtf8(dc, small_font_, kMutedText, Locale::Get("Name"),
           MakeRect(impact_left, header_top, name_right - column_gap,
                    header_bottom),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  DrawUtf8(dc, small_font_, kMutedText, Locale::Get("CPU"),
           MakeRect(name_right, header_top, cpu_right - column_gap,
                    header_bottom),
           DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
  DrawUtf8(dc, small_font_, kMutedText, Locale::Get("Memory"),
           MakeRect(cpu_right, header_top, memory_right - column_gap,
                    header_bottom),
           DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  DrawUtf8(dc, small_font_, kMutedText, Locale::Get("Disk I/O"),
           MakeRect(memory_right, header_top, io_right - column_gap,
                    header_bottom),
           DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  DrawUtf8(dc, small_font_, kMutedText, Locale::Get("Impact"),
           MakeRect(io_right, header_top, impact_right, header_bottom),
           DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  int impact_y = header_bottom;
  const int impact_row_height = Scale(28);
  const int available_impact_height =
      std::max(0, static_cast<int>(impact_panel.bottom) - impact_y);
  const std::size_t impact_capacity =
      static_cast<std::size_t>(available_impact_height / impact_row_height);
  const std::size_t impact_count =
      std::min({std::size_t{3}, model.top_impacts.size(), impact_capacity});
  for (std::size_t i = 0; i < impact_count; ++i) {
    const auto& process = model.top_impacts[i];
    DrawUtf8(dc, body_font_, kPrimaryText, process.name,
             MakeRect(impact_left, impact_y, name_right - column_gap,
                      impact_y + impact_row_height),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, metric_font_, ImpactColor(process.cpu_impact),
             FormatPercent(process.cpu_percent),
             MakeRect(name_right, impact_y, cpu_right - column_gap,
                      impact_y + impact_row_height),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, metric_font_, ImpactColor(process.memory_impact),
             FormatBytes(static_cast<double>(process.private_bytes)),
             MakeRect(cpu_right, impact_y, memory_right - column_gap,
                      impact_y + impact_row_height),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, metric_font_, ImpactColor(process.io_impact),
             FormatRate(process.io_bytes_per_sec),
             MakeRect(memory_right, impact_y, io_right - column_gap,
                      impact_y + impact_row_height),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, small_font_, ImpactColor(process.impact),
             Locale::Get(ImpactText(process.impact)),
             MakeRect(io_right, impact_y, impact_right,
                      impact_y + impact_row_height),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    impact_y += impact_row_height;
  }
  if (model.top_impacts.empty()) {
    DrawUtf8(dc, small_font_, kMutedText,
             Locale::Get("No process inventory available"),
             MakeRect(impact_left, impact_y, impact_right,
                      impact_y + impact_row_height),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }

  DrawUtf8(dc, section_font_, kPrimaryText, Locale::Get("Protected Workload"),
           MakeRect(protected_panel.left + panel_margin,
                    protected_panel.top + Scale(9), protected_panel.right,
                    protected_panel.top + Scale(36)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  int protected_y = protected_panel.top + Scale(42);
  const std::size_t protected_capacity = static_cast<std::size_t>(std::max(
      0, (Height(protected_panel) - Scale(42)) / Scale(39)));
  const std::size_t protected_count =
      std::min(
          {std::size_t{3}, model.protected_workloads.size(),
           protected_capacity});
  for (std::size_t i = 0; i < protected_count; ++i) {
    const auto& workload = model.protected_workloads[i];
    EllipseFill(dc,
                MakeRect(protected_panel.left + panel_margin,
                         protected_y + Scale(8),
                         protected_panel.left + panel_margin + Scale(7),
                         protected_y + Scale(15)),
                kSuccess);
    DrawUtf8(dc, body_font_, kPrimaryText, workload.name,
             MakeRect(protected_panel.left + panel_margin + Scale(16),
                      protected_y, protected_panel.right - panel_margin,
                      protected_y + Scale(22)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, small_font_, kSecondaryText,
             Locale::Get(workload.category) + "  ·  " +
                 Locale::Get(workload.reason),
             MakeRect(protected_panel.left + panel_margin + Scale(16),
                      protected_y + Scale(18),
                      protected_panel.right - panel_margin,
                      protected_y + Scale(38)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    protected_y += Scale(39);
  }
  if (model.protected_workloads.empty()) {
    DrawUtf8(dc, small_font_, kMutedText,
             Locale::Get(
                 "No active developer workload detected"),
             MakeRect(protected_panel.left + panel_margin, protected_y,
                      protected_panel.right - panel_margin,
                      protected_y + Scale(28)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }

  RoundedRectangle(dc, coding_panel, kAccentSoft, RGB(202, 218, 242),
                   Scale(7));
  const std::string coding_title = model.coding_mode.safe_mode
                                       ? "Recovery required"
                                       : model.coding_mode.active
                                             ? "Coding Mode Active"
                                             : "Coding Mode";
  DrawUtf8(dc, section_font_, kPrimaryText, Locale::Get(coding_title),
           MakeRect(coding_panel.left + panel_margin,
                    coding_panel.top + Scale(11), coding_panel.right,
                    coding_panel.top + Scale(38)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  const std::string description =
      model.coding_mode.operation_in_progress
          ? model.coding_mode.operation_status
          : model.coding_mode.safe_mode
          ? "New system changes are blocked until the active session is restored."
          : model.coding_mode.active
                ? std::to_string(model.coding_mode.active_actions) +
                      " active changes · " +
                      std::to_string(model.coding_mode.protected_workloads) +
                      " protected workloads"
                : "Reduce background impact while keeping development connections protected.";
  DrawUtf8(dc, body_font_, kSecondaryText, description,
           MakeRect(coding_panel.left + panel_margin,
                    coding_panel.top + Scale(39),
                    coding_panel.right - Scale(210),
                    coding_panel.bottom - Scale(10)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  const RECT coding_button =
      MakeRect(coding_panel.right - Scale(186),
               coding_panel.top + Scale(25),
               coding_panel.right - panel_margin,
               coding_panel.bottom - Scale(25));
  const DashboardUiCommand coding_command{
      DashboardUiAction::CodingModePrimary, DashboardPage::CodingMode};
  const bool disabled = model.coding_mode.safe_mode ||
                        model.coding_mode.operation_in_progress;
  RoundedRectangle(dc, coding_button,
                   disabled ? RGB(203, 209, 218)
                            : IsHovered(hovered, coding_command)
                                  ? kAccentHover
                                  : kAccent,
                   disabled ? RGB(203, 209, 218)
                            : IsHovered(hovered, coding_command)
                                  ? kAccentHover
                                  : kAccent,
                   Scale(5));
  DrawUtf8(dc, body_font_, kSurface,
           model.coding_mode.operation_in_progress
               ? "Working..."
               : model.coding_mode.safe_mode ? "View Recovery"
                    : model.coding_mode.active ? "Exit and Restore"
                                               : "Enter Coding Mode",
           coding_button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  if (!model.coding_mode.operation_in_progress) {
    AddTarget(coding_button, coding_command);
  }
}

void DashboardRenderer::DrawProcesses(
    HDC dc, const RECT& content, const DashboardViewModel& model,
    const ProcessViewOptions& options,
    const std::optional<DashboardUiCommand>& hovered) {
  RoundedRectangle(dc, content, kSurface, kBorder, Scale(7));
  const int margin = Scale(20);
  std::vector<const ProcessViewModel*> processes;
  processes.reserve(model.processes.size());
  std::string search = options.search;
  std::transform(search.begin(), search.end(), search.begin(), [](char value) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(value)));
  });
  for (const auto& process : model.processes) {
    if (options.filter == ProcessFilter::HighImpact &&
        process.impact != ImpactLevel::High) {
      continue;
    }
    if (options.filter == ProcessFilter::Protected &&
        !process.protected_workload) {
      continue;
    }
    std::string name = process.name;
    std::transform(name.begin(), name.end(), name.begin(), [](char value) {
      return static_cast<char>(
          std::tolower(static_cast<unsigned char>(value)));
    });
    if (!search.empty() && name.find(search) == std::string::npos) continue;
    processes.push_back(&process);
  }
  std::stable_sort(
      processes.begin(), processes.end(),
      [&options](const ProcessViewModel* left,
                 const ProcessViewModel* right) {
        double left_value = 0.0;
        double right_value = 0.0;
        switch (options.sort) {
          case ProcessSort::Impact:
            left_value = ImpactRank(left->impact);
            right_value = ImpactRank(right->impact);
            break;
          case ProcessSort::Cpu:
            left_value = left->cpu_percent;
            right_value = right->cpu_percent;
            break;
          case ProcessSort::Memory:
            left_value = static_cast<double>(left->private_bytes);
            right_value = static_cast<double>(right->private_bytes);
            break;
          case ProcessSort::DiskIo:
            left_value = left->read_bytes_per_sec + left->write_bytes_per_sec;
            right_value =
                right->read_bytes_per_sec + right->write_bytes_per_sec;
            break;
        }
        if (left_value != right_value) return left_value > right_value;
        if (left->name != right->name) return left->name < right->name;
        return left->pid < right->pid;
      });

  DrawUtf8(dc, section_font_, kPrimaryText,
           Locale::Get("Processes") + "  " + std::to_string(processes.size()) +
               " / " + std::to_string(model.processes.size()),
           MakeRect(content.left + margin, content.top + Scale(8),
                    content.right - margin, content.top + Scale(38)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);

  const int control_y = content.top + Scale(42);
  int filter_x = content.left + margin;
  auto draw_filter = [&](const char* label, int width,
                         ProcessFilter filter,
                         DashboardUiAction action) mutable {
    const RECT bounds = MakeRect(filter_x, control_y,
                                 filter_x + Scale(width),
                                 control_y + Scale(30));
    const DashboardUiCommand command{action, DashboardPage::Processes};
    const bool active = options.filter == filter;
    RoundedRectangle(dc, bounds,
                     active ? kAccentSoft
                            : IsHovered(hovered, command)
                                  ? RGB(245, 247, 250)
                                  : kSurface,
                     active ? kAccent : kBorder, Scale(5));
    DrawUtf8(dc, small_font_, active ? kAccent : kSecondaryText,
             Locale::Get(label), bounds,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddTarget(bounds, command);
    filter_x = bounds.right + Scale(6);
  };
  draw_filter("All", 48, ProcessFilter::All,
              DashboardUiAction::ProcessFilterAll);
  draw_filter("High Impact", 92, ProcessFilter::HighImpact,
              DashboardUiAction::ProcessFilterHighImpact);
  draw_filter("Protected", 80, ProcessFilter::Protected,
              DashboardUiAction::ProcessFilterProtected);

  const RECT search_box =
      MakeRect(content.right - margin - Scale(220), control_y,
               content.right - margin, control_y + Scale(30));
  const DashboardUiCommand search_command{
      DashboardUiAction::FocusProcessSearch, DashboardPage::Processes};
  RoundedRectangle(dc, search_box, kSurface,
                   options.search_focused ? kAccent : kBorder, Scale(5));
  DrawUtf8(dc, body_font_,
           options.search.empty() ? kMutedText : kPrimaryText,
           options.search.empty() ? Locale::Get("Search processes...")
                                  : options.search,
           MakeRect(search_box.left + Scale(10), search_box.top,
                    search_box.right - Scale(10), search_box.bottom),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  if (options.search_focused) {
    Line(dc, search_box.left + Scale(10), search_box.bottom - Scale(5),
         search_box.right - Scale(10), search_box.bottom - Scale(5), kAccent,
         Scale(1));
  }
  AddTarget(search_box, search_command);

  const int header_y = content.top + Scale(82);
  Fill(dc, MakeRect(content.left + Scale(1), header_y, content.right - Scale(1),
                    header_y + Scale(34)),
       RGB(248, 249, 251));
  const int usable = Width(content) - margin * 2;
  const std::array<int, 6> columns{
      content.left + margin,
      content.left + margin + static_cast<int>(usable * 0.40),
      content.left + margin + static_cast<int>(usable * 0.53),
      content.left + margin + static_cast<int>(usable * 0.67),
      content.left + margin + static_cast<int>(usable * 0.82),
      content.right - margin};
  const std::array<const char*, 5> labels{
      "Name", "CPU", "Memory", "Disk I/O", "Impact / Status"};
  const std::array<DashboardUiAction, 5> sort_actions{
      DashboardUiAction::None, DashboardUiAction::ProcessSortCpu,
      DashboardUiAction::ProcessSortMemory, DashboardUiAction::ProcessSortIo,
      DashboardUiAction::ProcessSortImpact};
  for (std::size_t i = 0; i < labels.size(); ++i) {
    std::string label = Locale::Get(labels[i]);
    const bool active =
        (i == 1 && options.sort == ProcessSort::Cpu) ||
        (i == 2 && options.sort == ProcessSort::Memory) ||
        (i == 3 && options.sort == ProcessSort::DiskIo) ||
        (i == 4 && options.sort == ProcessSort::Impact);
    if (active) label += "  ↓";
    const RECT bounds = MakeRect(columns[i], header_y,
                                 columns[i + 1] - Scale(8),
                                 header_y + Scale(34));
    DrawUtf8(dc, small_font_, active ? kAccent : kSecondaryText, label,
             bounds,
             i == 0 ? DT_LEFT | DT_VCENTER | DT_SINGLELINE
                    : DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    if (sort_actions[i] != DashboardUiAction::None) {
      AddTarget(bounds,
                DashboardUiCommand{sort_actions[i],
                                   DashboardPage::Processes});
    }
  }
  int row_y = header_y + Scale(34);
  const int row_height = Scale(38);
  const ProcessViewModel* selected = nullptr;
  for (const auto& process : model.processes) {
    if (process.pid == options.selected_pid) {
      selected = &process;
      break;
    }
  }
  const int detail_height = selected == nullptr ? 0 : Scale(108);
  const std::size_t visible_rows = static_cast<std::size_t>(
      std::max(0, (static_cast<int>(content.bottom) - row_y - detail_height -
                   Scale(8)) /
                      row_height));
  const std::size_t count =
      std::min<std::size_t>(visible_rows, processes.size());
  for (std::size_t i = 0; i < count; ++i) {
    const auto& process = *processes[i];
    const DashboardUiCommand row_command{DashboardUiAction::SelectProcess,
                                         DashboardPage::Processes,
                                         process.pid};
    if (process.pid == options.selected_pid) {
      Fill(dc, MakeRect(content.left + Scale(1), row_y,
                        content.right - Scale(1), row_y + row_height),
           kAccentSoft);
    } else if (IsHovered(hovered, row_command)) {
      Fill(dc, MakeRect(content.left + Scale(1), row_y,
                        content.right - Scale(1), row_y + row_height),
           RGB(247, 249, 252));
    } else if (i % 2 != 0) {
      Fill(dc, MakeRect(content.left + Scale(1), row_y,
                        content.right - Scale(1), row_y + row_height),
           RGB(251, 252, 253));
    }
    DrawUtf8(dc, body_font_, kPrimaryText, process.name,
             MakeRect(columns[0], row_y, columns[1] - Scale(8),
                      row_y + row_height),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, metric_font_, kPrimaryText,
             FormatPercent(process.cpu_percent),
             MakeRect(columns[1], row_y, columns[2] - Scale(8),
                      row_y + row_height),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    DrawUtf8(dc, metric_font_, kPrimaryText,
             FormatBytes(static_cast<double>(process.working_set_bytes)),
             MakeRect(columns[2], row_y, columns[3] - Scale(8),
                      row_y + row_height),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, metric_font_, kSecondaryText,
             FormatRate(process.read_bytes_per_sec +
                        process.write_bytes_per_sec),
             MakeRect(columns[3], row_y, columns[4] - Scale(8),
                      row_y + row_height),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const std::string status =
        std::string(Locale::Get(ImpactText(process.impact))) +
        (process.protected_workload ? Locale::Get(" · Protected")
                                    : Locale::Get(" · Normal"));
    DrawUtf8(dc, small_font_, ImpactColor(process.impact), status,
             MakeRect(columns[4], row_y, columns[5], row_y + row_height),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Line(dc, content.left + margin, row_y + row_height - 1,
         content.right - margin, row_y + row_height - 1, kBorder);
    AddTarget(MakeRect(content.left + Scale(1), row_y,
                       content.right - Scale(1), row_y + row_height),
              row_command);
    row_y += row_height;
  }
  if (processes.empty()) {
    DrawUtf8(dc, body_font_, kMutedText,
             Locale::Get("No process matches the current search and filter."),
             MakeRect(content.left + margin, row_y + Scale(18),
                      content.right - margin, row_y + Scale(58)),
             DT_LEFT | DT_TOP | DT_WORDBREAK);
  } else if (count < processes.size()) {
    DrawUtf8(dc, small_font_, kMutedText,
             Locale::Format(
                 "Showing {0} of {1} matching processes. Refine the search "
                 "to narrow the list.",
                 {std::to_string(count), std::to_string(processes.size())}),
             MakeRect(content.left + margin, row_y + Scale(4),
                      content.right - margin, row_y + Scale(28)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }

  if (selected != nullptr) {
    const RECT detail = MakeRect(content.left + Scale(1),
                                 content.bottom - detail_height,
                                 content.right - Scale(1),
                                 content.bottom - Scale(1));
    Fill(dc, detail, RGB(249, 250, 252));
    Line(dc, detail.left + margin, detail.top, detail.right - margin,
         detail.top, kBorder);
    DrawUtf8(dc, section_font_, kPrimaryText,
             selected->name + "  ·  PID " + std::to_string(selected->pid),
             MakeRect(detail.left + margin, detail.top + Scale(10),
                      detail.right - margin, detail.top + Scale(38)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, small_font_, kSecondaryText,
             Locale::Get(selected->process_class) + "  ·  " +
                 Locale::Get(selected->protection) +
                 (selected->protected_workload ? Locale::Get(" · Protected")
                                               : Locale::Get(" · Normal")),
             MakeRect(detail.left + margin, detail.top + Scale(38),
                      detail.right - margin, detail.top + Scale(62)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const std::string metrics =
        Locale::Get("CPU") + " " + FormatPercent(selected->cpu_percent) +
        "    " + Locale::Get("Memory") + " " +
        FormatBytes(static_cast<double>(selected->working_set_bytes)) +
        "    " + Locale::Get("Private") + " " +
        FormatBytes(static_cast<double>(selected->private_bytes)) +
        "    " + Locale::Get("Read") + " " +
        FormatRate(selected->read_bytes_per_sec) + "    " +
        Locale::Get("Write") + " " +
        FormatRate(selected->write_bytes_per_sec);
    DrawUtf8(dc, mono_font_, kPrimaryText, metrics,
             MakeRect(detail.left + margin, detail.top + Scale(66),
                      detail.right - margin, detail.bottom - Scale(8)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
}

void DashboardRenderer::DrawDiagnosis(HDC dc, const RECT& content,
                                      const DashboardViewModel& model) {
  const int gap = Scale(14);
  int y = content.top;
  if (model.diagnoses.empty()) {
    const RECT empty =
        MakeRect(content.left, y, content.right,
                 std::min(static_cast<int>(content.bottom), y + Scale(130)));
    RoundedRectangle(dc, empty, kSurface, kBorder, Scale(7));
    DrawUtf8(dc, section_font_, kPrimaryText,
             Locale::Get("No significant bottleneck detected."),
             MakeRect(empty.left + Scale(20), empty.top + Scale(22),
                      empty.right - Scale(20), empty.top + Scale(52)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawUtf8(dc, body_font_, kSecondaryText,
             Locale::Get(
                 "Diagnosis uses a sustained time window; isolated spikes do "
                 "not become conclusions."),
             MakeRect(empty.left + Scale(20), empty.top + Scale(60),
                      empty.right - Scale(20), empty.bottom - Scale(18)),
             DT_LEFT | DT_TOP | DT_WORDBREAK);
    return;
  }
  for (const auto& diagnosis : model.diagnoses) {
    if (y + Scale(126) > content.bottom) break;
    const RECT card = MakeRect(content.left, y, content.right, y + Scale(112));
    RoundedRectangle(dc, card, kSurface, kBorder, Scale(7));
    const COLORREF color = SeverityColor(diagnosis.severity);
    Fill(dc, MakeRect(card.left, card.top + Scale(12), card.left + Scale(4),
                      card.bottom - Scale(12)),
         color);
    DrawUtf8(dc, small_font_, color, Locale::Get(diagnosis.severity),
             MakeRect(card.left + Scale(20), card.top + Scale(12),
                      card.left + Scale(120), card.top + Scale(32)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawUtf8(dc, section_font_, kPrimaryText, Locale::Get(diagnosis.type),
             MakeRect(card.left + Scale(20), card.top + Scale(34),
                      card.right - Scale(20), card.top + Scale(60)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, body_font_, kSecondaryText, diagnosis.summary,
             MakeRect(card.left + Scale(20), card.top + Scale(64),
                      card.right - Scale(20), card.bottom - Scale(14)),
             DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    y = card.bottom + gap;
  }
}

void DashboardRenderer::DrawProtected(HDC dc, const RECT& content,
                                      const DashboardViewModel& model) {
  RoundedRectangle(dc, content, kSurface, kBorder, Scale(7));
  const int margin = Scale(20);
  DrawUtf8(dc, section_font_, kPrimaryText, Locale::Get("Protected Workload"),
           MakeRect(content.left + margin, content.top + Scale(12),
                    content.right - margin, content.top + Scale(42)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawUtf8(dc, small_font_, kSecondaryText,
           Locale::Get(
               "WorkBoost will not modify these active development tasks."),
           MakeRect(content.left + margin, content.top + Scale(40),
                    content.right - margin, content.top + Scale(66)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  int y = content.top + Scale(78);
  const int row_height = Scale(60);
  for (const auto& workload : model.protected_workloads) {
    if (y + row_height > content.bottom - Scale(8)) break;
    EllipseFill(dc,
                MakeRect(content.left + margin, y + Scale(17),
                         content.left + margin + Scale(9), y + Scale(26)),
                kSuccess);
    DrawUtf8(dc, body_font_, kPrimaryText,
             Locale::Get(workload.category) + "  ·  " + workload.name,
             MakeRect(content.left + margin + Scale(20), y,
                      content.left + static_cast<int>(Width(content) * 0.55),
                      y + Scale(30)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, small_font_, kSecondaryText, workload.detail,
             MakeRect(content.left + margin + Scale(20), y + Scale(27),
                      content.left + static_cast<int>(Width(content) * 0.55),
                      y + row_height),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, small_font_, kSecondaryText,
             Locale::Get("Protected because: ") +
                 Locale::Get(workload.reason),
             MakeRect(content.left + static_cast<int>(Width(content) * 0.55),
                      y, content.right - margin, y + row_height),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Line(dc, content.left + margin, y + row_height - 1,
         content.right - margin, y + row_height - 1, kBorder);
    y += row_height;
  }
  if (model.protected_workloads.empty()) {
    DrawUtf8(dc, body_font_, kMutedText,
             Locale::Get("No active remote, serial, capture, build, Git, or "
                         "development workload detected."),
             MakeRect(content.left + margin, y, content.right - margin,
                      y + Scale(52)),
             DT_LEFT | DT_TOP | DT_WORDBREAK);
  }
}

void DashboardRenderer::DrawCodingMode(
    HDC dc, const RECT& content, const DashboardViewModel& model,
    const CodingModeViewOptions& options,
    const std::optional<DashboardUiCommand>& hovered) {
  const int margin = Scale(20);
  const RECT summary =
      MakeRect(content.left, content.top, content.right,
               std::min(static_cast<int>(content.bottom),
                        static_cast<int>(content.top) + Scale(154)));
  RoundedRectangle(dc, summary, kSurface, kBorder, Scale(7));
  DrawUtf8(dc, page_title_font_, kPrimaryText,
           Locale::Get(model.coding_mode.safe_mode
                           ? "Recovery required"
                           : model.coding_mode.active ? "Coding Mode Active"
                                                      : "Coding Mode"),
           MakeRect(summary.left + margin, summary.top + Scale(16),
                    summary.right - Scale(220), summary.top + Scale(52)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawUtf8(dc, body_font_, kSecondaryText,
           model.coding_mode.operation_in_progress
               ? model.coding_mode.operation_status
               : Locale::Get(
                     "Reduce unnecessary background activity while keeping "
                     "SSH, serial, capture, Git, and build work protected."),
           MakeRect(summary.left + margin, summary.top + Scale(60),
                    summary.right - Scale(240), summary.top + Scale(104)),
           DT_LEFT | DT_TOP | DT_WORDBREAK);
  const std::string counts =
      Locale::Format("{0} planned changes",
                     {std::to_string(model.coding_mode.planned_actions)}) +
      "   ·   " +
      Locale::Format("{0} cleanup selections",
                     {std::to_string(options.cleanup_processes.size())}) +
      "   ·   " +
      Locale::Format("{0} protected workloads",
                     {std::to_string(model.coding_mode.protected_workloads)});
  DrawUtf8(dc, small_font_, kSecondaryText, counts,
           MakeRect(summary.left + margin, summary.bottom - Scale(40),
                    summary.right - Scale(230), summary.bottom - Scale(14)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  const RECT action_button =
      MakeRect(summary.right - Scale(202), summary.top + Scale(50),
               summary.right - margin, summary.top + Scale(94));
  const DashboardUiCommand command{DashboardUiAction::CodingModePrimary,
                                   DashboardPage::CodingMode};
  RoundedRectangle(dc, action_button,
                   model.coding_mode.safe_mode ||
                           model.coding_mode.operation_in_progress
                       ? RGB(203, 209, 218)
                       : IsHovered(hovered, command) ? kAccentHover : kAccent,
                   model.coding_mode.safe_mode ||
                           model.coding_mode.operation_in_progress
                       ? RGB(203, 209, 218)
                       : IsHovered(hovered, command) ? kAccentHover : kAccent,
                   Scale(5));
  DrawUtf8(dc, body_font_, kSurface,
           Locale::Get(model.coding_mode.operation_in_progress
                           ? "Working..."
                           : model.coding_mode.safe_mode ? "View Recovery"
                           : model.coding_mode.active ? "Exit and Restore"
                                                      : "Enter Coding Mode"),
           action_button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  if (!model.coding_mode.operation_in_progress) {
    AddTarget(action_button, command);
  }

  const RECT details =
      MakeRect(content.left, summary.bottom + Scale(16), content.right,
               content.bottom);
  if (model.coding_mode.active || model.coding_mode.safe_mode ||
      model.coding_mode.operation_in_progress) {
    RoundedRectangle(dc, details, kSurface, kBorder, Scale(7));
    DrawUtf8(dc, section_font_, kPrimaryText,
             Locale::Get(model.coding_mode.active ? "Active Changes"
                                                  : "Plan Preview"),
             MakeRect(details.left + margin, details.top + Scale(12),
                      details.right - margin, details.top + Scale(42)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextPage(dc,
                 MakeRect(details.left + margin, details.top + Scale(48),
                          details.right - margin, details.bottom - margin),
                 model.pages[static_cast<std::size_t>(
                     DashboardPage::CodingMode)]);
    return;
  }

  const int gap = Scale(14);
  const int left_width =
      std::max(Scale(560), static_cast<int>(Width(details) * 0.66));
  const RECT process_panel = MakeRect(
      details.left, details.top,
      std::min(details.right - Scale(280), details.left + left_width),
      details.bottom);
  const RECT pool_panel = MakeRect(process_panel.right + gap, details.top,
                                   details.right, details.bottom);
  RoundedRectangle(dc, process_panel, kSurface, kBorder, Scale(7));
  RoundedRectangle(dc, pool_panel, kSurface, kBorder, Scale(7));

  DrawUtf8(dc, section_font_, kPrimaryText, Locale::Get("Current Processes"),
           MakeRect(process_panel.left + margin,
                    process_panel.top + Scale(10),
                    process_panel.right - margin,
                    process_panel.top + Scale(38)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawUtf8(dc, small_font_, kSecondaryText,
           Locale::Get(
               "Click an available process to add it to the cleanup pool."),
           MakeRect(process_panel.left + margin,
                    process_panel.top + Scale(38),
                    process_panel.right - margin,
                    process_panel.top + Scale(62)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  const int process_header_y = process_panel.top + Scale(66);
  Fill(dc,
       MakeRect(process_panel.left + Scale(1), process_header_y,
                process_panel.right - Scale(1), process_header_y + Scale(30)),
       RGB(248, 249, 251));
  const int process_usable = Width(process_panel) - margin * 2;
  const std::array<int, 6> columns{
      process_panel.left + margin,
      process_panel.left + margin + static_cast<int>(process_usable * 0.35),
      process_panel.left + margin + static_cast<int>(process_usable * 0.48),
      process_panel.left + margin + static_cast<int>(process_usable * 0.66),
      process_panel.left + margin + static_cast<int>(process_usable * 0.84),
      process_panel.right - margin};
  const std::array<const char*, 5> labels{"Name", "CPU", "Memory",
                                          "Disk I/O", "State"};
  for (std::size_t index = 0; index < labels.size(); ++index) {
    DrawUtf8(dc, small_font_, kMutedText, Locale::Get(labels[index]),
             MakeRect(columns[index], process_header_y,
                      columns[index + 1] - Scale(6),
                      process_header_y + Scale(30)),
             index == 0 ? DT_LEFT | DT_VCENTER | DT_SINGLELINE
                        : DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
  }

  const auto is_selected = [&options](const ProcessViewModel& process) {
    return std::any_of(
        options.cleanup_processes.begin(), options.cleanup_processes.end(),
        [&process](const ProcessSelection& selected) {
          return selected.pid == process.pid &&
                 selected.expected_start_time_100ns ==
                     process.start_time_100ns;
        });
  };
  std::vector<const ProcessViewModel*> coding_processes;
  coding_processes.reserve(model.processes.size());
  for (const auto& process : model.processes) {
    coding_processes.push_back(&process);
  }
  std::stable_sort(
      coding_processes.begin(), coding_processes.end(),
      [&is_selected](const ProcessViewModel* left,
                     const ProcessViewModel* right) {
        const int left_rank = is_selected(*left)
                                  ? 0
                                  : left->cleanup_eligible ? 1 : 2;
        const int right_rank = is_selected(*right)
                                   ? 0
                                   : right->cleanup_eligible ? 1 : 2;
        return left_rank < right_rank;
      });
  const int process_row_height = Scale(40);
  int process_y = process_header_y + Scale(30);
  const std::size_t process_capacity = static_cast<std::size_t>(std::max(
      0L,
      (process_panel.bottom - process_y - Scale(28)) / process_row_height));
  const std::size_t maximum_offset =
      coding_processes.size() > process_capacity
          ? coding_processes.size() - process_capacity
          : 0;
  const std::size_t offset =
      std::min(options.process_scroll_offset, maximum_offset);
  const std::size_t process_end =
      std::min(coding_processes.size(), offset + process_capacity);
  for (std::size_t index = offset; index < process_end; ++index) {
    const auto& process = *coding_processes[index];
    const bool selected = is_selected(process);
    const DashboardUiCommand row_command{
        DashboardUiAction::ToggleCleanupProcess, DashboardPage::CodingMode,
        process.pid};
    const RECT row = MakeRect(process_panel.left + Scale(1), process_y,
                              process_panel.right - Scale(1),
                              process_y + process_row_height);
    if (selected) {
      Fill(dc, row, kAccentSoft);
    } else if (IsHovered(hovered, row_command)) {
      Fill(dc, row, RGB(247, 249, 252));
    } else if (index % 2 != 0) {
      Fill(dc, row, RGB(251, 252, 253));
    }
    const COLORREF text_color =
        process.cleanup_eligible || selected ? kPrimaryText : kMutedText;
    DrawUtf8(dc, body_font_, text_color, process.name,
             MakeRect(columns[0], process_y, columns[1] - Scale(6),
                      process_y + process_row_height),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, metric_font_, text_color,
             FormatPercent(process.cpu_percent),
             MakeRect(columns[1], process_y, columns[2] - Scale(6),
                      process_y + process_row_height),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    DrawUtf8(dc, metric_font_, text_color,
             FormatBytes(static_cast<double>(process.working_set_bytes)),
             MakeRect(columns[2], process_y, columns[3] - Scale(6),
                      process_y + process_row_height),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, metric_font_, text_color,
             FormatRate(process.read_bytes_per_sec +
                        process.write_bytes_per_sec),
             MakeRect(columns[3], process_y, columns[4] - Scale(6),
                      process_y + process_row_height),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const std::string state =
        selected ? Locale::Get("Selected")
                 : Locale::Get(process.cleanup_block_reason);
    DrawUtf8(dc, small_font_,
             selected ? kAccent
                      : process.cleanup_eligible ? kSuccess : kMutedText,
             state,
             MakeRect(columns[4], process_y, columns[5],
                      process_y + process_row_height),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Line(dc, process_panel.left + margin,
         process_y + process_row_height - 1, process_panel.right - margin,
         process_y + process_row_height - 1, kBorder);
    AddTarget(row, row_command);
    process_y += process_row_height;
  }
  if (coding_processes.empty()) {
    DrawUtf8(dc, body_font_, kMutedText,
             Locale::Get("No process inventory is available."),
             MakeRect(process_panel.left + margin, process_y + Scale(16),
                      process_panel.right - margin, process_y + Scale(52)),
             DT_LEFT | DT_TOP | DT_WORDBREAK);
  } else {
    DrawUtf8(dc, small_font_, kMutedText,
             Locale::Format("Showing {0}-{1} of {2}; use the mouse wheel.",
                            {std::to_string(offset + 1),
                             std::to_string(process_end),
                             std::to_string(coding_processes.size())}),
             MakeRect(process_panel.left + margin,
                      process_panel.bottom - Scale(28),
                      process_panel.right - margin,
                      process_panel.bottom - Scale(6)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }

  DrawUtf8(dc, section_font_, kPrimaryText, Locale::Get("Cleanup Pool"),
           MakeRect(pool_panel.left + margin, pool_panel.top + Scale(10),
                    pool_panel.right - margin,
                    pool_panel.top + Scale(38)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawUtf8(dc, small_font_, kSecondaryText,
           Locale::Get("Click a selected process to remove it."),
           MakeRect(pool_panel.left + margin, pool_panel.top + Scale(38),
                    pool_panel.right - margin,
                    pool_panel.top + Scale(62)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  int pool_y = pool_panel.top + Scale(72);
  const int pool_row_height = Scale(58);
  const int automatic_plan_height = Scale(132);
  const int pool_list_bottom =
      std::max(pool_y + Scale(58),
               static_cast<int>(pool_panel.bottom) - automatic_plan_height);
  const std::size_t pool_capacity = static_cast<std::size_t>(std::max(
      0, (pool_list_bottom - pool_y - Scale(28)) / pool_row_height));
  const std::size_t maximum_pool_offset =
      options.cleanup_processes.size() > pool_capacity
          ? options.cleanup_processes.size() - pool_capacity
          : 0;
  const std::size_t pool_offset =
      std::min(options.pool_scroll_offset, maximum_pool_offset);
  const std::size_t pool_end = std::min(
      options.cleanup_processes.size(), pool_offset + pool_capacity);
  for (std::size_t index = pool_offset; index < pool_end; ++index) {
    const auto& selected = options.cleanup_processes[index];
    const auto process = std::find_if(
        model.processes.begin(), model.processes.end(),
        [&selected](const ProcessViewModel& candidate) {
          return candidate.pid == selected.pid &&
                 candidate.start_time_100ns ==
                     selected.expected_start_time_100ns;
        });
    if (process == model.processes.end()) continue;
    const DashboardUiCommand row_command{
        DashboardUiAction::ToggleCleanupProcess, DashboardPage::CodingMode,
        process->pid};
    const RECT row = MakeRect(pool_panel.left + Scale(1), pool_y,
                              pool_panel.right - Scale(1),
                              pool_y + pool_row_height);
    Fill(dc, row,
         IsHovered(hovered, row_command) ? RGB(230, 239, 252)
                                         : kAccentSoft);
    DrawUtf8(dc, body_font_, kPrimaryText, process->name,
             MakeRect(row.left + margin, row.top + Scale(4),
                      row.right - margin, row.top + Scale(28)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const std::string metrics =
        "PID " + std::to_string(process->pid) + "  |  " +
        FormatPercent(process->cpu_percent) + "  |  " +
        FormatBytes(static_cast<double>(process->working_set_bytes)) +
        "  |  " +
        FormatRate(process->read_bytes_per_sec +
                   process->write_bytes_per_sec);
    DrawUtf8(dc, small_font_, kSecondaryText, metrics,
             MakeRect(row.left + margin, row.top + Scale(27),
                      row.right - margin, row.bottom - Scale(4)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Line(dc, row.left + margin, row.bottom - 1, row.right - margin,
         row.bottom - 1, RGB(202, 218, 242));
    AddTarget(row, row_command);
    pool_y += pool_row_height;
  }
  if (options.cleanup_processes.empty()) {
    DrawUtf8(dc, body_font_, kMutedText,
             Locale::Get("No processes selected."),
             MakeRect(pool_panel.left + margin, pool_y + Scale(20),
                      pool_panel.right - margin,
                      std::min(pool_list_bottom, pool_y + Scale(58))),
             DT_LEFT | DT_TOP | DT_WORDBREAK);
  } else if (pool_offset != 0 ||
             pool_end < options.cleanup_processes.size()) {
    DrawUtf8(dc, small_font_, kMutedText,
             Locale::Format("Showing {0}-{1} of {2}; use the mouse wheel.",
                            {std::to_string(pool_offset + 1),
                             std::to_string(pool_end),
                             std::to_string(
                                 options.cleanup_processes.size())}),
             MakeRect(pool_panel.left + margin,
                      pool_list_bottom - Scale(28),
                      pool_panel.right - margin,
                      pool_list_bottom - Scale(6)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }

  const int plan_top = pool_list_bottom + Scale(4);
  Line(dc, pool_panel.left + margin, plan_top, pool_panel.right - margin,
       plan_top, kBorder);
  DrawUtf8(dc, section_font_, kPrimaryText, Locale::Get("Automatic Plan"),
           MakeRect(pool_panel.left + margin, plan_top + Scale(6),
                    pool_panel.right - margin, plan_top + Scale(34)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  int action_y = plan_top + Scale(38);
  const std::size_t action_count =
      std::min<std::size_t>(2, model.coding_mode.actions.size());
  for (std::size_t index = 0; index < action_count; ++index) {
    const auto& action = model.coding_mode.actions[index];
    DrawUtf8(dc, body_font_, kPrimaryText, action.target,
             MakeRect(pool_panel.left + margin, action_y,
                      pool_panel.right - margin, action_y + Scale(22)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, small_font_, kSecondaryText,
             Locale::Get(action.action) + "  |  " + Locale::Get(action.risk),
             MakeRect(pool_panel.left + margin, action_y + Scale(20),
                      pool_panel.right - margin, action_y + Scale(40)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    action_y += Scale(44);
  }
  if (model.coding_mode.actions.empty()) {
    DrawUtf8(dc, small_font_, kMutedText,
             Locale::Get("No automatic actions."),
             MakeRect(pool_panel.left + margin, action_y,
                      pool_panel.right - margin, action_y + Scale(28)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
}

void DashboardRenderer::DrawRecovery(
    HDC dc, const RECT& content, const DashboardViewModel& model,
    const std::optional<DashboardUiCommand>& hovered) {
  const int margin = Scale(20);
  const RECT summary =
      MakeRect(content.left, content.top, content.right,
               std::min(static_cast<int>(content.bottom),
                        static_cast<int>(content.top) + Scale(154)));
  RoundedRectangle(dc, summary, kSurface, kBorder, Scale(7));
  DrawUtf8(dc, page_title_font_, kPrimaryText,
           Locale::Get(model.recovery.required ? "Recovery Required"
                                               : "Recovery & History"),
           MakeRect(summary.left + margin, summary.top + Scale(16),
                    summary.right - Scale(220), summary.top + Scale(52)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawUtf8(dc, body_font_, kSecondaryText,
           Locale::Get(model.recovery.required
                           ? "An unfinished Coding Mode session must be "
                             "restored before new changes are allowed."
                           : "No unfinished session. Completed sessions are "
                             "listed below."),
           MakeRect(summary.left + margin, summary.top + Scale(60),
                    summary.right - Scale(240), summary.top + Scale(104)),
           DT_LEFT | DT_TOP | DT_WORDBREAK);
  const std::string state =
      model.recovery.error.empty()
          ? Locale::Get("State: ") + Locale::Get(model.recovery.state) +
                "  ·  " +
                Locale::Format("{0} recorded action(s)",
                               {std::to_string(model.recovery.actions.size())})
          : model.recovery.error;
  DrawUtf8(dc, small_font_, kSecondaryText, state,
           MakeRect(summary.left + margin, summary.bottom - Scale(40),
                    summary.right - Scale(230), summary.bottom - Scale(14)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  const RECT action_button =
      MakeRect(summary.right - Scale(202), summary.top + Scale(50),
               summary.right - margin, summary.top + Scale(94));
  const DashboardUiCommand command{DashboardUiAction::RecoveryRestore,
                                   DashboardPage::Recovery};
  const bool enabled = model.recovery.can_restore &&
                       !model.coding_mode.operation_in_progress;
  RoundedRectangle(dc, action_button,
                   enabled ? (IsHovered(hovered, command) ? kAccentHover
                                                          : kAccent)
                           : RGB(203, 209, 218),
                   enabled ? (IsHovered(hovered, command) ? kAccentHover
                                                          : kAccent)
                           : RGB(203, 209, 218),
                   Scale(5));
  DrawUtf8(dc, body_font_, kSurface,
           Locale::Get(model.recovery.can_restore ? "Restore Session"
                                                  : "No Action Needed"),
           action_button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  if (enabled) AddTarget(action_button, command);

  const RECT details =
      MakeRect(content.left, summary.bottom + Scale(16), content.right,
               content.bottom);
  RoundedRectangle(dc, details, kSurface, kBorder, Scale(7));
  DrawUtf8(dc, section_font_, kPrimaryText,
           Locale::Get(model.recovery.required ? "Session Details"
                                               : "Completion History"),
           MakeRect(details.left + margin, details.top + Scale(12),
                    details.right - margin, details.top + Scale(42)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawTextPage(dc,
               MakeRect(details.left + margin, details.top + Scale(48),
                        details.right - margin, details.bottom - margin),
               model.pages[static_cast<std::size_t>(DashboardPage::Recovery)]);
}

void DashboardRenderer::DrawSettings(HDC dc, const RECT& content,
                                     const DashboardViewModel& model,
                                     const std::optional<DashboardUiCommand>& hovered) {
  const int margin = Scale(20);
  const RECT summary =
      MakeRect(content.left, content.top, content.right,
               std::min(static_cast<int>(content.bottom),
                        static_cast<int>(content.top) + Scale(132)));
  RoundedRectangle(dc, summary, kSurface, kBorder, Scale(7));
  DrawUtf8(dc, page_title_font_, kPrimaryText, Locale::Get("Settings"),
           MakeRect(summary.left + margin, summary.top + Scale(16),
                    summary.right - Scale(150), summary.top + Scale(52)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  // Language toggle: clicking switches the interface language.
  const DashboardUiCommand language_command{DashboardUiAction::ToggleLanguage,
                                            DashboardPage::Settings};
  const RECT language_button =
      MakeRect(summary.right - Scale(126), summary.top + Scale(17),
               summary.right - margin, summary.top + Scale(49));
  RoundedRectangle(dc, language_button,
                   IsHovered(hovered, language_command) ? kAccentSoft
                                                        : kSurface,
                   kBorder, Scale(5));
  DrawUtf8(dc, small_font_, kPrimaryText,
           Locale::IsChinese() ? "English" : "中文", language_button,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  AddTarget(language_button, language_command);
  DrawUtf8(dc, small_font_, kSecondaryText,
           Locale::Get("Effective configuration is read-only in this release; "
                       "edit the config files beside the executable to adjust "
                       "it."),
           MakeRect(summary.left + margin, summary.top + Scale(54),
                    summary.right - margin, summary.top + Scale(82)),
           DT_LEFT | DT_TOP | DT_WORDBREAK);
  DrawUtf8(dc, small_font_, kSecondaryText,
           Locale::Format("{0} ms sampling",
                          {std::to_string(model.settings.sample_interval_ms)}) +
               "  ·  " +
               Locale::Format("{0} s history",
                              {std::to_string(model.settings.history_seconds)}) +
               "  ·  " +
               Locale::Format("{0} process rules",
                              {std::to_string(
                                  model.settings.process_rule_count)}) +
               "  ·  " +
               Locale::Format("{0} service rules",
                              {std::to_string(model.settings.service_rule_count)}),
           MakeRect(summary.left + margin, summary.bottom - Scale(38),
                    summary.right - margin, summary.bottom - Scale(12)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  const RECT details =
      MakeRect(content.left, summary.bottom + Scale(16), content.right,
               content.bottom);
  RoundedRectangle(dc, details, kSurface, kBorder, Scale(7));
  DrawUtf8(dc, section_font_, kPrimaryText,
           Locale::Get("Startup Inventory & Configuration"),
           MakeRect(details.left + margin, details.top + Scale(12),
                    details.right - margin, details.top + Scale(42)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawTextPage(dc,
               MakeRect(details.left + margin, details.top + Scale(48),
                        details.right - margin, details.bottom - margin),
               model.pages[static_cast<std::size_t>(DashboardPage::Settings)]);
}

void DashboardRenderer::DrawTextPage(HDC dc, const RECT& content,
                                     const std::string& text) {
  RECT panel = content;
  RoundedRectangle(dc, panel, kSurface, kBorder, Scale(7));
  panel = Inset(panel, Scale(20), Scale(16));
  DrawUtf8(dc, mono_font_, kPrimaryText, text, panel,
           DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
}

}  // namespace workboost::gui
