#include <cstdio>
#include <string>
#include <thread>
#include <windows.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}

std::string utf8_from_wide(const wchar_t* w) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 1;
    const std::string path = utf8_from_wide(argv[1]);
    AVFormatContext* fmt = nullptr;
    int ret = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    std::printf("open ret=%d\n", ret);
    if (ret < 0) return 1;
    ret = avformat_find_stream_info(fmt, nullptr);
    std::printf("stream_info ret=%d streams=%d\n", ret, fmt->nb_streams);
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        const AVCodecParameters* p = st->codecpar;
        const char* fmt_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(p->format));
        std::printf("stream %u type=%d codec=%d pixfmt=%d(%s) w=%d h=%d profile=%d\n", i,
                    static_cast<int>(p->codec_type), static_cast<int>(p->codec_id),
                    static_cast<int>(p->format), fmt_name ? fmt_name : "?",
                    p->width, p->height, static_cast<int>(p->profile));
    }
    avformat_close_input(&fmt);
    return 0;
}