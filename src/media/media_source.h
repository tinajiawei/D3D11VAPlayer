#pragma once

#include <mutex>
#include <string>

#include "core/av_utils.h"
#include "core/error.h"

namespace me {

// 解封装模块：对 avformat 的薄封装。
// 容器解析（mp4/mkv/mov/ts/flv/webm/avi/wav/mp3...）交给 FFmpeg，
// 本项目自研的是"解码流程的抽离封装"（见 docs/04），这里是管线的入口。
class MediaSource {
public:
    Error open(const std::string& path);
    void close();

    // 读下一个包。EOF 时 out 为 nullptr（正常结束，不算错误）。
    Error read_packet(AvPacketPtr& out);

    // 跳转到指定秒（向后找最近关键帧）。
    Error seek(double seconds);

    AVStream* video_stream() const { return video_stream_; }
    AVStream* audio_stream() const { return audio_stream_; }
    AVFormatContext* context() const { return fmt_.get(); }
    double duration_seconds() const;
    bool has_video() const { return video_stream_ != nullptr; }
    // 视频旋转元数据（手机竖拍常见）：0/90/180/270，渲染时按此旋转画面
    int video_rotation() const { return video_rotation_; }
    bool has_audio() const { return audio_stream_ != nullptr; }

private:
    mutable std::mutex io_mutex_;  // av_read_frame 与 seek 不能并发（FFmpeg 上下文非线程安全）
    AvFormatPtr fmt_;
    AVStream* video_stream_ = nullptr;
    AVStream* audio_stream_ = nullptr;
    int video_rotation_ = 0;
};

}  // namespace me