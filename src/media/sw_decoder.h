#pragma once

#include "media/decoder.h"

namespace me {

// 软件解码器：libavcodec + CPU。
// push/pop 的"适配器"语义见 docs/04 第 3 节——
// 核心是把 avcodec_send_packet 的 EAGAIN 状态机封装成"一次 push 对应 N 次 pop"的拉模型。
class SwDecoder : public IDecoder {
public:
    Error open(const AVCodecParameters& params) override;
    void push(AVPacket* packet) override;
    PopResult pop(AVFrame* out) override;
    Error flush() override;
    void close() override;

    const char* name() const override { return "sw"; }

private:
    AvCodecCtxPtr ctx_;
    const AVCodec* codec_ = nullptr;
    AvPacketPtr pending_;     // 送不进去的包（send_packet 返回 EAGAIN 时保留）
    bool eof_pending_ = false;
};

}  // namespace me