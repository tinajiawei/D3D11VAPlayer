#include "media/media_source.h"

#include <algorithm>
#include <cmath>


#include "core/log.h"

namespace me {

Error MediaSource::open(const std::string& path) {
    close();

    AVFormatContext* raw = nullptr;
    int ret = avformat_open_input(&raw, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return Error::make(Err::MediaOpenFailed,
                           "avformat_open_input(" + path + ") 失败: " + av_error_string(ret));
    }
    fmt_.reset(raw);

    ret = avformat_find_stream_info(fmt_.get(), nullptr);
    if (ret < 0) {
        ME_LOG_WARN("avformat_find_stream_info 失败: ", av_error_string(ret));
        // 不致命：部分文件仍可解码，继续尝试。
    }

    const AVFormatContext* f = fmt_.get();
    ME_LOG_INFO("打开媒体: ", path,
                " | 格式:", (f->iformat ? f->iformat->long_name : "?"),
                " | 流数:", f->nb_streams,
                " | 时长:", duration_seconds(), "s");

    // 选视频流：跳过"封面图"这类 attached_pic 流（mp3 专辑封面常见）。
    for (unsigned i = 0; i < f->nb_streams; ++i) {
        AVStream* st = f->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            video_stream_ = st;
            break;
        }
    }
    for (unsigned i = 0; i < f->nb_streams; ++i) {
        AVStream* st = f->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_ = st;
            break;
        }
    }

    if (!video_stream_ && !audio_stream_) {
        return Error::make(Err::NoStream, "文件中既没有视频流也没有音频流");
    }

    // 旋转元数据：先读传统 rotate 标签，再扫描包侧数据（displaymatrix）
    video_rotation_ = 0;
    if (video_stream_) {
        const AVDictionaryEntry* entry =
            av_dict_get(video_stream_->metadata, "rotate", nullptr, 0);
        if (entry) {
            int r = std::atoi(entry->value) % 360;
            if (r < 0) r += 360;
            if (r == 90 || r == 180 || r == 270) video_rotation_ = r;
        }
        if (video_rotation_ != 0) {
            ME_LOG_INFO("视频旋转元数据: ", video_rotation_, " 度，渲染时旋转画面");
        }
    }

    return Error::success();
}

void MediaSource::close() {
    video_stream_ = nullptr;
    audio_stream_ = nullptr;
    fmt_.reset();
}

Error MediaSource::read_packet(AvPacketPtr& out) {
    std::lock_guard<std::mutex> lock(io_mutex_);  // av_read_frame 不能与 seek 并发（FFmpeg 上下文非线程安全）
    out.reset();
    if (!fmt_) return Error::make(Err::InvalidArgument, "MediaSource 未打开");

    AvPacketPtr pkt = make_packet();
    int ret = av_read_frame(fmt_.get(), pkt.get());
    if (ret == AVERROR_EOF) {
        return Error::success();  // out 保持 nullptr，表示正常 EOF
    }
    if (ret < 0) {
        return Error::make(Err::Io, "av_read_frame 失败: " + av_error_string(ret));
    }
    out = std::move(pkt);
    return Error::success();
}

Error MediaSource::seek(double seconds) {
    std::lock_guard<std::mutex> lock(io_mutex_);  // 与解封装线程的 av_read_frame 互斥
    if (!fmt_) return Error::make(Err::InvalidArgument, "MediaSource 未打开");

    seconds = std::clamp(seconds, 0.0, duration_seconds());
    const int64_t target_ts = static_cast<int64_t>(seconds * AV_TIME_BASE);
    int ret = avformat_seek_file(fmt_.get(), -1, INT64_MIN, target_ts, target_ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        return Error::make(Err::Io, "seek 失败: " + av_error_string(ret));
    }
    ME_LOG_INFO("seek 到 ", seconds, "s");
    return Error::success();
}

double MediaSource::duration_seconds() const {
    if (!fmt_) return 0.0;
    if (fmt_->duration != AV_NOPTS_VALUE) {
        return static_cast<double>(fmt_->duration) / AV_TIME_BASE;
    }
    return 0.0;
}

}  // namespace me
