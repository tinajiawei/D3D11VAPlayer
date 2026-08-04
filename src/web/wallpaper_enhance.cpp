#include "web/wallpaper_enhance.h"

#include "capture/loopback_capture.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>

#include <wininet.h>

#pragma comment(lib, "wininet.lib")

namespace me {

namespace {

// ---------- 简易 FFT（radix-2 迭代） ----------
void fft(std::vector<std::complex<double>>& a, bool invert) {
    const int n = static_cast<int>(a.size());
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * 3.14159265358979323846 / len * (invert ? -1.0 : 1.0);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (int k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (invert) {
        for (auto& x : a) x /= static_cast<double>(n);
    }
}

constexpr int kFftSize = 1024;   // 48kHz 下约 21ms 窗口
constexpr int kBinCount = 256;

void compute_spectrum(const std::vector<float>& mono, std::vector<float>& out) {
    std::vector<std::complex<double>> f(kFftSize);
    for (int i = 0; i < kFftSize; ++i) f[i] = mono[static_cast<size_t>(i)];
    fft(f, false);
    out.resize(kBinCount);
    for (int k = 0; k < kBinCount; ++k) {
        // 幅度归一化 + 平方根压缩（人耳近感知），乘增益让常见音量有可见波动
        const double mag = std::abs(f[static_cast<size_t>(k)]) / (kFftSize / 2.0);
        double v = std::sqrt(mag) * 2.6;
        if (v < 0.02) v = 0.0;
        out[static_cast<size_t>(k)] = static_cast<float>(std::min(1.0, v));
    }
}

// ---------- 极小 JSON 字段提取 ----------
// 跳过 _units 里的字符串字段（如 "temperature_2m":"°C"）和数组字段
double json_number(const std::string& s, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = 0;
    while ((p = s.find(k, p)) != std::string::npos) {
        const size_t colon = s.find(':', p + k.size());
        if (colon == std::string::npos) return 0.0;
        size_t q = colon + 1;
        while (q < s.size() && (s[q] == ' ' || s[q] == '\t')) ++q;
        if (q < s.size() && (s[q] == '"' || s[q] == '[')) { p = colon + 1; continue; }
        return std::strtod(s.c_str() + q, nullptr);
    }
    return 0.0;
}

// 字符串字段提取（"city":"北京"）
std::string json_string(const std::string& s, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    const size_t p = s.find(k);
    if (p == std::string::npos) return {};
    const size_t colon = s.find(':', p + k.size());
    if (colon == std::string::npos) return {};
    size_t q = colon + 1;
    while (q < s.size() && (s[q] == ' ' || s[q] == '\t')) ++q;
    if (q >= s.size() || s[q] != '"') return {};
    ++q;
    std::string out;
    while (q < s.size() && s[q] != '"') {
        if (s[q] == '\\' && q + 1 < s.size()) ++q;
        out += s[q++];
    }
    return out;
}

// daily 数组字段取第一个数值（"temperature_2m_max":[34.6]）
double json_array_first_number(const std::string& s, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = 0;
    while ((p = s.find(k, p)) != std::string::npos) {
        const size_t colon = s.find(':', p + k.size());
        if (colon == std::string::npos) return 0.0;
        size_t q = colon + 1;
        while (q < s.size() && (s[q] == ' ' || s[q] == '\t')) ++q;
        if (q < s.size() && s[q] == '[') return std::strtod(s.c_str() + q + 1, nullptr);
        p = colon + 1;
    }
    return 0.0;
}

// ---------- WinINet 简单 GET（走系统代理，与 PowerShell/浏览器一致） ----------
std::string http_get(const std::string& url) {
    std::string result;
    const std::wstring wurl(url.begin(), url.end());
    const HINTERNET net = InternetOpenW(L"MediaEngine/1.0", INTERNET_OPEN_TYPE_PRECONFIG,
                                        nullptr, nullptr, 0);
    if (!net) return result;
    const HINTERNET req = InternetOpenUrlW(net, wurl.c_str(), nullptr, 0,
                                           INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE |
                                               INTERNET_FLAG_NO_CACHE_WRITE,
                                           0);
    if (req) {
        char buf[8192] = {};
        DWORD read = 0;
        while (InternetReadFile(req, buf, sizeof(buf), &read) && read > 0) {
            result.append(buf, read);
            read = 0;
        }
        InternetCloseHandle(req);
    }
    InternetCloseHandle(net);
    return result;
}

std::string url_encode_utf8(const std::string& s) {
    const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if (c > 0x7F || c == ' ') {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<size_t>(len - 1), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::string utf8_to_narrow(const std::string& s) {
    // 城市名直接透传（调用方已保证 UTF-8）
    return s;
}

// WMO 天气代码 → 中文
const char* wmo_text(int code) {
    if (code == 0) return "晴";
    if (code <= 3) return "多云";
    if (code == 45 || code == 48) return "雾";
    if (code >= 51 && code <= 57) return "毛毛雨";
    if (code >= 61 && code <= 67) return "雨";
    if (code >= 71 && code <= 77) return "雪";
    if (code >= 80 && code <= 82) return "阵雨";
    if (code == 85 || code == 86) return "阵雪";
    if (code == 95) return "雷雨";
    if (code >= 96) return "雷雨冰雹";
    return "未知";
}

}  // namespace

WallpaperEnhance::~WallpaperEnhance() {
    audio_enabled_.store(false);
    weather_enabled_.store(false);
    if (audio_thread_.joinable()) audio_thread_.join();
    if (weather_thread_.joinable()) weather_thread_.join();
}

void WallpaperEnhance::set_webview_thread(DWORD thread_id) {
    thread_id_.store(thread_id);
}

void WallpaperEnhance::set_audio_enabled(bool on) {
    audio_enabled_.store(on);
    if (!on) {
        if (audio_thread_.joinable()) audio_thread_.join();  // 关闭时回收线程，允许再次开启
    } else if (!audio_thread_.joinable()) {
        audio_thread_ = std::thread(&WallpaperEnhance::audio_loop, this);
    }
}

void WallpaperEnhance::set_weather_enabled(bool on, const std::string& city) {
    weather_enabled_.store(on);
    if (!on) {
        if (weather_thread_.joinable()) weather_thread_.join();
    } else if (!weather_thread_.joinable()) {
        weather_thread_ = std::thread(&WallpaperEnhance::weather_loop, this, city);
    }
}

std::vector<float> WallpaperEnhance::take_spectrum() {
    std::lock_guard<std::mutex> lock(spec_mutex_);
    return spectrum_;
}

std::string WallpaperEnhance::take_weather_payload() {
    std::lock_guard<std::mutex> lock(weather_mutex_);
    return weather_payload_;
}

void WallpaperEnhance::audio_loop() {
    LoopbackCapture cap;
    if (!cap.open(48000, 2)) {
        std::fprintf(stderr, "[enhance] 环回采集打开失败，音频可视化不可用\n");
        audio_enabled_.store(false);
        return;
    }
    if (!cap.start()) {
        std::fprintf(stderr, "[enhance] 环回采集启动失败\n");
        audio_enabled_.store(false);
        cap.close();
        return;
    }
    std::fprintf(stderr, "[enhance] 音频可视化已启动（loopback %s）\n", cap.device_name().c_str());

    const int channels = cap.channels() > 0 ? cap.channels() : 2;
    std::vector<float> accum;
    DWORD last_post = 0;
    while (audio_enabled_.load()) {
        std::vector<float> chunk = cap.take_samples();
        if (chunk.empty()) {
            Sleep(4);
            continue;
        }
        accum.insert(accum.end(), chunk.begin(), chunk.end());
        const size_t need = static_cast<size_t>(kFftSize) * channels;
        while (accum.size() >= need) {
            // 混为单声道
            std::vector<float> mono(static_cast<size_t>(kFftSize));
            for (int i = 0; i < kFftSize; ++i) {
                float sum = 0.0f;
                for (int c = 0; c < channels; ++c) sum += accum[static_cast<size_t>(i * channels + c)];
                mono[static_cast<size_t>(i)] = sum / static_cast<float>(channels);
            }
            accum.erase(accum.begin(), accum.begin() + static_cast<std::ptrdiff_t>(need));
            std::vector<float> spec;
            compute_spectrum(mono, spec);
            {
                std::lock_guard<std::mutex> lock(spec_mutex_);
                spectrum_ = std::move(spec);
            }
            const DWORD now = GetTickCount();
            if (now - last_post >= 33) {  // ~30fps，ExecuteScript 有开销
                last_post = now;
                const DWORD tid = thread_id_.load();
                if (tid) PostThreadMessageW(tid, WM_APP + 1, kEnhanceMsgSpectrum, 0);
            }
        }
    }
    cap.stop();
    cap.close();
    std::fprintf(stderr, "[enhance] 音频可视化已停止\n");
}

void WallpaperEnhance::weather_loop(const std::string city_utf8) {
    // 1) 定位：有城市名 → Open-Meteo 地理编码；留空 → 按当前 IP 自动定位（ip-api.com 免费接口）
    double lat = 0.0, lon = 0.0;
    std::string city = city_utf8;
    if (city.empty()) {
        // HTTPS 接口（ip-api 免费版是 http，部分代理环境不稳）
        const std::string ip = http_get("https://ipwho.is/");
        lat = json_number(ip, "latitude");
        lon = json_number(ip, "longitude");
        if (lat != 0.0 && lon != 0.0) {
            city = json_string(ip, "city");
            if (city.empty()) city = json_string(ip, "region");
            std::fprintf(stderr, "[enhance] 天气: IP 定位 %s (%.4f, %.4f)\n",
                         city.c_str(), lat, lon);
        } else {
            std::fprintf(stderr, "[enhance] 天气: IP 定位失败，回退北京\n");
        }
    } else {
        const std::string geo_url =
            "https://geocoding-api.open-meteo.com/v1/search?name=" + url_encode_utf8(city) +
            "&count=1&language=zh&format=json";
        const std::string geo = http_get(geo_url);
        lat = json_number(geo, "latitude");
        lon = json_number(geo, "longitude");
        if (lat == 0.0 && lon == 0.0) {
            std::fprintf(stderr, "[enhance] 天气: 城市解析失败（%s）\n", city.c_str());
            weather_enabled_.store(false);
            return;
        }
    }
    if (lat == 0.0 && lon == 0.0) {
        lat = 39.9042;  // 北京兜底
        lon = 116.4074;
        if (city.empty()) city = "北京";
    }

    // 2) 天气预报（current + 当日高低温）
    char fc_url[512] = {};
    std::snprintf(fc_url, sizeof(fc_url),
                  "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                  "&current=temperature_2m,weather_code,wind_speed_10m"
                  "&daily=temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=1",
                  lat, lon);
    const std::string fc = http_get(fc_url);
    const double temp = json_number(fc, "temperature_2m");
    const double wind = json_number(fc, "wind_speed_10m");
    const double hi = json_array_first_number(fc, "temperature_2m_max");
    const double lo = json_array_first_number(fc, "temperature_2m_min");
    const int code = static_cast<int>(json_number(fc, "weather_code"));

    char payload[256] = {};
    std::snprintf(payload, sizeof(payload), "%s|%.1f|%s|%.0f|%.0f|%.0f",
                  city.c_str(), temp, wmo_text(code), hi, lo, wind);
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        weather_payload_ = payload;
    }
    const DWORD tid = thread_id_.load();
    if (tid) PostThreadMessageW(tid, WM_APP + 1, kEnhanceMsgWeather, 0);
    std::fprintf(stderr, "[enhance] 天气已更新: %s\n", payload);

    // 单次拉取后退出线程；再次开启会重新拉取
    weather_enabled_.store(false);
}

}  // namespace me