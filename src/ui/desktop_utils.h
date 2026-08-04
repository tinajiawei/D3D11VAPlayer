#pragma once

#include <windows.h>

#include <functional>
#include <memory>

namespace me {

// 桌面壁纸层信息（Win10 经典 + Win11 24H2 新模型统一描述）：
// - 经典模型（Win7~Win10、Win11 23H2 及以前）：Progman 收到 0x052C 后分裂，
//   顶层 WorkerW 承载壁纸（含 SHELLDLL_DefView 的图标 WorkerW 的兄弟窗口）；
// - 新模型（Win11 24H2 及部分 23H2/Canary）：Progman 带 WS_EX_NOREDIRECTIONBITMAP，
//   SHELLDLL_DefView 与 WorkerW 都变成 Progman 的子窗口，必须枚举子窗口才能找到。
struct DesktopLayer {
    HWND workerw = nullptr;      // 壁纸承载层（挂载目标）
    HWND defview = nullptr;      // 桌面图标层（SHELLDLL_DefView）
    HWND progman = nullptr;      // Progman 根窗口
    bool raised = false;         // 新模型：Progman 带 WS_EX_NOREDIRECTIONBITMAP
    bool child_workerw = false;  // WorkerW 为 Progman 的子窗口（新模型）

    bool ok() const { return workerw != nullptr; }
};

// 立即查找桌面壁纸层；打印 [workerw] 诊断（所有候选 hwnd/parent/vis/defview/rect）。
DesktopLayer find_desktop_layer();

// 兼容旧调用：仅返回 WorkerW，找不到返回 nullptr。
HWND find_desktop_workerw();

// 轮询等待桌面层出现（24H2 的 WorkerW 可能延迟创建），默认最多等 3 秒。
DesktopLayer wait_desktop_layer(unsigned timeout_ms = 3000);

// 保证 WorkerW 在 SHELLDLL_DefView 之下（同父窗口时调整相对 Z 序）。
bool ensure_workerw_zorder(const DesktopLayer& layer);

// 监听 WorkerW/DefView 销毁与 Explorer 重建（TaskbarCreated），自动触发重新挂载回调。
// 回调在工作线程执行；如需操作窗口，请转发到窗口所属线程（如 PostMessage）。
class DesktopLayerWatcher {
public:
    using Callback = std::function<void()>;

    DesktopLayerWatcher();
    ~DesktopLayerWatcher();
    DesktopLayerWatcher(const DesktopLayerWatcher&) = delete;
    DesktopLayerWatcher& operator=(const DesktopLayerWatcher&) = delete;

    // 监听 layer.workerw / layer.defview 销毁；explorer 重启也会触发。
    bool start(const DesktopLayer& layer, Callback cb);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace me