#include "web/webview_wallpaper.h"
#include "ui/desktop_utils.h"

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

bool looks_like_local_path(const std::string& s) {
    if (s.rfind("file://", 0) == 0) return true;
    if (s.size() >= 3 && s[1] == L':') return true;                  // X:\... 或 X:/...
    if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\') return true;  // UNC
    if (s.find('\\') != std::string::npos) return true;              // 含反斜杠的本地路径
    return false;
}

std::wstring wide_from_utf8(const std::string& s) {
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

// 本地路径 → file:/// URL：UTF-8 百分号编码非 ASCII 字符（UrlCreateFromPathW 对中文路径不可靠）
std::wstring make_file_url(const std::wstring& path) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8;
    if (len > 1) {
        utf8.resize(static_cast<size_t>(len - 1));
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), len, nullptr, nullptr);
    }
    const wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring url = L"file:///";
    for (const char ch : utf8) {
        if (ch == '\\') { url += L'/'; continue; }
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c > 0x7F || ch == '%' || ch == '#' || ch == '?' || ch == ' ') {
            url += L'%';
            url += hex[c >> 4];
            url += hex[c & 0xF];
        } else {
            url += static_cast<wchar_t>(ch);
        }
    }
    return url;
}

// 网页增强注入脚本：在页面脚本执行前定义 WE 兼容的音频注册接口 + 天气更新入口
const wchar_t* kEnhanceShimJs = LR"(
if (!window.__meAudioCallbacks) {
    window.__meAudioCallbacks = [];
    window.wallpaperRegisterAudioListener = function(cb) {
        if (typeof cb === 'function') window.__meAudioCallbacks.push(cb);
    };
    window.__meAudioPush = function(arr) {
        for (var i = 0; i < window.__meAudioCallbacks.length; ++i) {
            try { window.__meAudioCallbacks[i](arr); } catch (e) {}
        }
    };
}
// 页面脚本在 shim 注入前已执行：把页面暴露的全局监听器补注册进来
if (window.wallpaperAudioListener) window.wallpaperRegisterAudioListener(window.wallpaperAudioListener);
window.__meWeatherUpdate = function(p) {
    var el = document.getElementById('weather');
    if (!el) return;
    var a = String(p).split('|');
    el.style.display = 'block';
    el.innerHTML = a[0] + ' ' + a[1] + '℃ ' + a[2] + '  ' + a[3] + '~' + a[4] + '℃ 风' + a[5];
};
)";
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
    me::DesktopLayer layer;
    if (workerw) {
        layer.workerw = workerw;
    } else {
        layer = me::wait_desktop_layer();
        workerw = layer.workerw;
    }
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

    // 传入的 workerw 可能缺少 DefView 信息，补一次完整查找（新模型 Z 序需要）
    if (!layer.defview) {
        const me::DesktopLayer fresh = me::find_desktop_layer();
        if (fresh.defview) layer = fresh;
    }

    // 监听桌面层销毁/重建：WorkerW 被销毁或 Explorer 重启时自动重新挂载
    wallpaper_watcher_ = std::make_unique<me::DesktopLayerWatcher>();
    wallpaper_watcher_->start(layer, [this] {
        if (thread_id_) PostThreadMessageW(thread_id_, WM_APP + 1, kOpRemount, 0);
    });

    // 实验增强：音频可视化 / 天气（默认关闭，面板或 --web-enhance 开启）
    enhance_ = std::make_unique<me::WallpaperEnhance>();
    enhance_->set_webview_thread(thread_id_);
    if (audio_vis_enabled_.load()) enhance_->set_audio_enabled(true);
    if (weather_enabled_.load()) {
        std::string city;
        {
            std::lock_guard<std::mutex> lock(weather_city_mutex_);
            city = weather_city_;
        }
        enhance_->set_weather_enabled(true, city.empty() ? "北京" : city);
    }
    return true;
}

void WebViewWallpaper::destroy() {
    if (enhance_) {
        enhance_->set_audio_enabled(false);
        enhance_->set_weather_enabled(false, {});
        enhance_.reset();
    }
    if (wallpaper_watcher_) { wallpaper_watcher_->stop(); wallpaper_watcher_.reset(); }
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

void WebViewWallpaper::set_audio_visualization(bool on) {
    audio_vis_enabled_.store(on);
    if (enhance_) enhance_->set_audio_enabled(on);
}

void WebViewWallpaper::set_weather(bool on, const std::string& city) {
    weather_enabled_.store(on);
    {
        std::lock_guard<std::mutex> lock(weather_city_mutex_);
        weather_city_ = city;
    }
    if (enhance_) enhance_->set_weather_enabled(on, city.empty() ? "北京" : city);
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
    Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions> env_options;
    // 该 SDK 头文件未导出 CreateCoreWebView2EnvironmentOptions 辅助函数，用文档公开的 CLSID 创建
    // {2FDE08A8-8881-4BB5-A72B-8C90DC2D9D35}
    static const CLSID kClsidWebView2EnvOptions = {0x2fde08a8, 0x8881, 0x4bb5,
                                                      {0xa7, 0x2b, 0x8c, 0x90, 0xdc, 0x2d, 0x9d, 0x35}};
    CoCreateInstance(kClsidWebView2EnvOptions, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&env_options));
    if (env_options) {
        // 壁纸页需要无手势自动播放音频；本地页可能需要 file->file 访问
        env_options->put_AdditionalBrowserArguments(L"--autoplay-policy=no-user-gesture-required --allow-file-access-from-files");
    }
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, folder.c_str(), env_options.Get(),
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
                                EventRegistrationToken nav_token = {};
                                webview_->add_NavigationCompleted(
                                    Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                        [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                            BOOL ok = FALSE;
                                            COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                            args->get_IsSuccess(&ok);
                                            args->get_WebErrorStatus(&status);
                                            std::fprintf(stderr, "[webview] 导航完成 success=%d err=%d\\n", ok ? 1 : 0, static_cast<int>(status));
                                            // 旧版 SDK 没有 AddScriptToExecuteOnDocumentCreatedAsync：
                                            // 导航成功后注入 WE 兼容 shim 并补注册页面全局音频监听器
                                            if (ok && webview_) webview_->ExecuteScript(kEnhanceShimJs, nullptr);
                                            return S_OK;
                                        }).Get(), &nav_token);
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
        // 尽量取完整桌面层信息（新模型需要 DefView 做 Z 序）
        me::DesktopLayer layer = me::find_desktop_layer();
        if (!layer.ok()) layer.workerw = workerw;
        workerw_ = layer.workerw;
        me::ensure_workerw_zorder(layer);
        me::refresh_desktop_icons(layer);
        if (controller_) {
            // WebView2 不允许 SetParent 宿主窗口：用官方 put_ParentWindow 把内容挂到桌面层
            const HRESULT pr = controller_->put_ParentWindow(layer.workerw);
            RECT wrc = {};
            GetClientRect(layer.workerw, &wrc);
            controller_->put_Bounds(RECT{0, 0, wrc.right - wrc.left, wrc.bottom - wrc.top});
            std::fprintf(stderr, "[webview] 已挂载到 WorkerW: hr=0x%08X size=%dx%d\n",
                        static_cast<unsigned>(pr), wrc.right - wrc.left, wrc.bottom - wrc.top);
        }
        ShowWindow(hwnd, SW_HIDE);  // 空容器隐藏，内容窗口已挂到桌面层
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
                case kOpRemount: remount_thread(); break;
                case kOpSpectrum: push_audio_spectrum(); break;
                case kOpWeather: push_weather(); break;
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

    // 本地网页壁纸：文件夹 / index.html 路径 / file:// URL
    if (looks_like_local_path(url)) {
        const std::wstring wpath = wide_from_utf8(url);
        if (url.rfind("file://", 0) != 0) {
            // 用虚拟主机映射加载本地文件夹（官方推荐，绕开 file:// 限制，中文路径也可靠）：
            // 目录 → 映射整个目录；文件 → 映射其父目录，再导航到文件名
            std::wstring folder = wpath;
            std::wstring entry = L"index.html";
            const DWORD attr = GetFileAttributesW(wpath.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                if (GetFileAttributesW((wpath + L"\\index.html").c_str()) == INVALID_FILE_ATTRIBUTES &&
                    GetFileAttributesW((wpath + L"\\index.htm").c_str()) != INVALID_FILE_ATTRIBUTES) {
                    entry = L"index.htm";
                }
            } else {
                const size_t slash = wpath.find_last_of(L"\\/");
                if (slash != std::wstring::npos) {
                    entry = wpath.substr(slash + 1);
                    folder = wpath.substr(0, slash);
                } else {
                    folder = L".";
                }
            }
            Microsoft::WRL::ComPtr<ICoreWebView2_3> wv3;
            if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&wv3)))) {
                const HRESULT hr = wv3->SetVirtualHostNameToFolderMapping(
                    L"me-local-wallpaper", folder.c_str(),
                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                if (SUCCEEDED(hr)) {
                    const std::wstring nav = L"https://me-local-wallpaper/" + entry;
                    std::fprintf(stderr, "[webview] 本地网页壁纸: %ls -> %ls\n",
                                 folder.c_str(), nav.c_str());
                    webview_->Navigate(nav.c_str());
                    return;
                }
            }
            // 映射失败回退 file://（中文路径可能不可靠，但保底可用）
            std::wstring full = wpath;
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                full += L"\\" + entry;
            }
            const std::wstring file_url = make_file_url(full);
            std::fprintf(stderr, "[webview] 本地网页壁纸(回退file): %ls\n", file_url.c_str());
            webview_->Navigate(file_url.c_str());
            return;
        }
        std::fprintf(stderr, "[webview] 本地网页壁纸: %ls\n", wpath.c_str());
        webview_->Navigate(wpath.c_str());
        return;
    }

    // 无协议前缀时自动补全 https://（用户常输入 www.baidu.com）
    std::string fixed = url;
    if (fixed.find("://") == std::string::npos &&
        fixed.compare(0, 6, "about:") != 0 &&
        fixed.compare(0, 5, "data:") != 0 &&
        fixed.compare(0, 7, "file://") != 0) {
        fixed = "https://" + fixed;
    }
    std::wstring wurl = wide_from_utf8(fixed);
    webview_->Navigate(wurl.c_str());
}

