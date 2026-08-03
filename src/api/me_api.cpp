#include "api/me_api.h"

#include <memory>
#include <string>
#include <vector>

#include <windows.h>

#include "player/media_player.h"
#include "api/renderer_plugin.h"
#include "api/audio_plugin.h"
#include "api/sync_plugin.h"
#include "render/headless_renderer.h"
#include "audio/null_audio_sink.h"
#include "core/clock.h"
#include "core/log.h"

namespace {
using RendererCreateFn = void* (*)(int, void*, int, int, char*, int);
using RendererDestroyFn = void (*)(void*);

struct RendererPluginApi {
    HMODULE module = nullptr;
    RendererCreateFn create = nullptr;
    RendererDestroyFn destroy = nullptr;
};

RendererPluginApi& renderer_plugin_api() {
    static RendererPluginApi api = [] {
        RendererPluginApi a;
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
            wchar_t* slash = wcsrchr(path, static_cast<wchar_t>(92));
            if (slash) *slash = 0;
            wcscat_s(path, L"\\plugin\\2\\renderer.dll");
            a.module = LoadLibraryW(path);
            if (a.module) {
                a.create = reinterpret_cast<RendererCreateFn>(
                    GetProcAddress(a.module, "me_renderer_create"));
                a.destroy = reinterpret_cast<RendererDestroyFn>(
                    GetProcAddress(a.module, "me_renderer_destroy"));
                if (!a.create || !a.destroy) {
                    FreeLibrary(a.module);
                    a.module = nullptr;
                }
            }
        }
        return a;
    }();
    return api;
}

void* renderer_plugin_create(int type, void* hwnd, int width, int height,
                               char* errbuf, int errbuf_size) {
    RendererPluginApi& api = renderer_plugin_api();
    if (!api.module || !api.create) return nullptr;
    return api.create(type, hwnd, width, height, errbuf, errbuf_size);
}

void renderer_plugin_destroy(void* renderer) {
    RendererPluginApi& api = renderer_plugin_api();
    if (api.module && api.destroy) api.destroy(renderer);
}

using AudioCreateFn = void* (*)(int, char*, int);
using AudioDestroyFn = void (*)(void*);

struct AudioPluginApi {
    HMODULE module = nullptr;
    AudioCreateFn create = nullptr;
    AudioDestroyFn destroy = nullptr;
};

AudioPluginApi& audio_plugin_api() {
    static AudioPluginApi api = [] {
        AudioPluginApi a;
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
            wchar_t* slash = wcsrchr(path, static_cast<wchar_t>(92));
            if (slash) *slash = 0;
            wcscat_s(path, L"\\plugin\\2\\audio.dll");
            a.module = LoadLibraryW(path);
            if (a.module) {
                a.create = reinterpret_cast<AudioCreateFn>(
                    GetProcAddress(a.module, "me_audio_create"));
                a.destroy = reinterpret_cast<AudioDestroyFn>(
                    GetProcAddress(a.module, "me_audio_destroy"));
                if (!a.create || !a.destroy) {
                    FreeLibrary(a.module);
                    a.module = nullptr;
                }
            }
        }
        return a;
    }();
    return api;
}

void* audio_plugin_create(int type, char* errbuf, int errbuf_size) {
    AudioPluginApi& api = audio_plugin_api();
    if (!api.module || !api.create) return nullptr;
    return api.create(type, errbuf, errbuf_size);
}

using SyncCreateFn = void* (*)(int, char*, int);
using SyncDestroyFn = void (*)(void*);

struct SyncPluginApi {
    HMODULE module = nullptr;
    SyncCreateFn create = nullptr;
    SyncDestroyFn destroy = nullptr;
};

SyncPluginApi& sync_plugin_api() {
    static SyncPluginApi api = [] {
        SyncPluginApi a;
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
            wchar_t* slash = wcsrchr(path, static_cast<wchar_t>(92));
            if (slash) *slash = 0;
            wcscat_s(path, L"\\plugin\\2\\sync.dll");
            a.module = LoadLibraryW(path);
            if (a.module) {
                a.create = reinterpret_cast<SyncCreateFn>(
                    GetProcAddress(a.module, "me_sync_create"));
                a.destroy = reinterpret_cast<SyncDestroyFn>(
                    GetProcAddress(a.module, "me_sync_destroy"));
                if (!a.create || !a.destroy) {
                    FreeLibrary(a.module);
                    a.module = nullptr;
                }
            }
        }
        return a;
    }();
    return api;
}

