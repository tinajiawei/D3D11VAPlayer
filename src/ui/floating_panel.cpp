#include "ui/floating_panel.h"

#include <cstdio>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

namespace me {

void FloatingPanel::request_create(HINSTANCE instance, ID3D11Device* device,
                                   ID3D11DeviceContext* context) {
    if (!device || !context) return;
    if (window_.valid()) return;
    pending_instance_ = instance;
    pending_device_ = device;
    pending_context_ = context;
    create_pending_.store(true);
}

void FloatingPanel::request_destroy() {
    destroy_pending_.store(true);
}

void FloatingPanel::destroy_now() {
    destroy_pending_.store(false);
    create_pending_.store(false);
    destroy_impl();
}

bool FloatingPanel::create_impl(HINSTANCE instance, ID3D11Device* device,
                                ID3D11DeviceContext* context) {
    if (window_.valid()) return true;

    if (!window_.create(instance, width_, height_)) return false;

    // DXGI flip-model 交换链（与主设备同一 adapter）
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
        FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        window_.destroy();
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(device, window_.handle(), &desc, nullptr, nullptr,
                                               &swapchain_))) {
        window_.destroy();
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) ||
        FAILED(device->CreateRenderTargetView(back_buffer.Get(), nullptr, &rtv_))) {
        window_.destroy();
        return false;
    }

    // 面板专用 ImGui context（与主 context 完全隔离）
    main_ctx_ = ImGui::GetCurrentContext();
    panel_ctx_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(panel_ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // 面板位置固定，不写 ini
    const ImFontConfig font_cfg{};
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18.0f,
                                 &font_cfg, io.Fonts->GetGlyphRangesChineseFull());
    ImGui_ImplWin32_Init(window_.handle());
    ImGui_ImplDX11_Init(device, context);
    ImGui::SetCurrentContext(main_ctx_);
    window_.set_imgui_context(panel_ctx_);  // 面板输入框需要键盘事件

    device_ctx_ = context;
    std::fprintf(stderr, "[panel] 浮层控制面板已创建 %dx%d\n", width_, height_);
    return true;
}

void FloatingPanel::destroy_impl() {
    if (panel_ctx_) {
        ImGui::SetCurrentContext(panel_ctx_);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(panel_ctx_);
        panel_ctx_ = nullptr;
        // 关键：销毁面板 context 后必须切回主 context，否则当前 context 悬空
        if (main_ctx_) ImGui::SetCurrentContext(main_ctx_);
    }
    swapchain_.Reset();
    rtv_.Reset();
    window_.destroy();
    main_ctx_ = nullptr;
    device_ctx_ = nullptr;
    std::fprintf(stderr, "[panel] 浮层控制面板已销毁\n");
}

void FloatingPanel::render(ControlPanel& panel, PlaybackController& controller,
                           PanelRequest& requests, bool* show_panel) {
    // 先执行挂起的创建/销毁（渲染线程独占 DXGI/ImGui 资源）
    if (create_pending_.exchange(false)) {
        if (!create_impl(pending_instance_, pending_device_, pending_context_)) {
            std::fprintf(stderr, "[panel] 浮层控制面板创建失败\n");
        }
    }
    if (destroy_pending_.exchange(false)) {
        destroy_impl();
    }

    if (!panel_ctx_ || !device_ctx_ || !swapchain_) return;

    // 面板窗口创建在渲染线程：它的消息也由渲染线程泵送
    MSG msg;
    while (PeekMessageW(&msg, window_.handle(), 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 隐藏控制面板时同步隐藏浮层窗口，避免留下黑框并挡住鼠标
    if (show_panel && !*show_panel) {
        if (window_.valid() && IsWindowVisible(window_.handle())) ShowWindow(window_.handle(), SW_HIDE);
        return;
    }
    if (window_.valid() && !IsWindowVisible(window_.handle())) ShowWindow(window_.handle(), SW_SHOW);

    ImGui::SetCurrentContext(panel_ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(static_cast<float>(window_.mouse_x()),
                        static_cast<float>(window_.mouse_y()));
    io.AddMouseButtonEvent(0, window_.left_button_down());
    io.AddMouseWheelEvent(0.0f, static_cast<float>(window_.take_mouse_wheel()));

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    panel.draw(controller, requests, show_panel);
    ImGui::Render();

    device_ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    const float clear[4] = {0.08f, 0.08f, 0.10f, 1.0f};
    device_ctx_->ClearRenderTargetView(rtv_.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapchain_->Present(1, 0);

    ImGui::SetCurrentContext(main_ctx_);
}

}  // namespace me