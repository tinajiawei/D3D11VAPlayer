#pragma once

// 音频输出插件 ABI（docs/11 二期 M3）：WASAPI 后端从 media_engine.dll 拆出，
// 由 plugin\2\audio.dll 提供。与渲染器插件同一模式：extern "C" 工厂函数。
#if defined(_WIN32)
#  if defined(ME_AUDIO_PLUGIN_BUILD)
#    define ME_AUDIO_API __declspec(dllexport)
#  else
#    define ME_AUDIO_API __declspec(dllimport)
#  endif
#else
#  define ME_AUDIO_API
#endif

// 当前 ABI 版本：改接口（新增虚函数/字段）时必须升版本目录 plugin\<ver>
#define ME_AUDIO_ABI_VERSION 2

// 音频输出类型
#define ME_AUDIO_TYPE_WASAPI 0

#ifdef __cplusplus
extern "C" {
#endif

/* 返回 ME_AUDIO_ABI_VERSION，宿主据此校验插件兼容性 */
ME_AUDIO_API int me_audio_abi_version(void);

/* 创建"未初始化"的音频输出实例，返回 void*（实际为 me::IAudioSink*）。
 * 宿主随后通过 IAudioSink 接口调用 init/start/...（生命周期由宿主持有）；
 * 失败返回 nullptr，错误信息写入 error_buf（长度 > 0 时）。 */
ME_AUDIO_API void* me_audio_create(int type, char* error_buf, int error_buf_size);

/* 显式销毁音频输出实例（宿主也可走 IAudioSink 虚析构默认释放；
 * 此函数供需要"由创建方释放"的宿主使用）。 */
ME_AUDIO_API void me_audio_destroy(void* sink);

#ifdef __cplusplus
}
#endif