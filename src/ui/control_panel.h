#pragma once

#include "ui/playback_controller.h"

namespace me {

// 面板请求：由渲染线程产生，主线程消费（文件对话框必须跑在主线程）。
struct PanelRequest {
    bool open_file = false;
    bool prefer_hw = false;
    bool wallpaper_toggle = false;  // 请求切换壁纸模式
};

// Dear ImGui 控制面板（docs/00）：可隐藏（H 键），只通过 PlaybackController 交互。
class ControlPanel {
public:
    // 在渲染线程每帧调用
    void draw(PlaybackController& controller, PanelRequest& requests, bool* show_panel);

private:
    bool prefer_hw_ = true;
};

}  // namespace me
