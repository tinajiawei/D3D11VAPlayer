#include "web/webview_wallpaper.h"

#include <cstdio>
#include <string>

#include <objbase.h>
#include <ole2.h>
#include <wrl/client.h>
#include <wrl/event.h>

#include "webview2.h"

namespace me {

namespace {

// 找到桌面图标层（SHELLDLL_DefView）所在的 WorkerW：网页壁纸窗口挂到它下面
HWND find_worker_w() {
    const HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);
    }
    HWND workerw = nullptr;
    EnumWindows([](HWND top, LPARAM lp) -> BOOL {
        if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
            *reinterpret_cast<HWND*>(lp) = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&workerw));
    return workerw;
}

// 内置演示页：纯 CSS 动画（渐变 + 粒子 + 时钟），无网络依赖
const wchar_t* kDemoHtml = LR"html(
<!DOCTYPE html><html><head><meta charset="utf-8"><style>
html,body{margin:0;height:100%;overflow:hidden;background:#0b1020;font-family:'Microsoft YaHei';}
.sky{position:absolute;inset:0;background:linear-gradient(135deg,#0b1020,#1b2a5a,#3a1b5a,#0b1020);background-size:400% 400%;animation:flow 24s ease infinite;}
@keyframes flow{0%{background-position:0% 50%}50%{background-position:100% 50%}100%{background-position:0% 50%}}
.p{position:absolute;border-radius:50%;background:#7fd4ff;opacity:.7;animation:up linear infinite;}
@keyframes up{0%{transform:translateY(0);opacity:.7}100%{transform:translateY(-110vh);opacity:0}}
.clock{position:absolute;left:50%;top:45%;transform:translate(-50%,-50%);color:#e8f4ff;font-size:8vmin;text-align:center;text-shadow:0 0 24px #4fa8ff88;font-weight:300;letter-spacing:2px}
.clock .date{font-size:2.6vmin;opacity:.75;margin-top:1vmin}
</style></head><body>
<div class="sky"></div>
<script>
for(let i=0;i<28;i++){let p=document.createElement('div');p.className='p';
p.style.left=Math.random()*100+'vw';p.style.width=p.style.height=(3+Math.random()*7)+'px';
p.style.animationDuration=(6+Math.random()*10)+'s';p.style.animationDelay=(Math.random()*10)+'s';
document.body.appendChild(p);}
let c=document.createElement('div');c.className='clock';document.body.appendChild(c);
function tick(){const d=new Date();const pad=n=>String(n).padStart(2,'0');
c.innerHTML=pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds())+
'<div class="date">'+d.getFullYear()+'-'+pad(d.getMonth()+1)+'-'+pad(d.getDate())+'</div>';}
tick();setInterval(tick,1000);
</script></body></html>
)html";

}  // namespace

std::wstring WebViewWallpaper::user_data_folder() const {
    // 用户数据目录必须用纯 ASCII 路径：WebView2 浏览器进程在非 ASCII 路径下可能启动失败（E_ABORT）
    wchar_t buf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) > 0) {
        return std::wstring(buf) + L"\\MediaEngineWebView2";
    }
    return L"C:\\MediaEngineWebView2";
}

std::string WebViewWallpaper::url() const {
    std::lock_guard<std::mutex> lock(url_mutex_);
    return url_;
}

bool WebViewWallpaper::create(HWND workerw, const RECT& rc, const std::string& url) {
    if (active()) return true;
    if (!workerw) workerw = find_worker_w();
    if (!workerw) return false;

    {
        std::lock_guard<std::mutex> lock(url_mutex_);
        url_ = url;
    }
    thread_ = std::thread(&WebViewWallpaper::thread_main, this, workerw, rc, url);

    // 等待 STA 线程完成窗口/环境/控制器创建（最多 8 秒）
    for (int i = 0; i < 160 && !thread_ready_.load() && !thread_failed_.load(); ++i) {
        Sleep(50);
    }
    if (!thread_ready_.load()) {
        thread_failed_.store(true);
        if (thread_id_) PostThreadMessageW(thread_id_, WM_APP + 1, kOpClose, 0);
        if (thread_.joinable()) thread_.join();
        return false;
    }
    return true;
}

void WebViewWallpaper::destroy() {
    if (thread_id_ && thread_.joinable()) {
        PostThreadMessageW(thread_id_, WM_APP + 1, kOpClose, 0);
        thread_.join();
    }
    webview_ = nullptr;
    env_ = nullptr;
    controller_ = nullptr;
    thread_ready_.store(false);
    thread_failed_.store(false);
    thread_id_ = 0;
    hwnd_.store(nullptr);
}

void WebViewWallpaper::navigate(const std::string& url) {
    {
        std::lock_guard<std::mutex> lock(url_mutex_);
        url_ = url;
    }
    if (thread_id_) PostThreadMessageW(thread_id_, WM_APP + 1, kOpNavigate, 0);
}

void WebViewWallpaper::go_back() {
    if (thread_id_) PostThreadMessageW(thread_id_, WM_APP + 1, kOpBack, 0);
}

void WebViewWallpaper::go_forward() {
    if (thread_id_) PostThreadMessageW(thread_id_, WM_APP + 1, kOpForward, 0);
}

void WebViewWallpaper::reload() {
    if (thread_id_) PostThreadMessageW(thread_id_, WM_APP + 1, kOpReload, 0);
}

