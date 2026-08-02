#pragma once

#include "core/av_utils.h"
#include "core/error.h"

namespace me {

// 解码器抽象（见 docs/04）：
// 上层（MediaPlayer、帧队列、seek 流程）只认识这个接口，
// 软解/硬解/未来 AMF/QSV/NVDEC 都是它的一个实现。
//
// 用法协议：
//   open() 成功 -> 循环 { push(包); while (pop(帧) == Ok) 消费帧; }
//   push(nullptr) 表示流结束，随后 pop 会逐步吐出缓冲帧，直到 Eof。
//   flush() 用于 seek：清掉 B 帧重排状态，但保留 open 的编解码参数。
enum class PopResult {
    Ok,           // out 中有有效帧
    NeedMoreData, // 解码器需要更多输入，请继续 push
    Eof,          // 已冲刷完毕，不再有帧
    Failed,       // 解码出错，详情见 error()
};

class IDecoder {
public:
    virtual ~IDecoder() = default;

    virtual Error open(const AVCodecParameters& params) = 0;
    virtual void push(AVPacket* packet) = 0;  // nullptr => EOF/drain
    virtual PopResult pop(AVFrame* out) = 0;
    virtual Error flush() = 0;
    virtual void close() = 0;

    virtual const char* name() const = 0;
    virtual bool is_hardware() const { return false; }
    virtual const Error& error() const { return error_; }

protected:
    Error error_;  // 最近一次错误（pop 返回 Failed 时读取）
};

}  // namespace me