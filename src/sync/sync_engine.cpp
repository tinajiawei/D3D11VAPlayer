#include "sync/sync_engine.h"

#include <algorithm>
#include <cmath>

#include "api/iaudio_sink.h"
#include "core/log.h"

namespace me {

namespace {
constexpr double kMaxVideoDelay = 0.20;  // 一帧最多等 200ms（防跳变）
constexpr double kAudioStartTimeout = 1.5;  // 音频起播等待上限（4K 中段 seek 常超 500ms）
constexpr double kMinSpeed = 0.25;
constexpr double kMaxSpeed = 4.0;
}  // namespace

double SyncEngine::master_clock() const {
    // 音频主时钟：硬件已播位置 + seek 偏移（seek 时重新对齐）
    if (audio_) {
        std::lock_guard<std::mutex> lock(audio_lock_);
        if (audio_->is_active()) {
            if (audio_pending_) {
                // 超时兜底：若 1.5s 内音频没起播（如 4K 中段 seek 找关键帧较慢），
                // 回退单调时钟继续走（从目标位置起算），避免用错误偏移的主时钟造成卡顿/跳变；
                // 新音频帧到达时 audio_resume 会重新锚定。
                if (qpc_seconds() - freeze_start_qpc_ > kAudioStartTimeout) {
                    if (!freeze_expired_logged_) {
                        freeze_expired_logged_ = true;
                        ME_LOG_WARN("[sync] 等待音频起播超时(1500ms)，改用单调时钟继续");
                    }
                    audio_pending_ = false;
                    audio_fallback_ = true;
                    clock_.set_pos(pending_pos_);
                } else {
                    return pending_pos_;
                }
            }
            if (audio_fallback_) return clock_.get();
            // 内容时间 = 硬件已播位置 × 倍率（2x 时设备走 1 秒，内容已播 2 秒）
            return audio_->get_played_seconds() * clock_.get_rate() + audio_offset_;
        }
    }
    // 无音频/音频暂停：回退到单调时钟
    return clock_.get();
}

double SyncEngine::video_delay(double frame_pts, double last_pts, double last_delay,
                               double frame_duration) {
    // 帧间间隔（内容秒）：异常值（<=0 或 >=1s）用历史/帧长兜底
    const double rate = std::max(clock_.get_rate(), 0.01);
    double interval = frame_pts - last_pts;
    if (interval <= 0.0 || interval >= 1.0) {
        interval = last_delay > 0.0 ? last_delay * rate : frame_duration;
    }

    const double diff = frame_pts - master_clock();  // 内容秒；>0: 帧早于音频
    const double threshold = std::max(frame_duration, 0.01);

    // 返回值是"墙钟等待秒数"：内容时间差必须除以倍率（2x 时帧间隔 0.033s
    // 只等 0.0165s），否则变速时画面永远追不上/追过头音频。
    double wall;
    if (diff <= -threshold) {
        wall = 0.0;                 // 落后明显：立即上屏，由上层丢帧策略处理
    } else if (diff >= threshold) {
        wall = diff / rate;         // 超前明显：等到该帧的内容时间点
    } else {
        // 微小偏差：按帧间隔 + 偏差微调（墙钟），画面最平滑
        wall = interval / rate + diff / rate;
    }
    return std::clamp(wall, 0.0, kMaxVideoDelay);
}

void SyncEngine::set_speed(double rate) {
    rate = std::clamp(rate, kMinSpeed, kMaxSpeed);
    std::lock_guard<std::mutex> lock(audio_lock_);
    if (audio_ && audio_->is_active()) {
        const double pos = audio_->get_played_seconds();
        // 保持 master = pos×old + offset_old = pos×new + offset_new 连续
        audio_offset_ += pos * clock_.get_rate() - pos * rate;
    }
    clock_.set_rate(rate);
}

double SyncEngine::get_speed() const { return clock_.get_rate(); }

void SyncEngine::set_paused(bool paused) { clock_.set_paused(paused); }

bool SyncEngine::is_paused() const { return clock_.is_paused(); }

void SyncEngine::seek(double target_seconds) {
    std::lock_guard<std::mutex> lock(audio_lock_);
    if (audio_ && audio_->is_active()) {
        audio_offset_ = target_seconds - audio_->get_played_seconds() * clock_.get_rate();
    }
    clock_.set_pos(target_seconds);
}

void SyncEngine::freeze_until_audio(double pos, uint64_t gen) {
    std::lock_guard<std::mutex> lock(audio_lock_);
    audio_pending_ = true;
    audio_fallback_ = false;
    pending_pos_ = pos;
    pending_gen_ = gen;
    freeze_start_qpc_ = qpc_seconds();
    freeze_expired_logged_ = false;
    ME_LOG_INFO("[sync] 冻结主时钟等待音频起播: ", pos, "s gen=", gen);
}

bool SyncEngine::audio_resume(double first_pts, uint64_t gen) {
    std::lock_guard<std::mutex> lock(audio_lock_);
    if (!audio_pending_ || gen != pending_gen_) return false;  // 旧 seek 的帧不能锚定新 seek
    audio_pending_ = false;
    audio_fallback_ = false;
    freeze_expired_logged_ = false;
    if (audio_ && audio_->is_active()) {
        // 以"写入位置"锚定：第一帧进入设备缓冲时主时钟 = pts - 缓冲延迟，
        // 当它真正到达声卡时主时钟恰好等于 pts（音画对齐）
        const double written = audio_->get_written_seconds();
        audio_offset_ = first_pts - written * clock_.get_rate();
        ME_LOG_INFO("[sync] 音频起播锚定: first_pts=", first_pts, "s written=", written, "s");
        return true;
    } else {
        clock_.set_pos(first_pts);
        return true;
    }
}

void SyncEngine::align_to_video(double video_pts) {
    std::lock_guard<std::mutex> lock(audio_lock_);
    if (audio_ && audio_->is_active()) {
        audio_offset_ = video_pts - audio_->get_played_seconds() * clock_.get_rate();
    } else {
        clock_.set_pos(video_pts);
    }
    ME_LOG_INFO("[sync] 首帧对齐: video_pts=", video_pts, " audio_offset=", audio_offset_);
}

void SyncEngine::reset() {
    std::lock_guard<std::mutex> lock(audio_lock_);
    audio_offset_ = 0.0;
    audio_pending_ = false;
    audio_fallback_ = false;
    clock_.reset();
}

}  // namespace me
