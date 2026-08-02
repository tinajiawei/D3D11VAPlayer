#pragma once

#include <mutex>

namespace me {

// 全局单调时钟（QueryPerformanceCounter），秒为单位。
// 与 wall-clock 不同：它不会因系统时间调整而跳变，适合做播放基准。
double qpc_seconds();

// 播放时钟：位置 = 基准位置 + (当前时刻 - 基准时刻) * 倍率。
// 支持变速、暂停、seek（set_pos 后位置立即跳变）。
class PlaybackClock {
public:
    void reset();
    void set_pos(double pos_seconds);
    void set_rate(double rate);
    void set_paused(bool paused);

    double get() const;
    double get_rate() const;
    bool is_paused() const;

private:
    mutable std::mutex mutex_;
    double base_pos_ = 0.0;
    double base_qpc_ = qpc_seconds();
    double rate_ = 1.0;
    bool paused_ = false;
};

}  // namespace me
