#include "ui/host_window.h"
#include "ui/desktop_utils.h"

#include <cstdio>

#include <shellapi.h>
#include "imgui_impl_win32.h"

// 官方把该声明放在 #if 0 块内，需自行前向声明
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#include <windowsx.h>

namespace me {

bool HostWindow::create(const std::wstring& title, int width, int height) {
    instance_ = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &HostWindow::wnd_proc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    RECT rc = {0, 0, width, height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowExW(0, kClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;

    DragAcceptFiles(hwnd_, TRUE);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void HostWindow::destroy() {
    if (wallpaper_watcher_) { wallpaper_watcher_->stop(); wallpaper_watcher_.reset(); }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

// 找到桌面图标层（SHELLDLL_DefView）所在 WorkerW：壁纸窗口应挂在它下面

bool HostWindow::enter_wallpaper_mode() {
    if (!hwnd_ || wallpaper_mode_) return wallpaper_mode_;
    const me::DesktopLayer layer = me::wait_desktop_layer();
    if (!layer.ok()) return false;
    const HWND workerw = layer.workerw;

    // 保存原状，退出时恢复
    saved_style_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    saved_exstyle_ = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    saved_placement_.length = sizeof(saved_placement_);
    GetWindowPlacement(hwnd_, &saved_placement_);

    // 挂到 WorkerW 下：桌面图标之下、壁纸背景之上
    SetParent(hwnd_, workerw);
    SetWindowLongPtrW(hwnd_, GWL_STYLE,
                      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    me::ensure_workerw_zorder(layer);
    me::refresh_desktop_icons(layer);

    // 铺满当前窗口所在显示器
    const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, &mi)) {
        SetWindowPos(hwnd_, nullptr, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // 点击穿透：只加 WS_EX_TRANSPARENT（不加 WS_EX_LAYERED，flip-model 交换链不兼容 layered）
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, saved_exstyle_ | WS_EX_TRANSPARENT);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    wallpaper_mode_ = true;
    std::fprintf(stderr, "[wallpaper] 进入壁纸模式: workerw=%p raised=%d child=%d size=%dx%d\n",
                 static_cast<void*>(workerw), layer.raised ? 1 : 0,
                 layer.child_workerw ? 1 : 0, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top);

    // 监听 WorkerW/DefView 销毁与 Explorer 重启：桌面层重建后自动重新挂载
    wallpaper_watcher_ = std::make_unique<me::DesktopLayerWatcher>();
    wallpaper_watcher_->start(layer, [this] {
        if (hwnd_) PostMessageW(hwnd_, kMsgWallpaperRemount, 0, 0);
    });
    return true;
}

void HostWindow::exit_wallpaper_mode() {
    if (!hwnd_ || !wallpaper_mode_) return;
    if (wallpaper_watcher_) { wallpaper_watcher_->stop(); wallpaper_watcher_.reset(); }
    SetWindowLongPtrW(hwnd_, GWL_STYLE, saved_style_);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, saved_exstyle_);
    SetParent(hwnd_, nullptr);
    SetWindowPlacement(hwnd_, &saved_placement_);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    wallpaper_mode_ = false;
    std::fprintf(stderr, "[wallpaper] 退出壁纸模式\n");
}

void HostWindow::remount_wallpaper() {
    if (!hwnd_ || !wallpaper_mode_) return;
    const me::DesktopLayer layer = me::wait_desktop_layer(1500);
    if (!layer.ok()) {
        std::fprintf(stderr, "[wallpaper] 桌面层重建后仍未找到 WorkerW，稍后重试\n");
        return;
    }
    if (GetParent(hwnd_) != layer.workerw) {
        SetParent(hwnd_, layer.workerw);
        SetWindowLongPtrW(hwnd_, GWL_STYLE,
                          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
        me::ensure_workerw_zorder(layer);
        me::refresh_desktop_icons(layer);
        const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(monitor, &mi)) {
            SetWindowPos(hwnd_, nullptr, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        std::fprintf(stderr, "[wallpaper] 桌面层重建，已重新挂载 %p\n",
                     static_cast<void*>(layer.workerw));
    } else {
        me::ensure_workerw_zorder(layer);
    }
}

int HostWindow::take_mouse_wheel() {
    return mouse_wheel_.exchange(0);
}

LRESULT CALLBACK HostWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HostWindow* self = reinterpret_cast<HostWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<HostWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (self) return self->handle_message(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT HostWindow::handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_DROPFILES:
            handle_drop(wp);
            return 0;
        case WM_KEYDOWN:
            // 先喂给 ImGui（主窗口面板的输入框需要键盘事件），未被消费才走快捷键
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);  // 先喂给 ImGui
            // 输入框激活时（WantCaptureKeyboard）快捷键不执行，否则 M 会静音、空格会暂停
            if (on_key_ && !ImGui::GetIO().WantCaptureKeyboard) on_key_(static_cast<unsigned>(wp));
            return 0;
        case WM_KEYUP:
        case WM_CHAR:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_IME_CHAR:
            if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 0;
            return 0;
        case WM_MOUSEMOVE:
            mouse_x_.store(GET_X_LPARAM(lp));
            mouse_y_.store(GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSEWHEEL:
            mouse_wheel_.fetch_add(GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
            return 0;
        case WM_LBUTTONDOWN:
            left_down_.store(true);
            return 0;
        case WM_LBUTTONUP:
            left_down_.store(false);
            return 0;
        case WM_SIZE:
            if (on_resize_) on_resize_(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case kMsgWallpaperRemount:
            remount_wallpaper();
            return 0;
        default:
            if (on_app_message_ && on_app_message_(msg, wp, lp)) return 0;
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void HostWindow::handle_drop(WPARAM wp) {
    const HDROP drop = reinterpret_cast<HDROP>(wp);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count > 0) {
        const UINT len = DragQueryFileW(drop, 0, nullptr, 0);
        std::wstring path(len, L'\0');
        DragQueryFileW(drop, 0, path.data(), len + 1);
        if (on_file_drop_) on_file_drop_(path);
    }
    DragFinish(drop);
}

}  // namespace me
