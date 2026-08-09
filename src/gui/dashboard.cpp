#include "gui/dashboard.h"

#include "app/coding_mode_command.h"
#include "app/session_manager.h"
#include "core/logging/logger.h"
#include "gui/dashboard_model.h"
#include "gui/dashboard_renderer.h"
#include "platform/windows/serial_port_api.h"
#include "platform/windows/service_api.h"
#include "platform/windows/startup_api.h"
#include "platform/windows/system_collector.h"
#include "platform/windows/windows_utils.h"

#include <commdlg.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace workboost::gui {
namespace {

constexpr wchar_t kWindowClass[] = L"WorkBoostDashboardWindow";
constexpr UINT kModelReadyMessage = WM_APP + 17;
constexpr UINT kCodingCommandReadyMessage = WM_APP + 18;
constexpr UINT kTrayCallbackMessage = WM_APP + 19;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayMenuOpen = 1;
constexpr UINT kTrayMenuExit = 2;

std::size_t HistoryCapacity(const Config& config) {
  const int interval = std::max(250, config.sample_interval_ms);
  return static_cast<std::size_t>(
      std::max(1, config.history_seconds * 1000 / interval));
}

int SystemDpi() {
  HDC dc = GetDC(nullptr);
  if (dc == nullptr) return 96;
  const int dpi = std::max(96, GetDeviceCaps(dc, LOGPIXELSX));
  ReleaseDC(nullptr, dc);
  return dpi;
}

double DistanceToSegment(double px, double py, double ax, double ay, double bx,
                         double by) {
  const double abx = bx - ax;
  const double aby = by - ay;
  const double length_sq = abx * abx + aby * aby;
  double t = 0.0;
  if (length_sq > 0.0) {
    t = std::clamp(((px - ax) * abx + (py - ay) * aby) / length_sq, 0.0, 1.0);
  }
  const double cx = ax + t * abx - px;
  const double cy = ay + t * aby - py;
  return std::sqrt(cx * cx + cy * cy);
}

// Draws a green circle with a white check mark into a 32 bpp icon. The bitmap
// carries its own alpha channel, so no .ico resource is required.
HICON CreateAppIcon(int size) {
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = size;
  header.bV5Height = -size;  // top-down rows so row 0 is the top of the icon
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP color_bitmap = CreateDIBSection(
      screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits,
      nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (color_bitmap == nullptr || bits == nullptr) return nullptr;

  constexpr std::uint32_t kGreen = 0x34A34A;  // (r, g, b) = (0x34, 0xA3, 0x4A)
  auto* pixel = static_cast<std::uint32_t*>(bits);
  const double center = (size - 1) * 0.5;
  const double radius = size * 0.46;
  const double half_thickness = size * 0.045;
  const double p1x = center - (size - 1) * 0.21;
  const double p1y = center + (size - 1) * 0.04;
  const double p2x = center - (size - 1) * 0.04;
  const double p2y = center + (size - 1) * 0.21;
  const double p3x = center + (size - 1) * 0.23;
  const double p3y = center - (size - 1) * 0.20;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const double dx = x - center;
      const double dy = y - center;
      const double circle_coverage = std::clamp(
          radius + 0.5 - std::sqrt(dx * dx + dy * dy), 0.0, 1.0);
      const double check = std::max(
          std::clamp(half_thickness + 0.5 -
                         DistanceToSegment(x, y, p1x, p1y, p2x, p2y),
                     0.0, 1.0),
          std::clamp(half_thickness + 0.5 -
                         DistanceToSegment(x, y, p2x, p2y, p3x, p3y),
                     0.0, 1.0));
      // Alpha comes from the circle only, so the check never escapes it; the
      // check blends from green toward white. Colors are premultiplied, which
      // CreateIconIndirect expects for an alpha bitmap.
      const std::uint32_t alpha = static_cast<std::uint32_t>(
          255.0 * std::clamp(circle_coverage, 0.0, 1.0));
      const std::uint32_t red =
          static_cast<std::uint32_t>(((kGreen >> 16) & 0xFF) +
                                     (0xFF - ((kGreen >> 16) & 0xFF)) * check) *
          alpha / 255U;
      const std::uint32_t green =
          static_cast<std::uint32_t>(((kGreen >> 8) & 0xFF) +
                                     (0xFF - ((kGreen >> 8) & 0xFF)) * check) *
          alpha / 255U;
      const std::uint32_t blue =
          static_cast<std::uint32_t>((kGreen & 0xFF) +
                                     (0xFF - (kGreen & 0xFF)) * check) *
          alpha / 255U;
      pixel[y * size + x] = (alpha << 24) | (red << 16) | (green << 8) | blue;
    }
  }

