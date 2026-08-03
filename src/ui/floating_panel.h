#pragma once

#include <atomic>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "ui/control_panel.h"
#include "ui/panel_window.h"

struct ImGuiContext;

namespace me {

// 壁纸模式浮层控制面板（docs/11 二期 M2）：
// 独立置顶窗口 + 独立 ImGui context + 独立 DXGI 交换链。
// 线程模型：主线程只调用 request_create/request_destroy 发请求；
// 实际创建/销毁/绘制全部在引擎渲染线程执行（render()），
// 避免主线程与渲染线程同时触碰 D3D11 immediate context / ImGui context。
class FloatingPanel {
public:
    // 主线程调用（进入/退出壁纸模式时）
    void request_create(HINSTANCE instance, ID3D11Device* device, ID3D11DeviceContext* context);
    void request_destroy();

    // 渲染线程每帧调用：先执行挂起的创建/销毁，再绘制面板并 Present
    void render(ControlPanel& panel, PlaybackController& controller, PanelRequest& requests,
                bool* show_panel);

    // 兜底销毁：调用方必须保证渲染线程已停止（进程退出时）
    void destroy_now();

    bool active() const { return window_.valid(); }
    // 是否有挂起的创建/销毁请求（present 回调据此决定是否调用 render）
    bool pending() const { return create_pending_.load() || destroy_pending_.load(); }

private:
    bool create_impl(HINSTANCE instance, ID3D11Device* device, ID3D11DeviceContext* context);
    void destroy_impl();

    PanelWindow window_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    ImGuiContext* panel_ctx_ = nullptr;
    ImGuiContext* main_ctx_ = nullptr;
    ID3D11DeviceContext* device_ctx_ = nullptr;
    int width_ = 380;
    int height_ = 540;

    // 主线程写入、渲染线程消费的请求
    std::atomic<bool> create_pending_{false};
    std::atomic<bool> destroy_pending_{false};
    HINSTANCE pending_instance_ = nullptr;
    ID3D11Device* pending_device_ = nullptr;
    ID3D11DeviceContext* pending_context_ = nullptr;
};

}  // namespace me