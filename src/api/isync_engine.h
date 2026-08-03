#pragma once

#include <cstdint>

namespace me {

class IAudioSink;

// 音视频同步接口（参考 515 的 IAVSyn：同步是独立关注点，可插拔/可替换）。
// 一期实现：SyncEngine（音频主时钟 + 冻结锚定）；未来可换策略（如网络流 playout delay 自适应）。
class ISyncEngine {
public:
    virtual ~ISyncEngine() = default;

    virtual void attach_audio(IAudioSink* audio) = 0;
    virtual void detach_audio() = 0;

    virtual double master_clock() const = 0;
    virtual double position() const = 0;
    virtual void set_duration(double d) = 0;
    virtual double duration() const = 0;

    virtual double video_delay(double frame_pts, double last_pts, double last_delay,
                               double frame_duration) = 0;

    virtual void set_speed(double rate) = 0;
    virtual double get_speed() const = 0;
    virtual void set_paused(bool paused) = 0;
    virtual bool is_paused() const = 0;

    virtual void seek(double target_seconds) = 0;
    virtual void freeze_until_audio(double pos, uint64_t gen) = 0;
    virtual bool audio_resume(double first_pts, uint64_t gen) = 0;
    virtual void align_to_video(double video_pts) = 0;
    virtual void reset() = 0;
};

}  // namespace me