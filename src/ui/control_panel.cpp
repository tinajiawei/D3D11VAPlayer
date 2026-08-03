#include "ui/control_panel.h"

#include <string>
#include <cstdio>
#include "api/me_api.h"
#include "imgui.h"

namespace me {

    static std::string g_audio_error;  // 音频设备切换失败提示
void ControlPanel::draw(PlaybackController& controller, PanelRequest& requests, bool* show_panel) {
    // 音频设备列表缓存（2 秒刷新一次，避免每帧枚举 COM 设备）
    static std::vector<std::string> g_devices;
    static double g_dev_refresh = 0.0;
    const double now_qpc = me_now_seconds();
    if (g_devices.empty() || now_qpc - g_dev_refresh > 2.0) {
        g_devices = controller.audio_devices();
        g_dev_refresh = now_qpc;
    }
    if (!show_panel || !*show_panel) return;

    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("控制面板", show_panel)) {
        ImGui::End();
        return;
    }
    ImGui::Checkbox("优先硬件解码", &prefer_hw_);
    requests.prefer_hw = prefer_hw_;  // 每帧同步勾选框状态（拖文件也用这个值）

    // 防"面板不见了"：布局缓存/分辨率变化可能把窗口存到屏幕外，检测到整个窗口在屏幕外时复位
    const ImVec2 panel_pos = ImGui::GetWindowPos();
    const ImVec2 panel_size = ImGui::GetWindowSize();
    const ImVec2 view_origin = ImGui::GetMainViewport()->WorkPos;
    const ImVec2 view_size = ImGui::GetMainViewport()->WorkSize;
    const bool panel_offscreen =
        (panel_pos.x + panel_size.x < view_origin.x || panel_pos.y + panel_size.y < view_origin.y ||
         panel_pos.x > view_origin.x + view_size.x || panel_pos.y > view_origin.y + view_size.y);
    if (panel_offscreen) ImGui::SetWindowPos(ImVec2(12, 12));
    if (ImGui::Button("打开文件...")) {
        requests.open_file = true;
        requests.prefer_hw = prefer_hw_;
    }
    ImGui::SameLine();
    if (ImGui::Button("关闭文件")) controller.close();
    ImGui::SameLine();
    if (ImGui::Button("壁纸模式")) requests.wallpaper_toggle = true;

    if (!open_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "打开失败: %s", open_error_.c_str());
    }

    const bool has_media = controller.has_video() || controller.has_audio();
    if (has_media) {
        ImGui::Separator();

        // 进度条：拖动中保持用户的值，不被播放进度每帧覆盖（否则滑块永远"弹回"，seek 失效）
        const float duration = static_cast<float>(controller.duration());
        static bool scrubbing = false;
        static float scrub_value = 0.0f;
        float position = scrubbing ? scrub_value : static_cast<float>(controller.position());
        // 显示 "当前位置 / 总时长"：SliderFloat 的格式串只接收一个值（当前位置），
        // 直接写 "%.1f / %.1fs" 会导致总时长参数缺失，这里用预格式化标签传入
        char progress_label[64];
        if (duration > 0.0f) {
            std::snprintf(progress_label, sizeof(progress_label), "%.1f / %.1fs", position, duration);
        } else {
            std::snprintf(progress_label, sizeof(progress_label), "%.1fs", position);
        }
        if (ImGui::SliderFloat("进度", &position, 0.0f, duration > 0.0f ? duration : 1.0f,
                               progress_label, ImGuiSliderFlags_NoInput)) {
            scrub_value = position;  // 拖动中：记住用户的值，下一帧不再覆盖
            scrubbing = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            scrubbing = false;      // 松手：执行 seek 到用户拖到的位置
            controller.seek(position);
        }

        if (ImGui::Button(controller.paused() ? "播放" : "暂停")) controller.toggle_pause();

        float speed = static_cast<float>(controller.speed());
        if (ImGui::SliderFloat("速度", &speed, 0.25f, 4.0f, "%.2fx")) {
            controller.set_speed(speed);
        }

        float volume = controller.volume();
        if (ImGui::SliderFloat("音量", &volume, 0.0f, 1.0f, "%.2f")) {
            controller.set_volume(volume);
        }

        ImGui::Separator();
        ImGui::Text("解码器: %s", controller.decoder().c_str());
        ImGui::Text("音频设备: %s", controller.audio_device().c_str());
        if (!g_audio_error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "切换失败: %s", g_audio_error.c_str());
        }
        // 设备选择：用户可手动切到当前扬声器（默认设备失败时自动切走的问题）
        const auto& devices = g_devices;
        if (!devices.empty()) {
            const std::string current = controller.audio_device();
            if (ImGui::BeginCombo("输出设备", current.empty() ? "(未启用)" : current.c_str())) {
                for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
                    const bool selected = devices[i] == current;
                    if (ImGui::Selectable(devices[i].c_str(), selected)) {
                        const me::Error err = controller.set_audio_device(i);
                        g_audio_error = err.ok() ? std::string() : err.message();
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::Text("硬解: %s", controller.hw() ? "是" : "否");
        ImGui::Text("丢帧: %d", controller.dropped());
    } else {
        ImGui::TextDisabled("把媒体文件拖进窗口，或点上面的按钮打开");
    }

    ImGui::Separator();
    ImGui::TextDisabled("快捷键: 空格暂停 | ←→ 10 秒 | [ ] 变速 | M 静音 | H 隐藏面板");
    ImGui::End();
}

}  // namespace me
