#include "core/clock.h"

#include <windows.h>

namespace me {

double qpc_seconds() {
    static const double kFrequency = [] {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        return static_cast<double>(freq.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / kFrequency;
}

void PlaybackClock::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    base_pos_ = 0.0;
    base_qpc_ = qpc_seconds();
    rate_ = 1.0;
    paused_ = false;
}

void PlaybackClock::set_pos(double pos_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    base_pos_ = pos_seconds;
    base_qpc_ = qpc_seconds();
}

void PlaybackClock::set_rate(double rate) {
    // 先按旧倍率把当前时刻折算成位置，再以新倍率继续走，保证切换倍率瞬间位置连续。
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_) {
        rate_ = rate;
        return;
    }
    base_pos_ = base_pos_ + (qpc_seconds() - base_qpc_) * rate_;
    base_qpc_ = qpc_seconds();
    rate_ = rate;
}

void PlaybackClock::set_paused(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_ == paused) return;
    if (paused) {
        // 暂停：冻结当前位置
        base_pos_ = base_pos_ + (qpc_seconds() - base_qpc_) * rate_;
        paused_ = true;
    } else {
        // 恢复：从冻结位置继续走
        base_qpc_ = qpc_seconds();
        paused_ = false;
    }
}

double PlaybackClock::get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_) return base_pos_;
    return base_pos_ + (qpc_seconds() - base_qpc_) * rate_;
}

double PlaybackClock::get_rate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rate_;
}

bool PlaybackClock::is_paused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

}  // namespace me
