#pragma once

#include <atomic>
#include <string>

#include <windows.h>
#include <wrl/client.h>

#include "webview2.h"

namespace me {

// WebView2 网页壁纸（docs/11 二期 M4）：
// 用 WebView2 控件把网页渲染进一个子窗口，挂到桌面 WorkerW 下铺满屏幕。
// 初始化是异步的（CreateCoreWebView2Environment 回调），调用方轮询 is_ready()。
class WebViewWallpaper {
public:
    // 创建子窗口并挂到 workerw（铺满 rc）；url 为空时加载内置演示页
    bool create(HWND workerw, const RECT& rc, const std::string& url);
    void destroy();

    bool active() const { return hwnd_ != nullptr; }
    bool is_ready() const { return ready_.load(); }
    HWND handle() const { return hwnd_; }
    std::string url() const { return url_; }

    void navigate(const std::string& url);
    void go_back();
    void go_forward();
    void reload();

private:
    void resize_to_parent();
    std::wstring user_data_folder() const;

    static constexpr const wchar_t* kClassName = L"MediaEngineWebViewWindow";
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;  // WorkerW（控制器就绪后再挂入，避免跨进程父链导致 E_ABORT）
    std::atomic<bool> ready_{false};
    std::string url_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
};

}  // namespace me