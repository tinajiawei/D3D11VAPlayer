#include "ui/host_window.h"

#include <shellapi.h>
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
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

// 找到桌面图标层（SHELLDLL_DefView）所在 WorkerW：壁纸窗口应挂在它下面
static HWND find_worker_w() {
    const HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        // 通知 Progman 创建 WorkerW（部分系统需要先触发一次）
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);
    }
    HWND workerw = nullptr;
    EnumWindows([](HWND top, LPARAM lp) -> BOOL {
        if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
            *reinterpret_cast<HWND*>(lp) = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&workerw));
    return workerw;
}

bool HostWindow::enter_wallpaper_mode() {
    if (!hwnd_ || wallpaper_mode_) return wallpaper_mode_;
    const HWND workerw = find_worker_w();
    if (!workerw) return false;

    // 保存原状，退出时恢复
    saved_style_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    saved_exstyle_ = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    saved_placement_.length = sizeof(saved_placement_);
    GetWindowPlacement(hwnd_, &saved_placement_);

    // 挂到 WorkerW 下：桌面图标之下、壁纸背景之上
    SetParent(hwnd_, workerw);
    SetWindowLongPtrW(hwnd_, GWL_STYLE,
                      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);

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
    return true;
}

void HostWindow::exit_wallpaper_mode() {
    if (!hwnd_ || !wallpaper_mode_) return;
    SetWindowLongPtrW(hwnd_, GWL_STYLE, saved_style_);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, saved_exstyle_);
    SetParent(hwnd_, nullptr);
    SetWindowPlacement(hwnd_, &saved_placement_);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    wallpaper_mode_ = false;
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
            if (on_key_) on_key_(static_cast<unsigned>(wp));
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
        default:
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