void* sync_plugin_create(int type, char* errbuf, int errbuf_size) {
    SyncPluginApi& api = sync_plugin_api();
    if (!api.module || !api.create) return nullptr;
    return api.create(type, errbuf, errbuf_size);
}
}  // namespace

// ME_Player 是 C API 的全局不透明类型：内部持有引擎对象
struct ME_Player {
    std::unique_ptr<me::IRenderer> renderer;
    bool renderer_from_plugin_ = false;
    me::MediaPlayer player;
    ME_PresentCallback present_cb = nullptr;
    void* present_user = nullptr;
    std::string last_error;
    std::string decoder_cache;
    std::string device_cache;
    std::vector<std::string> devices_cache;
};

ME_API ME_Player* me_create_player(void* hwnd, int width, int height) {
    return me_create_player_ex(hwnd, width, height, 0);
}

ME_API ME_Player* me_create_player_ex(void* hwnd, int width, int height, int flags) {
    ME_Player* p = new ME_Player();
    const bool headless = (flags & ME_PLAYER_FLAG_HEADLESS) != 0;
    const bool null_audio = (flags & ME_PLAYER_FLAG_NULL_AUDIO) != 0;

    if (headless) {
        p->renderer = std::make_unique<me::HeadlessRenderer>();
        me::Error err = p->renderer->init(static_cast<HWND>(hwnd), width, height);
        if (!err.ok()) p->last_error = err.message();
    } else {
        char errbuf[256] = {};
        void* r = renderer_plugin_create(ME_RENDERER_TYPE_D3D11, hwnd, width, height,
                                         errbuf, static_cast<int>(sizeof(errbuf)));
        if (!r) {
            p->last_error = errbuf[0]
                ? ("渲染器插件创建失败: " + std::string(errbuf))
                : "渲染器插件加载失败: plugin\\2\\renderer.dll";
        } else {
            p->renderer.reset(static_cast<me::IRenderer*>(r));
            p->renderer_from_plugin_ = true;
        }
    }
    if (null_audio) {
        p->player.set_audio_sink(std::make_unique<me::NullAudioSink>());
    } else {
        char errbuf[256] = {};
        void* s = audio_plugin_create(ME_AUDIO_TYPE_WASAPI, errbuf,
                                    static_cast<int>(sizeof(errbuf)));
        if (s) {
            p->player.set_audio_sink(
                std::unique_ptr<me::IAudioSink>(static_cast<me::IAudioSink*>(s)));
        } else {
            // 音频是软依赖：插件缺失时回退空输出桩（无声），播放与同步不受影响
            p->player.set_audio_sink(std::make_unique<me::NullAudioSink>());
            const std::string msg = errbuf[0]
                ? ("音频插件创建失败（已回退无声）: " + std::string(errbuf))
                : "音频插件加载失败（已回退无声）: plugin\\2\\audio.dll";
            p->last_error = msg;
            ME_LOG_WARN(msg);
        }
    }
    {
        char errbuf[256] = {};
        void* s = sync_plugin_create(ME_SYNC_TYPE_MASTERCLOCK, errbuf,
                                    static_cast<int>(sizeof(errbuf)));
        if (s) {
            p->player.set_sync_engine(
                std::unique_ptr<me::ISyncEngine>(static_cast<me::ISyncEngine*>(s)));
        } else {
            // 同步是硬依赖：缺失时引擎不可用（open 会失败）
            p->last_error = errbuf[0]
                ? ("同步引擎插件创建失败: " + std::string(errbuf))
                : "同步引擎插件加载失败: plugin\\2\\sync.dll";
            ME_LOG_ERROR(p->last_error);
        }
    }
    p->player.set_renderer(p->renderer.get());
    p->player.set_present_hook([p] {
        if (p->present_cb) p->present_cb(p->present_user);
        p->renderer->present_swapchain();
    });
    return p;
}

ME_API void me_destroy_player(ME_Player* player) {
    if (!player) return;
    player->player.close();
    if (player->renderer_from_plugin_) {
        renderer_plugin_destroy(player->renderer.release());
    } else if (player->renderer) {
        player->renderer->shutdown();
    }
    delete player;
}

ME_API void me_set_log_level(int level) {
    me::Log::set_level(static_cast<me::LogLevel>(level));
}

ME_API double me_now_seconds(void) {
    return me::qpc_seconds();
}

