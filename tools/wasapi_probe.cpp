// WASAPI 输出端点诊断探针：逐个端点打印名称/默认标志/混音格式/初始化结果。
// 用于定位"扬声器没声音/0x88890008"是设备问题还是引擎问题。
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

EXTERN_C const CLSID CLSID_MMDeviceEnumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};

static std::string hr_hex(HRESULT hr) {
    char buf[32];
    sprintf_s(buf, "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

static std::string device_name(IMMDevice* device) {
    std::string name;
    ComPtr<IPropertyStore> store;
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

static void print_mix_format(IAudioClient* client, const char* label) {
    WAVEFORMATEX* mix = nullptr;
    const HRESULT hr = client->GetMixFormat(&mix);
    if (SUCCEEDED(hr) && mix) {
        std::printf("  %s: %u Hz, %u ch, tag=0x%04X, bits=%u\n",
                    label, mix->nSamplesPerSec, mix->nChannels,
                    mix->wFormatTag, mix->wBitsPerSample);
        CoTaskMemFree(mix);
    } else {
        std::printf("  %s: GetMixFormat 失败 %s\n", label, hr_hex(hr).c_str());
    }
}

// 尝试引擎同款参数：48000/2ch/float32 + 事件驱动；失败再试系统混音格式 + 轮询模式
static void probe_init(IMMDevice* device) {
    ComPtr<IAudioClient> client;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) {
        std::printf("  Activate 失败: %s\n", hr_hex(hr).c_str());
        return;
    }
    print_mix_format(client.Get(), "MixFormat");

    WAVEFORMATEXTENSIBLE wfx = {};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = 2;
    wfx.Format.nSamplesPerSec = 48000;
    wfx.Format.wBitsPerSample = 32;
    wfx.Format.nBlockAlign = 8;
    wfx.Format.nAvgBytesPerSec = 48000 * 8;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            2000000, 0, reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);
    std::printf("  引擎格式(48k/2ch/f32+event): %s\n", hr_hex(hr).c_str());
    if (SUCCEEDED(hr)) {
        UINT32 frames = 0;
        if (SUCCEEDED(client->GetBufferSize(&frames))) {
            std::printf("  初始化成功，缓冲 %u frames\n", frames);
        }
        return;
    }

    // 回退：系统混音格式 + 轮询
    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
        ComPtr<IAudioClient> client2;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client2))) {
            hr = client2->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, mix, nullptr);
            std::printf("  回退(混音格式+轮询): %s\n", hr_hex(hr).c_str());
        }
        CoTaskMemFree(mix);
    }
}

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::printf("CoInitializeEx: %s\n", hr_hex(cohr).c_str());

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        std::printf("创建枚举器失败: %s\n", hr_hex(hr).c_str());
        return 1;
    }

    ComPtr<IMMDevice> def;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
        std::printf("系统默认输出端点: %s\n", device_name(def.Get()).c_str());
    } else {
        std::printf("GetDefaultAudioEndpoint 失败\n");
    }

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        std::printf("枚举失败: %s\n", hr_hex(hr).c_str());
        return 1;
    }
    UINT count = 0;
    collection->GetCount(&count);
    std::printf("活动渲染端点数: %u\n\n", count);

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device)) || !device) continue;
        std::string name = device_name(device.Get());
        std::printf("[#%u] %s%s\n", i, name.c_str(),
                    (def && device.Get() == def.Get()) ? "  <<默认>>" : "");
        probe_init(device.Get());
        std::printf("\n");
    }

    CoUninitialize();
    return 0;
}