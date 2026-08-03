/* MediaEngine 引擎 C API（media_engine.dll）
 * 只导出窄 C 接口：UI / 壁纸宿主 / 控制台工具都通过它使用引擎，
 * C++ 内部（MediaPlayer/D3D11Renderer/…）完全封装在 DLL 内。
 * 约定：返回 const char* 的接口，指针在“下一次调用同对象任何接口”前有效，调用方应立即拷贝。
 */
#pragma once

#if defined(_WIN32)
#  if defined(ME_ENGINE_DLL_BUILD)
#    define ME_API __declspec(dllexport)
#  else
#    define ME_API __declspec(dllimport)
#  endif
#else
#  define ME_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ME_Player ME_Player;
typedef void (*ME_PresentCallback)(void* user);

/* 生命周期 */
ME_API ME_Player* me_create_player(void* hwnd, int width, int height);
/* 创建选项：ME_PLAYER_FLAG_HEADLESS = 无头渲染桩（HeadlessRenderer） */
/*           ME_PLAYER_FLAG_NULL_AUDIO = 空音频输出桩（NullAudioSink）    */
#define ME_PLAYER_FLAG_HEADLESS 1
#define ME_PLAYER_FLAG_NULL_AUDIO 2
ME_API ME_Player* me_create_player_ex(void* hwnd, int width, int height, int flags);
ME_API void me_destroy_player(ME_Player* player);
ME_API const char* me_last_error(ME_Player* player);  /* 最近一次失败原因 */
ME_API void me_set_log_level(int level);      /* 0=Debug 1=Info 2=Warn 3=Error */
ME_API double me_now_seconds(void);           /* 引擎单调时钟（QPC） */

/* 播放控制 */
ME_API int  me_open(ME_Player* player, const char* path_utf8, int prefer_hw);  /* 0=成功 */
ME_API void me_close(ME_Player* player);
ME_API void me_play(ME_Player* player);
ME_API void me_pause(ME_Player* player);
ME_API void me_toggle_pause(ME_Player* player);
ME_API void me_seek(ME_Player* player, double seconds);
ME_API void me_set_speed(ME_Player* player, double speed);
ME_API void me_set_volume(ME_Player* player, float volume);

/* 状态查询 */
ME_API double me_position(ME_Player* player);
ME_API double me_duration(ME_Player* player);
ME_API int me_has_video(ME_Player* player);
ME_API int me_has_audio(ME_Player* player);
ME_API int me_is_paused(ME_Player* player);
ME_API int me_is_ended(ME_Player* player);
ME_API int me_hw_active(ME_Player* player);
ME_API int me_dropped_frames(ME_Player* player);
ME_API double me_speed(ME_Player* player);
ME_API float me_volume(ME_Player* player);
ME_API const char* me_decoder_name(ME_Player* player);

/* 音频设备 */
ME_API int me_audio_device_count(ME_Player* player);
ME_API const char* me_audio_device_name(ME_Player* player);      /* 当前设备 */
ME_API const char* me_audio_device_name_at(ME_Player* player, int index);
ME_API int me_set_audio_device(ME_Player* player, int index);    /* 0=成功 */

/* 渲染集成 */
ME_API void me_set_present_callback(ME_Player* player, ME_PresentCallback cb, void* user);
ME_API void me_resize(ME_Player* player, int width, int height);
ME_API void* me_get_d3d11_device(ME_Player* player);   /* ID3D11Device*（供 ImGui 等外部渲染叠加） */
ME_API void* me_get_d3d11_context(ME_Player* player);  /* ID3D11DeviceContext* */

/* 无头模式统计（非无头返回 -1） */
ME_API long long me_headless_draw_count(ME_Player* player);
ME_API long long me_headless_present_count(ME_Player* player);

#ifdef __cplusplus
}
#endif
