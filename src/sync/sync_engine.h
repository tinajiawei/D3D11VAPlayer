#pragma once

#include <mutex>
#include <cstdint>

#include "core/clock.h"

namespace me {

class AudioOutput;

// 音画同步引擎（见 docs/03）：
//  - 主时钟：优先用音频硬件时钟（IAudioClock 已播位置 + seek 偏移），无音频时回退单调时钟；
//  - 视频调度：算出"这一帧还要等多久上屏"，落后过多由上层丢帧；
//  - 变速：统一缩放播放时钟；音频侧由解码线程重开重采样器配合（dst 采样率 = 设备采样率 * 倍率）。
class SyncEngine {
public:
    void attach_audio(AudioOutput* audio) { audio_ = audio; }
    void detach_audio() { audio_ = nullptr; }

    double master_clock() const;
    double position() const { return master_clock(); }

    void set_duration(double d) { duration_ = d; }
    double duration() const { return duration_; }

    // 计算这一帧应该再等多久上屏（秒）；返回 0 表示立刻上屏。
    double video_delay(double frame_pts, double last_pts, double last_delay, double frame_duration);

    void set_speed(double rate);   // 限幅 0.25 ~ 4.0
    double get_speed() const;
    void set_paused(bool paused);
    bool is_paused() const;
    void seek(double target_seconds);
    void freeze_until_audio(double pos, uint64_t gen);  // seek 后冻结主时钟，等第一帧新音频起播
    bool audio_resume(double first_pts, uint64_t gen);  // 只有代数匹配的第一帧才能锚定，返回是否真正锚定
    void align_to_video(double video_pts);  // 首帧对齐：吸收启动期音视频时钟差
    void reset();

private:
    AudioOutput* audio_ = nullptr;
    PlaybackClock clock_;
    mutable std::mutex audio_lock_;
    double audio_offset_ = 0.0;  // seek 后主时钟的偏差补偿
    mutable bool audio_pending_ = false;  // 等待 seek 后的第一帧音频（master_clock 超时兜底会清掉）
    double pending_pos_ = 0.0;   // 冻结期间的主时钟位置
    uint64_t pending_gen_ = 0;  // 冻结对应的 seek 代数（防止旧 seek 的帧锚定新 seek）
    double freeze_start_qpc_ = 0.0;       // 冻结起始墙钟（超时兜底，防止管线背压导致永久冻结）
    mutable bool freeze_expired_logged_ = false;
    double duration_ = 0.0;
};

}  // namespace me
