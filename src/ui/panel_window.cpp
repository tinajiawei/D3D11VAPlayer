#include "ui/panel_window.h"

#include <string>

#include <windowsx.h>

#include "imgui.h"
#include "imgui_impl_win32.h"

// 官方把该声明放在 #if 0 块内，需自行前向声明
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace me {

namespace {
// 面板位置记忆文件（exe 同目录 media_engine_panel.ini，[panel] x=/y=）
std::wstring panel_ini_path() {
    wchar_t buf[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, buf, MAX_PATH)) return L"media_engine_panel.ini";
    wchar_t* slash = wcsrchr(buf, L'\\');
    if (slash) *slash = L'\0';
    return std::wstring(buf) + L"\\media_engine_panel.ini";
}
}  // namespace

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
    DragAcceptFiles(hwnd_, TRUE);

    // 位置记忆：优先恢复上次保存的坐标；不在任何工作区内则回退右上角
    RECT wa = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = GetPrivateProfileIntW(L"panel", L"x", INT_MIN, panel_ini_path().c_str());
    int y = GetPrivateProfileIntW(L"panel", L"y", INT_MIN, panel_ini_path().c_str());
    const bool saved_ok = x != INT_MIN && y != INT_MIN &&
                          x + 40 < wa.right && x + width_ > wa.left &&
                          y + 40 < wa.bottom && y + height_ > wa.top;
    if (!saved_ok) {
        x = wa.right - width_ - 16;
        y = wa.top + 16;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

void PanelWindow::destroy() {
    if (hwnd_) {
        // 保存当前位置，下次进入壁纸模式恢复
        RECT rc = {};
        if (GetWindowRect(hwnd_, &rc)) {
            wchar_t buf[32] = {};
            swprintf_s(buf, L"%d", rc.left);
            WritePrivateProfileStringW(L"panel", L"x", buf, panel_ini_path().c_str());
            swprintf_s(buf, L"%d", rc.top);
            WritePrivateProfileStringW(L"panel", L"y", buf, panel_ini_path().c_str());
        }
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
    // 键盘消息转发给面板 ImGui context：InputText 才能接收字符
    const bool keyboard_msg = msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
                              msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_IME_CHAR;
    if (keyboard_msg && imgui_ctx_) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(imgui_ctx_));
        const bool handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp) != 0;
        ImGui::SetCurrentContext(prev);
        if (handled) return 0;
    }
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
            // NOACTIVATE 窗口点击不会自动获得焦点：显式聚焦以接收键盘输入
            SetFocus(hwnd);
            return 0;
        case WM_LBUTTONUP:
            left_down_.store(false);
            return 0;
        case WM_DROPFILES:
            handle_drop(wp);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void PanelWindow::handle_drop(WPARAM wp) {
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