  ICONINFO info{};
  info.fIcon = TRUE;
  info.hbmColor = color_bitmap;
  info.hbmMask = CreateBitmap(size, size, 1, 1, nullptr);
  HICON icon = CreateIconIndirect(&info);
  if (info.hbmMask != nullptr) DeleteObject(info.hbmMask);
  DeleteObject(color_bitmap);
  return icon;
}

class DashboardWindow {
 public:
  explicit DashboardWindow(Config config) : config_(std::move(config)) {}

  ~DashboardWindow() {
    StopWorker();
    if (coding_worker_.joinable()) coding_worker_.join();
    if (tray_menu_ != nullptr) DestroyMenu(tray_menu_);
    if (app_icon_ != nullptr) DestroyIcon(app_icon_);
  }

  int Run() {
    SetProcessDPIAware();
    dpi_ = SystemDpi();
    app_icon_ = CreateAppIcon(32);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_DBLCLKS;
    window_class.lpfnWndProc = &DashboardWindow::WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = app_icon_;
    window_class.hIconSm = app_icon_;
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return 1;
    }

    RECT initial_bounds{0, 0, MulDiv(1280, dpi_, 96),
                        MulDiv(800, dpi_, 96)};
    AdjustWindowRectEx(&initial_bounds, WS_OVERLAPPEDWINDOW, FALSE, 0);
    window_ = CreateWindowExW(
        0, kWindowClass, L"WorkBoost", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, initial_bounds.right - initial_bounds.left,
        initial_bounds.bottom - initial_bounds.top, nullptr, nullptr, instance,
        this);
    if (window_ == nullptr) return 1;
    ShowWindow(window_, SW_SHOWDEFAULT);
    UpdateWindow(window_);

    MSG message{};
    while (true) {
      const BOOL result = GetMessageW(&message, nullptr, 0, 0);
      if (result == 0) return static_cast<int>(message.wParam);
      if (result == -1) return 1;
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }

 private:
  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam) {
    DashboardWindow* self = reinterpret_cast<DashboardWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
      self = static_cast<DashboardWindow*>(create->lpCreateParams);
      self->window_ = window;
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
    }
    return self != nullptr ? self->HandleMessage(message, wparam, lparam)
                           : DefWindowProcW(window, message, wparam, lparam);
  }

  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_CREATE:
        return CreateControls() ? 0 : -1;
      case WM_SIZE:
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
      case WM_PAINT:
        Paint();
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_MOUSEMOVE:
        HandleMouseMove(lparam);
        return 0;
      case WM_MOUSELEAVE:
        mouse_tracking_ = false;
        if (hovered_) {
          hovered_.reset();
          InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
      case WM_LBUTTONUP:
        HandleClick(lparam);
        return 0;
      case WM_KEYDOWN:
        HandleKeyDown(wparam);
        return 0;
      case WM_GETMINMAXINFO: {
        auto* bounds = reinterpret_cast<MINMAXINFO*>(lparam);
        RECT minimum{0, 0, MulDiv(1000, dpi_, 96),
                     MulDiv(650, dpi_, 96)};
        AdjustWindowRectEx(&minimum, WS_OVERLAPPEDWINDOW, FALSE, 0);
        bounds->ptMinTrackSize.x = minimum.right - minimum.left;
        bounds->ptMinTrackSize.y = minimum.bottom - minimum.top;
        return 0;
      }
      case kModelReadyMessage:
        AcceptPendingModel();
        return 0;
      case kCodingCommandReadyMessage:
        AcceptCodingCommandResult();
        return 0;
      case kTrayCallbackMessage:
        HandleTrayNotification(lparam);
        return 0;
      case WM_SYSCOMMAND:
        if (wparam == SC_MINIMIZE) {
          ShowWindow(window_, SW_HIDE);
          AddTrayIcon();
          return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
      case WM_CLOSE:
        if (coding_command_running_.load()) {
          MessageBoxW(window_,
                      L"A Coding Mode operation is still running. Complete "
                      L"the operation before closing WorkBoost.",
                      L"WorkBoost", MB_OK | MB_ICONINFORMATION);
          return 0;
        }
        DestroyWindow(window_);
        return 0;
      case WM_DESTROY:
        RemoveTrayIcon();
        StopWorker();
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
  }

  bool CreateControls() {
    if (!renderer_.Initialize(window_)) return false;
    StartWorker();
    return true;
  }

  void Paint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window_, &paint);
    if (dc == nullptr) return;
    RECT client{};
    GetClientRect(window_, &client);
    renderer_.Paint(dc, client, latest_model_, current_page_, hovered_,
                    status_message_);
    EndPaint(window_, &paint);
  }

  void HandleMouseMove(LPARAM lparam) {
    const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    const auto next = renderer_.HitTest(point);
    if (next != hovered_) {
      hovered_ = next;
      InvalidateRect(window_, nullptr, FALSE);
    }
    if (!mouse_tracking_) {
      TRACKMOUSEEVENT tracking{};
      tracking.cbSize = sizeof(tracking);
      tracking.dwFlags = TME_LEAVE;
      tracking.hwndTrack = window_;
      mouse_tracking_ = TrackMouseEvent(&tracking) != FALSE;
    }
  }

  void HandleClick(LPARAM lparam) {
    const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    const auto command = renderer_.HitTest(point);
    if (!command) return;
    switch (command->action) {
      case DashboardUiAction::Navigate:
        current_page_ = command->page;
        break;
      case DashboardUiAction::Refresh:
        {
          std::lock_guard<std::mutex> lock(worker_mutex_);
          refresh_requested_ = true;
        }
        status_message_ = "Refreshing...";
        worker_condition_.notify_one();
        break;
      case DashboardUiAction::Export: ExportAll(); break;
      case DashboardUiAction::CodingModePrimary:
        if (!latest_model_) break;
        if (latest_model_->coding_mode.safe_mode) {
          current_page_ = DashboardPage::Recovery;
          break;
        }
        current_page_ = DashboardPage::CodingMode;
        if (coding_command_running_.load()) break;
        if (latest_model_->coding_mode.active) {
          const int answer = MessageBoxW(
              window_,
              L"Exit Coding Mode and restore every reversible action in "
              L"reverse order?\n\nWorkBoost will verify the restored state and "
              L"keep Safe Mode active if restoration cannot be confirmed.",
              L"Exit Coding Mode", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
          if (answer == IDYES) StartCodingCommand(CodingModeCommand::Exit);
        } else {
          std::ostringstream confirmation;
          confirmation << "Apply the reviewed Coding Mode plan?\n\n"
                       << latest_model_->coding_mode.planned_actions
                       << " planned action(s)\n"
                       << latest_model_->coding_mode.protected_workloads
                       << " protected workload(s)\n\n"
                       << "A 10-second baseline is captured first. All system "
                          "changes still pass ProtectionPolicy and "
                          "SafetyValidator.";
          const int answer = MessageBoxW(
              window_, windows::Utf8ToWide(confirmation.str()).c_str(),
              L"Enter Coding Mode",
              MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
          if (answer == IDYES) StartCodingCommand(CodingModeCommand::Enter);
        }
        break;
      case DashboardUiAction::RecoveryRestore:
        if (latest_model_ && latest_model_->recovery.can_restore &&
            !coding_command_running_.load()) {
          StartCodingCommand(CodingModeCommand::Restore);
        }
        break;
      case DashboardUiAction::None: break;
    }
    InvalidateRect(window_, nullptr, FALSE);
  }

  void HandleKeyDown(WPARAM key) {
    const std::size_t current = static_cast<std::size_t>(current_page_);
    if (key == VK_DOWN && current + 1 < kDashboardPageCount) {
      current_page_ = static_cast<DashboardPage>(current + 1);
    } else if (key == VK_UP && current != 0) {
      current_page_ = static_cast<DashboardPage>(current - 1);
    } else if (key != VK_F5) {
      return;
    } else {
      {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        refresh_requested_ = true;
      }
      status_message_ = "Refreshing...";
      worker_condition_.notify_one();
    }
    InvalidateRect(window_, nullptr, FALSE);
  }

  void AcceptPendingModel() {
    std::optional<DashboardViewModel> model;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      model = std::move(pending_model_);
      pending_model_.reset();
    }
    if (model) {
      ApplyCodingOperationState(&*model);
      latest_model_ = std::move(model);
      if (!coding_command_running_.load()) status_message_.clear();
      InvalidateRect(window_, nullptr, FALSE);
    }
  }

  void ApplyCodingOperationState(DashboardViewModel* model) const {
    model->coding_mode.operation_in_progress =
        coding_command_running_.load();
    model->coding_mode.operation_status = coding_operation_status_;
  }

  void StartCodingCommand(CodingModeCommand command) {
    if (coding_command_running_.exchange(true)) return;
    if (coding_worker_.joinable()) coding_worker_.join();

    active_coding_command_ = command;
    switch (command) {
      case CodingModeCommand::Enter:
        coding_operation_status_ =
            "Capturing a 10-second baseline, then applying the reviewed plan...";
        break;
      case CodingModeCommand::Exit:
        coding_operation_status_ =
            "Restoring reversible actions and verifying system state...";
        break;
      case CodingModeCommand::Restore:
        coding_operation_status_ =
            "Restoring the unfinished session and verifying system state...";
        break;
    }
    status_message_ = coding_operation_status_;
    if (latest_model_) ApplyCodingOperationState(&*latest_model_);
    InvalidateRect(window_, nullptr, FALSE);

    try {
      coding_worker_ = std::thread([this, command] {
        CodingModeCommandResult result;
        try {
          result = CodingModeCommandClient::Execute(command);
        } catch (const std::exception& error) {
          result.error.code = ERROR_UNHANDLED_EXCEPTION;
          result.error.context = "Execute Coding Mode command";
          result.error.message = error.what();
        } catch (...) {
          result.error.code = ERROR_UNHANDLED_EXCEPTION;
          result.error.context = "Execute Coding Mode command";
          result.error.message = "unexpected exception";
        }
        {
          std::lock_guard<std::mutex> lock(coding_result_mutex_);
          pending_coding_result_ = std::move(result);
        }
        PostMessageW(window_, kCodingCommandReadyMessage, 0, 0);
      });
    } catch (const std::exception& error) {
      coding_command_running_.store(false);
      coding_operation_status_.clear();
      if (latest_model_) ApplyCodingOperationState(&*latest_model_);
      status_message_ = "Could not start the Coding Mode operation.";
      MessageBoxW(window_, windows::Utf8ToWide(error.what()).c_str(),
                  L"Coding Mode failed", MB_OK | MB_ICONERROR);
    }
  }

  void AcceptCodingCommandResult() {
    std::optional<CodingModeCommandResult> result;
    {
      std::lock_guard<std::mutex> lock(coding_result_mutex_);
      result = std::move(pending_coding_result_);
      pending_coding_result_.reset();
    }
    if (!result) return;
    if (coding_worker_.joinable()) coding_worker_.join();

    coding_command_running_.store(false);
    coding_operation_status_.clear();
    if (latest_model_) ApplyCodingOperationState(&*latest_model_);

    if (result->Succeeded()) {
      const bool entered = active_coding_command_ == CodingModeCommand::Enter;
      status_message_ = entered ? "Coding Mode is active."
                                : "System state was restored successfully.";
      current_page_ = entered ? DashboardPage::CodingMode
                              : DashboardPage::Dashboard;
      MessageBoxW(window_,
                  entered
                      ? L"Coding Mode is active. WorkBoost recorded every "
                        L"applied action for deterministic rollback."
                      : L"The reversible session was restored and verified.",
                  entered ? L"Coding Mode active" : L"Recovery complete",
                  MB_OK | MB_ICONINFORMATION);
    } else {
      std::ostringstream details;
      details << "WorkBoost did not complete the requested operation.";
      if (result->error.code != 0) {
        details << "\r\n\r\n" << result->error.Describe();
      }
      if (result->launched) {
        details << "\r\nExit code: " << result->exit_code;
      }
      if (!result->output.empty()) {
        constexpr std::size_t kMaximumDisplayedOutput = 4096;
        details << "\r\n\r\nCommand output:\r\n"
                << result->output.substr(0, kMaximumDisplayedOutput);
        if (result->output.size() > kMaximumDisplayedOutput) {
          details << "\r\n[output truncated]";
        }
      }
      status_message_ = "Coding Mode operation failed; no result was hidden.";
      MessageBoxW(window_, windows::Utf8ToWide(details.str()).c_str(),
                  L"Coding Mode failed", MB_OK | MB_ICONERROR);
    }

    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      refresh_requested_ = true;
    }
    worker_condition_.notify_one();
    InvalidateRect(window_, nullptr, FALSE);
  }

  void PostModel(DashboardViewModel model) {
    if (stop_requested_.load()) return;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      pending_model_ = std::move(model);
    }
    PostMessageW(window_, kModelReadyMessage, 0, 0);
  }

  void PostCollectorError(const std::string& error) {
    DashboardViewModel model;
    model.mode = "Monitor unavailable";
    model.updated_at = windows::Iso8601Now();
    const std::string message =
        "MONITOR ERROR\r\n\r\n" + error +
        "\r\n\r\nNo system changes were attempted.";
    for (auto& page : model.pages) page = message;
    PostModel(std::move(model));
  }

  void StartWorker() {
    worker_ = std::thread([this] { WorkerLoop(); });
  }

  void StopWorker() {
    stop_requested_.store(true);
    worker_condition_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  void AddTrayIcon() {
    if (tray_icon_added_) return;
    tray_icon_ = {};
    tray_icon_.cbSize = sizeof(tray_icon_);
    tray_icon_.hWnd = window_;
    tray_icon_.uID = kTrayIconId;
    tray_icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray_icon_.uCallbackMessage = kTrayCallbackMessage;
    tray_icon_.hIcon = app_icon_ != nullptr
                           ? app_icon_
                           : LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(tray_icon_.szTip, L"WorkBoost", ARRAYSIZE(tray_icon_.szTip));
    tray_icon_.uVersion = NOTIFYICON_VERSION_4;
    tray_icon_added_ = Shell_NotifyIconW(NIM_ADD, &tray_icon_) != FALSE;
    if (!tray_icon_added_) return;
    Shell_NotifyIconW(NIM_SETVERSION, &tray_icon_);
    if (tray_tip_shown_) return;
    tray_tip_shown_ = true;
    tray_icon_.uFlags = NIF_INFO;
    lstrcpynW(tray_icon_.szInfoTitle, L"WorkBoost",
              ARRAYSIZE(tray_icon_.szInfoTitle));
    lstrcpynW(tray_icon_.szInfo,
              L"WorkBoost is still running. Double-click the tray icon to "
              L"open the dashboard.",
              ARRAYSIZE(tray_icon_.szInfo));
    tray_icon_.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &tray_icon_);
  }

  void RemoveTrayIcon() {
    if (!tray_icon_added_) return;
    Shell_NotifyIconW(NIM_DELETE, &tray_icon_);
    tray_icon_added_ = false;
  }

  void RestoreFromTray() {
    RemoveTrayIcon();
    ShowWindow(window_, SW_RESTORE);
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
  }

  void ShowTrayMenu() {
    if (tray_menu_ == nullptr) {
      tray_menu_ = CreatePopupMenu();
      AppendMenuW(tray_menu_, MF_STRING, kTrayMenuOpen, L"Open WorkBoost");
      AppendMenuW(tray_menu_, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(tray_menu_, MF_STRING, kTrayMenuExit, L"Exit");
    }
    SetForegroundWindow(window_);
    POINT cursor{};
    GetCursorPos(&cursor);
    const UINT command = static_cast<UINT>(TrackPopupMenu(
        tray_menu_, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN, cursor.x,
        cursor.y, 0, window_, nullptr));
    PostMessageW(window_, WM_NULL, 0, 0);
    if (command == kTrayMenuOpen) {
      RestoreFromTray();
    } else if (command == kTrayMenuExit) {
      if (coding_command_running_.load()) {
        MessageBoxW(window_,
                    L"A Coding Mode operation is still running. Complete "
                    L"the operation before closing WorkBoost.",
                    L"WorkBoost", MB_OK | MB_ICONINFORMATION);
      } else {
        DestroyWindow(window_);
      }
    }
  }

  void HandleTrayNotification(LPARAM lparam) {
    switch (static_cast<UINT>(lparam)) {
      case WM_LBUTTONDBLCLK:
        RestoreFromTray();
        break;
      case WM_RBUTTONUP:
        ShowTrayMenu();
        break;
      default:
        break;
    }
  }

  void AttachConfiguredServices(SystemSnapshot* snapshot) const {
    snapshot->services.clear();
    snapshot->service_inventory_complete = true;
    std::vector<std::string> names(
        config_.coding_profile.allow_service_stop.begin(),
        config_.coding_profile.allow_service_stop.end());
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
      const auto query = windows::ServiceApi::Query(name);
      if (query.success) {
        snapshot->services.push_back(query.service);
      } else {
        snapshot->service_inventory_complete = false;
      }
    }
  }

  void WorkerLoop() {
    windows::SystemCollector collector(config_);
    windows::WindowsError collector_error;
    if (!collector.Initialize(&collector_error)) {
      Logger::Instance().Write(LogLevel::Error, LogEvent::MonitorFailed,
                               collector_error.code);
      PostCollectorError(collector_error.Describe());
      return;
    }

    SnapshotHistory history(HistoryCapacity(config_));
    SessionManager sessions(windows::LocalAppDataDirectory());
    std::vector<SerialPortSnapshot> serial_ports;
    std::vector<StartupEntrySnapshot> startup_entries;
    std::vector<CompletionReportSummary> completion_reports;
    std::string serial_error;
    std::string startup_error;
    std::string report_error;
    std::chrono::steady_clock::time_point last_inventory{};
    bool inventory_loaded = false;
    const auto interval =
        std::chrono::milliseconds(std::max(250, config_.sample_interval_ms));

    while (!stop_requested_.load()) {
      bool forced_refresh = false;
      {
        std::unique_lock<std::mutex> lock(worker_mutex_);
        worker_condition_.wait_for(lock, interval, [this] {
          return stop_requested_.load() || refresh_requested_;
        });
        if (stop_requested_.load()) return;
        forced_refresh = refresh_requested_;
        refresh_requested_ = false;
      }

      const auto now = std::chrono::steady_clock::now();
      if (!inventory_loaded || forced_refresh ||
          now - last_inventory >= std::chrono::seconds(10)) {
        const auto serial = windows::SerialPortApi::QueryPresent();
        if (serial.success) {
          serial_ports = serial.ports;
          serial_error.clear();
        } else {
          serial_error = serial.error.Describe();
        }
        const auto startup = windows::StartupApi::QueryAll();
        if (startup.success) {
          startup_entries = startup.entries;
          startup_error.clear();
        } else {
          startup_error = startup.error.Describe();
        }
        completion_reports =
            sessions.ListCompletionReports(20, &report_error);
        last_inventory = now;
        inventory_loaded = true;
      }

      SystemSnapshot snapshot = collector.Sample(&collector_error);
      AttachConfiguredServices(&snapshot);
      history.Add(snapshot);

      std::optional<OptimizationSession> active_session;
      std::string recovery_error;
      if (sessions.HasActiveSession()) {
        active_session = sessions.LoadActive(&recovery_error);
        if (!active_session && recovery_error.empty()) {
          recovery_error = "active session could not be validated";
        }
      }
      PostModel(DashboardPresenter::Build(
          config_, snapshot, history, serial_ports, startup_entries,
          active_session, recovery_error, serial_error, startup_error,
          completion_reports, report_error));
    }
  }

  void ExportAll() const {
    if (!latest_model_) {
      MessageBoxW(window_, L"Wait for the first sample before exporting.",
                  L"WorkBoost", MB_OK | MB_ICONINFORMATION);
      return;
    }
    wchar_t file_name[MAX_PATH] = L"workboost-dashboard.txt";
    const wchar_t filter[] = L"Text report (*.txt)\0*.txt\0All files\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = file_name;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return;

    std::ostringstream report;
    report << "WorkBoost Dashboard\r\n"
           << "Generated: " << latest_model_->updated_at << "\r\n"
           << "Mode: " << latest_model_->mode << "\r\n";
    const auto& names = DashboardPresenter::PageNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
      report << "\r\n============================================================\r\n"
             << names[i] << "\r\n"
             << "============================================================\r\n"
             << latest_model_->pages[i] << "\r\n";
    }
    std::string error;
    if (!windows::AtomicWriteUtf8(std::filesystem::path(file_name),
                                  report.str(), &error)) {
      MessageBoxW(window_, windows::Utf8ToWide(error).c_str(),
                  L"Export failed", MB_OK | MB_ICONERROR);
      return;
    }
    MessageBoxW(window_, L"Dashboard report exported.", L"WorkBoost",
                MB_OK | MB_ICONINFORMATION);
  }

  Config config_;
  HWND window_{};
  int dpi_{96};
  DashboardRenderer renderer_;
  DashboardPage current_page_{DashboardPage::Dashboard};
  std::optional<DashboardUiCommand> hovered_;
  bool mouse_tracking_{};
  std::string status_message_{"Collecting system metrics..."};
  std::optional<DashboardViewModel> latest_model_;
  std::optional<DashboardViewModel> pending_model_;
  std::mutex pending_mutex_;
  std::thread worker_;
  std::atomic<bool> stop_requested_{};
  std::mutex worker_mutex_;
  std::condition_variable worker_condition_;
  bool refresh_requested_{};
  std::thread coding_worker_;
  std::atomic<bool> coding_command_running_{};
  std::mutex coding_result_mutex_;
  std::optional<CodingModeCommandResult> pending_coding_result_;
  CodingModeCommand active_coding_command_{CodingModeCommand::Enter};
  std::string coding_operation_status_;
  NOTIFYICONDATAW tray_icon_{};
  bool tray_icon_added_{};
  bool tray_tip_shown_{};
  HMENU tray_menu_{};
  HICON app_icon_{};
};

}  // namespace

int RunDashboard(const Config& config) {
  DashboardWindow dashboard(config);
  return dashboard.Run();
}

}  // namespace workboost::gui
