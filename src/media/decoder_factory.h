#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "media/decoder.h"

namespace me {

// 解码器工厂 = 插件加载器（参考 RDCodec 的 plugin 目录模式，docs/04）。
// 启动后扫描 exe 旁 plugin\1\*.dll，LoadLibrary 并收集 me_register_decoders 注册的
// 后端；新增解码后端 = 新增一个插件 DLL，播放器零改动。
class DecoderFactory {
public:
    using Creator = IDecoder* (*)();  // 返回未 open 的实例

    static void register_backend(std::string_view name, Creator creator);

    // prefer_hw 时先尝试所有硬件后端，失败自动降级软解
    static std::unique_ptr<IDecoder> create(const AVCodecParameters& params, bool prefer_hw);

    static std::string available_backends();
};

}  // namespace me