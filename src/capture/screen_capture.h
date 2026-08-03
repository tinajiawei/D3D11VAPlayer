#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace me {

// DXGI Desktop Duplication 屏幕采集（docs/11 二期 M6）：
// DuplicateOutput 拿到桌面纹理，AcquireNextFrame 取帧后拷回 CPU（BGRA8）。
// 这是经典的 Windows 屏幕采集 API（WGC 是它的现代替代，后续可换）。
class ScreenCapture {
public:
    bool open(ID3D11Device* device, ID3D11DeviceContext* context, UINT output_index = 0);
    void close();
    bool is_open() const { return duplication_ != nullptr; }

    // 抓一帧（非阻塞）：返回 true 时 bgra 为 BGRA8（stride = width*4）
    bool acquire_frame(std::vector<uint8_t>& bgra, int& width, int& height);

    int width() const { return width_; }
    int height() const { return height_; }
    uint64_t frames_captured() const { return frames_.load(); }

private:
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    std::atomic<uint64_t> frames_{0};
};

}  // namespace me