void WebViewWallpaper::thread_main(HWND workerw, RECT rc, const std::string& url) {
    // STA：WebView2 要求；主线程可能已被音频输出初始化为 MTA，不能共用
    const HRESULT ole_hr = OleInitialize(nullptr);
    if (FAILED(ole_hr) && ole_hr != RPC_E_CHANGED_MODE && ole_hr != S_FALSE) {
        std::fprintf(stderr, "[webview] OleInitialize 失败: 0x%08X\n", static_cast<unsigned>(ole_hr));
    }
    thread_id_ = GetCurrentThreadId();

    // 窗口与 WebView2 控制器必须在同一线程：窗口也在此创建
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WebViewWallpaper::wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    const HWND hwnd = CreateWindowExW(0, kClassName, L"MediaEngine WebView",
                                      WS_POPUP | WS_VISIBLE,
                                      rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                                      nullptr, nullptr, GetModuleHandleW(nullptr), this);
    hwnd_.store(hwnd);
    if (!hwnd) {
        thread_failed_.store(true);
        OleUninitialize();
        return;
    }

    const std::wstring folder = user_data_folder();
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, folder.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, done, hwnd, url](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result)) {
                    std::fprintf(stderr, "[webview] 环境创建失败: 0x%08X\n", static_cast<unsigned>(result));
                    thread_failed_.store(true);
                    SetEvent(done);
                    return result;
                }
                env_ = env;
                const HRESULT cr = env->CreateCoreWebView2Controller(
                    hwnd,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, done, url](HRESULT r2, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(r2)) {
                                std::fprintf(stderr, "[webview] 控制器创建失败: 0x%08X\n",
                                             static_cast<unsigned>(r2));
                                thread_failed_.store(true);
                                SetEvent(done);
                                return r2;
                            }
                            controller_ = controller;
                            if (SUCCEEDED(controller_->get_CoreWebView2(&webview_))) {
                                thread_ready_.store(true);
                                navigate_thread(url);
                                std::fprintf(stderr, "[webview] 网页壁纸已就绪\n");
                            }
                            SetEvent(done);
                            return S_OK;
                        }).Get());
                if (FAILED(cr)) {
                    thread_failed_.store(true);
                    SetEvent(done);
                    return cr;
                }
                return cr;
            }).Get());
    if (FAILED(hr)) {
        std::fprintf(stderr, "[webview] 环境创建调用失败: 0x%08X\n", static_cast<unsigned>(hr));
        thread_failed_.store(true);
        SetEvent(done);
    }

    // 边等待回调边泵消息：WebView2 回调需要消息泵派发，否则互相等死
    const DWORD deadline = GetTickCount() + 10000;
    bool created = false;
    while (!created && GetTickCount() < deadline) {
        const DWORD wr = MsgWaitForMultipleObjects(1, &done, FALSE, 200, QS_ALLINPUT);
        if (wr == WAIT_OBJECT_0) { created = true; break; }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (!created) {
        std::fprintf(stderr, "[webview] 环境/控制器创建超时\n");
        thread_failed_.store(true);
    }
    CloseHandle(done);

    // 就绪后挂到 WorkerW 并铺满（窗口在本线程，SetParent 安全）
    if (thread_ready_.load() && workerw) {
        SetParent(hwnd, workerw);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        resize_thread();
    }

    // 消息泵：处理导航/操作请求与 WebView2 内部消息
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_APP + 1) {
            switch (msg.wParam) {
                case kOpNavigate: {
                    std::lock_guard<std::mutex> lock(url_mutex_);
                    navigate_thread(url_);
                    break;
                }
                case kOpResize: resize_thread(); break;
                case kOpBack: if (webview_) webview_->GoBack(); break;
                case kOpForward: if (webview_) webview_->GoForward(); break;
                case kOpReload: if (webview_) webview_->Reload(); break;
                case kOpClose:
                    if (controller_) controller_->Close();
                    if (hwnd) DestroyWindow(hwnd);
                    hwnd_.store(nullptr);
                    PostQuitMessage(0);
                    break;
                default: break;
            }
        } else {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    OleUninitialize();
}

void WebViewWallpaper::navigate_thread(const std::string& url) {
    if (!webview_) return;
    if (url.empty()) {
        webview_->NavigateToString(kDemoHtml);
        return;
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    std::wstring wurl(static_cast<size_t>(len > 0 ? len - 1 : 0), L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wurl.data(), len);
    webview_->Navigate(wurl.c_str());
}

void WebViewWallpaper::resize_thread() {
    if (!controller_ || !hwnd_.load()) return;
    RECT rc = {};
    GetClientRect(GetParent(hwnd_.load()), &rc);
    controller_->put_Bounds(RECT{0, 0, rc.right - rc.left, rc.bottom - rc.top});
}

LRESULT CALLBACK WebViewWallpaper::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WebViewWallpaper* self = reinterpret_cast<WebViewWallpaper*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<WebViewWallpaper*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_.store(hwnd);
    }
    if (self) {
        if (msg == WM_SIZE && self->thread_id_) {
            PostThreadMessageW(self->thread_id_, WM_APP + 1, kOpResize, 0);
        }
        if (msg == WM_DESTROY) self->hwnd_.store(nullptr);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace me