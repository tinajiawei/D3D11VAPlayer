#include "audio/audio_output.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <propvarutil.h>
#include <functiondiscoverykeys_devpkey.h>
#include <thread>
#include <wrl/client.h>

#include "core/clock.h"
#include "core/log.h"

namespace me {

namespace {

// 部分 SDK 的导入库未提供 CLSID_MMDeviceEnumerator 符号（dumpbin 验证 mmdevapi/uuid/ole32 均无），
// 该 GUID 是公开稳定的 API 标识，直接在此定义最可靠。
EXTERN_C const CLSID CLSID_MMDeviceEnumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};

std::string hr_hex(HRESULT hr) {
    char buf[32];
    sprintf_s(buf, "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

bool format_is_float(const WAVEFORMATEX* fmt) {
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

// 免提/电话类端点（蓝牙 Hands-Free / AG Audio）：音乐播放选它通常无声或音质极差，一律跳过
bool is_hands_free_endpoint(const std::string& name) {
    return name.find("Hands-Free") != std::string::npos ||
           name.find("AG Audio") != std::string::npos ||
           name.find("免提") != std::string::npos;
}

std::string endpoint_friendly_name(IMMDevice* device, UINT fallback_index) {
    std::string name = "设备 #" + std::to_string(fallback_index);
    Microsoft::WRL::ComPtr<IPropertyStore> store;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
        PROPVARIANT var{};
        PropVariantInit(&var);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &var)) && var.pwszVal) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                name.resize(static_cast<size_t>(len - 1));
                WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, name.data(), len, nullptr, nullptr);
            }
        }
        PropVariantClear(&var);
    }
    return name;
}

}  // namespace

AudioOutput::~AudioOutput() { shutdown(); }

Error AudioOutput::init(int sample_rate, int channels, double buffer_seconds) {
    if (initialized_) return Error::success();  // 幂等

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return Error::make(Err::AudioFailed, "CoInitializeEx 失败: " + hr_hex(hr));
    }

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> fresh_enumerator;
    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&fresh_enumerator));
    if (FAILED(hr)) {
        return Error::make(Err::AudioFailed, "创建 MMDeviceEnumerator 失败: " + hr_hex(hr));
    }
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        if (enumerator_) enumerator_->Release();
        enumerator_ = fresh_enumerator.Detach();
    }
    if (FAILED(hr)) {
        return Error::make(Err::AudioFailed, "创建 MMDeviceEnumerator 失败: " + hr_hex(hr));
    }

    // 虚拟/远程音频设备可能短暂失效（AUDCLNT_E_DEVICE_INVALIDATED），
    // 每次重新取默认端点，最多重试 3 次。
    Error last_error = Error::make(Err::AudioFailed, "未知错误");
    for (int attempt = 1; attempt <= 3; ++attempt) {
        release_audio_objects();
        ME_LOG_INFO("[audio] 第 ", attempt, " 次初始化尝试");
        last_error = init_attempt(sample_rate, channels, buffer_seconds);
        if (last_error.ok()) return Error::success();
        ME_LOG_WARN("[audio] 第 ", attempt, " 次失败: ", last_error.message());
        if (attempt < 3) Sleep(300);
    }
    release_audio_objects();
    return last_error;
}

