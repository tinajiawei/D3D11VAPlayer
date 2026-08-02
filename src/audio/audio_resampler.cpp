#include "audio/audio_resampler.h"

#include "core/log.h"

namespace me {

Error AudioResampler::open(const AVChannelLayout& in_layout, AVSampleFormat in_format, int in_rate,
                           int out_rate, int out_channels) {
    close();
    in_rate_ = in_rate;
    out_rate_ = out_rate;
    out_channels_ = out_channels;

    AVChannelLayout out_layout{};
    av_channel_layout_default(&out_layout, out_channels);

    SwrContext* raw = nullptr;
    int ret = swr_alloc_set_opts2(&raw, &out_layout, AV_SAMPLE_FMT_FLT, out_rate,
                                  &in_layout, in_format, in_rate, 0, nullptr);
    if (ret < 0) {
        return error_from_av(ret, "swr_alloc_set_opts2");
    }
    swr_.reset(raw);

    ret = swr_init(swr_.get());
    if (ret < 0) {
        return error_from_av(ret, "swr_init");
    }

    ME_LOG_INFO("音频重采样开启: ", in_rate, "Hz -> ", out_rate, "Hz, ",
                out_channels, "ch, float32");
    return Error::success();
}

Error AudioResampler::convert(const AVFrame* in, std::vector<float>& out) {
    if (!swr_ || !in) return Error::make(Err::InvalidArgument, "重采样器未打开或无输入帧");
    return convert_impl(in, in->nb_samples, out);
}

Error AudioResampler::drain(std::vector<float>& out) {
    if (!swr_) return Error::success();
    return convert_impl(nullptr, 0, out);
}

Error AudioResampler::convert_impl(const AVFrame* in, int in_nb_samples, std::vector<float>& out) {
    const int64_t delay = swr_get_delay(swr_.get(), in_rate_);
    const int out_nb = static_cast<int>(
        av_rescale_rnd(delay + in_nb_samples, out_rate_, in_rate_, AV_ROUND_UP));
    if (out_nb <= 0) return Error::success();

    scratch_.resize(static_cast<size_t>(out_nb) * out_channels_);
    uint8_t* dst = reinterpret_cast<uint8_t*>(scratch_.data());

    const uint8_t** in_data = nullptr;
    if (in) in_data = const_cast<const uint8_t**>(in->extended_data);

    const int converted = swr_convert(swr_.get(), &dst, out_nb, in_data, in_nb_samples);
    if (converted < 0) {
        return error_from_av(converted, "swr_convert");
    }
    out.insert(out.end(), scratch_.begin(),
               scratch_.begin() + static_cast<size_t>(converted) * out_channels_);
    return Error::success();
}

void AudioResampler::close() {
    swr_.reset();
    in_rate_ = 0;
    scratch_.clear();
}

}  // namespace me
