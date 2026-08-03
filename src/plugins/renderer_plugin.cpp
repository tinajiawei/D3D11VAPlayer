#include "api/renderer_plugin.h"

#include <cstdio>
#include <cstring>

#include "render/d3d11_renderer.h"

namespace {

const char* copy_error(const me::Error& err, char* error_buf, int error_buf_size) {
    if (error_buf && error_buf_size > 0) {
        std::snprintf(error_buf, static_cast<size_t>(error_buf_size), "%s", err.message().c_str());
    }
    return nullptr;
}

}  // namespace

extern "C" {

ME_RENDERER_API int me_renderer_abi_version(void) {
    return ME_RENDERER_ABI_VERSION;
}

ME_RENDERER_API void* me_renderer_create(int type, void* hwnd, int width, int height,
                                         char* error_buf, int error_buf_size) {
    if (type != ME_RENDERER_TYPE_D3D11) {
        if (error_buf && error_buf_size > 0) {
            std::snprintf(error_buf, static_cast<size_t>(error_buf_size),
                          "renderer type %d not supported by this plugin", type);
        }
        return nullptr;
    }
    auto* renderer = new me::D3D11Renderer();
    const me::Error err = renderer->init(static_cast<HWND>(hwnd), width, height);
    if (!err.ok()) {
        copy_error(err, error_buf, error_buf_size);
        delete renderer;
        return nullptr;
    }
    return renderer;
}

ME_RENDERER_API void me_renderer_destroy(void* renderer) {
    if (!renderer) return;
    auto* r = static_cast<me::D3D11Renderer*>(renderer);
    r->shutdown();
    delete r;
}

}  // extern "C"