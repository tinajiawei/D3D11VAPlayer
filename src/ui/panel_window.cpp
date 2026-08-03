#include "ui/panel_window.h"

#include <windowsx.h>

namespace me {

bool PanelWindow::create(HINSTANCE instance, int width, int height) {
    instance_ = instance;
    width_ = width;
    height_ = height;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PanelWindow::wnd_proc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                            kClassName, L"MediaEngine 控制面板", WS_POPUP,
                            CW_USEDEFAULT, CW_USEDEFAULT, width_, height_,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;

    // 默认停在工作区右上角（后续可加位置记忆）
    RECT wa = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int x = wa.right - width_ - 16;
    const int y = wa.top + 16;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

void PanelWindow::destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

int PanelWindow::take_mouse_wheel() {
    return mouse_wheel_.exchange(0);
}

LRESULT CALLBACK PanelWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PanelWindow* self = reinterpret_cast<PanelWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<PanelWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (self) return self->handle_message(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PanelWindow::handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
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
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace me