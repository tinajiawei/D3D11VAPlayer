#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <mutex>
#include <thread>

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <dbghelp.h>
#include <psapi.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "ui/control_panel.h"
#include "ui/floating_panel.h"
#include "ui/tray_icon.h"
#include "web/webview_wallpaper.h"
#include "capture/capture_preview.h"
#include "capture/loopback_capture.h"
#include "ui/host_window.h"
#include "ui/media_sequence.h"
#include "ui/playback_controller.h"

namespace {

me::HostWindow g_window;
me::ControlPanel g_panel;
me::FloatingPanel g_floating_panel;
me::TrayIcon g_tray;
me::WebViewWallpaper g_web_wallpaper;
std::mutex g_request_mutex;
bool g_web_wallpaper_toggle_request = false;
std::string g_web_wallpaper_url;
me::CapturePreview g_capture_preview;
me::PlaybackController g_controller(nullptr);
bool g_show_panel = true;
std::atomic<bool> g_open_requested{false};
std::atomic<bool> g_open_prefer_hw{false};
std::atomic<bool> g_wallpaper_requested{false};
me::MediaSequence g_sequence;
std::atomic<int> g_sequence_type{0};           // SequenceType
std::atomic<bool> g_sequence_auto_next{false};
std::mutex g_current_mutex;
std::wstring g_current_media;
std::atomic<bool> g_seq_prev_requested{false};
std::atomic<bool> g_seq_next_requested{false};
std::atomic<bool> g_seq_auto_requested{false};
std::atomic<int> g_seq_type_requested{-1};
std::atomic<int> g_seq_auto_next_change{-1};
std::atomic<bool> g_seek_requested{false};
std::atomic<double> g_seek_target{0.0};
std::atomic<bool> g_web_pick_folder_requested{false};
std::atomic<int> g_web_audio_vis_state{1};   // 默认开（网页用到音频可视化接口才生效）
std::atomic<int> g_web_weather_state{1};   // 天气默认开，城市留空=IP 定位
std::atomic<int> g_web_bg_state{1};
std::atomic<int> g_web_sakura_state{1};
std::atomic<int> g_web_vis_state{1};
std::mutex g_web_city_mutex;
std::string g_web_city;
bool g_web_active_prev = false;
bool g_web_enhance_cli = false;
bool g_headless_cli = false;
bool g_wallpaper_keep = false;   // --headless 无头模式（HeadlessRenderer + NullAudioSink）
double g_run_seconds = 8.0;    // --run-seconds 无头运行结束时间
ME_Player* g_engine = nullptr;  // media_engine.dll 引擎实例

std::string utf8_from_wide(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), len, nullptr, nullptr);
    }
    return out;
}

void open_media(const std::wstring& path, bool prefer_hw) {
    me::Error err = g_controller.open(utf8_from_wide(path), prefer_hw);
    if (!err.ok()) {
        std::fprintf(stderr, "打开失败: %s\n", err.message().c_str());
        g_panel.set_open_error(err.message());
    } else {
        g_panel.set_open_error({});
        {
            std::lock_guard<std::mutex> lock(g_current_mutex);
            g_current_media = path;
        }
        g_sequence.rebuild(path, static_cast<me::SequenceType>(g_sequence_type.load()));
    }
}

// 回归测试入口：--reopen <秒> 在 N 秒后重开同一个文件（复现"播放中拖入新视频"）
void maybe_schedule_reopen(const std::wstring& path, bool prefer_hw, double after_seconds) {
    if (after_seconds <= 0.0 || path.empty()) return;
    std::thread([path, prefer_hw, after_seconds] {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int>(after_seconds * 1000.0)));
        std::fprintf(stderr, "[reopen] 触发重开: %ls\n", path.c_str());
        open_media(path, prefer_hw);
    }).detach();
}

