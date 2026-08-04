#include "ui/desktop_utils.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace me {

namespace {

constexpr UINT kProgmanSplitMsg = 0x052C;                // 触发 Progman 分裂出 WorkerW
constexpr LONG_PTR kNoRedirectionBitmap = 0x00200000L;   // WS_EX_NOREDIRECTIONBITMAP

struct WorkerInfo {
    HWND hwnd = nullptr;
    HWND parent = nullptr;
    bool visible = false;
    bool has_defview = false;
    RECT rect = {};
};

struct TopSearch {
    HWND defview = nullptr;     // 经典图标层里的 SHELLDLL_DefView
    HWND classic = nullptr;     // 经典壁纸层：图标 WorkerW 的兄弟 WorkerW
    HWND fullscreen = nullptr;  // Win11 备选：可见且覆盖主屏/工作区的顶层 WorkerW
    std::vector<WorkerInfo>* all = nullptr;
};

BOOL CALLBACK enum_top_level(HWND top, LPARAM lp) {
    auto* s = reinterpret_cast<TopSearch*>(lp);
    wchar_t cls[64] = {};
    GetClassNameW(top, cls, 64);
    if (wcscmp(cls, L"WorkerW") != 0) return TRUE;

    WorkerInfo info;
    info.hwnd = top;
    info.parent = GetParent(top);
    info.visible = IsWindowVisible(top) != FALSE;
    info.has_defview = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr;
    GetWindowRect(top, &info.rect);
    if (s->all) s->all->push_back(info);

    // 经典路径：该 WorkerW 含 DefView，它的兄弟 WorkerW 是壁纸层
    if (info.has_defview && !s->classic) {
        const HWND def = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
        if (def) s->defview = def;
        const HWND w = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        if (w && IsWindowVisible(w)) {
            s->classic = w;
            return FALSE;
        }
    }

    // Win11 备选：可见 WorkerW 且覆盖主屏幕（含任务栏）或工作区
    if (info.visible && !s->fullscreen) {
        const int screen_w = GetSystemMetrics(SM_CXSCREEN);
        const int screen_h = GetSystemMetrics(SM_CYSCREEN);
        const bool covers_screen = info.rect.left <= 0 && info.rect.top <= 0 &&
                                   info.rect.right >= screen_w && info.rect.bottom >= screen_h;
        RECT wa = {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const bool covers_workarea = info.rect.left <= wa.left && info.rect.top <= wa.top &&
                                     info.rect.right >= wa.right && info.rect.bottom >= wa.bottom;
        if (covers_screen || covers_workarea) {
            s->fullscreen = top;
        }
    }
    return TRUE;
}

struct ChildSearch {
    HWND defview = nullptr;  // Progman 子窗口中的 SHELLDLL_DefView（新模型）
    HWND workerw = nullptr;  // Progman 子窗口中的 WorkerW（新模型）
    std::vector<WorkerInfo>* all = nullptr;
};

BOOL CALLBACK enum_progman_child(HWND child, LPARAM lp) {
    auto* s = reinterpret_cast<ChildSearch*>(lp);
    wchar_t cls[64] = {};
    GetClassNameW(child, cls, 64);
    if (wcscmp(cls, L"SHELLDLL_DefView") == 0 && !s->defview) {
        s->defview = child;
    }
    if (wcscmp(cls, L"WorkerW") == 0) {
        if (s->all) {
            WorkerInfo info;
            info.hwnd = child;
            info.parent = GetParent(child);
            info.visible = IsWindowVisible(child) != FALSE;
            info.has_defview = FindWindowExW(child, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr;
            GetWindowRect(child, &info.rect);
            s->all->push_back(info);
        }
        if (!s->workerw) s->workerw = child;
    }
    return TRUE;
}

void dump_candidates(const std::vector<WorkerInfo>& all, HWND progman, bool raised) {
    std::fprintf(stderr, "[workerw] progman=%p raised=%d candidates=%zu\n",
                 static_cast<void*>(progman), raised ? 1 : 0, all.size());
    for (const auto& w : all) {
        std::fprintf(stderr,
                     "[workerw] hwnd=%p parent=%p vis=%d defview=%d rect=%dx%d at(%d,%d)\n",
                     static_cast<void*>(w.hwnd), static_cast<void*>(w.parent),
                     w.visible ? 1 : 0, w.has_defview ? 1 : 0,
                     w.rect.right - w.rect.left, w.rect.bottom - w.rect.top,
                     w.rect.left, w.rect.top);
    }
}

constexpr UINT kMsgWorkerwLost = WM_APP + 0x101;

}  // namespace

DesktopLayer find_desktop_layer() {
    DesktopLayer layer;
    const HWND progman = FindWindowW(L"Progman", nullptr);
    layer.progman = progman;
    if (!progman) {
        std::fprintf(stderr, "[workerw] 找不到 Progman\n");
        return layer;
    }

    // 触发分裂：Win10/23H2 经典路径创建顶层 WorkerW；24H2 创建 Progman 子 WorkerW
    SendMessageTimeoutW(progman, kProgmanSplitMsg, 0, 0, SMTO_NORMAL, 1000, nullptr);

    layer.raised = (GetWindowLongPtrW(progman, GWL_EXSTYLE) & kNoRedirectionBitmap) != 0;

    std::vector<WorkerInfo> all;
    TopSearch top;
    top.all = &all;
    EnumWindows(&enum_top_level, reinterpret_cast<LPARAM>(&top));

    ChildSearch child;
    child.all = &all;
    EnumChildWindows(progman, &enum_progman_child, reinterpret_cast<LPARAM>(&child));

    dump_candidates(all, progman, layer.raised);

    // 优先级：经典兄弟层 > 新模型 Progman 子 WorkerW > 可见全屏 WorkerW > Progman 回退
    if (top.classic) {
        layer.workerw = top.classic;
        layer.defview = top.defview;
        layer.child_workerw = false;
    } else if (child.workerw) {
        layer.workerw = child.workerw;
        layer.defview = child.defview;
        layer.child_workerw = true;
        // 新模型必须把 WorkerW 钉在 DefView 之下，否则壁纸会盖住图标
        ensure_workerw_zorder(layer);
    } else if (top.fullscreen) {
        layer.workerw = top.fullscreen;
        layer.defview = top.defview;
        layer.child_workerw = false;
    } else {
        layer.workerw = progman;  // 回退：直接挂 Progman
        layer.child_workerw = false;
        std::fprintf(stderr, "[workerw] 无可用 WorkerW，回退 Progman %p\n",
                     static_cast<void*>(progman));
    }

    if (layer.workerw) {
        std::fprintf(stderr, "[workerw] 使用桌面层 %p raised=%d child=%d defview=%p\n",
                     static_cast<void*>(layer.workerw), layer.raised ? 1 : 0,
                     layer.child_workerw ? 1 : 0, static_cast<void*>(layer.defview));
    }
    return layer;
}

HWND find_desktop_workerw() {
    return find_desktop_layer().workerw;
}

DesktopLayer wait_desktop_layer(unsigned timeout_ms) {
    const unsigned deadline = GetTickCount() + timeout_ms;
    DesktopLayer layer = find_desktop_layer();
    while (!layer.ok() && GetTickCount() < deadline) {
        Sleep(100);
        layer = find_desktop_layer();
    }
    return layer;
}

bool ensure_workerw_zorder(const DesktopLayer& layer) {
    if (!layer.workerw || !layer.defview) return false;
    // 只有同父窗口才需要调整相对 Z 序（经典模型：两个顶层 WorkerW；新模型：Progman 的两个子窗口）
    if (GetParent(layer.workerw) != GetParent(layer.defview)) return false;

    if (GetWindow(layer.workerw, GW_HWNDPREV) == layer.defview) return true;  // 已在下方
    SetWindowPos(layer.workerw, layer.defview, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    std::fprintf(stderr, "[workerw] 调整 Z 序: workerw=%p defview=%p\n",
                 static_cast<void*>(layer.workerw), static_cast<void*>(layer.defview));

    // 24H2 桌面切换会用一张快照盖在图标层上，需触发图标层重绘消除快照
    if (layer.child_workerw && layer.defview) {
        ShowWindow(layer.defview, SW_HIDE);
        Sleep(0);
        ShowWindow(layer.defview, SW_SHOWNORMAL);
    }
    return true;
}

// ---- DesktopLayerWatcher：监听 WorkerW/DefView 销毁与 Explorer 重建 ----

struct DesktopLayerWatcher::Impl {
    std::thread thread;
    std::mutex mtx;
    std::condition_variable cv;
    bool ready = false;
    std::atomic<bool> stop{false};
    std::atomic<HWND> msg_hwnd{nullptr};
    HWINEVENTHOOK hook = nullptr;
    UINT taskbar_msg = 0;
    HWND watch_workerw = nullptr;
    HWND watch_defview = nullptr;
    Callback cb;

    static thread_local Impl* current;
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void CALLBACK event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                    LONG id_object, LONG id_child,
                                    DWORD event_thread, DWORD event_time);
    void run();
};

thread_local DesktopLayerWatcher::Impl* DesktopLayerWatcher::Impl::current = nullptr;

DesktopLayerWatcher::DesktopLayerWatcher() : impl_(std::make_unique<Impl>()) {}

DesktopLayerWatcher::~DesktopLayerWatcher() { stop(); }

bool DesktopLayerWatcher::start(const DesktopLayer& layer, Callback cb) {
    if (impl_->thread.joinable()) return true;
    impl_->cb = std::move(cb);
    impl_->watch_workerw = layer.workerw;
    impl_->watch_defview = layer.defview;
    impl_->thread = std::thread(&Impl::run, impl_.get());
    std::unique_lock<std::mutex> lock(impl_->mtx);
    impl_->cv.wait(lock, [this] { return impl_->ready || impl_->stop.load(); });
    return impl_->ready;
}

void DesktopLayerWatcher::stop() {
    if (impl_->stop.exchange(true)) return;
    const HWND h = impl_->msg_hwnd.load();
    if (h) PostMessageW(h, WM_QUIT, 0, 0);
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->cb = nullptr;
    impl_->watch_workerw = nullptr;
    impl_->watch_defview = nullptr;
    impl_->msg_hwnd.store(nullptr);
    impl_->hook = nullptr;
}

void DesktopLayerWatcher::Impl::run() {
    current = this;
    // 必须用隐藏顶层窗口：HWND_MESSAGE 消息窗口收不到 TaskbarCreated 广播
    const HWND hwnd = CreateWindowExW(0, L"STATIC", L"me_DesktopLayerWatcher", WS_POPUP,
                                      0, 0, 0, 0, nullptr, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
    msg_hwnd.store(hwnd);
    if (!hwnd) {
        std::fprintf(stderr, "[workerw] 监听窗口创建失败\n");
        stop.store(true);
        {
            std::lock_guard<std::mutex> lock(mtx);
            ready = true;
        }
        cv.notify_all();
        return;
    }
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wnd_proc));
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    taskbar_msg = RegisterWindowMessageW(L"TaskbarCreated");
    hook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr,
                           &event_proc, 0, 0, WINEVENT_OUTOFCONTEXT);
    {
        std::lock_guard<std::mutex> lock(mtx);
        ready = true;
    }
    cv.notify_all();

    // stop() 可能先于本线程设置 msg_hwnd，此处补一次退出投递
    if (stop.load()) PostMessageW(hwnd, WM_QUIT, 0, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (hook) UnhookWinEvent(hook);
    DestroyWindow(hwnd);
}

LRESULT CALLBACK DesktopLayerWatcher::Impl::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == kMsgWorkerwLost) {
        if (!self->stop.load() && self->cb) self->cb();
        return 0;
    }
    if (self->taskbar_msg && msg == self->taskbar_msg) {
        // Explorer 重启：桌面层整体重建，触发重新挂载
        PostMessageW(hwnd, kMsgWorkerwLost, 0, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void CALLBACK DesktopLayerWatcher::Impl::event_proc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                                    LONG id_object, LONG, DWORD, DWORD) {
    if (event != EVENT_OBJECT_DESTROY || id_object != OBJID_WINDOW) return;
    auto* self = current;
    const HWND h = self ? self->msg_hwnd.load() : nullptr;
    if (!self || !h || self->stop.load()) return;
    if (hwnd == self->watch_workerw || hwnd == self->watch_defview) {
        PostMessageW(h, kMsgWorkerwLost, 0, 0);
    }
}

}  // namespace me