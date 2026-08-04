#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <thread>

#include "api/iaudio_sink.h"
#include "audio/audio_resampler.h"
#include "media/decoder_factory.h"
#include "media/frame_queue.h"
#include "media/media_source.h"
#include "media/packet_queue.h"
#include "api/irenderer.h"
#include "api/isync_engine.h"

namespace me {

// 播放器编排（docs/00、docs/01）：
//   解封装线程 -> 双包队列 -> 音/视频解码线程 -> 帧队列/环形缓冲 -> 渲染线程 + WASAPI。
// 所有控制入口（play/pause/seek/speed/volume）对线程安全，
// UI 层只通过 PlaybackController 访问本类。
class MediaPlayer {
public:
    MediaPlayer();
    ~MediaPlayer();

    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    // 渲染器由宿主窗口创建后注入；present_hook 在渲染线程每帧调用（用于叠加 ImGui）。
    void set_renderer(IRenderer* renderer) { renderer_ = renderer; }
    void set_present_hook(std::function<void()> hook) { present_hook_ = std::move(hook); }
    void set_audio_sink(std::unique_ptr<IAudioSink> sink);
    // 同步引擎由 C API 注入（plugin\2\sync.dll）；未注入时 open 失败
    void set_sync_engine(std::unique_ptr<ISyncEngine> engine);

    Error open(const std::string& path, bool prefer_hw = false);
    Error open_impl(const std::string& path, bool prefer_hw);  // open 的实际实现（失败时由 open 启动空转渲染）
    void close();
    void pause();
    void play();
    void toggle_pause();
    void seek(double seconds);
    void set_speed(double speed);
    void set_volume(float volume);

    // 查询（供 UI）
    double position() const;
    double duration() const { return sync_ ? sync_->duration() : 0.0; }
    bool is_paused() const { return sync_ ? sync_->is_paused() : false; }
    bool has_video() const { return has_video_.load(); }
    bool has_audio() const { return has_audio_.load(); }
    bool is_open() const { return opened_.load(); }
    bool is_ended() const { return ended_.load(); }
    bool hw_active() const { return hw_active_.load(); }
    int dropped_frames() const { return dropped_frames_.load(); }
    std::string decoder_name() const;
    std::string audio_device_name() const { return audio_->device_name(); }
    std::vector<std::string> audio_devices() const { return audio_->device_names(); }
    Error set_audio_device(int index);
    double speed() const { return speed_.load(); }
    float volume() const { return volume_.load(); }

private:
    void demux_loop();
    void video_decode_loop();
    bool recreate_video_decoder();  // seek/切换后硬解进入错误状态时重建
    void audio_decode_loop();
    void render_loop();
    void stop_threads();

    MediaSource source_;
    PacketQueue video_packets_{1024, 32 * 1024 * 1024};
    PacketQueue audio_packets_{1024, 16 * 1024 * 1024};
    FrameQueue video_frames_{8};
    std::unique_ptr<ISyncEngine> sync_;
    std::unique_ptr<IAudioSink> audio_;
    AudioResampler resampler_;
    IRenderer* renderer_ = nullptr;
    std::function<void()> present_hook_;

    std::unique_ptr<IDecoder> video_decoder_;
    std::unique_ptr<IDecoder> audio_decoder_;

    std::thread demux_thread_;
    std::thread video_thread_;
    std::thread audio_thread_;
    std::thread render_thread_;

    std::atomic<bool> running_{false};
    std::atomic<bool> opened_{false};
    std::atomic<bool> ended_{false};
    std::atomic<bool> has_video_{false};
    std::atomic<bool> has_audio_{false};
    bool audio_enabled_ = false;
    std::atomic<bool> hw_active_{false};
    bool prefer_hw_ = false;  // open 时的硬解偏好（重建解码器时沿用）
    std::atomic<bool> video_flush_requested_{false};
    std::atomic<bool> audio_flush_requested_{false};
    std::atomic<bool> audio_resampler_reopen_{false};  // 换设备/变速后强制重开重采样器
    std::atomic<bool> video_done_{false};
    std::atomic<bool> audio_done_{false};
    std::atomic<uint64_t> seek_gen_{0};
    std::atomic<double> seek_target_{0.0};        // 最近一次 seek 的目标秒数（精确跳转用）
    std::atomic<bool> discard_until_target_{false};  // seek 后丢弃目标点之前的音频帧
    std::atomic<int> dropped_frames_{0};
    std::atomic<double> speed_{1.0};
    std::atomic<float> volume_{1.0f};

    mutable std::mutex media_cv_mutex_;   // EOF 后等待 seek 唤醒（三个工作线程共用）
    std::condition_variable media_cv_;
    mutable std::mutex state_mutex_;
    std::string decoder_name_ = "-";
    double video_frame_duration_ = 1.0 / 30.0;
    int video_rotation_ = 0;   // 从解码帧侧数据探测的旋转（0/90/180/270）
    bool did_first_align_ = false;  // 首帧对齐只在首次播放执行（seek 后由 seek 设定主时钟）
    AvFramePtr last_frame_;   // 暂停/无新帧时重绘用
    double active_resample_rate_ = 0.0;
};

}  // namespace me
