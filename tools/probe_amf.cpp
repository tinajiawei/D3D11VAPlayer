#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
}

int main() {
    const char* names[] = {"h264_amf", "hevc_amf", "h264", "hevc"};
    for (const char* name : names) {
        const AVCodec* c = avcodec_find_decoder_by_name(name);
        std::printf("%s: %s\n", name, c ? "FOUND" : "missing");
        if (c) {
            for (int i = 0;; ++i) {
                const AVCodecHWConfig* cfg = avcodec_get_hw_config(c, i);
                if (!cfg) break;
                std::printf("  hwcfg[%d] dev=%d method=%d pix=%d\n", i,
                            static_cast<int>(cfg->device_type),
                            static_cast<int>(cfg->methods),
                            static_cast<int>(cfg->pix_fmt));
            }
        }
    }
    return 0;
}