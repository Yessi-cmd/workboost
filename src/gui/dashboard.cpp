#include "gui/dashboard.h"

#include "app/session_manager.h"
#include "core/logging/logger.h"
#include "gui/dashboard_model.h"
#include "platform/windows/serial_port_api.h"
#include "platform/windows/service_api.h"
#include "platform/windows/startup_api.h"
#include "platform/windows/system_collector.h"
#include "platform/windows/windows_utils.h"

#include <commdlg.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
constexpr int kNavigationId = 1001;
constexpr int kContentId = 1002;
constexpr int kRefreshId = 1003;
constexpr int kExportId = 1004;

std::size_t HistoryCapacity(const Config& config) {
  const int interval = std::max(250, config.sample_interval_ms);
  return static_cast<std::size_t>(
      std::max(1, config.history_seconds * 1000 / interval));
}

class DashboardWindow {
 public:
  explicit DashboardWindow(Config config) : config_(std::move(config)) {}

  ~DashboardWindow() {
    StopWorker();
    if (title_font_ != nullptr) DeleteObject(title_font_);
    if (body_font_ != nullptr) DeleteObject(body_font_);
    if (content_font_ != nullptr) DeleteObject(content_font_);
  }

  int Run() {
    SetProcessDPIAware();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = &DashboardWindow::WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return 1;
    }

    window_ = CreateWindowExW(
        0, kWindowClass, L"WorkBoost", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760, nullptr, nullptr, instance,
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
        LayoutControls(LOWORD(lparam), HIWORD(lparam));
        return 0;
      case WM_GETMINMAXINFO: {
        auto* bounds = reinterpret_cast<MINMAXINFO*>(lparam);
        bounds->ptMinTrackSize.x = 840;
        bounds->ptMinTrackSize.y = 560;
        return 0;
      }
      case WM_COMMAND:
        HandleCommand(LOWORD(wparam), HIWORD(wparam));
        return 0;
      case kModelReadyMessage:
        AcceptPendingModel();
        return 0;
      case WM_CLOSE:
        DestroyWindow(window_);
        return 0;
      case WM_DESTROY:
        StopWorker();
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
  }

  bool CreateControls() {
    title_font_ = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    body_font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    content_font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                FIXED_PITCH, L"Consolas");

    navigation_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        0, 0, 0, 0, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNavigationId)),
        GetModuleHandleW(nullptr), nullptr);
    title_ = CreateWindowExW(0, L"STATIC", L"Dashboard", WS_CHILD | WS_VISIBLE,
                             0, 0, 0, 0, window_, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    status_ = CreateWindowExW(0, L"STATIC", L"Initializing collector...",
                              WS_CHILD | WS_VISIBLE | SS_RIGHT, 0, 0, 0, 0,
                              window_, nullptr, GetModuleHandleW(nullptr),
                              nullptr);
    refresh_ = CreateWindowExW(
        0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshId)),
        GetModuleHandleW(nullptr), nullptr);
    export_ = CreateWindowExW(
        0, L"BUTTON", L"Export all", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kExportId)),
        GetModuleHandleW(nullptr), nullptr);
    content_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"Waiting for the first sample...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
            ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
            ES_AUTOHSCROLL,
        0, 0, 0, 0, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kContentId)),
        GetModuleHandleW(nullptr), nullptr);
    if (navigation_ == nullptr || title_ == nullptr || status_ == nullptr ||
        refresh_ == nullptr || export_ == nullptr || content_ == nullptr) {
      return false;
    }

    SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_),
                 TRUE);
    for (const HWND control :
         {navigation_, status_, refresh_, export_}) {
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(body_font_),
                   TRUE);
    }
    SendMessageW(content_, WM_SETFONT,
                 reinterpret_cast<WPARAM>(content_font_), TRUE);
    SendMessageW(content_, EM_SETLIMITTEXT, 1024 * 1024, 0);
    for (const char* name : DashboardPresenter::PageNames()) {
      const std::wstring wide_name = windows::Utf8ToWide(name);
      SendMessageW(navigation_, LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(wide_name.c_str()));
    }
    SendMessageW(navigation_, LB_SETCURSEL, 0, 0);
    StartWorker();
    return true;
  }

  void LayoutControls(int width, int height) const {
    constexpr int margin = 18;
    constexpr int navigation_width = 205;
    constexpr int header_height = 78;
    constexpr int button_width = 96;
    constexpr int button_height = 32;
    const int content_left = navigation_width + margin * 2;
    const int content_width = std::max(0, width - content_left - margin);
    MoveWindow(navigation_, margin, margin, navigation_width,
               std::max(0, height - margin * 2), TRUE);
    MoveWindow(title_, content_left, margin, std::max(0, content_width - 350),
               36, TRUE);
    MoveWindow(status_, std::max(content_left, width - 495), margin + 6, 270,
               24, TRUE);
    MoveWindow(refresh_, std::max(content_left, width - 211), margin,
               button_width, button_height, TRUE);
    MoveWindow(export_, std::max(content_left, width - 108), margin,
               button_width, button_height, TRUE);
    MoveWindow(content_, content_left, header_height, content_width,
               std::max(0, height - header_height - margin), TRUE);
  }

  void HandleCommand(int control_id, int notification) {
    if (control_id == kNavigationId && notification == LBN_SELCHANGE) {
      const LRESULT selection = SendMessageW(navigation_, LB_GETCURSEL, 0, 0);
      if (selection >= 0 &&
          selection < static_cast<LRESULT>(kDashboardPageCount)) {
        current_page_ = static_cast<std::size_t>(selection);
        RenderCurrentPage();
      }
    } else if (control_id == kRefreshId && notification == BN_CLICKED) {
      {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        refresh_requested_ = true;
      }
      worker_condition_.notify_one();
      SetWindowTextW(status_, L"Refreshing...");
    } else if (control_id == kExportId && notification == BN_CLICKED) {
      ExportAll();
    }
  }

  void RenderCurrentPage() const {
    const auto& names = DashboardPresenter::PageNames();
    SetWindowTextW(title_, windows::Utf8ToWide(names[current_page_]).c_str());
    if (!latest_model_) return;
    SetWindowTextW(
        content_,
        windows::Utf8ToWide(latest_model_->pages[current_page_]).c_str());
    const std::string status =
        latest_model_->mode + "  |  " + latest_model_->updated_at;
    SetWindowTextW(status_, windows::Utf8ToWide(status).c_str());
  }

  void AcceptPendingModel() {
    std::optional<DashboardViewModel> model;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      model = std::move(pending_model_);
      pending_model_.reset();
    }
    if (model) {
      latest_model_ = std::move(model);
      RenderCurrentPage();
    }
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
    std::string serial_error;
    std::string startup_error;
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
          active_session, recovery_error, serial_error, startup_error));
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
  HWND navigation_{};
  HWND title_{};
  HWND status_{};
  HWND refresh_{};
  HWND export_{};
  HWND content_{};
  HFONT title_font_{};
  HFONT body_font_{};
  HFONT content_font_{};
  std::size_t current_page_{};
  std::optional<DashboardViewModel> latest_model_;
  std::optional<DashboardViewModel> pending_model_;
  std::mutex pending_mutex_;
  std::thread worker_;
  std::atomic<bool> stop_requested_{};
  std::mutex worker_mutex_;
  std::condition_variable worker_condition_;
  bool refresh_requested_{};
};

}  // namespace

int RunDashboard(const Config& config) {
  DashboardWindow dashboard(config);
  return dashboard.Run();
}

}  // namespace workboost::gui
