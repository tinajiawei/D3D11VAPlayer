#include "media/d3d11va_decoder.h"

#include "core/log.h"

namespace me {

Error D3D11vaDecoder::open(const AVCodecParameters& params) {
    close();

    // 1. 创建 GPU 设备上下文（默认适配器）
    AVBufferRef* raw_device = nullptr;
    int ret = av_hwdevice_ctx_create(&raw_device, AV_HWDEVICE_TYPE_D3D11VA,
                                     nullptr, nullptr, 0);
    if (ret < 0) {
        return error_from_av(ret, "av_hwdevice_ctx_create(D3D11VA)");
    }
    hw_device_ctx_.reset(raw_device);

    codec_ = avcodec_find_decoder(params.codec_id);
    if (!codec_) return Error::make(Err::CodecNotFound, "找不到解码器");

    ctx_.reset(avcodec_alloc_context3(codec_));
    ret = avcodec_parameters_to_context(ctx_.get(), &params);
    if (ret < 0) return error_from_av(ret, "avcodec_parameters_to_context");

    // 2. 打开解码器时挂上硬件上下文
    ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_.get());
    ret = avcodec_open2(ctx_.get(), codec_, nullptr);
    if (ret < 0) return error_from_av(ret, "avcodec_open2(hw)");

    // 3. 校验解码器确实接受了硬解（FFmpeg 8 起 hw_pix_fmt 字段已移除，用 pix_fmt 判断）
    // 校验编码器确实提供 D3D11VA 硬件路径。
    // 注意：不能在这里检查 ctx_->pix_fmt —— FFmpeg 要到第一帧解码时才把 pix_fmt 切换成 d3d11，
    // open 时检查永远失败，会把硬解误判为不支持。
    bool has_hw_config = false;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec_, i);
        if (!cfg) break;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
            has_hw_config = true;
            break;
        }
    }
    if (!has_hw_config) {
        return Error::make(Err::Unsupported, "该编码格式/驱动不支持 D3D11VA 硬解");
    }
    hw_engaged_.store(true);

    ME_LOG_INFO("D3D11VA 硬解开启: ", codec_->name, " ", ctx_->width, "x", ctx_->height);
    return Error::success();
}

void D3D11vaDecoder::push(AVPacket* packet) {
    if (!ctx_) return;
    if (!packet) {
        eof_pending_ = true;
        return;
    }
    pending_.reset(av_packet_clone(packet));
}

PopResult D3D11vaDecoder::pop(AVFrame* out) {
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
        if (ret == AVERROR_EOF) {
            return PopResult::NeedMoreData;
        }
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

    {  // 一次性确认首帧真是硬件帧（d3d11），否则面板"硬解"会显示否
        static bool format_logged = false;
        if (!format_logged) {
            format_logged = true;
            const char* fmt_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(hw_frame_->format));
            ME_LOG_INFO("D3D11VA 首帧格式: ", fmt_name ? fmt_name : "?");
        }
    }
    if (hw_frame_->format != AV_PIX_FMT_D3D11) {
        // 硬解未实际挂载（驱动/格式原因）：当前帧是软解结果，直接透传
        hw_engaged_.store(false);
        av_frame_move_ref(out, hw_frame_.get());
        return PopResult::Ok;
    }

    // 把 GPU 帧拷回 CPU（NV12），复用 sw_frame_ 缓冲区
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
        error_ = error_from_av(ret, "av_hwframe_transfer_data");
        return PopResult::Failed;
    }
    av_frame_copy_props(sw_frame_.get(), hw_frame_.get());

    av_frame_move_ref(out, sw_frame_.get());
    return PopResult::Ok;
}

Error D3D11vaDecoder::flush() {
    if (!ctx_) return Error::success();
    pending_.reset();
    eof_pending_ = false;
    avcodec_flush_buffers(ctx_.get());
    return Error::success();
}

void D3D11vaDecoder::close() {
    ctx_.reset();
    codec_ = nullptr;
    hw_device_ctx_.reset();
    hw_frame_.reset();
    sw_frame_.reset();
    pending_.reset();
    eof_pending_ = false;
}

}  // namespace me
