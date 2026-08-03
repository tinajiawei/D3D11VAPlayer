#include "capture/screen_capture.h"

#include <cstdio>
#include <cstring>

namespace me {

bool ScreenCapture::open(ID3D11Device* device, ID3D11DeviceContext* context, UINT output_index) {
    close();
    if (!device || !context) return false;
    device_ = device;
    context_ = context;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
        FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter->EnumOutputs(output_index, &output)) ||
        FAILED(output.As(&output1))) {
        std::fprintf(stderr, "[screencap] 获取输出设备失败\n");
        close();
        return false;
    }

    const HRESULT hr = output1->DuplicateOutput(device_, &duplication_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[screencap] DuplicateOutput 失败: 0x%08X\n", static_cast<unsigned>(hr));
        close();
        return false;
    }
    std::fprintf(stderr, "[screencap] 屏幕采集就绪（output #%u）\n", output_index);
    return true;
}

void ScreenCapture::close() {
    duplication_.Reset();
    staging_.Reset();
    device_ = nullptr;
    context_ = nullptr;
    width_ = 0;
    height_ = 0;
}

bool ScreenCapture::acquire_frame(std::vector<uint8_t>& bgra, int& width, int& height) {
    if (!duplication_ || !device_ || !context_) return false;

    DXGI_OUTDUPL_FRAME_INFO info = {};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    const HRESULT hr = duplication_->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) {
        // 桌面变化/会话切换会返回 ACCESS_LOST：重建复制即可
        std::fprintf(stderr, "[screencap] AcquireNextFrame 失败: 0x%08X\n", static_cast<unsigned>(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) {
        duplication_->ReleaseFrame();
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    width = static_cast<int>(desc.Width);
    height = static_cast<int>(desc.Height);

    // 准备 CPU 可读的 staging 纹理
    if (!staging_ || width_ != width || height_ != height) {
        staging_.Reset();
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = desc.Width;
        sd.Height = desc.Height;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = desc.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&sd, nullptr, &staging_))) {
            duplication_->ReleaseFrame();
            return false;
        }
        width_ = width;
        height_ = height;
    }

    context_->CopyResource(staging_.Get(), texture.Get());
    duplication_->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    const size_t row_bytes = static_cast<size_t>(width) * 4;
    bgra.resize(row_bytes * static_cast<size_t>(height));
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < height; ++y) {
        std::memcpy(bgra.data() + row_bytes * static_cast<size_t>(y),
                    src + mapped.RowPitch * static_cast<size_t>(y), row_bytes);
    }
    context_->Unmap(staging_.Get(), 0);
    frames_.fetch_add(1);
    return true;
}

}  // namespace me