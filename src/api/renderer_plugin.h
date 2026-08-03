#pragma once

// 渲染器插件 ABI（docs/11 二期 M3）：
// 渲染后端从 media_engine.dll 拆出，由 plugin\2\renderer.dll 提供。
// 与解码器插件同一模式：extern "C" 工厂函数，跨 DLL 只传 C 类型/void*，
// 避免 C++ 类型与 STL 的 ABI 依赖。
#if defined(_WIN32)
#  if defined(ME_RENDERER_PLUGIN_BUILD)
#    define ME_RENDERER_API __declspec(dllexport)
#  else
#    define ME_RENDERER_API __declspec(dllimport)
#  endif
#else
#  define ME_RENDERER_API
#endif

// 当前 ABI 版本：改接口（新增虚函数/字段）时必须升版本目录 plugin\<ver>
#define ME_RENDERER_ABI_VERSION 2

// 渲染器类型
#define ME_RENDERER_TYPE_D3D11 0

#ifdef __cplusplus
extern "C" {
#endif

/* 返回 ME_RENDERER_ABI_VERSION，宿主据此校验插件兼容性 */
ME_RENDERER_API int me_renderer_abi_version(void);

/* 创建渲染器实例，返回 void*（实际为 me::IRenderer*）。
 * 失败返回 nullptr，错误信息写入 error_buf（长度 > 0 时）。 */
ME_RENDERER_API void* me_renderer_create(int type, void* hwnd, int width, int height,
                                         char* error_buf, int error_buf_size);

/* 销毁渲染器实例：必须由创建它的 DLL 释放，避免跨 DLL CRT 堆不一致 */
ME_RENDERER_API void me_renderer_destroy(void* renderer);

#ifdef __cplusplus
}
#endif