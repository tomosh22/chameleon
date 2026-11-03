#include "app.h"

#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "broker_link.h"
#include "cml/proc/proc.h"
#include "cml/ui/commands.h"
#include "cml/ui/controller.h"
#include "cml/ui/settings.h"
#include "cml/ui/ui_diag_ids.h"
#include "cml/ui/view_model.h"
#include "cml/util/path.h"
#include "editor_launch.h"
#include "imgui.h"

// Not declared in the backend header (it would drag windows.h in); see the comment there.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace cml::ui::win {

namespace {

constexpr wchar_t kWindowClass[] = L"ChameleonUiWindow";
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\Chameleon.cml_ui.single_instance";
constexpr wchar_t kShowMessageName[] = L"Chameleon.cml_ui.ShowWindow";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kTrayId = 1;
constexpr int kHotkeyId = 1;
constexpr int kMenuShow = 1001;
constexpr int kMenuReload = 1002;
constexpr int kMenuExit = 1003;

const ImVec4 kColorError(1.00f, 0.42f, 0.42f, 1.0f);
const ImVec4 kColorWarning(1.00f, 0.80f, 0.35f, 1.0f);
const ImVec4 kColorInfo(0.80f, 0.82f, 0.85f, 1.0f);
const ImVec4 kColorGood(0.45f, 0.85f, 0.50f, 1.0f);
const ImVec4 kColorDim(0.55f, 0.58f, 0.62f, 1.0f);
const ImVec4 kColorLink(0.55f, 0.75f, 1.00f, 1.0f);

std::string format_clock(std::int64_t unix_ms) {
    if (unix_ms <= 0) {
        return std::string();
    }
    ULARGE_INTEGER t;
    t.QuadPart = (static_cast<ULONGLONG>(unix_ms) + 11644473600000ULL) * 10000ULL;
    FILETIME ft;
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    SYSTEMTIME utc;
    SYSTEMTIME local;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return std::string();
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%02u:%02u:%02u", static_cast<unsigned>(local.wHour), static_cast<unsigned>(local.wMinute),
                  static_cast<unsigned>(local.wSecond));
    return buffer;
}

std::string base_name_of(const std::string& path) {
    return std::string(cml::path::base_name(std::string_view(path)));
}

std::string milliseconds(double value, bool present) {
    if (!present) {
        return "-";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f", value);
    return buffer;
}

ImVec4 severity_color(OutputSeverity severity) {
    switch (severity) {
        case OutputSeverity::Error: return kColorError;
        case OutputSeverity::Warning: return kColorWarning;
        case OutputSeverity::Info: break;
    }
    return kColorInfo;
}

ImVec4 level_color(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return kColorError;
        case LogLevel::Warn: return kColorWarning;
        case LogLevel::Info: return kColorInfo;
        case LogLevel::Debug:
        case LogLevel::Trace: break;
    }
    return kColorDim;
}

std::string action_label(const std::string& action) {
    if (action == "continue") return "Continue";
    if (action == "leave") return "Leave function";
    if (action == "ignore") return "Ignore";
    std::string label = action;
    if (!label.empty()) {
        label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    }
    return label;
}

template <std::size_t N>
void copy_to_buffer(char (&buffer)[N], const std::string& text) {
    std::snprintf(buffer, N, "%s", text.c_str());
}

enum class Tab { Output, Changes, History, Log, Settings };

class App {
public:
    App(HINSTANCE instance, AppOptions options) : instance_(instance), options_(std::move(options)) {}
    int run();

private:
    // window / device
    bool create_window();
    bool create_device();
    void cleanup_device();
    void create_render_target();
    void cleanup_render_target();
    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    // shell integration
    void add_tray_icon();
    void remove_tray_icon();
    void update_tray_tip();
    void show_tray_menu();
    void show_window();
    void hide_window();
    void register_hotkey();
    void unregister_hotkey();

    // actions (UI thread; they post work to the broker link)
    void do_reload(std::uint32_t pid, const std::string& module, bool confirm);
    void do_restart(std::uint32_t pid);
    void do_revert(std::uint32_t pid, const std::string& module, const std::string& scope);
    void do_exception_action(const ExceptionRow& row, const std::string& action);
    void open_in_editor(const SourceLocation& location);
    void apply_settings(const Settings& settings);
    void load_edit_buffers();

    // drawing
    void draw_frame();
    void draw_menu_bar();
    void draw_toolbar();
    void draw_status_line();
    void draw_processes();
    void draw_tabs();
    void draw_output();
    void draw_changes();
    void draw_history();
    void draw_log();
    void draw_settings();
    void draw_modals();

    HINSTANCE instance_;
    AppOptions options_;
    HWND hwnd_ = nullptr;
    HANDLE mutex_ = nullptr;
    UINT show_message_ = 0;
    UINT taskbar_created_ = 0;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain* swap_chain_ = nullptr;
    ID3D11RenderTargetView* target_view_ = nullptr;
    bool swap_chain_occluded_ = false;

    bool tray_added_ = false;
    bool hotkey_registered_ = false;
    std::string hotkey_error_;
    bool window_visible_ = false;
    bool quit_requested_ = false;

    Settings settings_;
    std::string settings_path_;
    std::vector<std::string> settings_warnings_;
    ViewModel vm_;
    std::unique_ptr<BrokerLink> link_;
    bool link_clean_ = true;