void toggle_wallpaper() {
    // 与网页壁纸互斥：先退出网页壁纸
    if (g_web_wallpaper.active()) {
        g_web_wallpaper.destroy();
        ShowWindow(g_window.handle(), SW_SHOW);
        std::fprintf(stderr, "[webwallpaper] 退出网页壁纸（切视频壁纸）\n");
    }
    if (g_window.wallpaper_mode()) {
        g_floating_panel.request_destroy();
        g_window.exit_wallpaper_mode();
        g_tray.set_wallpaper_mode(false);
        std::fprintf(stderr, "[wallpaper] 退出壁纸模式\n");
    } else if (g_window.enter_wallpaper_mode()) {
        if (g_engine) {
            auto* dev = static_cast<ID3D11Device*>(me_get_d3d11_device(g_engine));
            auto* ctx = static_cast<ID3D11DeviceContext*>(me_get_d3d11_context(g_engine));
            g_floating_panel.request_create(GetModuleHandleW(nullptr), dev, ctx);
        }
        std::fprintf(stderr, "[wallpaper] 进入壁纸模式（Ctrl+Alt+W 退出）\n");
        g_tray.set_wallpaper_mode(true);
    } else {
        std::fprintf(stderr, "[wallpaper] 进入壁纸模式失败（找不到 WorkerW）\n");
    }
}

// 引擎渲染线程每帧回调：叠加 ImGui 控制面板（Present 由引擎在回调后执行）
void toggle_web_wallpaper(const std::string& url) {
    if (g_web_wallpaper.active()) {
        g_floating_panel.request_destroy();
        g_web_wallpaper.destroy();
        ShowWindow(g_window.handle(), SW_SHOW);  // 恢复主窗口
        std::fprintf(stderr, "[webwallpaper] 退出网页壁纸\n");
        return;
    }
    // 与视频壁纸互斥：先退出视频壁纸模式（含浮层控制面板）
    if (g_window.wallpaper_mode()) {
        g_floating_panel.request_destroy();
        g_window.exit_wallpaper_mode();
        g_tray.set_wallpaper_mode(false);
    }
    RECT rc = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
    if (g_web_wallpaper.create(nullptr, rc, url)) {
        ShowWindow(g_window.handle(), SW_HIDE);  // 隐藏主窗口，网页壁纸铺满可见
        // 网页壁纸同样显示浮层控制面板（背景/效果/天气/音频都在面板上调节）
        if (g_engine) {
            auto* dev = static_cast<ID3D11Device*>(me_get_d3d11_device(g_engine));
            auto* ctx = static_cast<ID3D11DeviceContext*>(me_get_d3d11_context(g_engine));
            g_floating_panel.request_create(GetModuleHandleW(nullptr), dev, ctx);
        }
        std::fprintf(stderr, "[webwallpaper] 进入网页壁纸\n");
    } else {
        std::fprintf(stderr, "[webwallpaper] 创建失败（找不到 WorkerW）\n");
    }
}

