#pragma once

#include <string>
#include <vector>

#include "core/error.h"

namespace me {

// 音频输出接口（参考 515 的采集/渲染分层：设备后端可插拔）。
// 一期实现：WASAPI（AudioOutput）；未来可加空输出、采集回放等后端。
class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    virtual Error init(int sample_rate, int channels, double buffer_seconds) = 0;
    virtual Error start() = 0;
    virtual void stop() = 0;
    virtual void shutdown() = 0;
    virtual void pause_stream() = 0;
    virtual void resume_stream() = 0;
    virtual bool is_active() const = 0;

    virtual void write(const float* samples, size_t count) = 0;
    virtual void abort_ring() = 0;
    virtual void clear_ring() = 0;

    virtual double get_played_seconds() const = 0;   // 主时钟源：可听位置
    virtual double get_written_seconds() const = 0;  // 写入位置（锚定用）

    virtual void set_volume(float volume) = 0;
    virtual int sample_rate() const = 0;
    virtual int channels() const = 0;
    virtual std::string device_name() const = 0;
    virtual std::vector<std::string> device_names() const = 0;
    virtual Error switch_device(size_t index) = 0;
    virtual Error reset_stream() = 0;
};

}  // namespace me