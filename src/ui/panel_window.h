#pragma once

#include <atomic>

#include <windows.h>

namespace me {

// 壁纸模式浮层面板的裸窗口（docs/11 二期 M2）：
// 无边框置顶工具窗（WS_EX_TOOLWINDOW | TOPMOST | NOACTIVATE），默认停在工作区右上角。
// 只负责 HWND 与鼠标输入状态；ImGui 绘制由引擎渲染线程完成（与主窗口串行）。
class PanelWindow {
public:
    bool create(HINSTANCE instance, int width, int height);
    void destroy();
    bool valid() const { return hwnd_ != nullptr; }
    HWND handle() const { return hwnd_; }
    int width() const { return width_; }
    int height() const { return height_; }

    // 输入状态：主线程 WndProc 写，渲染线程读（与 HostWindow 同一模式）
    int take_mouse_wheel();
    bool left_button_down() const { return left_down_.load(); }
    int mouse_x() const { return mouse_x_.load(); }
    int mouse_y() const { return mouse_y_.load(); }

private:
    static constexpr const wchar_t* kClassName = L"MediaEnginePanelWindow";

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    int width_ = 380;
    int height_ = 540;
    std::atomic<int> mouse_wheel_{0};
    std::atomic<bool> left_down_{false};
    std::atomic<int> mouse_x_{0};
    std::atomic<int> mouse_y_{0};
};

}  // namespace me