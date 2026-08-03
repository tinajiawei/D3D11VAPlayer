#include "api/me_api.h"

#include <string>
#include <vector>

#include <windows.h>

#include "player/media_player.h"
#include "render/d3d11_renderer.h"
#include "core/clock.h"
#include "core/log.h"

// ME_Player 是 C API 的全局不透明类型：内部持有引擎对象
struct ME_Player {
    me::D3D11Renderer renderer;
    me::MediaPlayer player;
    ME_PresentCallback present_cb = nullptr;
    void* present_user = nullptr;
    std::string last_error;
    std::string decoder_cache;
    std::string device_cache;
    std::vector<std::string> devices_cache;
};

ME_API ME_Player* me_create_player(void* hwnd, int width, int height) {
    ME_Player* p = new ME_Player();
    me::Error err = p->renderer.init(static_cast<HWND>(hwnd), width, height);
    if (!err.ok()) {
        p->last_error = err.message();
    }
    p->player.set_renderer(&p->renderer);
    p->player.set_present_hook([p] {
        if (p->present_cb) p->present_cb(p->present_user);
        p->renderer.present_swapchain();
    });
    return p;
}

ME_API void me_destroy_player(ME_Player* player) {
    if (!player) return;
    player->player.close();
    player->renderer.shutdown();
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
    if (player) player->renderer.set_pending_size(width, height);
}

ME_API void* me_get_d3d11_device(ME_Player* player) {
    return player ? player->renderer.device() : nullptr;
}

ME_API void* me_get_d3d11_context(ME_Player* player) {
    return player ? player->renderer.context() : nullptr;
}
