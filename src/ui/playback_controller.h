#pragma once

#include <string>
#include <vector>

#include "core/error.h"
#include "player/media_player.h"

namespace me {

// 播放控制接口（docs/00 模块职责地图）：
// 控制面板与未来壁纸窗口都只通过它操作播放器，UI 与引擎完全解耦。
class PlaybackController {
public:
    explicit PlaybackController(MediaPlayer* player) : player_(player) {}

    Error open(const std::string& path, bool prefer_hw) { return player_->open(path, prefer_hw); }
    void close() { player_->close(); }
    void play() { player_->play(); }
    void pause() { player_->pause(); }
    void toggle_pause() { player_->toggle_pause(); }
    void seek(double seconds) { player_->seek(seconds); }
    void set_speed(double speed) { player_->set_speed(speed); }
    void set_volume(float volume) { player_->set_volume(volume); }

    double position() const { return player_->position(); }
    double duration() const { return player_->duration(); }
    bool paused() const { return player_->is_paused(); }
    bool ended() const { return player_->is_ended(); }
    bool has_video() const { return player_->has_video(); }
    bool has_audio() const { return player_->has_audio(); }
    bool hw() const { return player_->hw_active(); }
    int dropped() const { return player_->dropped_frames(); }
    float volume() const { return player_->volume(); }
    double speed() const { return player_->speed(); }
    std::string decoder() const { return player_->decoder_name(); }
    std::string audio_device() const { return player_->audio_device_name(); }
    std::vector<std::string> audio_devices() const { return player_->audio_devices(); }
    Error set_audio_device(int index) { return player_->set_audio_device(index); }

private:
    MediaPlayer* player_;
};

}  // namespace me