    // UI state
    std::uint32_t selected_pid_ = 0;
    std::string selected_module_;
    Tab tab_request_ = Tab::Output;
    bool tab_request_pending_ = false;
    bool output_autoscroll_ = true;
    bool show_warnings_ = true;
    bool show_info_ = true;
    LogFilter log_filter_;
    char log_text_[128] = {};
    bool log_autoscroll_ = true;
    bool open_exception_popup_ = false;
    bool open_confirm_popup_ = false;
    bool open_restart_popup_ = false;
    bool open_revert_all_popup_ = false;
    bool open_about_popup_ = false;
    std::uint32_t restart_pid_ = 0;
    std::string last_tray_tip_;

    // settings editor
    Settings edit_;
    char hotkey_buffer_[64] = {};
    char template_buffer_[512] = {};
    char program_buffer_[520] = {};
    char broker_buffer_[520] = {};
    char pipe_buffer_[260] = {};
    std::string settings_message_;
    bool settings_message_error_ = false;
};

// --- window procedure --------------------------------------------------------------------------------------------

LRESULT CALLBACK App::window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->hwnd_ = hwnd;
        }
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    return self->handle_message(hwnd, msg, wparam, lparam);
}

LRESULT App::handle_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui::GetCurrentContext() != nullptr && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        return TRUE;
    }
    if (show_message_ != 0 && msg == show_message_) {
        show_window();
        return 0;
    }
    if (taskbar_created_ != 0 && msg == taskbar_created_) {
        tray_added_ = false;
        add_tray_icon();
        return 0;
    }
    switch (msg) {
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) {
                if (settings_.minimize_to_tray && tray_added_) {
                    hide_window();
                }
                return 0;
            }
            if (device_ != nullptr && wparam != SIZE_MINIMIZED) {
                cleanup_render_target();
                swap_chain_->ResizeBuffers(0, static_cast<UINT>(LOWORD(lparam)), static_cast<UINT>(HIWORD(lparam)), DXGI_FORMAT_UNKNOWN, 0);
                create_render_target();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU) {
                return 0;  // no ALT application menu
            }
            break;
        case WM_HOTKEY:
            if (wparam == static_cast<WPARAM>(kHotkeyId)) {
                do_reload(0, std::string(), false);
                return 0;
            }
            break;
        case kTrayMessage:
            switch (LOWORD(lparam)) {
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK:
                    show_window();
                    break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    show_tray_menu();
                    break;
                default: break;
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kMenuShow: show_window(); return 0;
                case kMenuReload: do_reload(0, std::string(), false); return 0;
                case kMenuExit: quit_requested_ = true; DestroyWindow(hwnd); return 0;
                default: break;
            }
            break;
        case WM_CLOSE:
            if (!quit_requested_ && settings_.minimize_to_tray && tray_added_) {
                hide_window();
                return 0;
            }
            break;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// --- device ----------------------------------------------------------------------------------------------------------

bool App::create_device() {
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd_;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level;
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc,
                                                   &swap_chain_, &device_, &level, &context_);
    if (result == DXGI_ERROR_UNSUPPORTED) {
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc, &swap_chain_,
                                               &device_, &level, &context_);
    }
    if (FAILED(result)) {
        return false;
    }
    create_render_target();
    return true;
}

void App::create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    if (SUCCEEDED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) && back_buffer != nullptr) {
        device_->CreateRenderTargetView(back_buffer, nullptr, &target_view_);
        back_buffer->Release();
    }
}

void App::cleanup_render_target() {
    if (target_view_ != nullptr) {
        target_view_->Release();
        target_view_ = nullptr;
    }
}

void App::cleanup_device() {
    cleanup_render_target();
    if (swap_chain_ != nullptr) {
        swap_chain_->Release();
        swap_chain_ = nullptr;
    }
    if (context_ != nullptr) {
        context_->Release();
        context_ = nullptr;
    }
    if (device_ != nullptr) {
        device_->Release();
        device_ = nullptr;
    }
}

bool App::create_window() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = &App::window_proc;
    wc.hInstance = instance_;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    wc.hIconSm = wc.hIcon;
    if (RegisterClassExW(&wc) == 0) {
        return false;
    }
    hwnd_ = CreateWindowExW(0, kWindowClass, L"Chameleon", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr,
                            instance_, this);
    return hwnd_ != nullptr;
}

// --- shell integration -----------------------------------------------------------------------------------------------

void App::add_tray_icon() {
    if (tray_added_ || hwnd_ == nullptr) {
        return;
    }
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd_;
    nid.uID = static_cast<UINT>(kTrayId);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, L"Chameleon", static_cast<int>(sizeof nid.szTip / sizeof nid.szTip[0]));
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        tray_added_ = true;
    } else {
        vm_.add_log(LogLevel::Warn, Diag::make(ids::kUiTray, "the tray icon could not be created",
                                               "Closing the window exits the UI; the window stays reachable from the taskbar.")
                                        .format());
    }
}

void App::remove_tray_icon() {
    if (!tray_added_) {
        return;
    }
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd_;
    nid.uID = static_cast<UINT>(kTrayId);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    tray_added_ = false;
}

