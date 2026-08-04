#pragma once

#include "ui/playback_controller.h"

namespace me {

// 面板请求：由渲染线程产生，主线程消费（文件对话框必须跑在主线程）。
struct PanelRequest {
    bool open_file = false;
    bool prefer_hw = false;
    bool wallpaper_toggle = false;
    bool web_wallpaper_toggle = false;
    std::string web_url;  // 请求切换壁纸模式
};

// Dear ImGui 控制面板（docs/00）：可隐藏（H 键），只通过 PlaybackController 交互。
class ControlPanel {
public:
    // 在渲染线程每帧调用
    void draw(PlaybackController& controller, PanelRequest& requests, bool* show_panel,
             bool wallpaper_mode);
    void set_open_error(const std::string& message) { open_error_ = message; }
    std::string web_url() const { return web_url_buf_; }

private:
    bool prefer_hw_ = true;
    char web_url_buf_[256] = {};
    std::string open_error_;  // 最近一次打开失败的原因（面板显示）
};

}  // namespace me
