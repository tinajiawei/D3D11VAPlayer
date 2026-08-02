#include "media/sw_decoder.h"

#include "core/log.h"

namespace me {

Error SwDecoder::open(const AVCodecParameters& params) {
    close();

    codec_ = avcodec_find_decoder(params.codec_id);
    if (!codec_) {
        return Error::make(Err::CodecNotFound,
                           "找不到解码器 codec_id=" + std::to_string(params.codec_id));
    }

    ctx_.reset(avcodec_alloc_context3(codec_));
    int ret = avcodec_parameters_to_context(ctx_.get(), &params);
    if (ret < 0) {
        return error_from_av(ret, "avcodec_parameters_to_context");
    }

    ret = avcodec_open2(ctx_.get(), codec_, nullptr);
    if (ret < 0) {
        return error_from_av(ret, "avcodec_open2");
    }

    // 音频解码器的 pix_fmt 是 AV_PIX_FMT_NONE(-1)，av_get_pix_fmt_name 会返回 NULL，
    // 直接传给 operator<< 会触发 strlen(NULL) 崩溃，必须先判空。
    const char* pix_name = av_get_pix_fmt_name(ctx_->pix_fmt);
    ME_LOG_INFO("软解开启: ", codec_->name,
                " | 尺寸:", ctx_->width, "x", ctx_->height,
                " | 像素格式:", pix_name ? pix_name : "n/a");
    return Error::success();
}

void SwDecoder::push(AVPacket* packet) {
    if (!ctx_) return;
    if (!packet) {
        eof_pending_ = true;
        return;
    }
    // 复制一份放入 pending（接口不接管调用者的内存）
    pending_.reset(av_packet_clone(packet));
}

PopResult SwDecoder::pop(AVFrame* out) {
    if (!ctx_) return PopResult::Failed;

    // 1) 若还没送过 EOF，先把手上攒的包（或 EOF）送进解码器
    if (eof_pending_) {
        int ret = avcodec_send_packet(ctx_.get(), nullptr);
        eof_pending_ = false;
        if (ret < 0 && ret != AVERROR_EOF) {
            error_ = error_from_av(ret, "avcodec_send_packet(EOF)");
            return PopResult::Failed;
        }
    }
    if (pending_) {
        int ret = avcodec_send_packet(ctx_.get(), pending_.get());
        pending_.reset();
        if (ret == AVERROR_EOF) {
            // 解码器仍处于排空状态（前一次 EOF 未收尾）：这个包是过期包，防御性丢弃
            return PopResult::NeedMoreData;
        }
        if (ret == AVERROR(EAGAIN)) {
            // 理论不会发生（只有一个 pending），防御性保留语义
            error_ = Error::make(Err::DecodeFailed, "avcodec_send_packet EAGAIN（内部缓冲已满）");
            return PopResult::NeedMoreData;
        }
        if (ret < 0) {
            error_ = error_from_av(ret, "avcodec_send_packet");
            return PopResult::Failed;
        }
    }

    // 2) 取一帧
    int ret = avcodec_receive_frame(ctx_.get(), out);
    if (ret == AVERROR(EAGAIN)) return PopResult::NeedMoreData;
    if (ret == AVERROR_EOF) return PopResult::Eof;
    if (ret < 0) {
        error_ = error_from_av(ret, "avcodec_receive_frame");
        return PopResult::Failed;
    }
    return PopResult::Ok;
}

Error SwDecoder::flush() {
    if (!ctx_) return Error::success();
    pending_.reset();
    eof_pending_ = false;
    avcodec_flush_buffers(ctx_.get());
    ME_LOG_INFO("软解已 flush");
    return Error::success();
}

void SwDecoder::close() {
    ctx_.reset();
    codec_ = nullptr;
    pending_.reset();
    eof_pending_ = false;
}

}  // namespace me
