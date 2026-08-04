#include "player/media_player.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/clock.h"
#include "core/log.h"
#include "audio/null_audio_sink.h"

namespace me {

// 默认用 NullAudioSink 桩：引擎自包含可运行（无声但同步时钟可用）；
// 真实后端（WASAPI）由 C API 通过 set_audio_sink 注入（plugin\2\audio.dll）
MediaPlayer::MediaPlayer() : audio_(std::make_unique<NullAudioSink>()) {}
void MediaPlayer::set_audio_sink(std::unique_ptr<IAudioSink> sink) {
    if (sink) audio_ = std::move(sink);
}

void MediaPlayer::set_sync_engine(std::unique_ptr<ISyncEngine> engine) {
    if (engine) sync_ = std::move(engine);
}


namespace {
constexpr double kDropThresholdSeconds = -0.04;  // 视频落后音频超过 40ms 就丢帧
constexpr double kSpeedMin = 0.25;
constexpr double kSpeedMax = 4.0;
}  // namespace

MediaPlayer::~MediaPlayer() { close(); }

Error MediaPlayer::open(const std::string& path, bool prefer_hw) {
    const Error err = open_impl(path, prefer_hw);
    if (!err.ok()) {
        // 失败兜底：启动空转渲染线程，保持控制面板/浮层刷新
        // （壁纸模式下切换文件失败时，面板依赖渲染线程的 present 回调）
        if (renderer_ && !running_.load()) {
            running_.store(true);
            render_thread_ = std::thread(&MediaPlayer::render_loop, this);
            ME_LOG_WARN("媒体打开失败，已启动空转渲染线程保持面板刷新: ", err.message());
        }
    }
    return err;
}

void MediaPlayer::start_idle_render() {
    // 无媒体时也让渲染线程空转：控制面板（含网页壁纸 URL 输入）始终可见
    if (renderer_ && !running_.load()) {
        running_.store(true);
        render_thread_ = std::thread(&MediaPlayer::render_loop, this);
        ME_LOG_INFO("空转渲染线程启动（无媒体，控制面板可用）");
    }
}

Error MediaPlayer::open_impl(const std::string& path, bool prefer_hw) {
    prefer_hw_ = prefer_hw;
    if (!sync_) return Error::make(Err::MediaOpenFailed, "同步引擎不可用（sync 插件未注入）");
    close();

    // close() 会让包/帧队列进入永久终止态；重开新会话前必须复位，
    // 否则新解码线程一启动就因队列 aborted 而立刻退出（画面卡在上一段）
    video_packets_.reset();
    audio_packets_.reset();
    video_frames_.reset();
    video_flush_requested_.store(false);
    audio_flush_requested_.store(false);
    audio_resampler_reopen_.store(false);
    seek_target_.store(0.0);
    discard_until_target_.store(false);
    last_frame_.reset();  // 清掉上一段最后一帧，避免新会话瞬间闪旧画面

    Error err = source_.open(path);
    if (!err.ok()) return err;

    // 视频解码器（可软可硬，失败自动降级）
    if (source_.has_video()) {
        video_decoder_ = DecoderFactory::create(*source_.video_stream()->codecpar, prefer_hw);
        if (!video_decoder_) {
            return Error::make(Err::CodecOpenFailed, "视频解码器打开失败");
        }
        const AVRational avg = source_.video_stream()->avg_frame_rate;
        if (avg.num > 0 && avg.den > 0) {
            video_frame_duration_ = static_cast<double>(avg.den) / avg.num;
        }
    }

    // 音频解码器（软解；音频没有硬解插拔需求）
    if (source_.has_audio()) {
        audio_decoder_ = DecoderFactory::create(*source_.audio_stream()->codecpar, false);
        if (!audio_decoder_) {
            return Error::make(Err::CodecOpenFailed, "音频解码器打开失败");
        }
    }

    // 旋转探测：FFmpeg 8 的显示矩阵挂在解码帧侧数据（AV_FRAME_DATA_DISPLAYMATRIX）上，
    // 解码首帧即可读取；流元数据 rotate 标签作为兜底。
    video_rotation_ = source_.video_rotation();
    if (video_rotation_ == 0 && source_.has_video()) {
        for (int i = 0; i < 50 && video_rotation_ == 0; ++i) {
            AvPacketPtr probe_pkt;
            if (!source_.read_packet(probe_pkt).ok() || !probe_pkt) break;
            if (probe_pkt->stream_index != source_.video_stream()->index) continue;
            video_decoder_->push(probe_pkt.get());
            for (;;) {
                AvFramePtr probe_frame = make_frame();
                const PopResult r = video_decoder_->pop(probe_frame.get());
                if (r == PopResult::Ok) {
                    for (int j = 0; j < probe_frame->nb_side_data; ++j) {
                        const AVFrameSideData* sd = probe_frame->side_data[j];
                        if (sd->type == AV_FRAME_DATA_DISPLAYMATRIX &&
                            sd->size >= 9 * static_cast<int>(sizeof(int32_t))) {
                            const double deg = av_display_rotation_get(
                                reinterpret_cast<const int32_t*>(sd->data));
                            if (std::isfinite(deg)) {
                                int r = static_cast<int>(std::lround(deg)) % 360;
                                if (r < 0) r += 360;
                                if (r == 90 || r == 180 || r == 270) video_rotation_ = r;
                            }
                        }
                    }
                    break;
                }
                if (r == PopResult::NeedMoreData) break;  // 解码器要更多输入：跳出取下一个包
                break;  // Eof / Failed
            }
        }
        // 探测消耗了解码器状态和源位置，全部复位
        video_decoder_->flush();
        source_.close();
        {
            const Error reopen_err = source_.open(path);
            if (!reopen_err.ok()) return reopen_err;
        }
        if (video_rotation_ != 0) {
            ME_LOG_INFO("帧侧数据显示旋转: ", video_rotation_, " 度");
        }
    }
    if (renderer_) renderer_->set_frame_rotation(video_rotation_);
    has_video_.store(source_.has_video());
    has_audio_.store(source_.has_audio());
    hw_active_.store(video_decoder_ && video_decoder_->is_hardware());
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        decoder_name_ = video_decoder_ ? video_decoder_->name()
                                       : (audio_decoder_ ? audio_decoder_->name() : "-");
    }

