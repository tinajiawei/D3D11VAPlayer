#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

// 冒烟测试：验证 GCC 生成的 FFmpeg 导入库与 MSVC 链接兼容。
int main() {
    std::printf("FFmpeg 链接成功!\n");
    std::printf("  libavutil   %d.%d.%d\n", LIBAVUTIL_VERSION_MAJOR, LIBAVUTIL_VERSION_MINOR, LIBAVUTIL_VERSION_MICRO);
    std::printf("  libavcodec  %d.%d.%d\n", LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO);
    std::printf("  libavformat %d.%d.%d\n", LIBAVFORMAT_VERSION_MAJOR, LIBAVFORMAT_VERSION_MINOR, LIBAVFORMAT_VERSION_MICRO);
    std::printf("  avcodec_version() = %u\n", avcodec_version());
    std::printf("  avformat_version() = %u\n", avformat_version());
    return 0;
}
