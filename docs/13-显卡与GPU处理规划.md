# 13 显卡与 GPU 处理规划（GPU 直通 + 多厂商后端）

> 目标：把"解码在 GPU、拷贝回 CPU、再上传渲染"改成"全程 GPU"，并让 AMD / NVIDIA / Intel 显卡都能选到合适后端。
> 当前机器：桌面 AMD RX 6700 XT；笔记本 NVIDIA。

## 一、现状盘点

| 环节 | 现状 | 问题 |
| --- | --- | --- |
| 硬解 | D3D11VA / AMF 插件（FFmpeg hwaccel） | 帧经 av_hwframe_transfer_data 拷回 CPU（NV12），再上传 GPU 渲染，一次来回拷贝 |
| 10bit | HEVC Main 10 解码为 P010，渲染前 swscale 降 8bit | 丢失色深，HDR 无法直出 |
| 设备 | 解码器插件与渲染器**各自创建** D3D11 设备 | GPU 直通的前提是"同一设备"，当前不满足 |
| 后端 | 工厂按 d3d11va → amf → sw 顺序尝试 | NVIDIA 笔记本无 AMF；未按显卡品牌选择；多 GPU 未选适配器 |

## 二、目标架构：全程 GPU（零拷贝）

```
GPU 解码（纹理） → 帧队列持有纹理引用 → 渲染器直接采样 → 交换链
                     （不再 av_hwframe_transfer_data 到 CPU）
```

关键改动：

1. **共享 D3D11 设备**：引擎启动时创建一个设备（可由 renderer 插件创建并暴露），解码器插件通过注入的 `AVBufferRef* hw_device_ctx` 使用同一设备；渲染器用同一 `ID3D11Device`。
   - C API：`me_create_player_ex` 增加"设备策略"选项（自动选择 / 指定 GPU 索引）。
   - 插件 ABI：解码器 `open()` 增加可选 `hw_device_ctx` 参数；渲染器插件工厂返回设备供引擎注入解码器。
2. **解码器输出纹理**：硬解帧以 `ID3D11Texture2D*`（或 hw_frames_ctx 帧）进入帧队列，引用计数管理生命周期；软解帧仍走 AVFrame 路径。
3. **渲染器纹理采样**：`IRenderer` 增加 `draw_hw_texture(void* texture, int w, int h, int64_t pts)`，着色器直接绑定解码纹理（NV12/P010）；与现有 `draw_frame(AVFrame*)` 并存，按帧类型分流。
4. **10bit/HDR 直出**：P010 纹理由着色器直接采样（Y 平面 R16、UV 平面 R16G16 → RGB10/浮点），不再降 8bit。

## 三、多厂商后端矩阵

| 显卡 | 后端 | FFmpeg 类型 | 说明 |
| --- | --- | --- | --- |
| AMD | D3D11VA（通用） | D3D11VA | 兜底，已实现 |
| AMD | AMF | AMF | AMD 专有，已实现（amfrt64.dll 检测） |
| NVIDIA | NVDEC | CUDA（h264_nvdec/hevc_nvdec） | 新增 decoder_nvdec.dll，走 FFmpeg CUDA/NVDEC |
| NVIDIA | D3D11VA（通用） | D3D11VA | N 卡也支持，作为备选 |
| Intel | QSV | QSV | 预留 decoder_qsv.dll（需要 Intel 驱动） |
| 任意 | 软解 | — | 最终兜底 |

**后端选择策略**（按厂商自动排序，可用 `ME_HW_BACKEND` 强制）：
1. 枚举 DXGI 适配器，读取厂商（AMD/NVIDIA/Intel）、名称、显存、是否 WARP。
2. 根据首选适配器选择后端顺序：
   - AMD → amf → d3d11va → sw
   - NVIDIA → nvdec → d3d11va → sw
   - Intel → qsv → d3d11va → sw
3. 多 GPU（笔记本核显+独显）：`ME_GPU_INDEX` 环境变量或面板下拉指定；默认选**性能最高**的适配器（显存/名称启发式）。