ME_API const char* me_last_error(ME_Player* player) {
    return player ? player->last_error.c_str() : "";
}


ME_API int me_open(ME_Player* player, const char* path_utf8, int prefer_hw) {
    if (!player) return -1;
    if (!player->renderer) {
        player->last_error = "渲染器不可用（插件加载失败）";
        return -1;
    }
    const me::Error err = player->player.open(path_utf8 ? path_utf8 : "", prefer_hw != 0);
    player->last_error = err.message();
    return err.ok() ? 0 : -1;
}

ME_API void me_close(ME_Player* player) {
    if (player) player->player.close();
}

ME_API void me_play(ME_Player* player) {
    if (player) player->player.play();
}

ME_API void me_pause(ME_Player* player) {
    if (player) player->player.pause();
}

ME_API void me_toggle_pause(ME_Player* player) {
    if (player) player->player.toggle_pause();
}

ME_API void me_seek(ME_Player* player, double seconds) {
    if (player) player->player.seek(seconds);
}

ME_API void me_set_speed(ME_Player* player, double speed) {
    if (player) player->player.set_speed(speed);
}

ME_API void me_set_volume(ME_Player* player, float volume) {
    if (player) player->player.set_volume(volume);
}

ME_API double me_position(ME_Player* player) {
    return player ? player->player.position() : 0.0;
}

ME_API double me_duration(ME_Player* player) {
    return player ? player->player.duration() : 0.0;
}

ME_API int me_has_video(ME_Player* player) {
    return player && player->player.has_video() ? 1 : 0;
}

ME_API int me_has_audio(ME_Player* player) {
    return player && player->player.has_audio() ? 1 : 0;
}

ME_API int me_is_paused(ME_Player* player) {
    return player && player->player.is_paused() ? 1 : 0;
}

ME_API int me_is_ended(ME_Player* player) {
    return player && player->player.is_ended() ? 1 : 0;
}

ME_API int me_hw_active(ME_Player* player) {
    return player && player->player.hw_active() ? 1 : 0;
}

ME_API int me_dropped_frames(ME_Player* player) {
    return player ? player->player.dropped_frames() : 0;
}

ME_API double me_speed(ME_Player* player) {
    return player ? player->player.speed() : 1.0;
}

ME_API float me_volume(ME_Player* player) {
    return player ? player->player.volume() : 1.0f;
}

ME_API const char* me_decoder_name(ME_Player* player) {
    if (!player) return "-";
    player->decoder_cache = player->player.decoder_name();
    return player->decoder_cache.c_str();
}

ME_API int me_audio_device_count(ME_Player* player) {
    if (!player) return 0;
    player->devices_cache = player->player.audio_devices();
    return static_cast<int>(player->devices_cache.size());
}

ME_API const char* me_audio_device_name(ME_Player* player) {
    if (!player) return "";
    player->device_cache = player->player.audio_device_name();
    return player->device_cache.c_str();
}

ME_API const char* me_audio_device_name_at(ME_Player* player, int index) {
    if (!player || index < 0 || index >= static_cast<int>(player->devices_cache.size())) return "";
    return player->devices_cache[static_cast<size_t>(index)].c_str();
}

ME_API int me_set_audio_device(ME_Player* player, int index) {
    if (!player) return -1;
    const me::Error err = player->player.set_audio_device(index);
    player->last_error = err.message();
    return err.ok() ? 0 : -1;
}

ME_API void me_set_present_callback(ME_Player* player, ME_PresentCallback cb, void* user) {
    if (!player) return;
    player->present_cb = cb;
    player->present_user = user;
}

ME_API void me_resize(ME_Player* player, int width, int height) {
    if (player) player->renderer->set_pending_size(width, height);
}

ME_API void* me_get_d3d11_device(ME_Player* player) {
    return player ? player->renderer->device() : nullptr;
}

ME_API void* me_get_d3d11_context(ME_Player* player) {
    return player ? player->renderer->context() : nullptr;
}
ME_API long long me_headless_draw_count(ME_Player* player) {
    if (!player) return -1;
    auto* r = dynamic_cast<me::HeadlessRenderer*>(player->renderer.get());
    return r ? static_cast<long long>(r->draw_count()) : -1;
}

ME_API long long me_headless_present_count(ME_Player* player) {
    if (!player) return -1;
    auto* r = dynamic_cast<me::HeadlessRenderer*>(player->renderer.get());
    return r ? static_cast<long long>(r->present_count()) : -1;
}