    sync_->reset();
    sync_->set_duration(source_.duration_seconds());
    sync_->set_speed(speed_.load());

    // 音频输出：失败不致命（视频照常播，只是没声音）
    audio_enabled_ = false;
    if (has_audio_.load()) {
        Error aerr = audio_->init(48000, 2);
        if (!aerr.ok()) {
            ME_LOG_WARN("音频输出初始化失败（已尝试所有活动端点，静音播放）: ", aerr.message());
        } else {
            audio_->set_volume(volume_.load());
            Error serr = audio_->start();
            if (serr.ok()) {
                sync_->attach_audio(audio_.get());
                const AVCodecParameters* ap = source_.audio_stream()->codecpar;
                active_resample_rate_ = audio_->sample_rate() / speed_.load();
                resampler_.open(ap->ch_layout, static_cast<AVSampleFormat>(ap->format),
                                ap->sample_rate, static_cast<int>(std::lround(active_resample_rate_)),
                                audio_->channels());
                audio_enabled_ = true;
            } else {
                ME_LOG_WARN("音频流启动失败（静音播放）: ", serr.message());
            }
        }
    }
    opened_.store(true);
    ended_.store(false);
    video_done_.store(false);
    audio_done_.store(false);
    // 无音频输出时视为已完成，避免结束判定被卡住（必须先复位再置位）
    if (!audio_enabled_) audio_done_.store(true);
    did_first_align_ = false;  // 新文件重新允许首帧对齐（否则第二次打开会跳过对齐）
    dropped_frames_.store(0);

    running_.store(true);
    demux_thread_ = std::thread(&MediaPlayer::demux_loop, this);
    if (has_video_.load()) {
        video_thread_ = std::thread(&MediaPlayer::video_decode_loop, this);
        render_thread_ = std::thread(&MediaPlayer::render_loop, this);
    }
    if (has_audio_.load() && audio_enabled_) {
        audio_thread_ = std::thread(&MediaPlayer::audio_decode_loop, this);
    }

    ME_LOG_INFO("播放开始: 视频=", has_video_.load(), " 音频=", has_audio_.load(),
                " 解码器=", decoder_name(), " 时长=", duration(), "s");
    return Error::success();
}

