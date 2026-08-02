#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "media/decoder.h"

namespace me {

// 解码器工厂（注册表模式，见 docs/04 第 5 节）：
// 新增一种解码后端 = 实现 IDecoder + 注册一行，播放器零改动。
class DecoderFactory {
public:
    using Creator = std::function<std::unique_ptr<IDecoder>()>;

    static void register_backend(std::string_view name, Creator creator);

    // 按注册顺序尝试打开；prefer_hw 时先试硬解，失败自动降级软解。
    static std::unique_ptr<IDecoder> create(const AVCodecParameters& params, bool prefer_hw);

    static std::string available_backends();
};

}  // namespace me