Error AudioOutput::init_attempt(int sample_rate, int channels, double buffer_seconds) {
    Error last_error = Error::make(Err::AudioFailed, "没有可用的音频输出端点");

    // 1) 优先用户当前默认输出设备（即系统设置里的扬声器）。
    //    之前版本枚举优先导致声音输出到第一个可用的虚拟设备，用户听不到。
    Microsoft::WRL::ComPtr<IMMDevice> default_device;
    HRESULT hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &default_device);
    if (SUCCEEDED(hr) && default_device) {
        const std::string default_name = endpoint_friendly_name(default_device.Get(), 0);
        if (is_hands_free_endpoint(default_name)) {
            ME_LOG_WARN("[audio] 默认端点是免提/电话端点（", default_name, "），跳过");
        } else {
            ME_LOG_INFO("[audio] 优先尝试系统默认输出端点: ", default_name);
            last_error = try_device(default_device.Get(), sample_rate, channels, buffer_seconds);
            if (last_error.ok()) return Error::success();
            ME_LOG_WARN("[audio] 默认端点失败: ", last_error.message(), "，回退枚举所有活动端点");
        }
        release_audio_objects();
    }

    // 2) 兜底：枚举所有活动端点逐个尝试（默认设备失效的虚拟/远程环境）
    Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
    hr = enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            Microsoft::WRL::ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, &device)) || !device) continue;
            const std::string name = endpoint_friendly_name(device.Get(), i);
            if (is_hands_free_endpoint(name)) {
                ME_LOG_WARN("[audio] 跳过免提/电话端点 #", i, ": ", name);
                continue;
            }
            ME_LOG_INFO("[audio] 尝试备用端点 #", i, ": ", name);
            last_error = try_device(device.Get(), sample_rate, channels, buffer_seconds);
            if (last_error.ok()) return Error::success();
            ME_LOG_WARN("[audio] 备用端点 #", i, " 初始化失败: ", last_error.message());
            release_audio_objects();
        }
    }
    return last_error;
}