void App::update_tray_tip() {
    if (!tray_added_) {
        return;
    }
    std::string tip = "Chameleon - ";
    tip += to_string(vm_.connection());
    if (vm_.error_count() > 0) {
        tip += ", " + std::to_string(vm_.error_count()) + " error(s)";
    }
    if (tip == last_tray_tip_) {
        return;
    }
    last_tray_tip_ = tip;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd_;
    nid.uID = static_cast<UINT>(kTrayId);
    nid.uFlags = NIF_TIP;
    const std::wstring wide = proc::wide_from_utf8(tip);
    lstrcpynW(nid.szTip, wide.c_str(), static_cast<int>(sizeof nid.szTip / sizeof nid.szTip[0]));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::show_tray_menu() {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, kMenuShow, L"Show Chameleon");
    AppendMenuW(menu, MF_STRING, kMenuReload, L"Reload now");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");
    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(hwnd_);  // required so that the menu closes when the user clicks elsewhere
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void App::show_window() {
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
    window_visible_ = true;
}

void App::hide_window() {
    ShowWindow(hwnd_, SW_HIDE);
    window_visible_ = false;
}

void App::register_hotkey() {
    unregister_hotkey();
    hotkey_error_.clear();
    if (!settings_.hotkey_enabled || !settings_.reload_hotkey.valid()) {
        return;
    }
    const Hotkey& h = settings_.reload_hotkey;
    UINT modifiers = MOD_NOREPEAT;
    if (h.ctrl) modifiers |= MOD_CONTROL;
    if (h.alt) modifiers |= MOD_ALT;
    if (h.shift) modifiers |= MOD_SHIFT;
    if (h.win) modifiers |= MOD_WIN;
    if (RegisterHotKey(hwnd_, kHotkeyId, modifiers, h.vk)) {
        hotkey_registered_ = true;
        return;
    }
    const DWORD error = GetLastError();
    const Diag diag = Diag::make(ids::kUiHotkey,
                                 "the global hotkey " + format_hotkey(h) + " could not be registered (error " + std::to_string(error) + ")",
                                 error == ERROR_HOTKEY_ALREADY_REGISTERED ? "Another program uses this key combination; choose another one in the settings."
                                                                          : "Choose another key combination in the settings.");
    hotkey_error_ = diag.format();
    vm_.add_log(LogLevel::Warn, hotkey_error_);
    vm_.set_status_message(diag.id() + ": " + diag.message, StatusSeverity::Warning);
}

void App::unregister_hotkey() {
    if (hotkey_registered_) {
        UnregisterHotKey(hwnd_, kHotkeyId);
        hotkey_registered_ = false;
    }
}

// --- actions ---------------------------------------------------------------------------------------------------------

void App::do_reload(std::uint32_t pid, const std::string& module, bool confirm) {
    if (!link_) {
        return;
    }
    vm_.set_status_message("Reload requested...", StatusSeverity::Info);
    link_->post([pid, module, confirm](Controller& controller) { static_cast<void>(controller.reload(pid, module, confirm)); });
}

void App::do_restart(std::uint32_t pid) {
    if (!link_) {
        return;
    }
    vm_.set_status_message("Restart requested for process " + std::to_string(pid) + "...", StatusSeverity::Info);
    link_->post([pid](Controller& controller) { static_cast<void>(controller.restart(pid)); });
}

void App::do_revert(std::uint32_t pid, const std::string& module, const std::string& scope) {
    if (!link_) {
        return;
    }
    link_->post([pid, module, scope](Controller& controller) { static_cast<void>(controller.revert(pid, module, scope)); });
}

void App::do_exception_action(const ExceptionRow& row, const std::string& action) {
    if (!link_) {
        return;
    }
    const std::uint32_t pid = row.pid;
    const std::uint64_t id = row.exception_id;
    vm_.resolve_exception(pid, id);  // the modal closes at once; a failure below brings it back through `exceptions`
    link_->post([pid, id, action](Controller& controller) {
        if (!controller.exception_action(pid, id, action)) {
            static_cast<void>(controller.refresh_exceptions());
        }
    });
}

void App::open_in_editor(const SourceLocation& location) {
    Result<void> launched = launch_editor(settings_.editor, location);
    if (!launched) {
        vm_.set_status_message(launched.error().id() + ": " + launched.error().message, StatusSeverity::Error);
        vm_.add_output_text(launched.error().format(), OutputSeverity::Error, "ui");
    } else {
        vm_.set_status_message("Opened " + base_name_of(location.file) + ":" + std::to_string(location.line), StatusSeverity::Info);
    }
}

void App::apply_settings(const Settings& settings) {
    settings_ = settings;
    Result<void> saved = save_settings(settings_path_, settings_);
    if (!saved) {
        settings_message_ = saved.error().format();
        settings_message_error_ = true;
        vm_.add_log(LogLevel::Warn, settings_message_);
    } else {
        settings_message_ = "Saved " + settings_path_;
        settings_message_error_ = false;
    }
    register_hotkey();
    if (link_) {
        link_->update_settings(settings_);
    }
    parse_log_level(settings_.min_log_level, &log_filter_.min_level);
}

void App::load_edit_buffers() {
    edit_ = settings_;
    copy_to_buffer(hotkey_buffer_, format_hotkey(edit_.reload_hotkey));
    copy_to_buffer(template_buffer_, edit_.editor.custom_template);
    copy_to_buffer(program_buffer_, edit_.editor.program);
    copy_to_buffer(broker_buffer_, edit_.broker_path);
    copy_to_buffer(pipe_buffer_, edit_.control_pipe);
}

// --- drawing ---------------------------------------------------------------------------------------------------------

void App::draw_menu_bar() {
    if (!ImGui::BeginMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Settings...")) {
            tab_request_ = Tab::Settings;
            tab_request_pending_ = true;
        }
        if (ImGui::MenuItem("Hide to tray", nullptr, false, tray_added_)) {
            hide_window();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            quit_requested_ = true;
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Broker")) {
        if (ImGui::MenuItem("Reload all", nullptr, false, vm_.feature_available(Feature::Reload) && !vm_.busy())) {
            do_reload(0, std::string(), false);
        }
        if (ImGui::MenuItem("Wait for idle", nullptr, false, vm_.feature_available(Feature::Wait))) {
            link_->post([](Controller& controller) { static_cast<void>(controller.wait("idle", 60000)); });
        }
        if (ImGui::MenuItem("Wait for module indexes", nullptr, false, vm_.feature_available(Feature::Wait))) {
            link_->post([](Controller& controller) { static_cast<void>(controller.wait("index", 120000)); });
        }
        if (ImGui::MenuItem("Refresh status")) {
            link_->poke();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About")) {
            open_about_popup_ = true;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void App::draw_toolbar() {
    const bool connected = vm_.connection() == ConnectionState::Connected;
    const bool can_reload = connected && vm_.feature_available(Feature::Reload) && !vm_.busy();

    ImGui::BeginDisabled(!can_reload);
    if (ImGui::Button("Reload")) {
        do_reload(selected_pid_, selected_module_, false);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        const std::string target = selected_pid_ == 0 ? "all processes" : "process " + std::to_string(selected_pid_) +
                                                                              (selected_module_.empty() ? "" : ", " + base_name_of(selected_module_));
        ImGui::SetTooltip("Recompile changed files and patch %s\nGlobal hotkey: %s", target.c_str(),
                          settings_.hotkey_enabled ? format_hotkey(settings_.reload_hotkey).c_str() : "(off)");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!connected || !vm_.feature_available(Feature::Restart) || selected_pid_ == 0);
    if (ImGui::Button("Restart process")) {
        restart_pid_ = selected_pid_;
        open_restart_popup_ = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", !vm_.feature_available(Feature::Restart) ? "This broker has no 'restart' command."
                                                                         : "Select a process in the list first.");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!connected || !vm_.feature_available(Feature::Revert));
    if (ImGui::Button("Revert last")) {
        do_revert(selected_pid_, selected_module_, "last");
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert all")) {
        open_revert_all_popup_ = true;
    }
    ImGui::EndDisabled();
    if (!vm_.feature_available(Feature::Revert) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("This broker has no 'revert' command.");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!connected || !vm_.feature_available(Feature::SetOption));
    bool compile_on_save = settings_.compile_on_save;
    if (ImGui::Checkbox("Compile on save", &compile_on_save)) {
        Settings changed = settings_;
        changed.compile_on_save = compile_on_save;
        apply_settings(changed);
        load_edit_buffers();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    const char* state_text = to_string(vm_.connection());
    ImVec4 state_color = kColorDim;
    switch (vm_.connection()) {
        case ConnectionState::Connected: state_color = kColorGood; break;
        case ConnectionState::Failed: state_color = kColorError; break;
        case ConnectionState::Connecting: state_color = kColorWarning; break;
        case ConnectionState::Disconnected: break;
    }
    ImGui::TextColored(state_color, "Broker: %s", state_text);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", vm_.connection_text().c_str());
    }
}

void App::draw_status_line() {
    if (const auto& progress = vm_.progress()) {
        const float fraction = progress->total == 0 ? 0.0f : static_cast<float>(progress->done) / static_cast<float>(progress->total);
        char label[96];
        std::snprintf(label, sizeof label, "%s %llu/%llu", progress->stage.c_str(), static_cast<unsigned long long>(progress->done),
                      static_cast<unsigned long long>(progress->total));
        ImGui::ProgressBar(fraction, ImVec2(220.0f, 0.0f), label);
        ImGui::SameLine();
    } else if (vm_.busy()) {
        ImGui::TextColored(kColorWarning, "reloading...");
        ImGui::SameLine();
    }
    ImVec4 color = kColorInfo;
    if (vm_.status_severity() == StatusSeverity::Error) color = kColorError;
    if (vm_.status_severity() == StatusSeverity::Warning) color = kColorWarning;
    ImGui::TextColored(color, "%s", vm_.status_message().empty() ? "Ready" : vm_.status_message().c_str());
}

void App::draw_processes() {
    ImGui::TextUnformatted("Processes and modules");
    ImGui::Separator();
    if (ImGui::Selectable("All processes", selected_pid_ == 0)) {
        selected_pid_ = 0;
        selected_module_.clear();
    }
    const auto& processes = vm_.processes();
    if (processes.empty()) {
        ImGui::TextColored(kColorDim, "No process has registered with the broker yet.");
        ImGui::TextColored(kColorDim, "Start an application that calls cml::CreateDefaultAgent()\nand EnableModule().");
    }
    for (const ProcessRow& process : processes) {
        std::string label = "Process " + std::to_string(process.pid);
        for (const ModuleRow& m : process.modules) {
            if (m.enabled) {
                label += "  " + base_name_of(m.path);
                break;
            }
        }
        ImGui::PushID(static_cast<int>(process.pid));
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (selected_pid_ == process.pid && selected_module_.empty()) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        const bool open = ImGui::TreeNodeEx("process", flags, "%s", label.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            selected_pid_ = process.pid;
            selected_module_.clear();
        }
        if (open) {
            for (const ModuleRow& module : process.modules) {
                ImGui::PushID(module.path.c_str());
                bool enabled = module.enabled;
                ImGui::BeginDisabled();
                ImGui::Checkbox("##enabled", &enabled);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Modules are enabled and disabled by the application (cml::Agent::EnableModule / DisableModule);\n"
                                      "this broker has no control command for it, so the state is shown read-only.");
                }
                ImGui::SameLine();
                const bool selected = selected_pid_ == process.pid && selected_module_ == module.path;
                if (ImGui::Selectable(base_name_of(module.path).c_str(), selected)) {
                    selected_pid_ = process.pid;
                    selected_module_ = module.path;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", module.path.c_str());
                }
                if (module.enabled) {
                    ImGui::SameLine();
                    if (module.index_state == "loading") {
                        ImGui::TextColored(kColorWarning, "indexing %u%%", static_cast<unsigned>(module.index_percent));
                    } else if (module.index_state == "failed") {
                        ImGui::TextColored(kColorError, "index failed");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", module.index_message.c_str());
                        }
                    } else if (module.index_state == "pending") {
                        ImGui::TextColored(kColorWarning, "pending");
                    } else {
                        ImGui::TextColored(kColorGood, "ready");
                    }
                }
                ImGui::PopID();
            }
            if (process.modules_truncated) {
                ImGui::TextColored(kColorDim, "(module list truncated by the broker)");
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (vm_.status_truncated()) {
        ImGui::TextColored(kColorDim, "(process list truncated by the broker)");
    }
}

void App::draw_output() {
    if (ImGui::Button("Clear")) {
        vm_.clear_output();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Warnings", &show_warnings_);
    ImGui::SameLine();
    ImGui::Checkbox("Info", &show_info_);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &output_autoscroll_);
    ImGui::SameLine();
    ImGui::TextColored(vm_.error_count() > 0 ? kColorError : kColorDim, "%zu error(s)", vm_.error_count());
    ImGui::SameLine();
    ImGui::TextColored(vm_.warning_count() > 0 ? kColorWarning : kColorDim, "%zu warning(s)", vm_.warning_count());

    ImGui::BeginChild("output_lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const auto& lines = vm_.output();
    std::vector<std::size_t> visible;
    visible.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const OutputSeverity s = lines[i].severity;
        if ((s == OutputSeverity::Warning && !show_warnings_) || (s == OutputSeverity::Info && !show_info_)) {
            continue;
        }
        visible.push_back(i);
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const OutputLine& line = lines[visible[static_cast<std::size_t>(row)]];
            ImGui::PushID(static_cast<int>(line.seq & 0x7fffffff));
            std::string text = line.diag_id.empty() ? line.text : "[" + line.diag_id + "] " + line.text;
            if (line.location.clickable()) {
                ImGui::PushStyleColor(ImGuiCol_Text, line.severity == OutputSeverity::Error ? kColorError : kColorLink);
                if (ImGui::Selectable(text.c_str())) {
                    open_in_editor(line.location);
                }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Open %s:%d in the editor", line.location.file.c_str(), line.location.line);
                }
            } else {
                ImGui::TextColored(severity_color(line.severity), "%s", text.c_str());
            }
            if (ImGui::BeginPopupContextItem("line_menu")) {
                if (ImGui::MenuItem("Copy line")) {
                    ImGui::SetClipboardText(text.c_str());
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    clipper.End();
    if (output_autoscroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void App::draw_changes() {
    if (ImGui::Button("Clear")) {
        vm_.clear_changes();
    }
    ImGui::SameLine();
    ImGui::TextColored(kColorDim, "Files named by reloads this UI ran, patched modules, and watcher notices from the broker.");
    if (!ImGui::BeginTable("changes", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("File / module", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    const auto& changes = vm_.changes();
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        ImGui::PushID(static_cast<int>(it->seq & 0x7fffffff));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(format_clock(it->when_ms).c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(it->state == "watch" ? kColorWarning : kColorInfo, "%s", it->state.c_str());
        ImGui::TableNextColumn();
        const bool is_file = it->state == "patched" || it->state == "no patch";
        if (is_file) {
            if (ImGui::Selectable(it->path.c_str())) {
                SourceLocation location;
                location.file = it->path;
                location.line = 1;
                open_in_editor(location);
            }
        } else {
            ImGui::TextUnformatted(it->path.c_str());
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(it->detail.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void App::draw_history() {
    ImGui::TextColored(kColorDim, "Every patch applied since the UI connected. Timings come from the reload answer (milliseconds).");
    if (!ImGui::BeginTable("history", 11, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Gen", ImGuiTableColumnFlags_WidthFixed, 45.0f);
    ImGui::TableSetupColumn("Hooks", ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableSetupColumn("New fn", ImGuiTableColumnFlags_WidthFixed, 55.0f);
    ImGui::TableSetupColumn("Sweep", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Compile", ImGuiTableColumnFlags_WidthFixed, 65.0f);
    ImGui::TableSetupColumn("Link", ImGuiTableColumnFlags_WidthFixed, 55.0f);
    ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthFixed, 55.0f);
    ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableHeadersRow();
    const auto& patches = vm_.patches();
    for (auto it = patches.rbegin(); it != patches.rend(); ++it) {
        ImGui::PushID(static_cast<int>(it->seq & 0x7fffffff));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(format_clock(it->when_ms).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%u", static_cast<unsigned>(it->pid));
        ImGui::TableNextColumn();
        if (it->reverted) {
            ImGui::TextColored(kColorDim, "%s (reverted)", base_name_of(it->module).c_str());
        } else {
            ImGui::TextUnformatted(base_name_of(it->module).c_str());
        }
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(it->generation));
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(it->hooks));
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(it->new_functions));
        const PatchTiming& t = it->timing;
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(milliseconds(t.sweep, t.present).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(milliseconds(t.compile, t.present).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(milliseconds(t.link, t.present).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(milliseconds(t.load, t.present).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(milliseconds(t.total, t.present).c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void App::draw_log() {
    static const char* const kLevels[] = {"trace", "debug", "info", "warn", "error"};
    int level = static_cast<int>(log_filter_.min_level);
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::Combo("Level", &level, kLevels, 5)) {
        log_filter_.min_level = static_cast<LogLevel>(level);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::InputTextWithHint("##log_filter", "filter text", log_text_, sizeof log_text_)) {
        log_filter_.text = log_text_;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &log_autoscroll_);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        vm_.clear_log();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy visible")) {
        std::string all;
        for (const std::size_t index : vm_.filtered_log(log_filter_)) {
            const LogLine& line = vm_.log()[index];
            all += format_clock(line.when_ms) + " [" + to_string(line.level) + "] " + line.message + "\n";
        }
        ImGui::SetClipboardText(all.c_str());
    }

    ImGui::BeginChild("log_lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const std::vector<std::size_t> visible = vm_.filtered_log(log_filter_);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const LogLine& line = vm_.log()[visible[static_cast<std::size_t>(row)]];
            ImGui::TextColored(level_color(line.level), "%s [%s] %s", format_clock(line.when_ms).c_str(), to_string(line.level),
                               line.message.c_str());
        }
    }
    clipper.End();
    if (log_autoscroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void App::draw_settings() {
    ImGui::TextColored(kColorDim, "Stored in %s", settings_path_.c_str());
    for (const std::string& warning : settings_warnings_) {
        ImGui::TextColored(kColorWarning, "%s", warning.c_str());
    }
    ImGui::SeparatorText("Hotkey");
    ImGui::Checkbox("Global reload hotkey", &edit_.hotkey_enabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("##hotkey", hotkey_buffer_, sizeof hotkey_buffer_);
    ImGui::SameLine();
    ImGui::TextColored(kColorDim, "e.g. Ctrl+Alt+F11");
    if (!hotkey_error_.empty()) {
        ImGui::TextColored(kColorWarning, "%s", hotkey_error_.c_str());
    }

    ImGui::SeparatorText("Compilation");
    ImGui::Checkbox("Compile on save", &edit_.compile_on_save);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Parallel compiles (0 = broker default)", &edit_.jobs);
    edit_.jobs = std::clamp(edit_.jobs, 0, 256);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Max affected compilands (-1 = leave, 0 = no limit)", &edit_.max_affected_compilands);
    edit_.max_affected_compilands = std::max(edit_.max_affected_compilands, -1);

    ImGui::SeparatorText("Editor");
    int kind = static_cast<int>(edit_.editor.kind);
    static const char* const kKinds[] = {"Visual Studio Code (code -g)", "Visual Studio (devenv /edit)", "Custom command"};
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::Combo("Open errors with", &kind, kKinds, 3)) {
        edit_.editor.kind = static_cast<EditorKind>(kind);
    }
    ImGui::SetNextItemWidth(420.0f);
    ImGui::InputText("Program path (optional)", program_buffer_, sizeof program_buffer_);
    if (edit_.editor.kind == EditorKind::Custom) {
        ImGui::SetNextItemWidth(420.0f);
        ImGui::InputText("Command template", template_buffer_, sizeof template_buffer_);
        ImGui::TextColored(kColorDim, "Placeholders: {file} {line} {col}. Quote words with spaces. The first word is the program.");
    }
    {
        EditorConfig preview = edit_.editor;
        preview.custom_template = template_buffer_;
        preview.program = program_buffer_;
        SourceLocation sample;
        sample.file = R"(C:\src\example.cpp)";
        sample.line = 42;
        sample.column = 7;
        Result<EditorLaunch> launch = make_editor_launch(preview, sample);
        if (launch) {
            ImGui::TextColored(kColorDim, "Runs: %s", join_command_line(launch->argv).c_str());
        } else {
            ImGui::TextColored(kColorWarning, "%s", launch.error().message.c_str());
        }
    }

    ImGui::SeparatorText("Window and broker");
    ImGui::Checkbox("Closing the window hides it to the tray", &edit_.minimize_to_tray);
    ImGui::Checkbox("Start cml_broker.exe when none answers", &edit_.start_broker);
    ImGui::SetNextItemWidth(420.0f);
    ImGui::InputText("Broker path (empty = next to cml_ui.exe)", broker_buffer_, sizeof broker_buffer_);
    ImGui::SetNextItemWidth(420.0f);
    ImGui::InputText("Control pipe (empty = default)", pipe_buffer_, sizeof pipe_buffer_);

    ImGui::Separator();
    if (ImGui::Button("Apply and save")) {
        Settings next = edit_;
        next.editor.custom_template = template_buffer_;
        next.editor.program = program_buffer_;
        next.broker_path = broker_buffer_;
        next.control_pipe = pipe_buffer_;
        bool ok = true;
        if (std::strlen(hotkey_buffer_) == 0) {
            next.hotkey_enabled = false;
        } else if (Result<Hotkey> hotkey = parse_hotkey(hotkey_buffer_)) {
            next.reload_hotkey = *hotkey;
        } else {
            settings_message_ = hotkey.error().format();
            settings_message_error_ = true;
            ok = false;
        }
        if (ok) {
            next.min_log_level = to_string(log_filter_.min_level);
            apply_settings(next);
            load_edit_buffers();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert changes")) {
        load_edit_buffers();
        settings_message_.clear();
    }
    if (!settings_message_.empty()) {
        ImGui::TextColored(settings_message_error_ ? kColorError : kColorGood, "%s", settings_message_.c_str());
    }
}

void App::draw_tabs() {
    if (!ImGui::BeginTabBar("tabs")) {
        return;
    }
    const auto item = [&](const char* label, Tab tab, void (App::*draw)()) {
        ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
        if (tab_request_pending_ && tab_request_ == tab) {
            flags |= ImGuiTabItemFlags_SetSelected;
            tab_request_pending_ = false;
        }
        if (ImGui::BeginTabItem(label, nullptr, flags)) {
            (this->*draw)();
            ImGui::EndTabItem();
        }
    };
    std::string output_label = "Output";
    if (vm_.error_count() > 0) {
        output_label += " (" + std::to_string(vm_.error_count()) + ")";
    }
    output_label += "###output";
    item(output_label.c_str(), Tab::Output, &App::draw_output);
    item("Changes###changes", Tab::Changes, &App::draw_changes);
    item("Patch history###history", Tab::History, &App::draw_history);
    item("Log###log", Tab::Log, &App::draw_log);
    item("Settings###settings", Tab::Settings, &App::draw_settings);
    ImGui::EndTabBar();
}

void App::draw_modals() {
    // Exception (WP-20): Continue / Leave function / Ignore.
    if (vm_.current_exception() != nullptr && !ImGui::IsPopupOpen("Exception in target")) {
        open_exception_popup_ = true;
    }
    if (open_exception_popup_) {
        ImGui::OpenPopup("Exception in target");
        open_exception_popup_ = false;
        if (!window_visible_) {
            show_window();
        }
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Exception in target", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const ExceptionRow* row = vm_.current_exception();
        if (row == nullptr) {
            ImGui::CloseCurrentPopup();
        } else {
            const ExceptionRow current = *row;  // the list can change under us after an answer
            ImGui::TextColored(kColorError, "Exception %s", current.code.c_str());
            ImGui::Text("Process %u, thread %u", static_cast<unsigned>(current.pid), static_cast<unsigned>(current.tid));
            if (!current.function.empty()) {
                ImGui::Text("In %s + %s", current.function.c_str(), current.offset.c_str());
            }
            ImGui::Text("Module %s", current.module.c_str());
            ImGui::Text("Address %s", current.address.c_str());
            if (vm_.pending_exceptions().size() > 1) {
                ImGui::TextColored(kColorDim, "%zu more waiting", vm_.pending_exceptions().size() - 1);
            }
            ImGui::TextColored(kColorDim, "Fix the code and reload, then choose how the thread continues.");
            ImGui::Separator();
            for (const std::string& action : current.actions) {
                if (ImGui::Button(action_label(action).c_str())) {
                    do_exception_action(current, action);
                }
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }
        ImGui::EndPopup();
    }

    // CML5551: a reload that would recompile many files.
    if (vm_.confirm_prompt() && !ImGui::IsPopupOpen("Confirm large reload")) {
        open_confirm_popup_ = true;
    }
    if (open_confirm_popup_) {
        ImGui::OpenPopup("Confirm large reload");
        open_confirm_popup_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Confirm large reload", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::optional<ConfirmPrompt> prompt = vm_.confirm_prompt();
        if (!prompt) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::PushTextWrapPos(520.0f);
            ImGui::TextWrapped("%s", prompt->message.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Separator();
            if (ImGui::Button("Recompile all")) {
                const std::uint32_t pid = prompt->pid;
                const std::string module = prompt->module;
                vm_.clear_confirm_prompt();
                do_reload(pid, module, true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                vm_.clear_confirm_prompt();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    if (open_restart_popup_) {
        ImGui::OpenPopup("Restart process");
        open_restart_popup_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Restart process", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Restart process %u? It is closed and started again with its modules re-enabled.", static_cast<unsigned>(restart_pid_));
        if (ImGui::Button("Restart")) {
            do_restart(restart_pid_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (open_revert_all_popup_) {
        ImGui::OpenPopup("Revert all patches");
        open_revert_all_popup_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Revert all patches", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Revert every patch of %s?", selected_pid_ == 0 ? "all processes" : ("process " + std::to_string(selected_pid_)).c_str());
        if (ImGui::Button("Revert all")) {
            do_revert(selected_pid_, selected_module_, "all");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (open_about_popup_) {
        ImGui::OpenPopup("About Chameleon");
        open_about_popup_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("About Chameleon", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Chameleon broker UI");
        ImGui::Text("Broker: %s", vm_.broker_version().empty() ? "(not connected)" : vm_.broker_version().c_str());
        ImGui::Text("Dear ImGui %s (MIT)", ImGui::GetVersion());
        ImGui::Separator();
        if (ImGui::Button("Close")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void App::draw_frame() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar;
    ImGui::Begin("Chameleon", nullptr, flags);
    draw_menu_bar();
    draw_toolbar();
    ImGui::Separator();

    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("left", ImVec2(ImGui::GetContentRegionAvail().x * 0.34f, -footer), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    draw_processes();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("right", ImVec2(0.0f, -footer));
    draw_tabs();
    ImGui::EndChild();

    ImGui::Separator();
    draw_status_line();
    draw_modals();
    ImGui::End();
}

// --- main loop -------------------------------------------------------------------------------------------------------

int App::run() {
    show_message_ = RegisterWindowMessageW(kShowMessageName);
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");

    // Single instance: the second start brings the first window forward and ends.
    mutex_ = CreateMutexW(nullptr, FALSE, kSingleInstanceMutex);
    if (mutex_ != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            const HWND existing = FindWindowW(kWindowClass, nullptr);
            if (existing != nullptr) {
                PostMessageW(existing, show_message_, 0, 0);
                break;
            }
            Sleep(100);
        }
        CloseHandle(mutex_);
        return 0;
    }

    settings_path_ = options_.settings_path.empty() ? default_settings_path() : options_.settings_path;
    Result<Settings> loaded = load_settings(settings_path_, &settings_warnings_);
    if (loaded) {
        settings_ = *loaded;
    } else {
        vm_.add_log(LogLevel::Warn, loaded.error().format());
        vm_.set_status_message(loaded.error().id() + ": " + loaded.error().message, StatusSeverity::Warning);
    }
    parse_log_level(settings_.min_log_level, &log_filter_.min_level);

    ImGui_ImplWin32_EnableDpiAwareness();
    if (!create_window()) {
        MessageBoxW(nullptr, L"The window could not be created.", L"Chameleon", MB_ICONERROR);
        return 1;
    }
    if (!create_device()) {
        const Diag diag = Diag::make(ids::kUiWindow, "no Direct3D 11 device could be created", "Update the graphics driver, or run on a machine with Direct3D 11 (WARP is tried too).");
        MessageBoxW(nullptr, proc::wide_from_utf8(diag.format()).c_str(), L"Chameleon", MB_ICONERROR);
        cleanup_device();
        DestroyWindow(hwnd_);
        UnregisterClassW(kWindowClass, instance_);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // fixed layout; no imgui.ini next to the exe
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd_);
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_, context_);

    load_edit_buffers();
    add_tray_icon();
    register_hotkey();

    link_ = std::make_unique<BrokerLink>(vm_, settings_);
    link_->start();

    if (options_.start_hidden && tray_added_) {
        window_visible_ = false;
    } else {
        ShowWindow(hwnd_, SW_SHOWDEFAULT);
        UpdateWindow(hwnd_);
        window_visible_ = true;
    }

    const float clear_color[4] = {0.09f, 0.10f, 0.12f, 1.0f};
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }
        update_tray_tip();
        if (!window_visible_ && vm_.current_exception() != nullptr) {
            show_window();  // an exception waits for an answer: do not leave it behind a hidden window
        }
        if (!window_visible_ || IsIconic(hwnd_)) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            continue;
        }
        if (swap_chain_occluded_ && swap_chain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        swap_chain_occluded_ = false;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        {
            auto guard = vm_.lock();
            draw_frame();
        }
        ImGui::Render();
        context_->OMSetRenderTargets(1, &target_view_, nullptr);
        context_->ClearRenderTargetView(target_view_, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const HRESULT present = swap_chain_->Present(1, 0);
        swap_chain_occluded_ = present == DXGI_STATUS_OCCLUDED;
    }

    // Shutdown: stop the connections first (they use the view model), then the UI.
    unregister_hotkey();
    remove_tray_icon();
    link_clean_ = link_->stop(3000);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    if (hwnd_ != nullptr && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    UnregisterClassW(kWindowClass, instance_);
    if (mutex_ != nullptr) {
        CloseHandle(mutex_);
    }
    if (!link_clean_) {
        // A connection thread is still inside a long call and uses the view model: leave everything to the OS.
        static_cast<void>(link_.release());
        ExitProcess(0);
    }
    return 0;
}

}  // namespace

int run_app(HINSTANCE instance, const AppOptions& options) {
    App* app = new App(instance, options);
    const int code = app->run();
    delete app;
    return code;
}

}  // namespace cml::ui::win
