// 桌面壁纸层诊断工具：打印 Win10 经典 / Win11 24H2 新模型的 WorkerW 结构。
// 用法：desktop_probe.exe [--watch 秒数]   --watch 可选，每 2 秒刷新观察 WorkerW 变化
#include "ui/desktop_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <windows.h>

int main(int argc, char** argv) {
    int watch_seconds = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--watch" && i + 1 < argc) watch_seconds = std::atoi(argv[++i]);
    }
    const int rounds = watch_seconds > 0 ? (watch_seconds + 1) / 2 : 1;

    int ok = 0;
    for (int r = 0; r < rounds; ++r) {
        std::printf("===== 桌面层探测 %d/%d =====\n", r + 1, rounds);
        const me::DesktopLayer layer = me::find_desktop_layer();
        std::printf("结果: workerw=%p defview=%p progman=%p raised=%d child=%d\n",
                    static_cast<void*>(layer.workerw), static_cast<void*>(layer.defview),
                    static_cast<void*>(layer.progman), layer.raised ? 1 : 0,
                    layer.child_workerw ? 1 : 0);
        if (layer.ok()) {
            ok = 1;
            RECT rc = {};
            GetWindowRect(layer.workerw, &rc);
            std::printf("目标 WorkerW 区域: %dx%d at(%d,%d)\n",
                        rc.right - rc.left, rc.bottom - rc.top, rc.left, rc.top);
        }
        if (r + 1 < rounds) Sleep(2000);
    }
    std::printf("exit=%d\n", ok);
    return ok ? 0 : 1;
}