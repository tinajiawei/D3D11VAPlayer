#pragma once

#include <windows.h>

#include "core/av_utils.h"
#include "core/error.h"

namespace me {

// 渲染器接口（参考 515 的 IRDMediaRender：接口先行，实现可插拔）。
// 一期实现：D3D11Renderer；未来 WebView2 / 壁纸专用 / 空渲染（无头）都实现它。
class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual Error init(HWND hwnd, int width, int height) = 0;
    virtual void shutdown() = 0;
    virtual void set_pending_size(int width, int height) = 0;
    virtual Error draw_frame(const AVFrame* frame) = 0;
    virtual Error present_swapchain() = 0;
    virtual void set_frame_rotation(int rotation) = 0;
    virtual bool is_ready() const = 0;
    virtual void* device() const = 0;    // ID3D11Device*（具体后端类型由实现决定）
    virtual void* context() const = 0;   // ID3D11DeviceContext*
    virtual int width() const = 0;
    virtual int height() const = 0;
};

}  // namespace me