void present_callback(void*) {
    if (!ImGui::GetCurrentContext()) return;  // 引擎空转渲染线程可能先于 ImGui 初始化启动
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(static_cast<float>(g_window.mouse_x()),
                         static_cast<float>(g_window.mouse_y()));
    io.AddMouseButtonEvent(0, g_window.left_button_down());
    io.AddMouseWheelEvent(0.0f, static_cast<float>(g_window.take_mouse_wheel()));

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    me::PanelRequest requests;
    const bool wallpaper_mode = g_window.wallpaper_mode();
    const me::SequenceInfo seq = g_sequence.snapshot(
        static_cast<me::SequenceType>(g_sequence_type.load()), g_sequence_auto_next.load());
    const bool web_wallpaper_active = g_web_wallpaper.active();
    if (g_floating_panel.active() || g_floating_panel.pending()) {
        g_floating_panel.render(g_panel, g_controller, requests, &g_show_panel, wallpaper_mode,
                                 web_wallpaper_active, seq);
    } else {
        g_panel.draw(g_controller, requests, &g_show_panel, wallpaper_mode, web_wallpaper_active, seq);
    }
    g_open_prefer_hw.store(requests.prefer_hw);  // 勾选/取消后立即生效：拖入新文件也用这个值
    if (requests.wallpaper_toggle) g_wallpaper_requested.store(true);
    if (requests.open_file) {
        g_open_requested.store(true);
    }
    if (requests.sequence_prev) g_seq_prev_requested.store(true);
    if (requests.sequence_next) g_seq_next_requested.store(true);
    if (requests.sequence_type >= 0) g_seq_type_requested.store(requests.sequence_type);
    if (requests.sequence_auto_next >= 0) g_seq_auto_next_change.store(requests.sequence_auto_next);
    if (requests.seek_requested) {
        g_seek_requested.store(true);
        g_seek_target.store(requests.seek_target);
    }
    if (requests.web_pick_folder) g_web_pick_folder_requested.store(true);
    if (requests.web_audio_vis >= 0) g_web_audio_vis_state.store(requests.web_audio_vis);
    if (requests.web_weather >= 0) g_web_weather_state.store(requests.web_weather);
    if (requests.web_background >= 1) g_web_bg_state.store(requests.web_background);
    if (requests.web_sakura >= 0) g_web_sakura_state.store(requests.web_sakura);
    if (requests.web_vis_model >= 0) g_web_vis_state.store(requests.web_vis_model);
    if (requests.web_weather_city_edited) {
        std::lock_guard<std::mutex> lock(g_web_city_mutex);
        g_web_city = requests.web_weather_city;
    }
    // 播完自动下一个（壁纸模式同样生效：图片/视频轮播）
    if (g_sequence_auto_next.load() && g_controller.ended() && !g_controller.paused()) {
        g_seq_auto_requested.store(true);
    }

    if (requests.web_wallpaper_toggle) {
        std::lock_guard<std::mutex> lock(g_request_mutex);
        g_web_wallpaper_toggle_request = true;
        g_web_wallpaper_url = requests.web_url;
    }

    g_capture_preview.draw(io.DisplaySize.x, io.DisplaySize.y);

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace

void write_wav_16(const std::string& path, const std::vector<float>& samples, int rate, int ch) {
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "wb");
    if (!f) return;
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size()) * 2;
    const uint32_t file_size = 44 + data_bytes;
    fwrite("RIFF", 1, 4, f);
    const uint32_t riff_size = file_size - 8;
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    const uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    const uint16_t audio_fmt = 1;
    fwrite(&audio_fmt, 2, 1, f);
    const uint16_t channels = static_cast<uint16_t>(ch);
    fwrite(&channels, 2, 1, f);
    const uint32_t sample_rate = static_cast<uint32_t>(rate);
    fwrite(&sample_rate, 4, 1, f);
    const uint32_t byte_rate = sample_rate * channels * 2;
    fwrite(&byte_rate, 4, 1, f);
    const uint16_t block_align = static_cast<uint16_t>(channels * 2);
    fwrite(&block_align, 2, 1, f);
    const uint16_t bits = 16;
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
    for (float s : samples) {
        const int16_t v = static_cast<int16_t>(std::clamp(s, -1.0f, 1.0f) * 32767.0f);
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
}

int run_capture_audio(double seconds) {
    me::LoopbackCapture cap;
    if (!cap.open()) {
        std::fprintf(stderr, "[loopcap] 打开采集失败\n");
        return 1;
    }
    cap.start();
    std::vector<float> all;
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() < seconds) {
        std::vector<float> chunk = cap.take_samples();
        all.insert(all.end(), chunk.begin(), chunk.end());
        Sleep(50);
    }
    cap.stop();
    std::vector<float> tail = cap.take_samples();
    all.insert(all.end(), tail.begin(), tail.end());
    double rms = 0.0;
    for (float s : all) rms += static_cast<double>(s) * s;
    rms = all.empty() ? 0.0 : std::sqrt(rms / static_cast<double>(all.size()));
    write_wav_16("capture_loopback.wav", all, cap.sample_rate(), cap.channels());
    std::fprintf(stderr, "[loopcap] 采集 %.1fs: %zu 采样, RMS=%.4f, 已写 capture_loopback.wav\n",
                seconds, all.size(), rms);
    return 0;
}

