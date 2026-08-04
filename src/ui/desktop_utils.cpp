#include "ui/desktop_utils.h"

#include <cstdio>

namespace me {

namespace {

struct WorkerSearch {
    HWND classic = nullptr;   // Win10：含 SHELLDLL_DefView 的兄弟 WorkerW
    HWND win11 = nullptr;     // Win11：可见且覆盖主工作区的 WorkerW
};

BOOL CALLBACK enum_worker_w(HWND top, LPARAM lp) {
    auto* search = reinterpret_cast<WorkerSearch*>(lp);

    // 经典路径：该窗口含 SHELLDLL_DefView，它的兄弟 WorkerW 是壁纸层（Win10）
    if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
        const HWND w = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        if (w && IsWindowVisible(w)) {
            search->classic = w;
            return FALSE;
        }
    }

    // Win11 备选：可见的 WorkerW 且覆盖主工作区（壁纸层）
    wchar_t cls[64] = {};
    GetClassNameW(top, cls, 64);
    if (wcscmp(cls, L"WorkerW") == 0 && IsWindowVisible(top)) {
        RECT rc = {};
        RECT wa = {};
        GetWindowRect(top, &rc);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const bool covers_workarea = rc.left <= wa.left && rc.top <= wa.top &&
                                     rc.right >= wa.right && rc.bottom >= wa.bottom;
        if (covers_workarea && !search->win11) {
            search->win11 = top;
            return FALSE;
        }
    }
    return TRUE;
}

}  // namespace

HWND find_desktop_workerw() {
    const HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);
    }

    WorkerSearch search = {};
    EnumWindows(&enum_worker_w, reinterpret_cast<LPARAM>(&search));

    HWND result = search.classic ? search.classic : search.win11;
    if (search.classic && search.win11 && search.classic != search.win11) {
        std::fprintf(stderr, "[workerw] Win10 层=%p Win11 层=%p，优先 Win10\n",
                     static_cast<void*>(search.classic), static_cast<void*>(search.win11));
    }
    if (result) {
        std::fprintf(stderr, "[workerw] 使用桌面层 %p\n", static_cast<void*>(result));
    } else {
        std::fprintf(stderr, "[workerw] 未找到 WorkerW（Win11 可能需额外处理）\n");
    }
    return result;
}

}  // namespace me