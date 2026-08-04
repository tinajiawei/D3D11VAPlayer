#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <windows.h>

#include "ui/desktop_utils.h"

namespace me {

// Win32 宿主窗口：消息循环、拖拽文件、键盘快捷键、鼠标输入状态。
// 输入状态由主线程 WndProc 写入，渲染线程（ImGui 帧）读取，跨线程安全。
class HostWindow {
public:
    using FileDropCallback = std::function<void(const std::wstring& path)>;
    using KeyCallback = std::function<void(unsigned vk)>;
    using ResizeCallback = std::function<void(int width, int height)>;

    bool create(const std::wstring& title, int width, int height);
    void destroy();

    // 壁纸模式：把窗口挂到桌面 WorkerW 下（图标之下、背景之上），并点击穿透
    bool enter_wallpaper_mode();
    void exit_wallpaper_mode();
    bool wallpaper_mode() const { return wallpaper_mode_; }
    HWND handle() const { return hwnd_; }

    void set_file_drop_callback(FileDropCallback callback) { on_file_drop_ = std::move(callback); }
    void set_key_callback(KeyCallback callback) { on_key_ = std::move(callback); }
    void set_resize_callback(ResizeCallback callback) { on_resize_ = std::move(callback); }
    // 应用级消息（托盘回调等）：返回 true 表示已消费
    using AppMessageCallback = std::function<bool(UINT msg, WPARAM wp, LPARAM lp)>;
    void set_app_message_callback(AppMessageCallback callback) { on_app_message_ = std::move(callback); }

    // ImGui 输入：主线程 WndProc 写，渲染线程读（避免跨线程调用 ImGui 输入接口的竞态）
    int take_mouse_wheel();
    bool left_button_down() const { return left_down_.load(); }
    int mouse_x() const { return mouse_x_.load(); }
    int mouse_y() const { return mouse_y_.load(); }

private:
    static constexpr const wchar_t* kClassName = L"MediaEngineHostWindow";
    static constexpr UINT kMsgWallpaperRemount = WM_APP + 0x201;

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void handle_drop(WPARAM wp);
    void remount_wallpaper();  // WorkerW 销毁/Explorer 重启后重新挂载

    HWND hwnd_ = nullptr;
    bool wallpaper_mode_ = false;
    LONG_PTR saved_style_ = 0;
    LONG_PTR saved_exstyle_ = 0;
    WINDOWPLACEMENT saved_placement_{};
    HINSTANCE instance_ = nullptr;
    FileDropCallback on_file_drop_;
    KeyCallback on_key_;
    ResizeCallback on_resize_;
    AppMessageCallback on_app_message_;
    std::atomic<int> mouse_wheel_{0};
    std::atomic<bool> left_down_{false};
    std::atomic<int> mouse_x_{0};
    std::atomic<int> mouse_y_{0};
    std::unique_ptr<me::DesktopLayerWatcher> wallpaper_watcher_;
};

}  // namespace me
