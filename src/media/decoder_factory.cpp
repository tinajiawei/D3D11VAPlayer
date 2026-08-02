#include "media/decoder_factory.h"

#include <vector>

#include "core/log.h"
#include "media/d3d11va_decoder.h"
#include "media/sw_decoder.h"

namespace me {

namespace {

struct Backend {
    std::string name;
    DecoderFactory::Creator creator;
};

std::vector<Backend>& backends() {
    static std::vector<Backend> registry;
    return registry;
}

// 注册软解（永远兜底）
struct SwRegistrar {
    SwRegistrar() { DecoderFactory::register_backend("sw", [] { return std::make_unique<SwDecoder>(); }); }
};
SwRegistrar g_sw_registrar;

// 注册 D3D11VA 硬解（插拔式：以后 AMF/QSV/NVDEC 同样加一行）
struct HwRegistrar {
    HwRegistrar() {
        DecoderFactory::register_backend("d3d11va", [] { return std::make_unique<D3D11vaDecoder>(); });
    }
};
HwRegistrar g_hw_registrar;

}  // namespace

void DecoderFactory::register_backend(std::string_view name, Creator creator) {
    backends().push_back(Backend{std::string(name), std::move(creator)});
}

std::unique_ptr<IDecoder> DecoderFactory::create(const AVCodecParameters& params, bool prefer_hw) {
    // 硬解优先：尝试所有注册的硬件后端
    if (prefer_hw) {
        for (auto& backend : backends()) {
            auto decoder = backend.creator();
            if (!decoder->is_hardware()) continue;
            Error err = decoder->open(params);
            if (err.ok()) {
                ME_LOG_INFO("解码器选择: ", backend.name);
                return decoder;
            }
            ME_LOG_WARN("硬解后端 ", backend.name, " 打开失败: ", err.message(), "，尝试下一个");
        }
    }
    // 软解兜底：尝试所有注册的软件后端
    for (auto& backend : backends()) {
        auto decoder = backend.creator();
        if (decoder->is_hardware()) continue;
        Error err = decoder->open(params);
        if (err.ok()) {
            ME_LOG_INFO("解码器选择: ", backend.name);
            return decoder;
        }
        ME_LOG_WARN("软解后端 ", backend.name, " 打开失败: ", err.message());
    }
    return nullptr;
}

std::string DecoderFactory::available_backends() {
    std::string result;
    for (auto& backend : backends()) {
        if (!result.empty()) result += ", ";
        result += backend.name;
    }
    return result;
}

}  // namespace me
