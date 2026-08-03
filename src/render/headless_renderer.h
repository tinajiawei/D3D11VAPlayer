#pragma once

#include <atomic>

#include "api/irenderer.h"

namespace me {

// 无头渲染桩（IRenderer 的测试实现，docs/11 二期 M1）：
// 不创建窗口/交换链/GPU 资源，只统计 draw/present 次数并记录尺寸。
// 用途：CI / 单元测试 / --headless 无窗口回归，验证编排层不依赖具体渲染后端。
class HeadlessRenderer : public IRenderer {
public:
    Error init(HWND hwnd, int width, int height) override;
    void shutdown() override;
    void set_pending_size(int width, int height) override;
    Error draw_frame(const AVFrame* frame) override;
    Error present_swapchain() override;
    void set_frame_rotation(int rotation) override { frame_rotation_.store(rotation); }
    bool is_ready() const override { return true; }
    void* device() const override { return nullptr; }
    void* context() const override { return nullptr; }
    int width() const override { return width_.load(); }
    int height() const override { return height_.load(); }

    // 桩统计（供测试断言）
    uint64_t draw_count() const { return draw_count_.load(); }
    uint64_t present_count() const { return present_count_.load(); }
    int frame_rotation() const { return frame_rotation_.load(); }

private:
    std::atomic<int> width_{0};
    std::atomic<int> height_{0};
    std::atomic<int> frame_rotation_{0};
    std::atomic<uint64_t> draw_count_{0};
    std::atomic<uint64_t> present_count_{0};
};

}  // namespace me