#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

namespace me {

// 屏幕采集实时预览（docs/11 二期 M6）：
// start() 启动采集线程（DXGI Desktop Duplication），每帧把 BGRA 上传为 D3D11 纹理；
// draw() 由渲染线程（present 回调）调用，用 ImGui::Image 全屏显示。
class CapturePreview {
public:
    ~CapturePreview();

    // owner：采集结束后向其发送 WM_CLOSE 退出应用（回归模式）
    void start(ID3D11Device* device, ID3D11DeviceContext* context, double seconds, HWND owner);
    void stop();

    bool active() const { return running_.load(); }
    uint64_t frames() const { return frames_.load(); }

    // 渲染线程调用：全屏绘制当前帧
    void draw(float width, float height);

private:
    void run_loop(ID3D11Device* device, ID3D11DeviceContext* context, double seconds, HWND owner);
    bool upload_bgra(const std::vector<uint8_t>& bgra, int w, int h,
                     ID3D11Device* device, ID3D11DeviceContext* context);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_{0};
    std::mutex mutex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    int tex_w_ = 0;
    int tex_h_ = 0;
};

}  // namespace me