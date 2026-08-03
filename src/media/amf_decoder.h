#pragma once

#include "media/decoder.h"

#include <atomic>

namespace me {

// AMD AMF 硬解解码器（docs/11 二期 M5）：
// 走 FFmpeg 的 AV_HWDEVICE_TYPE_AMF（内部动态加载 amfrt64.dll），
// 帧先拷回 CPU（NV12）再走统一渲染路径，与 D3D11VA 后端同一套接口。
class AmfDecoder : public IDecoder {
public:
    Error open(const AVCodecParameters& params) override;
    void push(AVPacket* packet) override;
    PopResult pop(AVFrame* out) override;
    Error flush() override;
    void close() override;

    const char* name() const override { return "amf"; }
    bool is_hardware() const override { return hw_engaged_.load(); }

private:
    AvCodecCtxPtr ctx_;
    const AVCodec* codec_ = nullptr;
    AvBufferPtr hw_device_ctx_;
    AvFramePtr hw_frame_;
    AvFramePtr sw_frame_;
    AvPacketPtr pending_;
    bool eof_pending_ = false;
    std::atomic<bool> hw_engaged_{true};
};

}  // namespace me