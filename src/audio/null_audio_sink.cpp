#include "audio/null_audio_sink.h"

#include "core/clock.h"

namespace me {

Error NullAudioSink::init(int sample_rate, int channels, double) {
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    channels_ = channels > 0 ? channels : 2;
    written_samples_.store(0);
    reset_count_.store(0);
    return Error::success();
}

Error NullAudioSink::start() {
    active_.store(true);
    paused_.store(false);
    paused_accum_.store(0.0);
    base_qpc_.store(qpc_seconds());
    return Error::success();
}

void NullAudioSink::stop() { active_.store(false); }

void NullAudioSink::shutdown() { active_.store(false); }

void NullAudioSink::pause_stream() {
    if (!active_.load() || paused_.load()) return;
    const double now = qpc_seconds();
    paused_accum_.store(paused_accum_.load() + (now - base_qpc_.load()));
    paused_.store(true);
}

void NullAudioSink::resume_stream() {
    if (!active_.load() || !paused_.load()) return;
    base_qpc_.store(qpc_seconds());
    paused_.store(false);
}

void NullAudioSink::write(const float*, size_t count) {
    written_samples_.fetch_add(static_cast<uint64_t>(count));
}

void NullAudioSink::clear_ring() {
    // 丢弃设备缓冲：把"已播位置"的墙钟基准重置到当前
    base_qpc_.store(qpc_seconds());
}

double NullAudioSink::get_played_seconds() const {
    const double accum = paused_accum_.load();
    if (paused_.load()) return accum;
    return accum + (qpc_seconds() - base_qpc_.load());
}

double NullAudioSink::get_written_seconds() const {
    return static_cast<double>(written_samples_.load()) / static_cast<double>(sample_rate_);
}

Error NullAudioSink::reset_stream() {
    reset_count_.fetch_add(1);
    clear_ring();
    return Error::success();
}

}  // namespace me