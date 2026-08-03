#include "render/headless_renderer.h"

namespace me {

Error HeadlessRenderer::init(HWND, int width, int height) {
    width_.store(width);
    height_.store(height);
    return Error::success();
}

void HeadlessRenderer::shutdown() {}

void HeadlessRenderer::set_pending_size(int width, int height) {
    width_.store(width);
    height_.store(height);
}

Error HeadlessRenderer::draw_frame(const AVFrame*) {
    draw_count_.fetch_add(1);
    return Error::success();
}

Error HeadlessRenderer::present_swapchain() {
    present_count_.fetch_add(1);
    return Error::success();
}

}  // namespace me