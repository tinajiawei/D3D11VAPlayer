#include <cstdio>
#include <string>
#include <thread>
#include <windows.h>

extern "C" {
#include <libavformat/avformat.h>
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

    // 与 app 相同：在子线程里 read
    std::thread t([fmt] {
        for (int n = 0; n < 3; ++n) {
            AVPacket* pkt = av_packet_alloc();
            const int r = av_read_frame(fmt, pkt);
            std::printf("thread read %d ret=%d size=%d stream=%d\n", n, r,
                        r >= 0 ? pkt->size : -1, r >= 0 ? pkt->stream_index : -1);
            av_packet_free(&pkt);
            if (r < 0) break;
        }
    });
    t.join();
    avformat_close_input(&fmt);
    return 0;
}