#include "api/audio_plugin.h"

#include <cstdio>

#include "audio/audio_output.h"

extern "C" {

ME_AUDIO_API int me_audio_abi_version(void) {
    return ME_AUDIO_ABI_VERSION;
}

ME_AUDIO_API void* me_audio_create(int type, char* error_buf, int error_buf_size) {
    if (type != ME_AUDIO_TYPE_WASAPI) {
        if (error_buf && error_buf_size > 0) {
            std::snprintf(error_buf, static_cast<size_t>(error_buf_size),
                          "audio type %d not supported by this plugin", type);
        }
        return nullptr;
    }
    // 返回未初始化实例：宿主（MediaPlayer）通过 IAudioSink 控制 init/start/stop
    return new me::AudioOutput();
}

ME_AUDIO_API void me_audio_destroy(void* sink) {
    if (!sink) return;
    auto* s = static_cast<me::AudioOutput*>(sink);
    s->shutdown();
    delete s;
}

}  // extern "C"