Error AudioOutput::try_device(IMMDevice* device, int sample_rate, int channels, double buffer_seconds) {
    device_ = device;
    device_->AddRef();  // 成员持有一份引用，release_audio_objects 负责释放

    // 读取设备友好名（确认输出到了用户正在用的扬声器）
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        device_name_.clear();
    Microsoft::WRL::ComPtr<IPropertyStore> store;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
        PROPVARIANT var{};
        PropVariantInit(&var);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &var)) && var.pwszVal) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                device_name_.resize(static_cast<size_t>(len - 1));
                WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, device_name_.data(), len, nullptr, nullptr);
            }
        }
        PropVariantClear(&var);
    }
    }
    ME_LOG_INFO("[audio] 输出设备: ", device_name_.empty() ? "(未知)" : device_name_);

    HRESULT hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&client_));
    if (FAILED(hr)) {
        return Error::make(Err::AudioFailed, "激活 IAudioClient 失败: " + hr_hex(hr));
    }

    // 首选：float32 / 目标采样率 / 立体声（共享模式）
    WAVEFORMATEXTENSIBLE wfx = {};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = static_cast<WORD>(channels);
    wfx.Format.nSamplesPerSec = sample_rate;
    wfx.Format.wBitsPerSample = 32;
    wfx.Format.nBlockAlign = static_cast<WORD>(channels * 4);
    wfx.Format.nAvgBytesPerSec = sample_rate * channels * 4;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
                                      : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    // 首选：float32 / 目标采样率 / 立体声（共享模式）。
    // 注意：不做 IsFormatSupported 预检——实测某蓝牙端点（Tmall Genie）调用后会把端点置为失效态，
    // 后续全新 client 也会初始化失败（0x88890008）；直接让 Initialize 报错再走回退（探针验证此序列可行）。
    bool event_driven = true;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             static_cast<UINT64>(buffer_seconds * 1e7), 0,
                             reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);
    if (FAILED(hr)) {
        ME_LOG_WARN("[audio] 事件驱动初始化失败: ", hr_hex(hr), "，回退普通轮询模式");
        event_driven = false;
        // 首次 Initialize 失败可能让该 client 进入失效态（0x88890008）：
        // 释放旧 client，用全新 client + 系统混音格式 + 轮询模式重试
        if (render_client_) { render_client_->Release(); render_client_ = nullptr; }
        if (client_) { client_->Release(); client_ = nullptr; }
        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(&client_));
        if (SUCCEEDED(hr)) hr = client_->GetMixFormat(&mix_fmt_);
        if (SUCCEEDED(hr)) {
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                     static_cast<UINT64>(buffer_seconds * 1e7), 0, mix_fmt_, nullptr);
        }
        if (FAILED(hr)) {
            // 蓝牙端点瞬时失效常见：等一拍再试一次全新 client
            if (client_) { client_->Release(); client_ = nullptr; }
            Sleep(200);
            hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&client_));
            if (SUCCEEDED(hr)) {
                hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                         static_cast<UINT64>(buffer_seconds * 1e7), 0, mix_fmt_, nullptr);
            }
        }
        if (FAILED(hr)) {
            return Error::make(Err::AudioFailed, "IAudioClient::Initialize 失败: " + hr_hex(hr));
        }
        // 回退成功：按系统混音格式配置输出
        sample_rate_ = mix_fmt_->nSamplesPerSec;
        channels_ = mix_fmt_->nChannels;
        float_out_ = format_is_float(mix_fmt_);
        ME_LOG_INFO("[audio] 回退到系统混音格式: ", sample_rate_, "Hz, ", channels_, "ch, ",
                    float_out_ ? "float32" : "pcm16");
    } else {
        sample_rate_ = sample_rate;
        channels_ = channels;
        float_out_ = true;
    }

    hr = client_->GetBufferSize(&buffer_frames_);
    if (FAILED(hr)) {
        return Error::make(Err::AudioFailed, "GetBufferSize 失败: " + hr_hex(hr));
    }

    hr = client_->GetService(IID_PPV_ARGS(&render_client_));
    if (FAILED(hr)) {
        return Error::make(Err::AudioFailed, "获取 IAudioRenderClient 失败: " + hr_hex(hr));
    }

    // 音频主时钟（可选：拿不到就用计数回退）
    client_->GetService(IID_PPV_ARGS(&audio_clock_));

    if (event_driven) {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) {
            return Error::make(Err::AudioFailed, "CreateEvent 失败");
        }
        hr = client_->SetEventHandle(event_);
        if (FAILED(hr)) {
            return Error::make(Err::AudioFailed, "SetEventHandle 失败: " + hr_hex(hr));
        }
    }

    // 环形缓冲：4 倍设备缓冲（约 0.8s），给解码留波动空间
    ring_.store(std::make_shared<RingBuffer<float>>(static_cast<size_t>(buffer_frames_) * channels_ * 4));
    scratch_.resize(static_cast<size_t>(buffer_frames_) * channels_);

    initialized_ = true;
    ME_LOG_INFO("WASAPI 输出格式: ", sample_rate_, "Hz, ", channels_, "ch, ",
                float_out_ ? "float32" : "pcm16", ", 模式: ", event_driven ? "事件驱动" : "轮询");
    ME_LOG_INFO("WASAPI 就绪: 缓冲 ", buffer_frames_, " frames");
    return Error::success();
}

Error AudioOutput::start() {
    if (!initialized_ || running_.load()) return Error::success();
    HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        return Error::make(Err::AudioFailed, "IAudioClient::Start 失败: " + hr_hex(hr));
    }
    running_ = true;
    thread_ = std::thread(&AudioOutput::run_loop, this);
    ME_LOG_DEBUG("[audio] 回调线程已创建");
    active_ = true;
    return Error::success();
}

void AudioOutput::stop() {
    if (!running_.exchange(false)) return;
    if (event_) SetEvent(event_);  // 唤醒等待中的回调线程
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        if (client_) client_->Stop();
    }
    active_ = false;
}

void AudioOutput::shutdown() {
    stop();
    release_com();
}

void AudioOutput::pause_stream() {
    paused_.store(true);
    std::lock_guard<std::mutex> lock(client_mutex_);
    if (client_ && active_) client_->Stop();
}

void AudioOutput::resume_stream() {
    paused_.store(false);
    std::lock_guard<std::mutex> lock(client_mutex_);
    if (client_ && active_) client_->Start();
}