void WebViewWallpaper::push_audio_spectrum() {
    if (!webview_) return;
    const std::vector<float> spec = enhance_ ? enhance_->take_spectrum() : std::vector<float>{};
    if (spec.empty()) return;
    std::string js = "window.__meAudioPush&&window.__meAudioPush([";
    for (size_t i = 0; i < spec.size(); ++i) {
        char buf[24] = {};
        std::snprintf(buf, sizeof(buf), "%g", spec[i]);
        js += buf;
        if (i + 1 < spec.size()) js += ',';
    }
    js += "]);";
    webview_->ExecuteScript(wide_from_utf8(js).c_str(), nullptr);
}

void WebViewWallpaper::push_weather() {
    if (!webview_ || !enhance_) return;
    const std::string payload = enhance_->take_weather_payload();
    if (payload.empty()) return;
    const std::string js = "window.__meWeatherUpdate&&window.__meWeatherUpdate('" + payload + "');";
    webview_->ExecuteScript(wide_from_utf8(js).c_str(), nullptr);
}
void WebViewWallpaper::remount_thread() {
    if (!controller_) return;
    const me::DesktopLayer layer = me::wait_desktop_layer(1500);
    if (!layer.ok()) return;
    workerw_ = layer.workerw;
    me::ensure_workerw_zorder(layer);
    me::refresh_desktop_icons(layer);
    const HRESULT pr = controller_->put_ParentWindow(layer.workerw);
    RECT wrc = {};
    GetClientRect(layer.workerw, &wrc);
    controller_->put_Bounds(RECT{0, 0, wrc.right - wrc.left, wrc.bottom - wrc.top});
    std::fprintf(stderr, "[webview] 桌面层重建后重新挂载: hr=0x%08X size=%dx%d\n",
                 static_cast<unsigned>(pr), wrc.right - wrc.left, wrc.bottom - wrc.top);
}

void WebViewWallpaper::resize_thread() {
    if (!controller_) return;
    const HWND target = workerw_ ? workerw_ : hwnd_.load();
    if (!target) return;
    RECT rc = {};
    GetClientRect(target, &rc);
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