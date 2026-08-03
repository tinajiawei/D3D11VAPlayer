#pragma once

#include "media/decoder.h"

// 解码器插件导出宏：编译插件 DLL 时定义 ME_DECODER_PLUGIN_BUILD
#if defined(_WIN32)
#  if defined(ME_DECODER_PLUGIN_BUILD)
#    define ME_DECODER_API __declspec(dllexport)
#  else
#    define ME_DECODER_API __declspec(dllimport)
#  endif
#else
#  define ME_DECODER_API
#endif

namespace me {

// 插件工厂：返回未 open 的解码器实例（由宿主调用 IDecoder::open）
using DecoderCreateFn = IDecoder* (*)();

// 宿主传给插件的注册表（C 风格函数指针结构，避免跨 DLL 的 C++ 类型/STL ABI 依赖）
struct DecoderRegistry {
    void (*add)(const char* name, DecoderCreateFn factory);
};

}  // namespace me

// 每个解码器插件 DLL 必须导出此函数（extern "C"，符号名 me_register_decoders）
extern "C" ME_DECODER_API void me_register_decoders(me::DecoderRegistry* registry);