void AudioOutput::write(const float* samples, size_t count) {
    // 阻塞等空间：让解码节奏跟随声卡消费，而不是瞬间灌满后丢弃（否则没声音）
    if (auto r = ring_.load()) r->push_blocking(samples, count);
}


std::vector<std::string> AudioOutput::device_names() const {
    IMMDeviceEnumerator* enumerator = nullptr;
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        enumerator = enumerator_;
    }
    if (!enumerator) return {};  // 音频未初始化（无音轨文件）时返回空列表
    std::vector<std::string> names;
    Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        return names;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device)) || !device) continue;
        Microsoft::WRL::ComPtr<IPropertyStore> store;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) {
            names.push_back("设备 #" + std::to_string(i));
            continue;
        }
        PROPVARIANT var{};
        PropVariantInit(&var);
        std::string name = "设备 #" + std::to_string(i);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &var)) && var.pwszVal) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                name.resize(static_cast<size_t>(len - 1));
                WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, name.data(), len, nullptr, nullptr);
            }
        }
        PropVariantClear(&var);
        if (is_hands_free_endpoint(name)) continue;  // 免提/电话端点不出现在可选列表
        names.push_back(std::move(name));
    }
    return names;
}

std::string AudioOutput::device_name() const {
    std::lock_guard<std::mutex> lock(client_mutex_);
    return device_name_;
}

Error AudioOutput::switch_device(size_t index) {
    if (!enumerator_) return Error::make(Err::AudioFailed, "音频未初始化");
    Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) return Error::make(Err::AudioFailed, "枚举输出设备失败");
    UINT count = 0;
    collection->GetCount(&count);
    // 与 device_names() 使用同一过滤：免提/电话端点不参与索引
    std::vector<Microsoft::WRL::ComPtr<IMMDevice>> music_devices;
    for (UINT i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IMMDevice> d;
        if (FAILED(collection->Item(i, &d)) || !d) continue;
        if (is_hands_free_endpoint(endpoint_friendly_name(d.Get(), i))) continue;
        music_devices.push_back(d);
    }
    if (index >= music_devices.size()) return Error::make(Err::InvalidArgument, "设备索引越界");
    Microsoft::WRL::ComPtr<IMMDevice> device = music_devices[index];

    // 停旧流、释放旧设备，保留枚举器
    stop();
    release_audio_objects();

    // 蓝牙/虚拟设备常有瞬时失效（AUDCLNT_E_DEVICE_INVALIDATED），重试几次
    Error err = Error::make(Err::AudioFailed, "未知错误");
    for (int attempt = 1; attempt <= 3; ++attempt) {
        err = try_device(device.Get(), 48000, 2, 0.2);
        if (err.ok()) break;
        ME_LOG_WARN("[audio] 切换设备第 ", attempt, " 次失败: ", err.message());
        release_audio_objects();
        if (attempt < 3) Sleep(300);
    }
    if (!err.ok()) return err;
    if (!err.ok()) return err;

    Error serr = start();
    if (!serr.ok()) return serr;
    if (paused_.load()) {
        std::lock_guard<std::mutex> lock(client_mutex_);
        if (client_) client_->Stop();
    }
    ME_LOG_INFO("[audio] 已切换到设备: ", device_name());
    return Error::success();
}
void AudioOutput::abort_ring() {
    if (auto r = ring_.load()) r->abort();
}

