#include "media/amf_decoder.h"

#include "core/log.h"

namespace me {

Error AmfDecoder::open(const AVCodecParameters& params) {
    close();

    // 1. 创建 AMF 设备上下文（FFmpeg 内部动态加载 amfrt64.dll）
    AVBufferRef* raw_device = nullptr;
    int ret = av_hwdevice_ctx_create(&raw_device, AV_HWDEVICE_TYPE_AMF,
                                     nullptr, nullptr, 0);
    if (ret < 0) {
        return error_from_av(ret, "av_hwdevice_ctx_create(AMF)");
    }
    hw_device_ctx_.reset(raw_device);

    // FFmpeg 的 AMF 支持是独立解码器（h264_amf/hevc_amf），需按名查找
    codec_ = (params.codec_id == AV_CODEC_ID_HEVC)
                 ? avcodec_find_decoder_by_name("hevc_amf")
                 : (params.codec_id == AV_CODEC_ID_H264)
                       ? avcodec_find_decoder_by_name("h264_amf")
                       : avcodec_find_decoder(params.codec_id);
    if (!codec_) return Error::make(Err::CodecNotFound, "找不到 AMF 解码器");

    ctx_.reset(avcodec_alloc_context3(codec_));
    ret = avcodec_parameters_to_context(ctx_.get(), &params);
    if (ret < 0) return error_from_av(ret, "avcodec_parameters_to_context");

    ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_.get());
    ret = avcodec_open2(ctx_.get(), codec_, nullptr);
    if (ret < 0) return error_from_av(ret, "avcodec_open2(hw)");

    // 校验解码器确实提供 AMF 硬件路径
    bool has_hw_config = false;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec_, i);
        if (!cfg) break;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            cfg->device_type == AV_HWDEVICE_TYPE_AMF) {
            has_hw_config = true;
            break;
        }
    }
    if (!has_hw_config) {
        return Error::make(Err::Unsupported, "该编码格式驱动不支持 AMF 硬解");
    }
    hw_engaged_.store(true);

    ME_LOG_INFO("AMF 硬解开启: ", codec_->name, " ", ctx_->width, "x", ctx_->height);
    return Error::success();
}

void AmfDecoder::push(AVPacket* packet) {
    if (!ctx_) return;
    if (!packet) {
        eof_pending_ = true;
        return;
    }
    pending_.reset(av_packet_clone(packet));
}

PopResult AmfDecoder::pop(AVFrame* out) {
    if (!ctx_) return PopResult::Failed;

    if (eof_pending_) {
        const int ret = avcodec_send_packet(ctx_.get(), nullptr);
        eof_pending_ = false;
        if (ret < 0 && ret != AVERROR_EOF) {
            error_ = error_from_av(ret, "avcodec_send_packet(EOF)");
            return PopResult::Failed;
        }
    }
    if (pending_) {
        const int ret = avcodec_send_packet(ctx_.get(), pending_.get());
        pending_.reset();
        if (ret < 0) {
            error_ = error_from_av(ret, "avcodec_send_packet");
            return PopResult::Failed;
        }
    }

    if (!hw_frame_) hw_frame_ = make_frame();
    int ret = avcodec_receive_frame(ctx_.get(), hw_frame_.get());
    if (ret == AVERROR(EAGAIN)) return PopResult::NeedMoreData;
    if (ret == AVERROR_EOF) return PopResult::Eof;
    if (ret < 0) {
        error_ = error_from_av(ret, "avcodec_receive_frame");
        return PopResult::Failed;
    }

    static bool format_logged = false;
    if (!format_logged) {
        format_logged = true;
        const char* fmt_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(hw_frame_->format));
        ME_LOG_INFO("AMF 首帧格式: ", fmt_name ? fmt_name : "?");
    }

    // 已是系统内存 NV12/YUV420P 直接透传；否则（GPU 纹理）拷回 CPU
    const AVPixelFormat fmt = static_cast<AVPixelFormat>(hw_frame_->format);
    if (fmt == AV_PIX_FMT_NV12 || fmt == AV_PIX_FMT_YUV420P) {
        av_frame_move_ref(out, hw_frame_.get());
        return PopResult::Ok;
    }

    if (!sw_frame_) sw_frame_ = make_frame();
    if (!sw_frame_->buf[0]) {
        sw_frame_->format = AV_PIX_FMT_NV12;
        sw_frame_->width = hw_frame_->width;
        sw_frame_->height = hw_frame_->height;
        ret = av_frame_get_buffer(sw_frame_.get(), 0);
        if (ret < 0) {
            error_ = error_from_av(ret, "av_frame_get_buffer");
            return PopResult::Failed;
        }
    }
    ret = av_hwframe_transfer_data(sw_frame_.get(), hw_frame_.get(), 0);
    if (ret < 0) {
        // 传输失败则按软解帧透传（面板硬解状态会回落）
        hw_engaged_.store(false);
        ME_LOG_WARN("AMF 帧拷回 CPU 失败，透传原始帧: ", error_from_av(ret, "transfer").message());
        av_frame_move_ref(out, hw_frame_.get());
        return PopResult::Ok;
    }
    av_frame_copy_props(sw_frame_.get(), hw_frame_.get());
    av_frame_move_ref(out, sw_frame_.get());
    return PopResult::Ok;
}

Error AmfDecoder::flush() {
    if (!ctx_) return Error::success();
    pending_.reset();
    eof_pending_ = false;
    avcodec_flush_buffers(ctx_.get());
    return Error::success();
}

void AmfDecoder::close() {
    ctx_.reset();
    codec_ = nullptr;
    hw_device_ctx_.reset();
    hw_frame_.reset();
    sw_frame_.reset();
    pending_.reset();
    eof_pending_ = false;
    error_ = Error::success();
    hw_engaged_.store(true);
}

}  // namespace me