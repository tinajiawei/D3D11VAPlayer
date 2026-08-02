#pragma once

#include "media/decoder.h"
#include <atomic>

namespace me {

// D3D11VA 硬件解码器（docs/04 第 4 节）：
// 解码计算在 GPU 上完成，帧先经 av_hwframe_transfer_data 拷回 CPU 侧 NV12，
// 再走与软解完全相同的渲染路径（一期选择统一渲染路径，GPU 直通留作进阶）。
class D3D11vaDecoder : public IDecoder {
public:
    Error open(const AVCodecParameters& params) override;
    void push(AVPacket* packet) override;
    PopResult pop(AVFrame* out) override;
    Error flush() override;
    void close() override;

    const char* name() const override { return "d3d11va"; }
    bool is_hardware() const override { return hw_engaged_.load(); }

private:
    AvCodecCtxPtr ctx_;
    const AVCodec* codec_ = nullptr;
    AvBufferPtr hw_device_ctx_;  // GPU 设备上下文
    AvFramePtr hw_frame_;        // GPU 侧帧（D3D11 纹理）
    AvFramePtr sw_frame_;        // 拷贝回 CPU 的 NV12 帧
    AvPacketPtr pending_;
    bool eof_pending_ = false;
    std::atomic<bool> hw_engaged_{true};  // 首帧确认硬解确实挂载（open 时 pix_fmt 尚未切换）
};

}  // namespace me