## 四、实施阶段

### 阶段 A：共享设备 + GPU 直通（D3D11VA）
- 引擎级共享 D3D11 设备（renderer 插件创建并注入解码器）。
- D3D11VA 解码器输出纹理帧，帧队列改持纹理引用。
- 渲染器 `draw_hw_texture` + NV12 纹理采样。
- 软解/硬解并存分流。
- 验收：AMD 机器 4K HEVC 硬解播放，任务管理器 CPU 占用显著下降，无 CPU 拷贝日志。

### 阶段 B：NVDEC + 适配器选择
- `decoder_nvdec` 插件（FFmpeg CUDA，h264_nvdec/hevc_nvdec，输出 D3D11 纹理需 CUDA-D3D11 interop，或先回退 CPU 拷贝做通再直通）。
- 适配器枚举与选择（DXGI），`ME_GPU_INDEX`、面板 GPU 选择。
- 笔记本双显卡（核显 + N 卡）实测：选 N 卡解码、同一渲染设备。
- 验收：笔记本 N 卡 4K H.264/HEVC 硬解流畅；切换 GPU 后端日志清晰。

### 阶段 C：10bit/HDR 直出
- 渲染器着色器支持 P010（10bit）纹理采样。
- 解码器直通路径保留 P010，不再 swscale。
- 验收：10bit HEVC 画面无 8bit 色带；面板显示"10bit"。

### 阶段 D：QSV/AMF 直通 + 设备热切换
- QSV 插件（Intel 预留）。
- AMF 直通（AMF 输出 D3D11 纹理）。
- 显卡热插拔/驱动重置后的设备重建（渲染器设备丢失恢复）。
- GPU 信息面板（厂商/显存/后端/占用）。

## 五、风险与技术要点

1. **同一设备是硬前提**：解码器与渲染器设备不一致时只能回退 CPU 拷贝路径（保留现有 transfer 路径作兜底）。
2. **纹理生命周期**：帧队列持 ID3D11Texture2D 引用（AddRef/Release），渲染完成才释放；避免 GPU 资源泄漏。
3. **CUDA-D3D11 interop**：NVDEC 输出 CUDA 帧，直通渲染需要 CUDA Graphics Resource 与 D3D11 纹理互通；复杂度较高，可先做"NVDEC 解码 + CPU 拷贝"（阶段 B 第一版），直通后续。
4. **10bit 着色器**：P010 采样用 R16/R16G16 纹理，shader 转 RGB 注意范围（limited range）与色调映射（HDR 暂以直通显示为准）。
5. **插件 ABI 演进**：IDecoder::open 增参、IRenderer 增方法 = ABI 变化，必须升 `plugin\<ver>` 目录（renderer 插件到 plugin\2 保持，解码器插件接口变更时升 plugin\2 或统一版本）。
6. **回退策略**：任何直通路径失败（设备不一致/纹理格式不支持）自动走现有 AVFrame 路径，保证"永远能播"。

## 六、验收矩阵

| 场景 | 期望 |
| --- | --- |
| AMD 桌面 4K HEVC 10bit | AMF/D3D11VA 直通，CPU 占用 < 20%，10bit 无降级 |
| NVIDIA 笔记本 4K H.264/HEVC | NVDEC 硬解，GPU 选择 N 卡，流畅 |
| 核显笔记本（无独显） | D3D11VA/QSV 回退，不崩溃 |
| 双显卡热切换 | 面板可指定 GPU，切换后播放不中断 |
| 软解兜底 | 任意直通失败自动软解，播放不中断 |

## 七、下一步（立即动手项）

1. 阶段 A-1：**共享设备**（renderer 插件暴露设备 → me_api 注入解码器）——所有后续的基础。
2. 阶段 A-2：D3D11VA 纹理直通 + 帧队列纹理化。
3. 阶段 B-1：decoder_nvdec 插件（先 CPU 拷贝跑通 N 卡硬解）。