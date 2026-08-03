#include "ui/tray_icon.h"

#include <shellapi.h>

namespace me {

namespace {
constexpr UINT kTrayId = 1;
constexpr UINT kTrayCallback = WM_APP + 1;
}  // namespace

bool TrayIcon::create(HINSTANCE instance, HWND target) {
    if (added_ || !target) return false;
    instance_ = instance;
    target_ = target;

    NOTIFYICONDATAW nd = {};
    nd.cbSize = sizeof(nd);
    nd.hWnd = target_;
    nd.uID = kTrayId;
    nd.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nd.uCallbackMessage = kTrayCallback;
    nd.hIcon = LoadIconW(instance_, IDI_APPLICATION);
    wcscpy_s(nd.szTip, L"MediaEngine 播放器");
    added_ = Shell_NotifyIconW(NIM_ADD, &nd) != FALSE;
    return added_;
}

void TrayIcon::destroy() {
    if (!added_) return;
    NOTIFYICONDATAW nd = {};
    nd.cbSize = sizeof(nd);
    nd.hWnd = target_;
    nd.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nd);
    added_ = false;
    target_ = nullptr;
}

bool TrayIcon::handle_message(WPARAM wp, LPARAM lp) {
    if (wp != kTrayId) return false;
    switch (lp) {
        case WM_RBUTTONUP:
            show_menu();
            return true;
        case WM_LBUTTONDBLCLK:
            if (on_command_) on_command_(kTrayShowPanel);
            return true;
        default:
            return false;
    }
}

void TrayIcon::show_menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kTrayShowPanel, L"显示控制面板");
    AppendMenuW(menu, MF_STRING, kTrayHidePanel, L"隐藏控制面板");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (wallpaper_mode_ ? MF_CHECKED : 0), kTrayToggleWallpaper,
                L"壁纸模式");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit, L"退出");

    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(target_);  // 保证菜单能立即响应键盘/点击后正常消失
    const int cmd = static_cast<int>(TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                                    pt.x, pt.y, 0, target_, nullptr));
    DestroyMenu(menu);
    if (cmd > 0 && on_command_) on_command_(cmd);
}

}  // namespace me