#include "capture/loopback_capture.h"

#include <algorithm>
#include <cstdio>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <wrl/client.h>

namespace me {

LoopbackCapture::~LoopbackCapture() {
    close();
}

bool LoopbackCapture::open(int sample_rate, int channels) {
    close();
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    channels_ = channels > 0 ? channels : 2;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // 已初始化则忽略

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator_));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[loopcap] 枚举器创建失败: 0x%08X\n", static_cast<unsigned>(hr));
        return false;
    }
    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia, &device_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[loopcap] 获取默认输出端点失败: 0x%08X\n", static_cast<unsigned>(hr));
        close();
        return false;
    }

    {
        WAVEFORMATEX* probe_fmt = nullptr;
        IAudioClient* probe_client = nullptr;
        if (SUCCEEDED(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(&probe_client))) &&
            SUCCEEDED(probe_client->GetMixFormat(&probe_fmt))) {
            const bool telephony = probe_fmt->nSamplesPerSec < 32000;
            CoTaskMemFree(probe_fmt);
            probe_client->Release();
            if (telephony) {
                device_->Release();
                device_ = nullptr;
                Microsoft::WRL::ComPtr<IMMDeviceCollection> coll;
                if (SUCCEEDED(enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) {
                    UINT count = 0;
                    coll->GetCount(&count);
                    for (UINT i = 0; i < count; ++i) {
                        IMMDevice* cand = nullptr;
                        if (FAILED(coll->Item(i, &cand))) continue;
                        IAudioClient* ac = nullptr;
                        WAVEFORMATEX* mf = nullptr;
                        if (SUCCEEDED(cand->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                    reinterpret_cast<void**>(&ac))) &&
                            SUCCEEDED(ac->GetMixFormat(&mf)) &&
                            mf->nSamplesPerSec >= 44100) {
                            device_ = cand;
                            CoTaskMemFree(mf);
                            ac->Release();
                            std::fprintf(stderr, "[loopcap] 已切换到非电话类端点 #%u\n", i);
                            break;
                        }
                        if (mf) CoTaskMemFree(mf);
                        if (ac) ac->Release();
                        cand->Release();
                    }
                }
                if (!device_) {
                    std::fprintf(stderr, "[loopcap] 找不到可用端点\n");
                    close();
                    return false;
                }
            }
        }
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&client_));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[loopcap] IAudioClient 激活失败: 0x%08X\n", static_cast<unsigned>(hr));
        close();
        return false;
    }

    WAVEFORMATEX* mix_fmt = nullptr;
    hr = client_->GetMixFormat(&mix_fmt);
    if (FAILED(hr)) {
        close();
        return false;
    }
    sample_rate_ = static_cast<int>(mix_fmt->nSamplesPerSec);
    channels_ = static_cast<int>(mix_fmt->nChannels);
    float_out_ = mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;

    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             0, 0, mix_fmt, nullptr);
    CoTaskMemFree(mix_fmt);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[loopcap] 回环初始化失败: 0x%08X\n", static_cast<unsigned>(hr));
        close();
        return false;
    }

    hr = client_->GetService(IID_PPV_ARGS(&capture_));
    if (FAILED(hr)) {
        close();
        return false;
    }
    client_->GetBufferSize(&buffer_frames_);
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        close();
        return false;
    }
    hr = client_->SetEventHandle(event_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[loopcap] SetEventHandle 失败: 0x%08X\n", static_cast<unsigned>(hr));
        close();
        return false;
    }

    std::fprintf(stderr, "[loopcap] 回环采集就绪: %d Hz, %d ch\n", sample_rate_, channels_);
    return true;
}

void LoopbackCapture::close() {
    stop();
    if (capture_) { capture_->Release(); capture_ = nullptr; }
    if (client_) { client_->Release(); client_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
    if (event_) { CloseHandle(event_); event_ = nullptr; }
}

bool LoopbackCapture::start() {
    if (running_ || !client_) return false;
    running_.store(true);
    thread_ = std::thread(&LoopbackCapture::run_loop, this);
    return true;
}

void LoopbackCapture::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (event_) SetEvent(event_);
    if (thread_.joinable()) thread_.join();
}

std::vector<float> LoopbackCapture::take_samples() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<float> out;
    out.swap(samples_);
    return out;
}

void LoopbackCapture::run_loop() {
    const HRESULT start_hr = client_->Start();
    if (FAILED(start_hr)) {
        std::fprintf(stderr, "[loopcap] Start 失败: 0x%08X\n", static_cast<unsigned>(start_hr));
        running_.store(false);
        return;
    }
    while (running_.load()) {
        // 事件驱动优先，超时也轮询读取（部分端点事件不可靠，与音频输出同一回退策略）
        WaitForSingleObject(event_, 500);
        UINT32 packet = 0;
        while (capture_->GetNextPacketSize(&packet) == S_OK && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            const HRESULT hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            if (frames > 0 && (data || silent)) {  // SILENT 缓冲也是有效采样（静音）
                std::vector<float> chunk(static_cast<size_t>(frames) * channels_);
                if (silent || !data) {
                    // 闈欓煶锛氬～涓?0
                } else if (float_out_) {
                    std::copy_n(reinterpret_cast<const float*>(data), chunk.size(), chunk.data());
                } else {
                    const auto* pcm16 = reinterpret_cast<const int16_t*>(data);
                    for (size_t i = 0; i < chunk.size(); ++i) {
                        chunk[i] = static_cast<float>(pcm16[i]) / 32768.0f;
                    }
                }
                std::lock_guard<std::mutex> lock(mutex_);
                samples_.insert(samples_.end(), chunk.begin(), chunk.end());
                total_samples_.fetch_add(chunk.size());
            }
            capture_->ReleaseBuffer(frames);
        }
        if (total_samples_.load() == 0) {
            std::fprintf(stderr, "[loopcap] 尚无数据包（端点可能空闲）\\n");
            break;
        }
    }
    client_->Stop();
}

}  // namespace me