void MediaPlayer::close() {
    if (running_.exchange(false) || opened_.load()) {
        media_cv_.notify_all();  // 唤醒 EOF 挂起等待的解封装/解码线程，否则 join 永远卡死
        video_packets_.abort();
        audio_packets_.abort();
        video_frames_.abort();
        if (audio_->is_active()) audio_->stop();
        audio_->abort_ring();  // 唤醒阻塞在环形缓冲写入的音频解码线程，否则 join 卡死
        stop_threads();
        resampler_.close();
        audio_->shutdown();
        if (sync_) sync_->detach_audio();
        source_.close();
        video_decoder_.reset();
        audio_decoder_.reset();
        opened_.store(false);
        ended_.store(false);
    }
}

void MediaPlayer::pause() {
    if (sync_) sync_->set_paused(true);
    if (has_audio_.load()) audio_->pause_stream();
}

void MediaPlayer::play() {
    if (sync_) sync_->set_paused(false);
    if (has_audio_.load()) audio_->resume_stream();
}

void MediaPlayer::toggle_pause() {
    if (is_paused()) play();
    else pause();
}

void MediaPlayer::seek(double seconds) {
    if (!opened_.load()) return;
    seconds = std::clamp(seconds, 0.0, duration());

    const uint64_t new_gen = seek_gen_.load() + 1;
    // 顺序很重要：先用新代数冻结主时钟，再 flush 队列/解码器、重置设备缓冲、
    // 最后让解封装跳转并发布新代数——任何旧 seek 的帧都无法锚定新 seek
    if (sync_) {
        if (has_audio_.load() && audio_enabled_) sync_->freeze_until_audio(seconds, new_gen);
        sync_->seek(seconds);
    }
    video_flush_requested_.store(true);
    audio_flush_requested_.store(true);
    video_packets_.flush();
    audio_packets_.flush();
    video_frames_.flush();
    if (has_audio_.load()) audio_->clear_ring();
    if (has_audio_.load() && audio_enabled_) {
        Error rerr = audio_->reset_stream();
        if (!rerr.ok()) ME_LOG_ERROR("seek 重置音频流失败（可能无声）: ", rerr.message());
    }
    source_.seek(seconds);
    seek_target_.store(seconds);
    discard_until_target_.store(true);
    seek_gen_.store(new_gen);
    media_cv_.notify_all();  // 唤醒 EOF 后挂起的解封装/解码线程，否则 seek 后画面定死
    ended_.store(false);
}

void MediaPlayer::set_speed(double speed) {
    speed = std::clamp(speed, kSpeedMin, kSpeedMax);
    speed_.store(speed);
    if (sync_) sync_->set_speed(speed);
    // 拖动速度时事件非常密集：每次重开重采样器都会清空环形缓冲，声音被反复切断。
    // 这里做 80ms 去抖：拖动期间只更新主时钟，稳定后重开一次。
    static double last_reopen_qpc = 0.0;
    const double now = qpc_seconds();
    if (now - last_reopen_qpc > 0.08) {
        last_reopen_qpc = now;
        audio_resampler_reopen_.store(true);
        // 冻结主时钟直到新倍率的第一帧音频写入：
        // 避免重开窗口期音频内容落后（旧速率残音 + 静音）导致听起来"变慢"
        if (has_audio_.load() && audio_enabled_) {
            if (sync_) sync_->freeze_until_audio(sync_->position(), seek_gen_.load());
        }
    }
}

void MediaPlayer::set_volume(float volume) {
    volume_.store(volume);
    audio_->set_volume(volume);
}

double MediaPlayer::position() const {
    if (!opened_.load()) return 0.0;
    return sync_ ? sync_->position() : 0.0;
}

Error MediaPlayer::set_audio_device(int index) {
    if (!has_audio_.load()) {
        return Error::make(Err::InvalidArgument, "当前媒体没有音频流");
    }
    const double pos = position();  // 保持当前播放位置
    Error err = audio_->switch_device(static_cast<size_t>(index));
    if (!err.ok()) {
        // 切换失败时旧设备已释放：回退默认设备，避免播放继续但无声
        ME_LOG_ERROR("切换扬声器失败: ", err.message(), "，回退默认设备");
        err = audio_->init(48000, 2);
        if (err.ok()) err = audio_->start();
        if (!err.ok()) {
            ME_LOG_ERROR("回退默认设备失败: ", err.message());
            return err;
        }
    }
    if (sync_) {
        sync_->seek(pos);  // 切换后重新对齐主时钟
        if (has_audio_.load() && audio_enabled_) sync_->freeze_until_audio(pos, seek_gen_.load());
    }
    audio_resampler_reopen_.store(true);  // 新设备采样率可能不同，强制重开重采样器
    audio_->clear_ring();
    return Error::success();
}

