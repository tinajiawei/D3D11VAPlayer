#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>
#include <wrl/client.h>

#include "webview2.h"

#include "ui/desktop_utils.h"

namespace me {

// WebView2 网页壁纸（docs/11 二期 M4）：
// 用 WebView2 控件把网页渲染进一个子窗口，挂到桌面 WorkerW 下铺满屏幕。
// 线程模型：主线程可能已被音频输出初始化为 MTA，而 WebView2 要求 STA，
// 且控制器父窗口必须与创建线程一致——因此窗口、环境、控制器全部在专用 STA 线程创建。
class WebViewWallpaper {
public:
    bool create(HWND workerw, const RECT& rc, const std::string& url);
    void destroy();

    bool active() const { return hwnd_.load() != nullptr; }
    bool is_ready() const { return thread_ready_.load(); }
    HWND handle() const { return hwnd_.load(); }
    std::string url() const;

    // 主线程调用：请求经线程消息投递到 STA 线程执行
    void navigate(const std::string& url);
    void go_back();
    void go_forward();
    void reload();

private:
    enum WebOp { kOpNavigate = 1, kOpClose, kOpResize, kOpBack, kOpForward, kOpReload, kOpRemount };

    void thread_main(HWND workerw, RECT rc, const std::string& url);
    void navigate_thread(const std::string& url);  // 必须在 STA 线程内调用
    void resize_thread();                          // 必须在 STA 线程内调用
    void remount_thread();                          // 桌面层重建后重新挂载（STA 线程内）
    std::wstring user_data_folder() const;

    static constexpr const wchar_t* kClassName = L"MediaEngineWebViewWindow";
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    std::atomic<HWND> hwnd_{nullptr};
    HWND workerw_ = nullptr;  // 桌面层（仅 STA 线程使用）
    std::thread thread_;
    std::atomic<bool> thread_ready_{false};
    std::atomic<bool> thread_failed_{false};
    DWORD thread_id_ = 0;
    mutable std::mutex url_mutex_;
    std::string url_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    std::unique_ptr<me::DesktopLayerWatcher> wallpaper_watcher_;
};

}  // namespace me