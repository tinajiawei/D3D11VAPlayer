#pragma once

// 同步引擎插件 ABI（docs/11 二期 M3）：音画同步逻辑从 media_engine.dll 拆出，
// 由 plugin\2\sync.dll 提供。与渲染器/音频插件同一模式：extern "C" 工厂函数。
#if defined(_WIN32)
#  if defined(ME_SYNC_PLUGIN_BUILD)
#    define ME_SYNC_API __declspec(dllexport)
#  else
#    define ME_SYNC_API __declspec(dllimport)
#  endif
#else
#  define ME_SYNC_API
#endif

// 当前 ABI 版本：改接口（新增虚函数/字段）时必须升版本目录 plugin\<ver>
#define ME_SYNC_ABI_VERSION 2

// 同步引擎类型
#define ME_SYNC_TYPE_MASTERCLOCK 0

#ifdef __cplusplus
extern "C" {
#endif

/* 返回 ME_SYNC_ABI_VERSION，宿主据此校验插件兼容性 */
ME_SYNC_API int me_sync_abi_version(void);

/* 创建同步引擎实例，返回 void*（实际为 me::ISyncEngine*）。
 * 同步是硬依赖：宿主（MediaPlayer）无同步引擎时 open 直接失败。
 * 失败返回 nullptr，错误信息写入 error_buf（长度 > 0 时）。 */
ME_SYNC_API void* me_sync_create(int type, char* error_buf, int error_buf_size);

/* 显式销毁同步引擎实例（宿主也可走 ISyncEngine 虚析构默认释放） */
ME_SYNC_API void me_sync_destroy(void* sync);

#ifdef __cplusplus
}
#endif