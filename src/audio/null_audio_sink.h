#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "api/iaudio_sink.h"

namespace me {

// 空音频输出桩（IAudioSink 的测试实现，docs/11 二期 M1）：
// 不接触 WASAPI/COM，用单调时钟模拟"正在播放"，位置按真实时间推进。
// 用途：无声卡/无窗口环境的管线回归（--headless）与单元测试。
class NullAudioSink : public IAudioSink {
public:
    NullAudioSink() = default;

    Error init(int sample_rate, int channels, double buffer_seconds) override;
    Error start() override;
    void stop() override;
    void shutdown() override;
    void pause_stream() override;
    void resume_stream() override;
    bool is_active() const override { return active_.load(); }

    void write(const float* samples, size_t count) override;
    void abort_ring() override {}
    void clear_ring() override;

    double get_played_seconds() const override;
    double get_written_seconds() const override;
    void set_volume(float) override {}
    int sample_rate() const override { return sample_rate_; }
    int channels() const override { return channels_; }
    std::string device_name() const override { return "NullAudioSink"; }
    std::vector<std::string> device_names() const override { return {"NullAudioSink"}; }
    Error switch_device(size_t) override { return Error::success(); }
    Error reset_stream() override;

    // 桩统计（供测试断言）
    uint64_t written_samples() const { return written_samples_.load(); }
    int reset_count() const { return reset_count_.load(); }

private:
    std::atomic<bool> active_{false};
    std::atomic<bool> paused_{false};
    std::atomic<uint64_t> written_samples_{0};
    std::atomic<int> reset_count_{0};
    std::atomic<double> base_qpc_{0.0};     // 开始/恢复播放的墙钟基准
    std::atomic<double> paused_accum_{0.0}; // 暂停前已累计的播放秒数
    int sample_rate_ = 48000;
    int channels_ = 2;
};

}  // namespace me