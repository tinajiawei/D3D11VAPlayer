#pragma once

#include <string>
#include <vector>

#include "api/me_api.h"
#include "core/error.h"

namespace me {

// 播放控制接口（docs/00 模块职责地图）：
// 引擎已拆成 media_engine.dll，面板与未来壁纸窗口只通过本门面调用 C API（me_*）。
class PlaybackController {
public:
    explicit PlaybackController(ME_Player* player) : player_(player) {}
    void set_player(ME_Player* player) { player_ = player; }

    Error open(const std::string& path, bool prefer_hw) {
        if (!player_) return Error::make(Err::MediaOpenFailed, "引擎未创建");
        if (me_open(player_, path.c_str(), prefer_hw ? 1 : 0) == 0) return Error::success();
        const char* e = me_last_error(player_);
        return Error::make(Err::MediaOpenFailed, e && *e ? e : "打开失败");
    }
    void close() { if (player_) me_close(player_); }
    void play() { if (player_) me_play(player_); }
    void pause() { if (player_) me_pause(player_); }
    void toggle_pause() { if (player_) me_toggle_pause(player_); }
    void seek(double seconds) { if (player_) me_seek(player_, seconds); }
    void set_speed(double speed) { if (player_) me_set_speed(player_, speed); }
    void set_volume(float volume) { if (player_) me_set_volume(player_, volume); }

    double position() const { return player_ ? me_position(player_) : 0.0; }
    double duration() const { return player_ ? me_duration(player_) : 0.0; }
    bool paused() const { return player_ ? me_is_paused(player_) != 0 : false; }
    bool ended() const { return player_ ? me_is_ended(player_) != 0 : false; }
    bool has_video() const { return player_ ? me_has_video(player_) != 0 : false; }
    bool has_audio() const { return player_ ? me_has_audio(player_) != 0 : false; }
    bool hw() const { return player_ ? me_hw_active(player_) != 0 : false; }
    int dropped() const { return player_ ? me_dropped_frames(player_) : 0; }
    float volume() const { return player_ ? me_volume(player_) : 1.0f; }
    double speed() const { return player_ ? me_speed(player_) : 1.0; }
    std::string decoder() const {
        return player_ ? (me_decoder_name(player_) ? me_decoder_name(player_) : "-") : "-";
    }
    std::string audio_device() const {
        return player_ ? (me_audio_device_name(player_) ? me_audio_device_name(player_) : "") : "";
    }
    std::vector<std::string> audio_devices() const {
        std::vector<std::string> names;
        if (!player_) return names;
        const int n = me_audio_device_count(player_);
        names.reserve(static_cast<size_t>(n > 0 ? n : 0));
        for (int i = 0; i < n; ++i) names.push_back(me_audio_device_name_at(player_, i));
        return names;
    }
    Error set_audio_device(int index) {
        if (!player_) return Error::make(Err::AudioFailed, "引擎未创建");
        if (me_set_audio_device(player_, index) == 0) return Error::success();
        const char* e = me_last_error(player_);
        return Error::make(Err::AudioFailed, e && *e ? e : "切换失败");
    }

private:
    ME_Player* player_;
};

}  // namespace me