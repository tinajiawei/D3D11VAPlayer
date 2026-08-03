#pragma once

#include <atomic>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include "core/av_utils.h"
#include "api/irenderer.h"

namespace me {

// D3D11 渲染器：flip-model 交换链 + YUV→RGB 像素着色器。
// 渲染线程是唯一的 D3D11 使用者（draw_frame 与 present_swapchain 分离，
// 让上层可以插入 ImGui 等叠加绘制后再 Present）。
class D3D11Renderer : public IRenderer {
public:
    Error init(HWND hwnd, int width, int height) override;
    void shutdown() override;

    // 主线程请求缩放（WM_SIZE），渲染线程在下一帧应用
    void set_pending_size(int width, int height) override;

    // 把一帧画进后台缓冲（不 Present）
    Error draw_frame(const AVFrame* frame) override;

    // 提交后台缓冲到屏幕（vsync）
    Error present_swapchain() override;

    void clear_black();
    // 调试：把当前后台缓冲读回并写成 BMP（F12 触发，验证画面用）
    void debug_dump(const char* path);
    // 请求渲染线程在下一帧导出画面（D3D11 上下文只能在渲染线程使用）
    void request_dump() { dump_requested_.store(true); }

    // 视频旋转（0/90/180/270）：手机竖拍文件需要旋转画面显示
    void set_frame_rotation(int rotation) override { frame_rotation_ = rotation; }

    bool is_ready() const override { return device_ != nullptr; }
    void* device() const override { return device_.Get(); }
    void* context() const override { return context_.Get(); }
    int width() const override { return width_; }
    int height() const override { return height_; }

private:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    Error create_resources(HWND hwnd, int width, int height);
    Error ensure_textures(int width, int height, AVPixelFormat format);
    Error upload_planes(const AVFrame* frame, AVPixelFormat format);
    const AVFrame* normalize_format(const AVFrame* in);
    void apply_pending_size();
    void create_rtv();
    Error compile_shaders();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11Buffer> params_buffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> rasterizer_;  // 关闭背面剔除（全屏三角形）
    ComPtr<ID3D11Texture2D> y_tex_, u_tex_, v_tex_;
    ComPtr<ID3D11ShaderResourceView> y_srv_, u_srv_, v_srv_;

    SwsContext* sws_ = nullptr;      // 非 420p/NV12 输入转 NV12 用
    AvFramePtr sws_frame_;
    int sws_w_ = 0, sws_h_ = 0;
    AVPixelFormat sws_in_fmt_ = AV_PIX_FMT_NONE;

    std::atomic<int> pending_w_{0}, pending_h_{0};
    int width_ = 0, height_ = 0;
    int tex_w_ = 0, tex_h_ = 0;
    int frame_rotation_ = 0;
    std::atomic<bool> dump_requested_{false};
    AVPixelFormat tex_fmt_ = AV_PIX_FMT_NONE;
};

}  // namespace me