Error AudioOutput::reset_stream() {
    if (!initialized_ || !client_) return Error::success();
    // 丢弃设备缓冲里 seek 前的旧音频：否则旧数据会继续播放，
    // 主时钟相对新画面产生固定偏移（音画不同步）
    if (auto r = ring_.load()) r->abort();  // 唤醒阻塞在旧环上的音频解码线程
    const size_t capacity = static_cast<size_t>(buffer_frames_) * channels_ * 4;
    std::lock_guard<std::mutex> lock(client_mutex_);
    client_->Stop();
    client_->Reset();
    played_frames_.store(0);
    ring_.store(std::make_shared<RingBuffer<float>>(capacity));
    scratch_.resize(static_cast<size_t>(buffer_frames_) * channels_);
    if (!paused_.load()) {
        HRESULT hr = client_->Start();
        if (FAILED(hr)) {
            // 部分虚拟/蓝牙设备 Reset 后立即 Start 会瞬时失败，重试一次
            Sleep(30);
            hr = client_->Start();
        }
        if (FAILED(hr)) return Error::make(Err::AudioFailed, "Reset 后重启失败: " + hr_hex(hr));
    }
    ME_LOG_INFO("[audio] 已重置设备缓冲（seek）");
    return Error::success();
}

double AudioOutput::get_played_seconds() const {
    // 主时钟用"已写帧数 - 设备缓冲水位"（即声卡真正消费的位置），
    // 而不是已写帧数：后者一直领先实际可听声音约一个设备缓冲（最多 0.2s），
    // 会造成画面稳定快于声音。
    UINT32 padding = 0;
    bool have_padding = false;
    double hw_sec = -1.0;
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        if (client_ && SUCCEEDED(client_->GetCurrentPadding(&padding))) {
            have_padding = true;
        }
        if (audio_clock_) {
            UINT64 pos = 0, qpc = 0;
            if (SUCCEEDED(audio_clock_->GetPosition(&pos, &qpc))) {
                hw_sec = static_cast<double>(pos) / 1e7;
            }
        }
    }
    const uint64_t written = played_frames_.load();
    const double counter_sec =
        have_padding && written >= padding
            ? static_cast<double>(written - padding) / sample_rate_
            : static_cast<double>(written) / sample_rate_;
    if (hw_sec >= 0.0) {
        // IAudioClock 可信度校验：部分虚拟/蓝牙驱动的 IAudioClock 位置推进异常
        // （本机实测约 4% 速度）。判定规则：
        //  1) 与帧计数位置偏差 < 0.5s；
        //  2) 推进速率与帧计数速率接近（0.5x~1.5x）。
        // 一旦判定异常，本会话不再使用 IAudioClock——否则 seek/重置后帧计数归零的瞬间，
        // 坏时钟与计数短暂"看起来一致"，主时钟会按错误速率走（画面与进度条变慢）。
        static bool hw_clock_bad = false;
        static double last_hw = -1.0, last_cnt = -1.0, last_t = 0.0;
        if (!hw_clock_bad) {
            const double now = qpc_seconds();
            if (last_hw >= 0.0 && now - last_t > 0.2) {
                const double dcnt = counter_sec - last_cnt;
                if (dcnt > 1e-6) {
                    const double ratio = (hw_sec - last_hw) / dcnt;
                    if (ratio < 0.5 || ratio > 1.5) hw_clock_bad = true;
                }
            }
            if (std::fabs(hw_sec - counter_sec) >= 0.5) hw_clock_bad = true;
            last_hw = hw_sec;
            last_cnt = counter_sec;
            last_t = now;
            if (hw_clock_bad) {
                ME_LOG_WARN("IAudioClock 不可靠，本会话禁用硬件时钟（回退帧计数）");
            } else {
                return hw_sec;
            }
        }
    }
    return counter_sec;
}

