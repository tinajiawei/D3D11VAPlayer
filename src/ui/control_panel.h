#pragma once

#include "ui/media_sequence.h"
#include "ui/playback_controller.h"

namespace me {

// 面板请求：由渲染线程产生，主线程消费（文件对话框必须跑在主线程）。
struct PanelRequest {
    bool open_file = false;
    bool prefer_hw = false;
    bool wallpaper_toggle = false;
    bool web_wallpaper_toggle = false;
    bool web_pick_folder = false;  // 选择本地网页壁纸文件夹
    int web_audio_vis = -1;   // 0/1：网页音频可视化(实验)
    int web_weather = -1;    // 0/1：网页天气(实验)
    std::string web_weather_city;
    bool web_weather_city_edited = false;  // 城市输入框被编辑过（含清空）
    int web_background = -1; // 1~31：网页背景编号
    int web_sakura = -1;     // 0/1：樱花
    int web_vis_model = -1;  // 0/1/2：音频可视化模型
    std::string web_url;  // 请求切换壁纸模式
    int sequence_type = -1;      // >=0：用户切换了扫描类型（SequenceType）
    bool sequence_prev = false;
    bool sequence_next = false;
    int sequence_auto_next = -1; // 0/1：用户切换了自动播放下一个
    bool seek_requested = false;  // 进度条松手：seek 交给主线程执行，避免阻塞渲染线程
    double seek_target = 0.0;
};

// Dear ImGui 控制面板（docs/00）：可隐藏（H 键），只通过 PlaybackController 交互。
class ControlPanel {
public:
    // 在渲染线程每帧调用
    void draw(PlaybackController& controller, PanelRequest& requests, bool* show_panel,
             bool wallpaper_mode, bool web_wallpaper_active, const SequenceInfo& seq);
    void set_open_error(const std::string& message) { open_error_ = message; }
    void set_web_url(const std::string& url);  // 外部（文件夹选择器）填入 URL 输入框
    std::string web_url() const { return web_url_buf_; }

private:
    bool prefer_hw_ = true;
    char web_url_buf_[256] = {};
    bool web_audio_vis_ = true;   // 默认开（页面用到才生效）
    bool web_weather_on_ = true;   // 天气默认开启，城市留空=IP 自动定位
    char web_weather_city_[64] = "";
    int web_background_ = 1;
    bool web_sakura_ = true;
    int web_vis_model_ = 1;
    std::string open_error_;  // 最近一次打开失败的原因（面板显示）
};

}  // namespace me
