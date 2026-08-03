#include "web/webview_wallpaper.h"

#include <cstdio>
#include <string>

#include <objbase.h>
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

bool WebViewWallpaper::create(HWND workerw, const RECT& rc, const std::string& url) {
    if (hwnd_) return true;
    if (!workerw) workerw = find_worker_w();
    if (!workerw) return false;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // 已初始化则忽略（MTA 也可用）

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WebViewWallpaper::wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    parent_ = workerw;
    // 先以顶层窗口创建控制器（跨进程父链会让 WebView2 返回 E_ABORT），就绪后再挂到 WorkerW
    hwnd_ = CreateWindowExW(0, kClassName, L"MediaEngine WebView",
                            WS_POPUP | WS_VISIBLE,
                            rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                            nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    url_ = url;
    const std::wstring folder = user_data_folder();
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, folder.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result)) {
                    std::fprintf(stderr, "[webview] 环境创建失败: 0x%08X\n", static_cast<unsigned>(result));
                    return result;
                }
                env_ = env;
                return env->CreateCoreWebView2Controller(
                    hwnd_,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT r2, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(r2)) {
                                std::fprintf(stderr, "[webview] 控制器创建失败: 0x%08X\n", static_cast<unsigned>(r2));
                                return r2;
                            }
                            controller_ = controller;
                            if (SUCCEEDED(controller_->get_CoreWebView2(&webview_))) {
                                // 就绪后挂入 WorkerW 并铺满
                                if (parent_) {
                                    SetParent(hwnd_, parent_);
                                    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                                                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                                }
                                ready_.store(true);
                                resize_to_parent();
                                navigate(url_);
                                std::fprintf(stderr, "[webview] 网页壁纸已就绪: %s\n", url_.c_str());
                            }
                            return S_OK;
                        }).Get());
            }).Get());
    if (FAILED(hr)) {
        std::fprintf(stderr, "[webview] 初始化调用失败: 0x%08X\n", static_cast<unsigned>(hr));
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    return true;
}

void WebViewWallpaper::destroy() {
    if (controller_) {
        controller_->Close();
        controller_ = nullptr;
    }
    webview_ = nullptr;
    env_ = nullptr;
    ready_.store(false);
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void WebViewWallpaper::navigate(const std::string& url) {
    url_ = url;
    if (!ready_ || !webview_) return;
    if (url_.empty()) {
        webview_->NavigateToString(kDemoHtml);
    } else {
        const int len = MultiByteToWideChar(CP_UTF8, 0, url_.c_str(), -1, nullptr, 0);
        std::wstring wurl(static_cast<size_t>(len > 0 ? len - 1 : 0), L'\0');
        if (len > 0) MultiByteToWideChar(CP_UTF8, 0, url_.c_str(), -1, wurl.data(), len);
        webview_->Navigate(wurl.c_str());
    }
}

void WebViewWallpaper::go_back() { if (ready_ && webview_) webview_->GoBack(); }
void WebViewWallpaper::go_forward() { if (ready_ && webview_) webview_->GoForward(); }
void WebViewWallpaper::reload() { if (ready_ && webview_) webview_->Reload(); }

void WebViewWallpaper::resize_to_parent() {
    if (!controller_ || !hwnd_) return;
    RECT rc = {};
    GetClientRect(GetParent(hwnd_), &rc);
    controller_->put_Bounds(RECT{0, 0, rc.right - rc.left, rc.bottom - rc.top});
}

LRESULT CALLBACK WebViewWallpaper::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WebViewWallpaper* self = reinterpret_cast<WebViewWallpaper*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<WebViewWallpaper*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (self) {
        if (msg == WM_SIZE && self->controller_) self->resize_to_parent();
        if (msg == WM_DESTROY) self->hwnd_ = nullptr;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace me