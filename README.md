# MediaEngine — 学习型 C++ 媒体引擎

一个从零封装的 C++20 媒体播放引擎：FFmpeg 只负责**容器解析**与**底层解码算法**，码流处理、解码流程抽象、音视频渲染（D3D11）、音频输出（WASAPI）、A/V 同步全部自研。本项目以学习为首要目标，配套完整中文文档。

## 已实现

- 解封装：mp4 / mkv / mov / ts / flv / webm / avi / wav / mp3（FFmpeg demuxer）
- 解码：H.264 / HEVC（D3D11VA 硬解已激活）+ AV1（软解）；失败自动降级软解；AAC / MP3 / PCM / FLAC / Opus
- 解码器插件化：后端是 `plugin\1\` 下的插件 DLL（decoder_sw / decoder_d3d11va），新增后端 = 新增一个 DLL
- 引擎 DLL：media_engine.dll 导出窄 C API（api/me_api.h），UI 与引擎解耦
- 模块接口层：IRenderer / IAudioSink / ISyncEngine 三个抽象（src/api/*.h），渲染/音频/同步可整体替换，为后续 DLL 插件化打底
- 图片/GIF：PNG / JPEG / GIF / WebP 走同一条解码管线，动画由 pts 驱动
- 渲染：D3D11 flip-model 交换链 + 像素着色器做 YUV→RGB（支持 YUV420P / NV12）
- 音频：WASAPI 共享模式事件驱动输出、libswresample 重采样、音量控制、变速
- 同步：音频主时钟，视频追帧/丢帧，暂停/seek/0.25x–4x 变速
- UI：Win32 宿主窗口 + Dear ImGui 控制面板（可隐藏、可整体替换）
- 壁纸模式（一期原型）：挂到桌面 WorkerW 下铺满屏幕 + 点击穿透，Ctrl+Alt+W 或面板按钮切换

## 目录结构

```
├── docs/                  # 学习文档（00 架构 → 07 术语与路线）
├── src/
│   ├── core/              # 日志、错误、单调时钟、环形缓冲、FFmpeg RAII
│   ├── media/             # 解封装、包/帧队列、解码器抽象（软解/硬解/工厂）
│   ├── sync/              # 音画同步引擎
│   ├── audio/             # WASAPI 输出、重采样
│   ├── render/            # D3D11 渲染器
│   ├── player/            # 播放器编排（线程模型）
│   └── ui/                # Win32 窗口、ImGui 面板、PlaybackController
│   ├── api/               # 模块接口（IRenderer/IAudioSink/ISyncEngine）+ media_engine.dll 的 C API（me_*）
├── tests/                 # 单元测试（自写轻量断言）
├── samples/               # ffmpeg 生成的验收样例
└── third_party/imgui/     # Dear ImGui 源码
```

## 构建

依赖：Visual Studio 2022（MSVC）+ CMake + Ninja；FFmpeg 8.x shared 开发包（`bin/include/lib` 三件套）。

```bat
call "D:\Microsoft Visual Studio\chanppin\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFFMPEG_ROOT=E:\...\ffmpeg-8.1.2-full_build-shared
cmake --build build
```

FFmpeg 的 DLL 会自动拷到每个 exe 旁。

> ⚠️ 修改任何头文件的**成员布局**后，必须 `cmake --build build --clean-first` 全量重建；
> 本项目多次复现增量构建导致 ABI 错位（对象偏移不一致 → 随机崩溃），最严重一次是 `device_names()` 读错 `enumerator_` 偏移的 0xC0000005。

## 运行

```bat
build\src\media_player_app.exe samples\test_h264.mp4        # 直接打开文件
build\src\media_player_app.exe samples\test_hevc.mp4 --hw   # 优先硬解
```

也可以把文件拖进窗口，或点面板"打开文件"。

### 快捷键

| 键 | 功能 |
| --- | --- |
| 空格 | 播放/暂停 |
| ← / → | 后退/前进 10 秒 |
| [ / ] | 减速/加速（0.25x–4x） |
| M | 静音/恢复 |
| H | 显示/隐藏控制面板 |

### 回归/验收参数（详见 docs/08）

| 参数 | 作用 |
| --- | --- |
| `--seek <秒>` | 2s 后连跳 target→0.6×→0.8×（模拟来回拖进度条） |
| `--eof-seek <秒>` | 播到自然结束后再 seek（EOF 唤醒） |
| `--speed <倍率>` | 3s 时变速、8s 恢复 1x |
| `--pause-test` | 4s 暂停、6s 恢复 |
| `--device <索引>` | 3s 时切换扬声器（含失败回退） |
| `--reopen <秒>` | N 秒后重开同一文件（播放中拖入新文件） |
| `--wallpaper` | 2s 进入壁纸模式、7s 退出（自动回归） |
| `--hw` | 优先 D3D11VA 硬解，失败自动降级软解 |

## 测试

```bat
build\tests\media_engine_tests.exe
build\src\ffmpeg_smoke.exe   # FFmpeg 导入库与 MSVC 兼容性冒烟
build\src\wasapi_probe.exe    # WASAPI 端点诊断（扬声器无声排查）

一期验收（29 个场景：13 格式 + 10 同步场景 + 6 软硬解）：
```bat
powershell -ExecutionPolicy Bypass -File tools\acceptance.ps1
```
结果见 docs/08-一期验收报告.md 与 tools\acceptance_results.md。
```

## 学习入口

1. [docs/00-架构总览.md](docs/00-架构总览.md) — 先看数据流全景图
2. [docs/01-线程模型.md](docs/01-线程模型.md) — 生产-消费者队列
3. [docs/02-关键数据结构.md](docs/02-关键数据结构.md) — 队列/环形缓冲/时钟
4. [docs/03-时间戳与同步.md](docs/03-时间戳与同步.md) — 音画同步算法
5. [docs/04-解码器抽象.md](docs/04-解码器抽象.md) — 软解/硬解插拔
6. [docs/05-资源与生命周期.md](docs/05-资源与生命周期.md) — RAII 与线程退出
7. [docs/06-调试与排障.md](docs/06-调试与排障.md) — 真实崩溃排查案例
8. [docs/07-术语表与进阶路线.md](docs/07-术语表与进阶路线.md) — 名词与路线图
9. [docs/08-一期验收报告.md](docs/08-一期验收报告.md) — 一期验收矩阵与已知限制

## 已知限制

- 音频输出会枚举所有活动端点逐个尝试（虚拟/远程设备的默认端点常报 DEVICE_INVALIDATED）；全部失败才静音降级并提示；
- 硬解一期为"解码在 GPU、帧回读 CPU"的统一渲染路径，GPU 直通留作进阶（见 07 篇）；
- 变速的音频会变调（未做 time-stretch），这是教学取舍；
- 壁纸钉桌面、多显示器、网页嵌入属二期（见 07 篇路线图）。

## 许可证说明

- FFmpeg：gyan.dev GPL 全功能版，个人学习无碍；分发需按 GPL 要求处理
- Dear ImGui：MIT
- 本项目代码：学习用途