std::string MediaPlayer::decoder_name() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return decoder_name_;
}

// ---------- 线程：解封装（生产者） ----------
void MediaPlayer::demux_loop() {
    ME_LOG_INFO("解封装线程启动");
    uint64_t demux_gen = seek_gen_.load();
    for (;;) {
        const uint64_t g = seek_gen_.load();
        if (g != demux_gen) demux_gen = g;
        const uint64_t pgen = demux_gen;  // 这批包的代数：期间发生 seek 则旧包作废
        ME_LOG_DEBUG("[demux] before read");
        AvPacketPtr packet;
        Error err = source_.read_packet(packet);
        ME_LOG_DEBUG("[demux] after read ok=", err.ok(), " pkt=", (bool)packet);
        if (!err.ok()) {
            ME_LOG_ERROR("读包失败: ", err.message());
            video_packets_.push(nullptr, pgen);
            audio_packets_.push(nullptr, pgen);
            break;
        }
        if (!packet) {  // EOF：挂起等待 seek 而不是退出（seek 后需继续从新位置读包）
            video_packets_.push(nullptr, pgen);
            audio_packets_.push(nullptr, pgen);
            std::unique_lock<std::mutex> lock(media_cv_mutex_);
            media_cv_.wait(lock, [&] { return !running_.load() || seek_gen_.load() != demux_gen; });
            if (!running_.load()) break;
            demux_gen = seek_gen_.load();
            continue;
        }
        if (has_video_.load() && packet->stream_index == source_.video_stream()->index) {
            ME_LOG_DEBUG("[demux] push video packet size=", packet->size);
            video_packets_.push(std::move(packet), pgen);
        } else if (has_audio_.load() && packet->stream_index == source_.audio_stream()->index) {
            audio_packets_.push(std::move(packet), pgen);
        }
        // 其他流（字幕等）直接丢弃
        if (!running_.load()) break;
    }
    ME_LOG_INFO("解封装线程结束");
}

// ---------- 线程：视频解码（消费者 1） ----------
bool MediaPlayer::recreate_video_decoder() {
    if (!source_.has_video()) return false;
    auto decoder = DecoderFactory::create(*source_.video_stream()->codecpar, prefer_hw_);
    if (!decoder) return false;
    video_decoder_ = std::move(decoder);
    hw_active_.store(video_decoder_->is_hardware());
    ME_LOG_WARN("视频解码器已重建: ", video_decoder_->name());
    return true;
}

void MediaPlayer::video_decode_loop() {
    ME_LOG_INFO("视频解码线程启动");
    bool eof = false;
    int recreate_count = 0;
    bool skip_until_keyframe = false;
    uint64_t last_vd_gen = seek_gen_.load();
    while (running_.load()) {
        if (eof) {
            // EOF 后挂起等待 seek（不退出线程，seek 后继续解码新位置）
            video_done_.store(true);  // EOF：让渲染端能判定播放结束
            std::unique_lock<std::mutex> lock(media_cv_mutex_);
            media_cv_.wait(lock, [&] { return !running_.load() || seek_gen_.load() != last_vd_gen; });
            if (!running_.load()) break;
            last_vd_gen = seek_gen_.load();
            video_done_.store(false);
            eof = false;
            continue;
        }
        if (video_flush_requested_.exchange(false)) {
            video_decoder_->flush();
            recreate_count = 0;
        }

        bool draining = false;
        int drain_guard = 0;
        AvPacketPtr packet = video_packets_.pop();
        if (!packet) {
            if (video_packets_.aborted()) break;
            eof = true;  // EOF 哨兵
            video_decoder_->push(nullptr);
            draining = true;
        } else {
            if (skip_until_keyframe && !(packet->flags & AV_PKT_FLAG_KEY)) {
                continue;  // 解码器重建后跳过非关键帧，直到下一个关键帧
            }
            skip_until_keyframe = false;
            video_decoder_->push(packet.get());
        }

        for (;;) {
            AvFramePtr frame = make_frame();
            const PopResult result = video_decoder_->pop(frame.get());
            if (result == PopResult::Ok) {
                if (video_flush_requested_.load()) break;  // seek 已请求 flush：丢弃本帧
                ME_LOG_DEBUG("[vdec] frame decoded fmt=", frame->format);
                if (!video_frames_.push(std::move(frame))) return;  // 队列已 abort
                hw_active_.store(video_decoder_->is_hardware());  // 首帧后校正硬解标志（可能在首帧才降级）
            } else if (result == PopResult::NeedMoreData) {
                if (draining) {
                    if (++drain_guard > 128) break;  // 防呆：排空不能无限循环
                    continue;
                }
                break;
            } else if (result == PopResult::Eof) {
                eof = true;
                break;
            } else {
                ME_LOG_ERROR("视频解码失败: ", video_decoder_->error().message());
                if (recreate_count < 2 && recreate_video_decoder()) {
                    ++recreate_count;
                    skip_until_keyframe = true;
                    video_done_.store(false);
                    eof = false;
                    break;  // 跳出内层，外层继续读包（跳到下一关键帧）
                }
                eof = true;
                break;
            }
        }
    }
    video_done_.store(true);
    ME_LOG_INFO("视频解码线程结束");
}

