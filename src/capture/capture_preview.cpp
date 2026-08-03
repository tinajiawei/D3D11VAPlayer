#include "capture/capture_preview.h"

#include <chrono>
#include <cstdio>

#include "imgui.h"
#include "capture/screen_capture.h"

namespace me {

namespace {

// 简易 BMP 写出（BGRA -> 24bit BGR 自底向上），用于回归验证采集内容
void write_bmp(const char* path, const std::vector<uint8_t>& bgra, int w, int h) {
    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f) return;
    const uint32_t row_size = static_cast<uint32_t>(w) * 3;
    const uint32_t pad = (4 - (row_size % 4)) % 4;
    const uint32_t pixel_bytes = (row_size + pad) * static_cast<uint32_t>(h);
    const uint32_t file_size = 54 + pixel_bytes;
    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    header[2] = static_cast<uint8_t>(file_size);
    header[3] = static_cast<uint8_t>(file_size >> 8);
    header[4] = static_cast<uint8_t>(file_size >> 16);
    header[5] = static_cast<uint8_t>(file_size >> 24);
    header[10] = 54;
    header[14] = 40;
    header[18] = static_cast<uint8_t>(w);
    header[19] = static_cast<uint8_t>(w >> 8);
    header[20] = static_cast<uint8_t>(w >> 16);
    header[21] = static_cast<uint8_t>(w >> 24);
    header[22] = static_cast<uint8_t>(h);
    header[23] = static_cast<uint8_t>(h >> 8);
    header[24] = static_cast<uint8_t>(h >> 16);
    header[25] = static_cast<uint8_t>(h >> 24);
    header[26] = 1;   // planes
    header[28] = 24;  // bpp
    fwrite(header, 1, sizeof(header), f);
    std::vector<uint8_t> row(row_size + pad, 0);
    for (int y = h - 1; y >= 0; --y) {  // 自底向上
        const uint8_t* src = bgra.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
        for (int x = 0; x < w; ++x) {
            row[static_cast<size_t>(x) * 3 + 0] = src[static_cast<size_t>(x) * 4 + 2];  // B
            row[static_cast<size_t>(x) * 3 + 1] = src[static_cast<size_t>(x) * 4 + 1];  // G
            row[static_cast<size_t>(x) * 3 + 2] = src[static_cast<size_t>(x) * 4 + 0];  // R
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    std::fprintf(stderr, "[screencap] 已保存验证帧: %s (%dx%d)\n", path, w, h);
}

}  // namespace

CapturePreview::~CapturePreview() {
    stop();
}

void CapturePreview::start(ID3D11Device* device, ID3D11DeviceContext* context,
                           double seconds, HWND owner) {
    if (running_.load()) return;
    running_.store(true);
    thread_ = std::thread(&CapturePreview::run_loop, this, device, context, seconds, owner);
}

void CapturePreview::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void CapturePreview::run_loop(ID3D11Device* device, ID3D11DeviceContext* context,
                              double seconds, HWND owner) {
    ScreenCapture capture;
    if (!device || !context || !capture.open(device, context)) {
        running_.store(false);
        return;
    }
    std::vector<uint8_t> bgra;
    int w = 0, h = 0;
    const auto begin = std::chrono::steady_clock::now();
    uint64_t last_saved = 0;
    while (running_.load()) {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - begin).count();
        if (elapsed >= seconds) break;

        if (capture.acquire_frame(bgra, w, h)) {
            if (upload_bgra(bgra, w, h, device, context)) {
                frames_.fetch_add(1);
            }
            // 每 30 帧存一张验证图
            const uint64_t f = capture.frames_captured();
            if (f - last_saved >= 30 || (last_saved == 0 && f >= 1)) {
                last_saved = f;
                write_bmp("capture_frame.bmp", bgra, w, h);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::fprintf(stderr, "[screencap] 采集结束: %llu 帧\n",
                 static_cast<unsigned long long>(capture.frames_captured()));
    running_.store(false);
    if (owner) PostMessageW(owner, WM_CLOSE, 0, 0);
}

bool CapturePreview::upload_bgra(const std::vector<uint8_t>& bgra, int w, int h,
                                 ID3D11Device* device, ID3D11DeviceContext* context) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!texture_ || tex_w_ != w || tex_h_ != h) {
        texture_.Reset();
        srv_.Reset();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &texture_))) return false;
        if (FAILED(device->CreateShaderResourceView(texture_.Get(), nullptr, &srv_))) return false;
        tex_w_ = w;
        tex_h_ = h;
    }
    context->UpdateSubresource(texture_.Get(), 0, nullptr, bgra.data(),
                               static_cast<UINT>(w) * 4, 0);
    return true;
}

void CapturePreview::draw(float width, float height) {
    if (!running_.load()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!srv_) return;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::Begin("capture", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
    ImGui::Image(reinterpret_cast<ImTextureID>(srv_.Get()), ImVec2(width, height));
    ImGui::End();
}

}  // namespace me