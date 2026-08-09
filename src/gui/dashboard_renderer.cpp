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
#include <unordered_map>
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
    const ProcessViewOptions& process_options) {
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
              process_options);
    return;
  }
  {
    SelectScope select(buffer, bitmap);
    DrawFrame(buffer, client, model, current_page, hovered, status_message,
              process_options);
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
    const ProcessViewOptions& process_options) {
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
      DrawCodingMode(dc, content, *model, hovered);
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
  const int top_height =
      std::clamp(static_cast<int>(available * 0.58), Scale(220), Scale(300));
  const int bottom_height =
      std::clamp(available - top_height, Scale(132), Scale(180));
  const int left_width = static_cast<int>(Width(content) * 0.56);
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

  const int panel_margin = Scale(16);
  DrawUtf8(dc, section_font_, kPrimaryText, Locale::Get("System Overview"),
           MakeRect(system_panel.left + panel_margin,
                    system_panel.top + Scale(10), system_panel.right,
                    system_panel.top + Scale(38)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  const int label_width = Scale(72);
  const int value_width = Scale(160);
  const int row_height = Scale(32);
  const int subtitle_height = Scale(14);
  int row_y = system_panel.top + Scale(44);
  auto draw_metric = [&](const std::string& label, double ratio,
                         const std::string& value, const std::string& detail,
                         const std::string& subtitle) {
    const int row_top = row_y;
    DrawUtf8(dc, body_font_, kPrimaryText, Locale::Get(label),
             MakeRect(system_panel.left + panel_margin, row_top,
                      system_panel.left + panel_margin + label_width,
                      row_top + (subtitle.empty() ? row_height : Scale(18))),
             subtitle.empty()
                 ? DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS
                 : DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    const int bar_left = system_panel.left + panel_margin + label_width;
    const int bar_right =
        std::max(bar_left + Scale(32),
                 static_cast<int>(system_panel.right) - panel_margin -
                     value_width);
    const int bar_y = row_top + Scale(15);
    const RECT track = MakeRect(bar_left, bar_y, bar_right, bar_y + Scale(5));
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
    DrawUtf8(dc, metric_font_, kPrimaryText, value,
             MakeRect(bar_right + Scale(10), row_top,
                      system_panel.right - panel_margin, row_top + Scale(20)),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!detail.empty()) {
      DrawUtf8(dc, small_font_, kSecondaryText, detail,
               MakeRect(bar_right + Scale(10), row_top + Scale(17),
                        system_panel.right - panel_margin,
                        row_top + row_height),
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    // Model label sits directly under its row, left of the value column.
    if (!subtitle.empty()) {
      DrawUtf8(dc, small_font_, kSecondaryText, subtitle,
               MakeRect(system_panel.left + panel_margin,
                        row_top + row_height, bar_right,
                        row_top + row_height + subtitle_height),
               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    row_y += subtitle.empty() ? row_height : row_height + subtitle_height;
  };
  draw_metric("CPU", model.system.cpu_percent / 100.0,
              FormatPercent(model.system.cpu_percent), std::string{},
              model.system.cpu_model);
  draw_metric("Memory", model.system.memory_used_ratio,
              FormatBytes(static_cast<double>(model.system.memory_used_bytes)) +
                  " / " +
                  FormatBytes(
                      static_cast<double>(model.system.memory_total_bytes)),
              Locale::Format(
                  "{0} available",
                  {FormatBytes(static_cast<double>(
                      model.system.available_memory_bytes))}),
              model.system.memory_model);
  draw_metric("Commit", model.system.commit_ratio,
              FormatPercent(model.system.commit_ratio * 100.0),
              Locale::Format(
                  "{0} page reads/s",
                  {std::to_string(
                      static_cast<int>(model.system.page_reads_per_sec))}),
              std::string{});
  const std::size_t disk_count = std::min<std::size_t>(2, model.disks.size());
  for (std::size_t i = 0; i < disk_count; ++i) {
    const auto& disk = model.disks[i];
    draw_metric(disk.media + " " + disk.name, disk.active_percent / 100.0,
                FormatPercent(disk.active_percent),
                FormatLatency(disk.latency_ms), std::string{});
  }
  if (model.disks.empty()) {
    DrawUtf8(dc, small_font_, kMutedText,
             Locale::Get("No physical disk counters available"),
             MakeRect(system_panel.left + panel_margin, row_y,
                      system_panel.right - panel_margin, row_y + row_height),
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
  struct ImpactAggregate {
    std::string name;
    double io_bytes_per_sec{};
    double cpu_percent{};
    ImpactLevel impact{ImpactLevel::Low};
  };
  std::vector<ImpactAggregate> impact_processes;
  std::unordered_map<std::string, std::size_t> impact_by_name;
  for (const auto& process : model.processes) {
    const auto [position, inserted] =
        impact_by_name.emplace(process.name, impact_processes.size());
    if (inserted) {
      impact_processes.push_back(
          ImpactAggregate{process.name, 0.0, 0.0, ImpactLevel::Low});
    }
    auto& aggregate = impact_processes[position->second];
    aggregate.io_bytes_per_sec +=
        process.read_bytes_per_sec + process.write_bytes_per_sec;
    aggregate.cpu_percent += process.cpu_percent;
    if (ImpactRank(process.impact) > ImpactRank(aggregate.impact)) {
      aggregate.impact = process.impact;
    }
  }
  std::stable_sort(impact_processes.begin(), impact_processes.end(),
                   [](const auto& left, const auto& right) {
                     if (ImpactRank(left.impact) != ImpactRank(right.impact)) {
                       return ImpactRank(left.impact) > ImpactRank(right.impact);
                     }
                     return left.io_bytes_per_sec != right.io_bytes_per_sec
                                ? left.io_bytes_per_sec > right.io_bytes_per_sec
                                : left.cpu_percent > right.cpu_percent;
                   });
  int impact_y = impact_panel.top + Scale(42);
  const std::size_t impact_count =
      std::min<std::size_t>(3, impact_processes.size());
  for (std::size_t i = 0; i < impact_count; ++i) {
    const auto& process = impact_processes[i];
    DrawUtf8(dc, body_font_, kPrimaryText, process.name,
             MakeRect(impact_panel.left + panel_margin, impact_y,
                      impact_panel.left + static_cast<int>(Width(impact_panel) * 0.48),
                      impact_y + Scale(26)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, metric_font_, kSecondaryText,
             FormatRate(process.io_bytes_per_sec),
             MakeRect(impact_panel.left + static_cast<int>(Width(impact_panel) * 0.48),
                      impact_y, impact_panel.right - Scale(74),
                      impact_y + Scale(26)),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawUtf8(dc, small_font_, ImpactColor(process.impact),
             ImpactText(process.impact),
             MakeRect(impact_panel.right - Scale(66), impact_y,
                      impact_panel.right - panel_margin, impact_y + Scale(26)),
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    impact_y += Scale(28);
  }
  if (impact_processes.empty()) {
    DrawUtf8(dc, small_font_, kMutedText,
             Locale::Get("No process inventory available"),
             MakeRect(impact_panel.left + panel_margin, impact_y,
                      impact_panel.right - panel_margin, impact_y + Scale(28)),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }

  DrawUtf8(dc, section_font_, kPrimaryText, Locale::Get("Protected Workload"),
           MakeRect(protected_panel.left + panel_margin,
                    protected_panel.top + Scale(9), protected_panel.right,
                    protected_panel.top + Scale(36)),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  int protected_y = protected_panel.top + Scale(42);
  const std::size_t protected_count =
      std::min<std::size_t>(3, model.protected_workloads.size());
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