// ---------- 线程：音频解码（消费者 2） ----------
void MediaPlayer::audio_decode_loop() {
    if (!audio_enabled_) return;
    ME_LOG_INFO("音频解码线程启动");
    bool eof = false;
    uint64_t last_ad_gen = seek_gen_.load();
    std::vector<float> pcm;
    bool resume_pending = false;  // 等待 seek/换设备后的第一帧音频来锚定主时钟
    uint64_t packet_gen_ = 0;  // 当前音频包所属的 seek 代数（旧包解出的帧直接丢弃）

    while (running_.load()) {
        if (eof) {
            // EOF 后挂起等待 seek（不退出线程，seek 后继续解码新位置）
            audio_done_.store(true);  // EOF：让渲染端能判定播放结束
            sync_->audio_resume(sync_->position(), seek_gen_.load());  // 若 seek 后音频立即 EOF，解除冻结
            std::unique_lock<std::mutex> lock(media_cv_mutex_);
            media_cv_.wait(lock, [&] { return !running_.load() || seek_gen_.load() != last_ad_gen; });
            if (!running_.load()) break;
            last_ad_gen = seek_gen_.load();
            audio_done_.store(false);
            audio_->clear_ring();
            eof = false;
            continue;
        }
        if (audio_flush_requested_.exchange(false)) {
            audio_decoder_->flush();
            // seek：重开重采样器，清掉旧样本残留（swr 内部 delay 会混入新音频开头）
            if (resampler_.is_open()) {
                const AVCodecParameters* ap = source_.audio_stream()->codecpar;
                resampler_.close();
                const int target_rate = static_cast<int>(std::lround(audio_->sample_rate() / speed_.load()));
                Error rerr = resampler_.open(ap->ch_layout, static_cast<AVSampleFormat>(ap->format),
                                             ap->sample_rate, target_rate, audio_->channels());
                if (!rerr.ok()) {
                    ME_LOG_ERROR("seek 重开重采样器失败（稍后重试）: ", rerr.message());
                    audio_resampler_reopen_.store(true);
                } else {
                    active_resample_rate_ = audio_->sample_rate() / speed_.load();
                    audio_->clear_ring();
                    resume_pending = true;
                }

            }
            audio_decoder_->flush();
        }

        // 换设备/变速：立即按新设备采样率 × 倍率重开重采样器，
        // 避免旧速率样本被新设备按错误速率消费（快/慢）
        if (audio_resampler_reopen_.exchange(false)) {
            if (resampler_.is_open()) {
                const AVCodecParameters* ap = source_.audio_stream()->codecpar;
                resampler_.close();
                const int target_rate = static_cast<int>(std::lround(audio_->sample_rate() / speed_.load()));
                Error rerr = resampler_.open(ap->ch_layout, static_cast<AVSampleFormat>(ap->format),
                                             ap->sample_rate, target_rate, audio_->channels());
                if (!rerr.ok()) {
                    // 重开失败会让重采样器处于关闭态：立刻重试，否则本帧 convert 失败导致无声
                    ME_LOG_ERROR("变速重开重采样器失败（稍后重试）: ", rerr.message());
                    audio_resampler_reopen_.store(true);
                    continue;
                }
                active_resample_rate_ = audio_->sample_rate() / speed_.load();
                audio_->clear_ring();
                ME_LOG_INFO("设备/变速重采样: ", active_resample_rate_, "Hz");
                resume_pending = true;
            }
        }
        // 变速/换设备：目标倍率或设备采样率变化时重开重采样器
        if (resampler_.is_open()) {
            const double target_rate = audio_->sample_rate() / speed_.load();
            if (std::fabs(target_rate - active_resample_rate_) > 1.0) {
                const AVCodecParameters* ap = source_.audio_stream()->codecpar;
                resampler_.close();
                Error rerr = resampler_.open(ap->ch_layout, static_cast<AVSampleFormat>(ap->format),
                                             ap->sample_rate, static_cast<int>(std::lround(target_rate)),
                                             audio_->channels());
                if (!rerr.ok()) {
                    ME_LOG_ERROR("变速重采样失败（稍后重试）: ", rerr.message());
                    audio_resampler_reopen_.store(true);
                    continue;
                }
                active_resample_rate_ = target_rate;
                audio_->clear_ring();
                ME_LOG_INFO("变速重采样: ", active_resample_rate_, "Hz");
            }
        }

        bool draining = false;
        int drain_guard = 0;
        AvPacketPtr packet = audio_packets_.pop(&packet_gen_);
        if (!packet) {
            if (audio_packets_.aborted()) break;
            eof = true;  // EOF 哨兵
            audio_decoder_->push(nullptr);
            draining = true;
        } else {

            audio_decoder_->push(packet.get());
        }

        for (;;) {
            AvFramePtr frame = make_frame();
            const PopResult result = audio_decoder_->pop(frame.get());
            if (result == PopResult::Ok) {
                if (audio_flush_requested_.load() || seek_gen_.load() != packet_gen_) break;  // seek 已请求 flush 或包已过期：丢弃本帧
                pcm.clear();
                Error err = resampler_.convert(frame.get(), pcm);
                if (err.ok() && !pcm.empty()) {
                    if (resume_pending) {
                        const double apts = frame->pts != AV_NOPTS_VALUE
                            ? static_cast<double>(frame->pts) * av_q2d(source_.audio_stream()->time_base)
                            : sync_->position();
                        // 精确跳转：seek 后丢弃目标点之前的音频帧，
                        // 否则锚定会落在前一关键帧，把主时钟拉回去重播一段（加速时尤其明显）
                        if (discard_until_target_.load() &&
                            apts < seek_target_.load() - 0.05) {
                            continue;
                        }
                        if (sync_->audio_resume(apts, seek_gen_.load())) {
                            resume_pending = false;
                            discard_until_target_.store(false);
                        }
                    }
                    audio_->write(pcm.data(), pcm.size());
                    // 调试：每秒记录写入音频帧的 pts，用于验证内容速率是否跟随倍率
                    static double last_pts_log = 0.0;
                    const double now_q = qpc_seconds();
                    if (frame->pts != AV_NOPTS_VALUE && now_q - last_pts_log > 1.0) {
                        last_pts_log = now_q;
                        ME_LOG_DEBUG("[adec] 写入音频 pts=",
                                     static_cast<double>(frame->pts) * av_q2d(source_.audio_stream()->time_base),
                                     "s");
                    }
                } else if (!err.ok()) {
                    ME_LOG_WARN("音频重采样失败: ", err.message());
                }
            } else if (result == PopResult::NeedMoreData) {
                if (draining) {
                    if (++drain_guard > 128) break;  // 防呆：排空不能无限循环
                    continue;
                }
                break;
            } else if (result == PopResult::Eof) {
                eof = true;
                break;
            } else {
                ME_LOG_ERROR("音频解码失败: ", audio_decoder_->error().message());
                eof = true;
                break;
            }
        }
    }

    // EOF：冲刷重采样器里剩余的样本
    if (resampler_.is_open()) {
        pcm.clear();
        resampler_.drain(pcm);
        if (!pcm.empty()) audio_->write(pcm.data(), pcm.size());
    }
    audio_done_.store(true);
    ME_LOG_INFO("音频解码线程结束");
}

