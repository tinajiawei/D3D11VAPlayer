#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace me {

// 网页壁纸增强（实验，默认关闭）：
//  - 系统音频可视化：WASAPI loopback 采集 → FFT → 256 个 0~1 频谱值 → 注入页面
//    （模拟 Wallpaper Engine 的 wallpaperRegisterAudioListener 回调）；
//  - 天气注入：Open-Meteo（开放接口，无需 key）拉取 → 注入页面 #weather 显示。
// 所有跨线程通信：工作线程 PostThreadMessage 通知 WebView STA 线程，由它调 ExecuteScript。
enum { kEnhanceMsgSpectrum = 0x51, kEnhanceMsgWeather = 0x52 };

class WallpaperEnhance {
public:
    ~WallpaperEnhance();

    // WebView 控制器所在 STA 线程（消息泵会处理频谱/天气消息）
    void set_webview_thread(DWORD thread_id);

    void set_audio_enabled(bool on);
    void set_weather_enabled(bool on, const std::string& city);

    // 由 WebView STA 线程消费
    std::vector<float> take_spectrum();      // 256 个 0~1
    std::string take_weather_payload();      // "城市|气温|天气|最高|最低|风速"

private:
    void audio_loop();
    void weather_loop(const std::string city);

    std::atomic<bool> audio_enabled_{false};
    std::atomic<bool> weather_enabled_{false};
    std::atomic<DWORD> thread_id_{0};
    std::thread audio_thread_;
    std::thread weather_thread_;

    std::mutex spec_mutex_;
    std::vector<float> spectrum_;

    std::mutex weather_mutex_;
    std::string weather_payload_;
    bool weather_busy_ = false;
};

}  // namespace me