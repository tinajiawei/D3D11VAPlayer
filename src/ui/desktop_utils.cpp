#include "ui/desktop_utils.h"

#include <cstdio>
#include <vector>

namespace me {

namespace {

struct WorkerInfo {
    HWND hwnd = nullptr;
    bool visible = false;
    bool has_defview = false;
    RECT rect = {};
};

struct WorkerSearch {
    HWND classic = nullptr;   // Win10：含 SHELLDLL_DefView 的兄弟 WorkerW
    HWND win11 = nullptr;     // Win11：可见且覆盖主屏幕/工作区的 WorkerW
    std::vector<WorkerInfo> all;  // 诊断：所有 WorkerW
};

BOOL CALLBACK enum_worker_w(HWND top, LPARAM lp) {
    auto* search = reinterpret_cast<WorkerSearch*>(lp);
    wchar_t cls[64] = {};
    GetClassNameW(top, cls, 64);

    // 记录所有 WorkerW 供诊断
    if (wcscmp(cls, L"WorkerW") == 0) {
        WorkerInfo info;
        info.hwnd = top;
        info.visible = IsWindowVisible(top) != FALSE;
        info.has_defview = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr;
        GetWindowRect(top, &info.rect);
        search->all.push_back(info);
    }

    // 经典路径：该窗口含 SHELLDLL_DefView，它的兄弟 WorkerW 是壁纸层（Win10）
    if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
        const HWND w = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        if (w && IsWindowVisible(w) && !search->classic) {
            search->classic = w;
            return FALSE;
        }
    }

    // Win11 备选：可见 WorkerW 且覆盖主屏幕（含任务栏）或工作区
    if (wcscmp(cls, L"WorkerW") == 0 && IsWindowVisible(top)) {
        const int screen_w = GetSystemMetrics(SM_CXSCREEN);
        const int screen_h = GetSystemMetrics(SM_CYSCREEN);
        RECT rc = {};
        GetWindowRect(top, &rc);
        const bool covers_screen = rc.left <= 0 && rc.top <= 0 &&
                                   rc.right >= screen_w && rc.bottom >= screen_h;
        RECT wa = {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const bool covers_workarea = rc.left <= wa.left && rc.top <= wa.top &&
                                     rc.right >= wa.right && rc.bottom >= wa.bottom;
        if ((covers_screen || covers_workarea) && !search->win11) {
            search->win11 = top;
            return FALSE;
        }
    }
    return TRUE;
}

void dump_worker_windows(const WorkerSearch& search) {
    for (const auto& w : search.all) {
        std::fprintf(stderr,
                     "[workerw] hwnd=%p vis=%d defview=%d rect=%dx%d at(%d,%d)\n",
                     static_cast<void*>(w.hwnd), w.visible ? 1 : 0,
                     w.has_defview ? 1 : 0,
                     w.rect.right - w.rect.left, w.rect.bottom - w.rect.top,
                     w.rect.left, w.rect.top);
    }
}

}  // namespace

HWND find_desktop_workerw() {
    const HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);
    }

    WorkerSearch search = {};
    EnumWindows(&enum_worker_w, reinterpret_cast<LPARAM>(&search));
    dump_worker_windows(search);

    HWND result = search.classic ? search.classic : search.win11;
    if (!result && progman) {
        // Win11 某些版本没有可用的 WorkerW：回退挂到 Progman（经典壁纸另一分支）
        result = progman;
        std::fprintf(stderr, "[workerw] 无可用 WorkerW，回退 Progman %p\n", static_cast<void*>(progman));
    }
    if (result) {
        std::fprintf(stderr, "[workerw] 使用桌面层 %p\n", static_cast<void*>(result));
    }
    return result;
}

}  // namespace me