// ---------- 线程：渲染（同步 + 上屏） ----------
void MediaPlayer::render_loop() {
    ME_LOG_INFO("渲染线程启动");
    uint64_t gen = seek_gen_.load();
    double last_pts = 0.0;
    bool have_last_pts = false;
    double last_delay = 0.0;
    const double time_base = source_.has_video() ? av_q2d(source_.video_stream()->time_base) : 0.0;

    while (running_.load()) {
        const uint64_t g = seek_gen_.load();
        if (g != gen) {
            gen = g;
            have_last_pts = false;
            last_delay = 0.0;
            last_frame_.reset();
        }

        // 暂停：保持当前画面，仅刷新叠加 UI
        if (sync_->is_paused()) {
            // seek 后旧帧已清空：暂停中也取一帧预览，避免黑屏
            if (!last_frame_) {
                AvFramePtr preview = video_frames_.pop_front();
                if (preview && seek_gen_.load() == gen) last_frame_ = std::move(preview);
            }
            if (last_frame_ && renderer_) renderer_->draw_frame(last_frame_.get());
            if (present_hook_) present_hook_();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));  // 暂停时 60fps 刷新足够，避免 CPU 空转
            continue;
        }

        AvFramePtr frame = video_frames_.pop_front();

        // seek 可能发生在 pop 之后：此时拿到的可能是 seek 前的旧帧，直接丢弃
        if (seek_gen_.load() != gen) continue;
        if (!frame) {
            static int empty_log_count = 0;
            if (++empty_log_count % 60 == 1) ME_LOG_DEBUG("[render] empty queue");
            // 空队列：保持最后一帧画面（避免 flip-model 呈现未定义内容导致黑屏）
            if (last_frame_ && renderer_) renderer_->draw_frame(last_frame_.get());
            if (present_hook_) present_hook_();
            if (video_done_.load() && (!has_audio_.load() || audio_done_.load())) {
                ended_.store(true);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));  // 静态画面 60fps 刷新足够，避免 CPU 空转
            continue;
        }

        double pts = 0.0;
        if (frame->pts != AV_NOPTS_VALUE) {
            pts = static_cast<double>(frame->pts) * time_base;
        } else if (have_last_pts) {
            pts = last_pts + video_frame_duration_;  // 缺失时间戳递推
        }

        // 落后音频太多：丢帧追上（docs/03 第 3 节）
        if (!have_last_pts && !did_first_align_) {  // 首帧对齐只在首次播放执行（避免 seek 旧帧错位主时钟）
            sync_->align_to_video(pts);
            did_first_align_ = true;
        }
        const double diff = pts - sync_->master_clock();
        if (diff < kDropThresholdSeconds) {
            ME_LOG_DEBUG("[loop] DROP pts=", pts, " diff=", diff);
            dropped_frames_.fetch_add(1);
            last_pts = pts;
            have_last_pts = true;
            continue;
        }

        const double delay = sync_->video_delay(pts, have_last_pts ? last_pts : pts,
                                               last_delay, video_frame_duration_);

        // 分片睡眠等待，保证能及时响应暂停/退出
        if (delay > 0.0) {
            double remain = delay;
            while (remain > 0.0 && running_.load() && !sync_->is_paused()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                remain -= 0.005;
            }
        }
        if (!running_.load()) break;
        // seek 可能发生在等待期间：绘制前再查一次代次，旧帧直接丢弃（避免错位主时钟）
        if (seek_gen_.load() != gen) continue;

        ME_LOG_DEBUG("[render] 上屏 pts=", pts, " diff=", diff, " delay=", delay);
        const double t_draw0 = qpc_seconds();
        if (renderer_) renderer_->draw_frame(frame.get());
        const double t_draw1 = qpc_seconds();
        ME_LOG_DEBUG("[loop] DRAW_MS=", (t_draw1 - t_draw0) * 1000.0);

        last_frame_ = std::move(frame);

        const double t_pre0 = qpc_seconds();
        if (present_hook_) present_hook_();
        const double t_pre1 = qpc_seconds();
        ME_LOG_DEBUG("[loop] PRESENT_MS=", (t_pre1 - t_pre0) * 1000.0);

        last_pts = pts;
        have_last_pts = true;
        last_delay = delay;
    }
    ME_LOG_INFO("渲染线程结束");
}

void MediaPlayer::stop_threads() {
    if (demux_thread_.joinable()) demux_thread_.join();
    if (video_thread_.joinable()) video_thread_.join();
    if (audio_thread_.joinable()) audio_thread_.join();
    if (render_thread_.joinable()) render_thread_.join();
}

}  // namespace me
