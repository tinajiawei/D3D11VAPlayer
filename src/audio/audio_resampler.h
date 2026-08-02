#pragma once

#include <vector>

#include "core/av_utils.h"
#include "core/error.h"

namespace me {

// libswresample 封装：把解码出的任意采样格式统一成输出设备格式（float32 交错）。
// 变速时以"输出采样率 = 设备采样率 * 倍率"重新打开（详见 docs/03 变速一节）。
class AudioResampler {
public:
    Error open(const AVChannelLayout& in_layout, AVSampleFormat in_format, int in_rate,
               int out_rate, int out_channels);

    // 转换一帧音频，结果追加到 out（float32 交错）。
    Error convert(const AVFrame* in, std::vector<float>& out);

    // 冲刷重采样器内部剩余的样本（EOF 时调用）。
    Error drain(std::vector<float>& out);

    void close();
    bool is_open() const { return swr_ != nullptr; }

private:
    Error convert_impl(const AVFrame* in, int in_nb_samples, std::vector<float>& out);

    SwrPtr swr_;
    int in_rate_ = 0;
    int out_rate_ = 48000;
    int out_channels_ = 2;
    std::vector<float> scratch_;
};

}  // namespace me
