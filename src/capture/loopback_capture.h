#pragma once

#include <atomic>
#include <mutex>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

namespace me {

// WASAPI loopback 音频采集（docs/11 二期 M6）：
// 抓取"系统正在播放的声音"（默认输出端点的回环），输出 float32 交错 PCM。
// 事件驱动模式：GetBuffer 回调线程绝不阻塞，采集线程按事件取数。
class LoopbackCapture {
public:
    ~LoopbackCapture();

    // 打开默认输出端点的回环采集；sample_rate/channels 为输出格式（一般跟随系统混音）
    bool open(int sample_rate = 48000, int channels = 2);
    void close();

    bool start();
    void stop();

    bool is_capturing() const { return running_.load(); }

    // 取出累积的采样（float32 交错），并清空内部缓冲
    std::vector<float> take_samples();

    uint64_t total_samples() const { return total_samples_.load(); }
    int sample_rate() const { return sample_rate_; }
    int channels() const { return channels_; }
    std::string device_name() const { return device_name_; }

private:
    void run_loop();

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    HANDLE event_ = nullptr;
    UINT32 buffer_frames_ = 0;
    bool float_out_ = true;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::vector<float> samples_;
    std::mutex mutex_;
    std::atomic<uint64_t> total_samples_{0};
    int sample_rate_ = 48000;
    int channels_ = 2;
    std::string device_name_;
};

}  // namespace me