void AudioOutput::run_loop() {
    ME_LOG_DEBUG("[audio] 回调线程进入循环");
    double last_log_ = 0.0;
    size_t write_fail_count_ = 0;
    double now_ = 0.0;
    while (running_.load()) {
        if (event_) {
            const DWORD wait = WaitForSingleObject(event_, 100);
            if (!running_.load()) break;
            if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) break;
        } else {
            // 轮询模式（无事件句柄）：低频轮询设备缓冲水位
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!running_.load()) break;
        }

        if (paused_.load()) continue;  // 暂停时设备已 Stop：不写、不计数
        UINT32 padding = 0;
        HRESULT last_padding_hr_;
        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            last_padding_hr_ = client_->GetCurrentPadding(&padding);
        }
        if (FAILED(last_padding_hr_)) continue;
        const UINT32 frames = padding < buffer_frames_ ? buffer_frames_ - padding : 0;
        const auto ring = ring_.load();
        if (now_ - last_log_ > 0.9) {
            last_log_ = now_;
            ME_LOG_DEBUG("[audio] loop: padding=", padding, " frames=", frames, " ring=", ring ? ring->size() : 0u, " hr_padding=0x", hr_hex(last_padding_hr_));
        }
        if (frames == 0) continue;

        bool write_failed = false;
        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            BYTE* data = nullptr;
            HRESULT hr = render_client_->GetBuffer(frames, &data);
            if (FAILED(hr)) {
                render_client_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
                write_failed = true;
            } else {
                now_ = qpc_seconds();
                const size_t want = static_cast<size_t>(frames) * channels_;
                if (scratch_.size() < want) scratch_.resize(want);
                size_t got = ring ? ring->pop(scratch_.data(), want) : 0;
                if (got < want) std::fill(scratch_.begin() + got, scratch_.begin() + want, 0.0f);

                const float vol = volume_.load();
                if (float_out_) {
                    auto* dst = reinterpret_cast<float*>(data);
                    for (size_t i = 0; i < want; ++i) dst[i] = scratch_[i] * vol;
                } else {
                    auto* dst = reinterpret_cast<short*>(data);
                    for (size_t i = 0; i < want; ++i) {
                        float s = scratch_[i] * vol;
                        s = std::clamp(s, -1.0f, 1.0f);
                        dst[i] = static_cast<short>(s * 32767.0f);
                    }
                }

                played_frames_.fetch_add(frames);
                render_client_->ReleaseBuffer(frames, 0);
            }
        }
        if (write_failed) {
            // 设备瞬时失效：避免空转，并累计失败次数；连续失败则重置设备流自愈
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            if (++write_fail_count_ >= 30) {
                write_fail_count_ = 0;
                ME_LOG_WARN("[audio] 连续写入失败，尝试重置设备流");
                reset_stream();
            }
            continue;
        }
        write_fail_count_ = 0;
        if (now_ - last_log_ > 0.9) {
            last_log_ = now_;
            UINT64 pos = 0, qpc = 0;
            const HRESULT hr = audio_clock_ ? audio_clock_->GetPosition(&pos, &qpc) : E_FAIL;
            ME_LOG_DEBUG("[audio] 已播放帧=", played_frames_.load(), " GetPosition=", pos, " hr=", hr_hex(hr));
        }
    }
}

void AudioOutput::release_audio_objects() {
    stop();
    paused_.store(false);  // 新会话默认播放态，避免暂停状态跨会话残留导致无声
    played_frames_.store(0);  // 切换文件/设备时归零：否则新会话主时钟锚定在旧文件位置上

    if (auto r = ring_.load()) r->abort();
    if (mix_fmt_) CoTaskMemFree(mix_fmt_);
    mix_fmt_ = nullptr;
    if (event_) CloseHandle(event_);
    event_ = nullptr;
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        if (audio_clock_) audio_clock_->Release();
        if (render_client_) render_client_->Release();
        if (client_) client_->Release();
        if (device_) device_->Release();
        audio_clock_ = nullptr;
        render_client_ = nullptr;
        client_ = nullptr;
        device_ = nullptr;
    }
    ring_.store(nullptr);
    scratch_.clear();
    initialized_ = false;
    active_ = false;
}

void AudioOutput::release_com() {
    release_audio_objects();
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        if (enumerator_) enumerator_->Release();
        enumerator_ = nullptr;
    }
    CoUninitialize();
}

}  // namespace me
