#pragma once

// FFmpeg 资源的 RAII 封装：把所有 av_*_alloc/av_*_free 配对收敛到 unique_ptr 删除器，
// 从根上避免"忘了释放"和"异常路径泄漏"。

#include <memory>
#include <string>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "core/error.h"

namespace me {

struct AvPacketDeleter {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
using AvPacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;

struct AvFrameDeleter {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;

struct AvFormatDeleter {
    void operator()(AVFormatContext* f) const { avformat_close_input(&f); }
};
using AvFormatPtr = std::unique_ptr<AVFormatContext, AvFormatDeleter>;

struct AvCodecCtxDeleter {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
using AvCodecCtxPtr = std::unique_ptr<AVCodecContext, AvCodecCtxDeleter>;

struct AvBufferDeleter {
    void operator()(AVBufferRef* b) const { av_buffer_unref(&b); }
};
using AvBufferPtr = std::unique_ptr<AVBufferRef, AvBufferDeleter>;

struct SwrDeleter {
    void operator()(SwrContext* s) const { swr_free(&s); }
};
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;

// 把 FFmpeg 的负错误码翻译成可读字符串（如 "Invalid data found when processing input"）。
std::string av_error_string(int ret);

// 把 FFmpeg 错误码包成统一的 Error 对象。
Error error_from_av(int ret, std::string_view what);

inline AvPacketPtr make_packet() { return AvPacketPtr(av_packet_alloc()); }
inline AvFramePtr make_frame() { return AvFramePtr(av_frame_alloc()); }

}  // namespace me
