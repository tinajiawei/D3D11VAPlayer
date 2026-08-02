#include "render/d3d11_renderer.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3dcompiler.h>

#include "core/log.h"

namespace me {

namespace {

// 全屏三角形（SV_VertexID 生成，无需顶点缓冲）
const char* kVertexShaderSrc = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 xy = float2(float((id << 1) & 2), float(id & 2));
    o.pos = float4(xy * 2.0 - 1.0, 0.0, 1.0);
    // uv.y 必须翻转：D3D 的屏幕底部是 clip y=-1，而纹理顶行是 v=0，
    // 不翻转会把图像上下颠倒（经典全屏三角形坑）。
    o.uv = float2(xy.x, 1.0 - xy.y);
    return o;
}
)";

// YUV -> RGB（BT.601 limited range），NV12 时 U 纹理存 UV 交织
const char* kPixelShaderSrc = R"(
Texture2D texY : register(t0);
Texture2D texU : register(t1);
Texture2D texV : register(t2);
SamplerState smp : register(s0);

cbuffer Params : register(b0) {
    int isNv12;
    int rotate;
    int2 _pad;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 ps_main(VSOut i) : SV_Target {
    // 旋转元数据：顺时针旋转画面使其正立（90/180/270）
    float2 uv = i.uv;
    if (rotate == 90)  uv = float2(i.uv.y, 1.0 - i.uv.x);
    else if (rotate == 180) uv = float2(1.0 - i.uv.x, 1.0 - i.uv.y);
    else if (rotate == 270) uv = float2(1.0 - i.uv.y, i.uv.x);
    float y = texY.Sample(smp, uv).r;
    float u, v;
    if (isNv12 != 0) {
        float2 uv2 = texU.Sample(smp, uv).rg;
        u = uv2.r;
        v = uv2.g;
    } else {
        u = texU.Sample(smp, uv).r;
        v = texV.Sample(smp, uv).r;
    }
    float yy = (y - 16.0 / 255.0) * (255.0 / 219.0);
    float uu = (u - 128.0 / 255.0) * (255.0 / 224.0);
    float vv = (v - 128.0 / 255.0) * (255.0 / 224.0);
    float r = yy + 1.402 * vv;
    float g = yy - 0.344136 * uu - 0.714136 * vv;
    float b = yy + 1.772 * uu;
    return float4(r, g, b, 1.0);
}
)";

struct Params {
    int is_nv12 = 0;
    int rotate = 0;
    int pad[2] = {0, 0};
};

Microsoft::WRL::ComPtr<ID3D11Texture2D> MakeTexture(ID3D11Device* device, int width, int height,
                                                    DXGI_FORMAT format) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    device->CreateTexture2D(&desc, nullptr, &texture);
    return texture;
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> MakeSrv(ID3D11Device* device,
                                                         ID3D11Texture2D* texture) {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    D3D11_TEXTURE2D_DESC tex_desc{};
    texture->GetDesc(&tex_desc);
    D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format = tex_desc.Format;
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(texture, &desc, &srv);
    return srv;
}

}  // namespace

Error D3D11Renderer::init(HWND hwnd, int width, int height) {
    shutdown();
    width_ = width;
    height_ = height;

    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

    // 显式选择真实硬件 GPU：虚拟显示适配器（Oray/GameViewer 等）的驱动可能让 GPU
    // 命令队列阻塞，导致渲染几帧后永久卡死。优先选非软件、有独立显存的适配器。
    Microsoft::WRL::ComPtr<IDXGIFactory1> dxgi_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> chosen_adapter;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)))) {
        for (UINT i = 0;; ++i) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (dxgi_factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            const UINT64 vram_mb = desc.DedicatedVideoMemory / (1024 * 1024);
            const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            ME_LOG_INFO("DXGI 适配器 #", i, ": 显存=", vram_mb, "MB",
                        software ? " [软件]" : "");
            if (!software && vram_mb > 0) {
                chosen_adapter = adapter;
                break;
            }
        }
    }

    HRESULT hr = D3D11CreateDevice(chosen_adapter.Get(),
                                   chosen_adapter ? D3D_DRIVER_TYPE_UNKNOWN
                                                  : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, flags, feature_levels, 2, D3D11_SDK_VERSION,
                                   &device_, nullptr, &context_);
    if (FAILED(hr)) {
        return Error::make(Err::RenderFailed, "D3D11CreateDevice 失败: " + std::to_string(hr));
    }

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return Error::make(Err::RenderFailed, "CreateDXGIFactory1 失败");

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;

    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &desc, nullptr, nullptr,
                                         &swapchain_);
    if (FAILED(hr)) return Error::make(Err::RenderFailed, "CreateSwapChainForHwnd 失败");

    create_rtv();

    Error err = compile_shaders();
    if (!err.ok()) return err;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    device_->CreateRasterizerState(&rd, &rasterizer_);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device_->CreateSamplerState(&sd, &sampler_);

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(Params);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device_->CreateBuffer(&bd, nullptr, &params_buffer_);

    ME_LOG_INFO("D3D11 渲染器就绪: ", width, "x", height, " (flip-model)");
    return Error::success();
}

void D3D11Renderer::shutdown() {
    if (context_) context_->ClearState();
    sws_ = nullptr;
    sws_frame_.reset();
    y_tex_.Reset(); u_tex_.Reset(); v_tex_.Reset();
    y_srv_.Reset(); u_srv_.Reset(); v_srv_.Reset();
    rtv_.Reset();
    swapchain_.Reset();
    vertex_shader_.Reset();
    pixel_shader_.Reset();
    params_buffer_.Reset();
    sampler_.Reset();
    rasterizer_.Reset();
    context_.Reset();
    device_.Reset();
}

void D3D11Renderer::set_pending_size(int width, int height) {
    pending_w_.store(width);
    pending_h_.store(height);
}

Error D3D11Renderer::draw_frame(const AVFrame* frame) {
    if (!device_ || !frame) return Error::success();

    apply_pending_size();

    const AVFrame* f = normalize_format(frame);
    if (!f) return Error::make(Err::RenderFailed, "无法将帧转换为 NV12/420p");

    const auto fmt = static_cast<AVPixelFormat>(f->format);
    Error err = ensure_textures(f->width, f->height, fmt);
    if (!err.ok()) return err;
    err = upload_planes(f, fmt);
    if (!err.ok()) return err;

    // 等比适配（letterbox）：内容按宽高比居中显示，不拉伸。旋转 90/270 时内容显示宽高互换。
    int content_w = f->width;
    int content_h = f->height;
    if (frame_rotation_ == 90 || frame_rotation_ == 270) {
        std::swap(content_w, content_h);
    }
    const double window_aspect = static_cast<double>(width_) / height_;
    const double content_aspect = static_cast<double>(content_w) / content_h;
    int vx = 0, vy = 0, vw = width_, vh = height_;
    if (content_aspect > window_aspect) {
        vw = width_;
        vh = static_cast<int>(width_ / content_aspect);
        vy = (height_ - vh) / 2;
    } else {
        vh = height_;
        vw = static_cast<int>(height_ * content_aspect);
        vx = (width_ - vw) / 2;
    }

    // 视口外的黑边需要清屏

    // 视口外的黑边需要清屏
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context_->ClearRenderTargetView(rtv_.Get(), black);

    const D3D11_VIEWPORT viewport = {static_cast<float>(vx), static_cast<float>(vy),
                                     static_cast<float>(vw), static_cast<float>(vh),
                                     0.0f, 1.0f};
    context_->RSSetViewports(1, &viewport);
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);

    Params params{};
    params.is_nv12 = (fmt == AV_PIX_FMT_NV12) ? 1 : 0;
    params.rotate = frame_rotation_;
    context_->UpdateSubresource(params_buffer_.Get(), 0, nullptr, &params, 0, 0);

    ID3D11ShaderResourceView* srvs[3] = {y_srv_.Get(), u_srv_.Get(), v_srv_.Get()};
    context_->RSSetState(rasterizer_.Get());
    context_->PSSetConstantBuffers(0, 1, params_buffer_.GetAddressOf());
    context_->PSSetShaderResources(0, 3, srvs);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
    context_->Draw(3, 0);

    return Error::success();

    if (dump_requested_.exchange(false)) {
        ME_LOG_INFO("[render] dump requested");
        debug_dump("E:\\新建文件夹\\chatgpt\\build\\src\\debug_frame.bmp");
    }
}


void D3D11Renderer::debug_dump(const char* path) {
    ME_LOG_INFO("[dump] step0 enter");
    if (!context_ || !rtv_) { ME_LOG_WARN("[dump] no ctx/rtv"); return; }
    // 用交换链取后台缓冲（比 rtv_->GetResource 更稳，rtv_ 在窗口调整时可能悬垂）
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&tex))) || !tex) {

        ME_LOG_WARN("[dump] GetBuffer 失败");
        return;
    }
    D3D11_TEXTURE2D_DESC td{};
    ME_LOG_INFO("[dump] step2 after GetDesc");
    tex->GetDesc(&td);
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stage;
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, &stage))) { ME_LOG_WARN("[dump] CreateTexture2D 失败"); return; }
    ME_LOG_INFO("[dump] step3 after CreateTexture2D");
    context_->CopyResource(stage.Get(), tex.Get());
    ME_LOG_INFO("[dump] step4 after CopyResource");
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context_->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &map))) { ME_LOG_WARN("[dump] Map 失败"); return; }
    ME_LOG_INFO("[dump] step5 after Map");
    const int w = static_cast<int>(td.Width);
    const int h = static_cast<int>(td.Height);
    // 简易 24 位 BMP（自底向上行序）
    const int row_bytes = w * 3;
    const int pad = (4 - (row_bytes % 4)) % 4;
    const int data_size = (row_bytes + pad) * h;
    std::vector<uint8_t> bmp(54 + data_size, 0);
    bmp[0] = 'B'; bmp[1] = 'M';
    const uint32_t file_size = static_cast<uint32_t>(bmp.size());
    std::memcpy(&bmp[2], &file_size, 4);
    const uint32_t data_offset = 54;
    std::memcpy(&bmp[10], &data_offset, 4);
    const uint32_t dib_size = 40;
    std::memcpy(&bmp[14], &dib_size, 4);
    const int32_t bw = w, bh = h;
    std::memcpy(&bmp[18], &bw, 4);
    std::memcpy(&bmp[22], &bh, 4);
    const uint16_t planes = 1, bpp = 24;
    std::memcpy(&bmp[26], &planes, 2);
    std::memcpy(&bmp[28], &bpp, 2);
    const uint32_t compression = 0;
    std::memcpy(&bmp[30], &compression, 4);
    const uint32_t image_size = static_cast<uint32_t>(data_size);
    std::memcpy(&bmp[34], &image_size, 4);
    const uint8_t* src = static_cast<const uint8_t*>(map.pData);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<size_t>(h - 1 - y) * map.RowPitch;
        uint8_t* dst = bmp.data() + 54 + static_cast<size_t>(y) * (row_bytes + pad);
        for (int x = 0; x < w; ++x) {
            dst[x * 3 + 0] = row[x * 4 + 0];  // B
            dst[x * 3 + 1] = row[x * 4 + 1];  // G
            dst[x * 3 + 2] = row[x * 4 + 2];  // R
        }
    }
    context_->Unmap(stage.Get(), 0);
    // 中文路径必须用宽字符打开（ANSI fopen 在 GBK 系统上打不开 UTF-8 路径）
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wpath(static_cast<size_t>(wlen - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen);
    FILE* f = nullptr;
    if (_wfopen_s(&f, wpath.c_str(), L"wb") == 0 && f) {
        std::fwrite(bmp.data(), 1, bmp.size(), f);
        std::fclose(f);
        ME_LOG_INFO("[render] 画面已存为 ", path);
    }
}
Error D3D11Renderer::present_swapchain() {
    // 调试：F12 请求时导出当前画面（present 在渲染线程，任何分支都会执行）
    if (dump_requested_.exchange(false)) {
        ME_LOG_INFO("[render] dump requested");
        debug_dump("E:\\新建文件夹\\chatgpt\\build\\src\\debug_frame.bmp");
    }
    if (!swapchain_) return Error::success();
    // 不阻塞 vsync：虚拟/远程显示器可能没有正常垂直同步信号，Present(1,0) 会永久阻塞；
    // 帧率由播放器自身的延迟调度控制（docs/03），无需依赖显示器刷新。
    const HRESULT hr = swapchain_->Present(0, 0);
    if (FAILED(hr) && hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) {
        ME_LOG_DEBUG("Present 返回 ", static_cast<unsigned>(hr & 0xffff));
    }
    return Error::success();
}

void D3D11Renderer::clear_black() {
    if (!context_ || !rtv_) { ME_LOG_WARN("[dump] no ctx/rtv"); return; }
}

Error D3D11Renderer::ensure_textures(int width, int height, AVPixelFormat format) {
    if (format != AV_PIX_FMT_YUV420P && format != AV_PIX_FMT_NV12) {
        return Error::make(Err::Unsupported, "不支持的像素格式");
    }
    if (y_tex_ && tex_w_ == width && tex_h_ == height && tex_fmt_ == format) {
        return Error::success();  // 复用已有纹理
    }

    y_tex_.Reset(); u_tex_.Reset(); v_tex_.Reset();
    y_srv_.Reset(); u_srv_.Reset(); v_srv_.Reset();

    y_tex_ = MakeTexture(device_.Get(), width, height, DXGI_FORMAT_R8_UNORM);
    if (!y_tex_) return Error::make(Err::RenderFailed, "创建 Y 纹理失败");
    y_srv_ = MakeSrv(device_.Get(), y_tex_.Get());
    if (!y_srv_) return Error::make(Err::RenderFailed, "创建 Y SRV 失败");

    if (format == AV_PIX_FMT_NV12) {
        u_tex_ = MakeTexture(device_.Get(), width / 2, height / 2, DXGI_FORMAT_R8G8_UNORM);
        if (!u_tex_) return Error::make(Err::RenderFailed, "创建 UV 纹理失败");
        u_srv_ = MakeSrv(device_.Get(), u_tex_.Get());
        v_srv_ = u_srv_;  // NV12：V 分量从 UV 纹理的 g 通道取
    } else {
        u_tex_ = MakeTexture(device_.Get(), width / 2, height / 2, DXGI_FORMAT_R8_UNORM);
        v_tex_ = MakeTexture(device_.Get(), width / 2, height / 2, DXGI_FORMAT_R8_UNORM);
        if (!u_tex_ || !v_tex_) return Error::make(Err::RenderFailed, "创建 U/V 纹理失败");
        u_srv_ = MakeSrv(device_.Get(), u_tex_.Get());
        v_srv_ = MakeSrv(device_.Get(), v_tex_.Get());
    }

    tex_w_ = width;
    tex_h_ = height;
    tex_fmt_ = format;
    return Error::success();
}

