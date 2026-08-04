#pragma once

#include <functional>

#include <windows.h>

namespace me {

// 托盘命令 ID（菜单项与双击动作共用）
enum TrayCommand {
    kTrayShowPanel = 1,
    kTrayHidePanel,
    kTrayToggleWallpaper,
    kTrayToggleWebWallpaper,  // 切换网页壁纸（隐藏主窗口时也能控制）
    kTrayExit,
};

// 系统托盘图标（docs/11 二期 M2）：
// 壁纸模式下面板可以隐藏，托盘是找回面板/切换壁纸/退出的兜底入口。
// 菜单命令通过回调交给 main.cpp 处理（避免本类依赖全局状态）。
class TrayIcon {
public:
    using CommandCallback = std::function<void(int command)>;

    bool create(HINSTANCE instance, HWND target);
    void destroy();
    bool created() const { return added_; }

    // 处理主窗口转发的 WM_APP+1 回调消息；返回 true 表示已消费
    bool handle_message(WPARAM wp, LPARAM lp);

    void set_command_callback(CommandCallback cb) { on_command_ = std::move(cb); }
    void set_wallpaper_mode(bool on) { wallpaper_mode_ = on; }

private:
    void show_menu();

    HWND target_ = nullptr;
    HINSTANCE instance_ = nullptr;
    bool added_ = false;
    bool wallpaper_mode_ = false;
    CommandCallback on_command_;
};

}  // namespace me