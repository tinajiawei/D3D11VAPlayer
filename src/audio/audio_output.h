#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <vector>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include "core/error.h"
#include "api/iaudio_sink.h"
#include "core/ring_buffer.h"

namespace me {

// WASAPI 共享模式、事件驱动音频输出。
// 数据通路：音频解码线程 -> RingBuffer<float32> -> 本类回调线程 -> 硬件缓冲。
// 回调线程绝不阻塞（宁可写静音），这是音频不爆音的第一原则。
class AudioOutput : public IAudioSink {
public:
    AudioOutput() = default;
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    Error init(int sample_rate = 48000, int channels = 2, double buffer_seconds = 0.2) override;
    Error start() override;
    void stop();            // 停回调线程并停流
    void shutdown();        // stop + 释放 COM 资源
    void pause_stream();    // IAudioClient::Stop：暂停时硬件时钟冻结，主时钟保持一致
    void resume_stream() override;
    Error reset_stream();  // seek 时丢弃设备缓冲旧音频并复位播放位置

    bool is_active() const override { return active_.load(); }
    bool is_initialized() const { return initialized_; }

    void write(const float* samples, size_t count);  // 阻塞：等环形缓冲有空间（背压）
    void abort_ring();                                 // 唤醒阻塞的写入者（关闭时调用）
    void clear_ring() override { if (auto r = ring_.load()) r->clear(); }

    double get_played_seconds() const;  // 音频主时钟来源
    double get_written_seconds() const override { return static_cast<double>(played_frames_.load()) / sample_rate_; }  // 写入位置（含设备缓冲）
    float volume() const { return volume_.load(); }
    void set_volume(float v) { volume_.store(v); }

    int sample_rate() const { return sample_rate_; }
    // 当前使用的输出设备名（面板/日志确认扬声器用）
    std::string device_name() const;  // 加锁拷贝，避免面板线程与设备切换线程竞争字符串
    // 枚举所有活动输出设备（友好名列表，供面板选择）
    std::vector<std::string> device_names() const override;
    // 切换到指定设备（面板选择扬声器用）；当前播放位置由调用方保持
    Error switch_device(size_t index) override;
    int channels() const { return channels_; }

private:
    void run_loop();
    Error init_attempt(int sample_rate, int channels, double buffer_seconds);
    Error try_device(IMMDevice* device, int sample_rate, int channels, double buffer_seconds);
    void release_audio_objects();
    void release_com();

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_client_ = nullptr;
    IAudioClock* audio_clock_ = nullptr;
    HANDLE event_ = nullptr;
    UINT32 buffer_frames_ = 0;
    WAVEFORMATEX* mix_fmt_ = nullptr;  // 系统分配，需 CoTaskMemFree
    bool float_out_ = true;

    // 原子共享指针：设备切换/关闭时音频解码线程可能正阻塞在 push_blocking 上，
    // 直接替换 unique_ptr 会形成指针竞争（use-after-free），共享所有权保证旧缓冲区安全析构
    std::atomic<std::shared_ptr<RingBuffer<float>>> ring_;
    std::vector<float> scratch_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> active_{false};
    std::atomic<float> volume_{1.0f};
    std::atomic<uint64_t> played_frames_{0};
    mutable std::mutex client_mutex_;  // 串行化 IAudioClient 调用（回调线程 vs 控制线程）
    bool initialized_ = false;
    std::string device_name_;
    int sample_rate_ = 48000;
    int channels_ = 2;
};

}  // namespace me