void print_address_module(void* addr) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &mod)) {
        std::fprintf(stderr, "[crash]   at 0x%p (未知模块)\n", addr);
        return;
    }
    MODULEINFO mi = {};
    if (GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi))) {
        wchar_t name[MAX_PATH] = {};
        GetModuleBaseNameW(GetCurrentProcess(), mod, name, MAX_PATH);
        std::fprintf(stderr, "[crash]   at 0x%p  %ls + 0x%llX\n", addr, name,
                     static_cast<ULONGLONG>(static_cast<BYTE*>(addr) -
                                            static_cast<BYTE*>(mi.lpBaseOfDll)));
    } else {
        std::fprintf(stderr, "[crash]   at 0x%p 模块=%p\n", addr, static_cast<void*>(mod));
    }
}

void write_minidump(EXCEPTION_POINTERS* ep) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* dot = wcsrchr(path, L'.');
    if (dot) *dot = L'\0';
    wcscat_s(path, L"_crash.dmp");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), ep, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      MiniDumpNormal, ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(file);
    std::fprintf(stderr, "[crash] 已写转储: %ls\n", path);
}

LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    std::fprintf(stderr, "[crash] 未处理异常: 0x%08X at 0x%p (tid=%u)\n",
                 static_cast<unsigned>(ep->ExceptionRecord->ExceptionCode),
                 ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    print_address_module(ep->ExceptionRecord->ExceptionAddress);

    // 调用栈：打印每帧的模块偏移（无 PDB 也能定位模块；结合本机构建 PDB 可还原函数）
    void* frames[32] = {};
    const USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
    for (USHORT i = 1; i < count; ++i) {
        print_address_module(frames[i]);
    }
    write_minidump(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

int wmain(int argc, wchar_t** argv) {
    // 必须在创建任何窗口前启用：高 DPI 屏幕按物理像素渲染（壁纸分辨率修复）
    ImGui_ImplWin32_EnableDpiAwareness();
    // 默认隐藏控制台窗口；--debug / --console 时显示（日志仍写入 stderr）
    bool show_console = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argw = argv[i];
        if (argw == L"--debug" || argw == L"--console") show_console = true;
    }
    if (const HWND con = GetConsoleWindow()) ShowWindow(con, show_console ? SW_SHOW : SW_HIDE);
    SetUnhandledExceptionFilter(&crash_handler);
    SetConsoleOutputCP(CP_UTF8);  // 让中文日志在控制台正确显示
    SetConsoleCP(CP_UTF8);
    me_set_log_level(1);  // Info
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--debug") me_set_log_level(0);  // Debug
    }
    double capture_audio_seconds = -1.0;
    double capture_screen_seconds = -1.0;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--headless") g_headless_cli = true;
        if (std::wstring(argv[i]) == L"--wallpaper-keep") g_wallpaper_keep = true;
        if (std::wstring(argv[i]) == L"--capture-audio" && i + 1 < argc) capture_audio_seconds = _wtof(argv[i + 1]);
        if (std::wstring(argv[i]) == L"--capture-screen" && i + 1 < argc) capture_screen_seconds = _wtof(argv[i + 1]);
    }
    if (capture_audio_seconds > 0.0) return run_capture_audio(capture_audio_seconds);
    std::printf("MediaEngine v0.1\n");

    g_window.set_file_drop_callback([](const std::wstring& path) {
        open_media(path, g_open_prefer_hw.load());
    });
    g_floating_panel.set_file_drop_callback([](const std::wstring& path) {
        open_media(path, g_open_prefer_hw.load());
    });
    g_window.set_key_callback([](unsigned vk) {
        switch (vk) {
            case VK_SPACE: g_controller.toggle_pause(); break;
            case VK_LEFT:  g_controller.seek(g_controller.position() - 10.0); break;
            case VK_RIGHT: g_controller.seek(g_controller.position() + 10.0); break;
            case 'H':      g_show_panel = !g_show_panel; break;
            case 'M':      g_controller.set_volume(g_controller.volume() > 0.01f ? 0.0f : 1.0f); break;
            case VK_OEM_4: g_controller.set_speed(g_controller.speed() / 1.25); break;  // [ 减速
            case VK_OEM_6: g_controller.set_speed(g_controller.speed() * 1.25); break;  // ] 加速
            case VK_F12: break;  // 调试导出暂禁用
            default: break;
        }
    });
    g_window.set_resize_callback([](int w, int h) { me_resize(g_engine, w, h); });

    if (!g_headless_cli && !g_window.create(L"MediaEngine — 学习型媒体引擎", 1280, 720)) {
        std::fprintf(stderr, "创建窗口失败\n");
        return 1;
    }

    // 引擎（media_engine.dll）持有渲染器与播放器
    g_engine = g_headless_cli
                  ? me_create_player_ex(nullptr, 1280, 720,
                                        ME_PLAYER_FLAG_HEADLESS | ME_PLAYER_FLAG_NULL_AUDIO)
                  : me_create_player(g_window.handle(), 1280, 720);
    g_controller.set_player(g_engine);
    if (g_engine) {
        const char* err = me_last_error(g_engine);
        if (err && *err) std::fprintf(stderr, "引擎初始化: %s\n", err);
    }

    // ImGui 初始化（此后所有 ImGui 调用都发生在渲染线程）
    if (!g_headless_cli) {
        g_window.set_app_message_callback([](UINT, WPARAM wp, LPARAM lp) {
            return g_tray.handle_message(wp, lp);
        });
        g_tray.set_command_callback([](int cmd) {
            switch (cmd) {
                case me::kTrayShowPanel: g_show_panel = true; break;
                case me::kTrayHidePanel: g_show_panel = false; break;
                case me::kTrayToggleWallpaper: g_wallpaper_requested.store(true); break;
                case me::kTrayExit: PostMessageW(g_window.handle(), WM_CLOSE, 0, 0); break;
                case me::kTrayToggleWebWallpaper: {
                    std::lock_guard<std::mutex> lock(g_request_mutex);
                    g_web_wallpaper_toggle_request = true;
                    g_web_wallpaper_url = g_web_wallpaper.active() ? g_web_wallpaper.url() : g_panel.web_url();
                    break;
                }
                default: break;
            }
        });
        g_tray.create(GetModuleHandleW(nullptr), g_window.handle());
    }

    if (!g_headless_cli) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "media_engine_imgui.ini";
    const ImFontConfig font_cfg{};
    const float dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(g_window.handle());
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18.0f * dpi_scale,
                                 &font_cfg, io.Fonts->GetGlyphRangesChineseFull());
    ImGui_ImplWin32_Init(g_window.handle());
    ImGui_ImplDX11_Init(static_cast<ID3D11Device*>(me_get_d3d11_device(g_engine)),
                        static_cast<ID3D11DeviceContext*>(me_get_d3d11_context(g_engine)));

    // 渲染线程每帧：引擎画视频帧后回调 present_callback 叠加面板，再 Present
    me_set_present_callback(g_engine, &present_callback, nullptr);

    // 全局热键：Ctrl+Alt+W 切换壁纸模式（进入壁纸后窗口点击穿透，只能用热键退出）
    RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT, 'W');
    }

    // 命令行参数
    bool prefer_hw_cli = false;
    double reopen_after = -1.0;
    double seek_after_seconds = -1.0;
    double eof_seek_target = -1.0;
    double speed_test = -1.0;
    bool pause_test = false;
    bool wallpaper_test = false;
    std::wstring web_wallpaper_url_cli;
    int device_test = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--hw") prefer_hw_cli = true;
        if (std::wstring(argv[i]) == L"--seek" && i + 1 < argc) {
            seek_after_seconds = _wtof(argv[i + 1]);
        }
        if (std::wstring(argv[i]) == L"--reopen" && i + 1 < argc) {
            reopen_after = _wtof(argv[i + 1]);
        }
        if (std::wstring(argv[i]) == L"--eof-seek" && i + 1 < argc) {
            eof_seek_target = _wtof(argv[i + 1]);
        }
        if (std::wstring(argv[i]) == L"--speed" && i + 1 < argc) {
            speed_test = _wtof(argv[i + 1]);
        }
        if (std::wstring(argv[i]) == L"--pause-test") {
            pause_test = true;
        }
        if (std::wstring(argv[i]) == L"--device" && i + 1 < argc) {
            device_test = _wtoi(argv[i + 1]);
        }
        if (std::wstring(argv[i]) == L"--run-seconds" && i + 1 < argc) {
            g_run_seconds = _wtof(argv[i + 1]);
        }
        if (std::wstring(argv[i]) == L"--wallpaper") {
            wallpaper_test = true;
        }
        if (std::wstring(argv[i]) == L"--web-wallpaper" && i + 1 < argc) {
            web_wallpaper_url_cli = argv[i + 1];
        }
        if (std::wstring(argv[i]) == L"--web-enhance") g_web_enhance_cli = true;
    }
    std::wstring first_media;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg(argv[i]);
        if ((arg == L"--seek" || arg == L"--eof-seek" || arg == L"--speed" || arg == L"--device" || arg == L"--run-seconds" || arg == L"--web-wallpaper" || arg == L"--capture-audio" || arg == L"--capture-screen") && i + 1 < argc) {
            ++i;  // 跳过带值的参数
            continue;
        }
        if (arg == L"--pause-test" || arg == L"--wallpaper" || arg == L"--wallpaper-keep" || arg == L"--headless" || arg == L"--web-wallpaper" || arg == L"--capture-audio" || arg == L"--capture-screen" || arg == L"--web-enhance") {
            continue;
        }
        if (arg == L"--reopen" && i + 1 < argc) {
            ++i;  // 跳过 --reopen 的值，避免被当成媒体路径
            continue;
        }
        if (arg != L"--hw" && arg != L"--debug" && arg != L"--console") {
            first_media = arg;
            if (capture_screen_seconds <= 0.0) open_media(arg, prefer_hw_cli);
            break;
        }
    }
    maybe_schedule_reopen(first_media, prefer_hw_cli, reopen_after);

    if (seek_after_seconds > 0.0) {
        std::thread([target = seek_after_seconds] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            std::fprintf(stderr, "[seek-test] 跳转到 %.1fs\n", target);
            g_controller.seek(target);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            std::fprintf(stderr, "[seek-test] 快速拖回 %.1fs\n", target * 0.6);
            g_controller.seek(target * 0.6);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            std::fprintf(stderr, "[seek-test] 再拖到 %.1fs\n", target * 0.8);
            g_controller.seek(target * 0.8);
        }).detach();
    }
    if (eof_seek_target >= 0.0) {
        std::thread([target = eof_seek_target] {
            // 等播放自然结束（ended）后再 seek，覆盖 EOF→seek 唤醒场景
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            for (int i = 0; i < 250; ++i) {
                if (g_controller.ended()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            std::fprintf(stderr, "[eof-seek] EOF 后跳转到 %.1fs\n", target);
            g_controller.seek(target);
        }).detach();
    }
    if (speed_test > 0.0) {
        std::thread([rate = speed_test] {
            // 模拟拖动速度条：每 150ms 换一档，覆盖 0.5x-3x 快速来回
            const double sweep[] = {1.0, 1.5, 2.0, 2.5, 3.0, 2.5, 2.0, 1.5, 1.0, 0.75, 0.5, 0.75, 1.0};
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
            for (double s : sweep) {
                std::fprintf(stderr, "[speed-test] 变速到 %.2fx\n", s);
                g_controller.set_speed(s);
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            std::fprintf(stderr, "[speed-test] 拖动结束，保持 %.2fx\n", rate);
            g_controller.set_speed(rate);
            std::this_thread::sleep_for(std::chrono::milliseconds(5000));
        }).detach();
    }
    if (pause_test) {
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(4000));
            std::fprintf(stderr, "[pause-test] 暂停\n");
            g_controller.pause();
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            std::fprintf(stderr, "[pause-test] 恢复\n");
            g_controller.play();
        }).detach();
    }
    if (device_test >= 0) {
        std::thread([idx = device_test] {
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
            std::fprintf(stderr, "[device-test] 切换设备 #%d\n", idx);
            me::Error err = g_controller.set_audio_device(idx);
            std::fprintf(stderr, "[device-test] 切换结果: %s\n", err.ok() ? "OK" : err.message().c_str());
        }).detach();
    }
    if (wallpaper_test && !g_headless_cli) {
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            toggle_wallpaper();
            if (!g_wallpaper_keep) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5000));
                toggle_wallpaper();
            }
        }).detach();
    }

    // 主线程：消息循环 + 处理"打开文件"请求（对话框必须在主线程）
    if (capture_screen_seconds > 0.0 && !g_headless_cli) {
        auto* cdev = static_cast<ID3D11Device*>(me_get_d3d11_device(g_engine));
        auto* cctx = static_cast<ID3D11DeviceContext*>(me_get_d3d11_context(g_engine));
        g_capture_preview.start(cdev, cctx, capture_screen_seconds, g_window.handle());
    }

    if (!web_wallpaper_url_cli.empty() && !g_headless_cli) {
        const std::string wurl = utf8_from_wide(web_wallpaper_url_cli);
        // WebView2 控制器要求宿主窗口所在线程有消息泵：只能经主循环消费请求
        std::thread([wurl] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            {
                std::lock_guard<std::mutex> lock(g_request_mutex);
                g_web_wallpaper_toggle_request = true;
                g_web_wallpaper_url = wurl;
            }
            if (!g_wallpaper_keep) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5000));
                {
                    std::lock_guard<std::mutex> lock(g_request_mutex);
                    g_web_wallpaper_toggle_request = true;
                    g_web_wallpaper_url = wurl;
                }
            }
        }).detach();
    }

    if (g_headless_cli) {
        std::fprintf(stderr, "[headless] 管线运行 %.1fs 后退出\n", g_run_seconds);
        Sleep(static_cast<DWORD>(g_run_seconds * 1000.0));
        std::fprintf(stderr, "[headless] draw=%lld present=%lld\n",
                     me_headless_draw_count(g_engine), me_headless_present_count(g_engine));
        me_destroy_player(g_engine);
        g_engine = nullptr;
        return 0;
    }

    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY && msg.wParam == 1) { toggle_wallpaper(); continue; }
            if (msg.message == WM_QUIT) {
                g_floating_panel.request_destroy();
                me_destroy_player(g_engine);  // 先停渲染线程
                g_floating_panel.destroy_now();
                g_web_wallpaper.destroy();
                g_tray.destroy();
                ImGui_ImplDX11_Shutdown();
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
                g_engine = nullptr;
                g_window.destroy();
                return static_cast<int>(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_seek_requested.exchange(false)) {
            // 主线程执行 seek：avformat_seek_file 可能阻塞数百 ms，不能占用渲染线程
            g_controller.seek(g_seek_target.load());
        }
        {
            const int auto_change = g_seq_auto_next_change.exchange(-1);
            if (auto_change >= 0) g_sequence_auto_next.store(auto_change != 0);
        }
        {
            const int type_change = g_seq_type_requested.exchange(-1);
            if (type_change >= 0) {
                g_sequence_type.store(type_change);
                std::wstring cur;
                {
                    std::lock_guard<std::mutex> lock(g_current_mutex);
                    cur = g_current_media;
                }
                if (!cur.empty()) {
                    g_sequence.rebuild(cur, static_cast<me::SequenceType>(type_change));
                }
            }
        }
        if (g_seq_prev_requested.exchange(false)) {
            std::wstring p;
            if (g_sequence.prev(p)) open_media(p, g_open_prefer_hw.load());
        }
        if (g_seq_next_requested.exchange(false)) {
            std::wstring p;
            if (g_sequence.next(p)) open_media(p, g_open_prefer_hw.load());
        }
        if (g_seq_auto_requested.exchange(false)) {
            std::wstring p;
            if (g_sequence.next(p)) open_media(p, g_open_prefer_hw.load());
        }
        if (g_web_pick_folder_requested.exchange(false)) {
            BROWSEINFOW bi = {};
            bi.hwndOwner = g_window.wallpaper_mode() ? g_floating_panel.handle() : g_window.handle();
            bi.lpszTitle = L"选择网页壁纸文件夹（含 index.html）";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t folder[MAX_PATH] = {};
                if (SHGetPathFromIDListW(pidl, folder)) {
                    const std::string path = utf8_from_wide(folder);
                    g_panel.set_web_url(path);
                    if (g_web_wallpaper.active()) {
                        g_web_wallpaper.navigate(path);
                    } else {
                        toggle_web_wallpaper(path);
                    }
                }
                CoTaskMemFree(pidl);
            }
        }
        if (g_wallpaper_requested.exchange(false)) toggle_wallpaper();
        {
            std::lock_guard<std::mutex> lock(g_request_mutex);
            if (g_web_wallpaper_toggle_request) {
                g_web_wallpaper_toggle_request = false;
                const std::string url = g_web_wallpaper_url;
                toggle_web_wallpaper(url);
            }
        }
        if (g_web_enhance_cli) {
            // 测试入口：--web-enhance 同时开启音频可视化 + 天气（IP 定位）
            g_web_audio_vis_state.store(1);
            g_web_weather_state.store(1);
        }
        {
            const bool web_active = g_web_wallpaper.active();
            if (web_active) {
                static bool a_applied = false;
                static bool w_applied = false;
                static int bg_applied = -1;
                static int vis_applied = -1;
                static bool sak_applied = false;
                static std::string city_applied;
                const bool want_a = g_web_audio_vis_state.load() != 0;
                const bool want_w = g_web_weather_state.load() != 0;
                const int want_bg = g_web_bg_state.load();
                const bool want_sak = g_web_sakura_state.load() != 0;
                const int want_vis = g_web_vis_state.load();
                std::string city;
                {
                    std::lock_guard<std::mutex> lock(g_web_city_mutex);
                    city = g_web_city;
                }
                if (!g_web_active_prev || want_a != a_applied) {
                    g_web_wallpaper.set_audio_visualization(want_a);
                    a_applied = want_a;
                }
                if (!g_web_active_prev || want_w != w_applied || city != city_applied) {
                    g_web_wallpaper.set_weather(want_w, city);  // 空城市 = IP 自动定位
                    w_applied = want_w;
                    city_applied = city;
                }
                if (!g_web_active_prev || want_bg != bg_applied || want_sak != sak_applied ||
                    want_vis != vis_applied) {
                    g_web_wallpaper.set_wallpaper_props(want_bg, want_sak, want_vis);
                    bg_applied = want_bg;
                    sak_applied = want_sak;
                    vis_applied = want_vis;
                }
            }
            g_web_active_prev = web_active;
        }
        if (g_open_requested.exchange(false)) {
            wchar_t file[MAX_PATH] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            // 壁纸模式下主窗口是点击穿透的桌面层，文件对话框不能挂在它下面（会跑到图标后面）
            ofn.hwndOwner = g_window.wallpaper_mode() ? g_floating_panel.handle() : g_window.handle();
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"媒体文件\0*.mp4;*.mkv;*.mov;*.ts;*.flv;*.webm;*.avi;*.wav;*.mp3;*.flac;*.opus;*.m4a;*.aac;*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp\0所有文件\0*.*\0";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                open_media(ofn.lpstrFile, g_open_prefer_hw.load());
            }
        }
        Sleep(2);
    }
}