Error D3D11Renderer::upload_planes(const AVFrame* frame, AVPixelFormat format) {
    if (format == AV_PIX_FMT_NV12) {
        context_->UpdateSubresource(y_tex_.Get(), 0, nullptr,
                                    frame->data[0], frame->linesize[0], 0);
        context_->UpdateSubresource(u_tex_.Get(), 0, nullptr,
                                    frame->data[1], frame->linesize[1], 0);
    } else {
        context_->UpdateSubresource(y_tex_.Get(), 0, nullptr,
                                    frame->data[0], frame->linesize[0], 0);
        context_->UpdateSubresource(u_tex_.Get(), 0, nullptr,
                                    frame->data[1], frame->linesize[1], 0);
        context_->UpdateSubresource(v_tex_.Get(), 0, nullptr,
                                    frame->data[2], frame->linesize[2], 0);
    }
    return Error::success();
}

const AVFrame* D3D11Renderer::normalize_format(const AVFrame* in) {
    const auto fmt = static_cast<AVPixelFormat>(in->format);
    if (fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_NV12) return in;

    // 其他格式（如 yuvj/rgb 等）先转成 NV12
    if (!sws_ || sws_w_ != in->width || sws_h_ != in->height || sws_in_fmt_ != fmt) {
        sws_ = sws_getContext(in->width, in->height, fmt,
                              in->width, in->height, AV_PIX_FMT_NV12,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) return nullptr;
        sws_w_ = in->width;
        sws_h_ = in->height;
        sws_in_fmt_ = fmt;
        sws_frame_.reset();
    }
    if (!sws_frame_) {
        sws_frame_ = make_frame();
        sws_frame_->format = AV_PIX_FMT_NV12;
        sws_frame_->width = in->width;
        sws_frame_->height = in->height;
        if (av_frame_get_buffer(sws_frame_.get(), 0) < 0) return nullptr;
    }
    sws_scale(sws_, in->data, in->linesize, 0, in->height,
              sws_frame_->data, sws_frame_->linesize);
    return sws_frame_.get();
}

void D3D11Renderer::apply_pending_size() {
    const int w = pending_w_.exchange(0);
    const int h = pending_h_.exchange(0);
    if (w <= 0 || h <= 0 || (w == width_ && h == height_)) return;

    context_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    const HRESULT hr = swapchain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr)) {
        width_ = w;
        height_ = h;
        create_rtv();
        ME_LOG_INFO("渲染器尺寸变化: ", w, "x", h);
    } else {
        ME_LOG_WARN("ResizeBuffers 失败: ", static_cast<unsigned>(hr & 0xffff));
    }
}

void D3D11Renderer::create_rtv() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer) {
        device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &rtv_);
    }
}

Error D3D11Renderer::compile_shaders() {
    Microsoft::WRL::ComPtr<ID3DBlob> vs_blob, ps_blob, err_blob;

    HRESULT hr = D3DCompile(kVertexShaderSrc, strlen(kVertexShaderSrc), nullptr, nullptr,
                            nullptr, "vs_main", "vs_5_0", 0, 0, &vs_blob, &err_blob);
    if (FAILED(hr)) {
        const char* msg = err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "";
        return Error::make(Err::RenderFailed, "顶点着色器编译失败: " + std::string(msg));
    }
    hr = D3DCompile(kPixelShaderSrc, strlen(kPixelShaderSrc), nullptr, nullptr, nullptr,
                    "ps_main", "ps_5_0", 0, 0, &ps_blob, &err_blob);
    if (FAILED(hr)) {
        const char* msg = err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "";
        return Error::make(Err::RenderFailed, "像素着色器编译失败: " + std::string(msg));
    }

    device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                nullptr, &vertex_shader_);
    device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                               nullptr, &pixel_shader_);
    if (!vertex_shader_ || !pixel_shader_) {
        return Error::make(Err::RenderFailed, "创建着色器对象失败");
    }
    return Error::success();
}

}  // namespace me
