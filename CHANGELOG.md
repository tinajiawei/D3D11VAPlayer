# 版本记录

## v0.2.10（2026-08）— seek 卡顿修复（切换文件 + 拖动进度条）
- 修复切换文件后音频“已播位置”未归零：新文件首帧对齐不再出现 audio_offset=-219s 这类错误锚定
- 音频起播等待上限 500ms → 1.5s；超时后回退单调时钟（从 seek 目标起算），音频帧到达时自动重新锚定，不再用错误偏移继续走
- 进度条 seek 改为主线程异步执行：4K 中段 seek 的 avformat_seek_file 不再阻塞渲染线程/面板

## v0.2.9（2026-08）— 文件夹序列播放（提前加入）
- 打开媒体后自动扫描其所在目录（含子文件夹），按类型生成序列：视频 / 图片 / 全部
- 面板新增“序列播放”区：扫描类型选择、上/下一个、当前序号、播放完自动下一个
- 壁纸模式下同样可用：图片/视频轮播（自动下一个）
- 序列按 Windows 自然排序（StrCmpLogicalW）；当前文件类型与过滤不符时仍插入列表
- 新增 MediaSequence 模块与单元测试（递归扫描/过滤/边界）

## v0.2.8（2026-08）— 壁纸模式控制面板交互修复
- 浮层控制面板支持直接拖入文件更换壁纸（壁纸模式下主窗口点击穿透，拖放原来会被桌面吃掉，需点两次按钮）
- 壁纸按钮按状态显示“壁纸模式 / 退出壁纸模式”，避免误点退出
- 壁纸模式下“打开文件”对话框改挂到浮层面板，不再跑到桌面图标后面
- 面板显示提示：壁纸模式下可直接拖图片/视频到面板更换

## v0.2.7（2026-08）— 高 DPI 壁纸分辨率修复
- 启用 per-monitor DPI 感知（Per-Monitor V2）：高 DPI 笔记本（如 2560x1440 @ 150%）壁纸按物理像素全分辨率渲染，不再被系统拉伸发虚
- 主窗口/浮层面板字体按 DPI 缩放；跨 DPI 显示器移动时按系统建议尺寸调整
- desktop_probe 同步启用 DPI 感知，报告物理分辨率

## v0.2.6（2026-08）— CLI 小修
- 修复 --console / --debug 被当作媒体路径打开的问题；纯参数启动不再弹“打开失败”
- 正常使用无需任何参数，双击 exe 即可

## v0.2.5（2026-08）— 崩溃定位与 24H2 挂载加固
- 崩溃处理器升级：打印异常线程、模块名、模块偏移（RVA）、32 帧调用栈，并自动写 media_player_app_crash.dmp（minidump）
- 渲染器加固：draw_frame / Present 增加 RTV 空指针保护；ResizeBuffers/GetBuffer 失败时尝试恢复 RTV，避免下一帧空指针崩溃
- 图标层快照刷新（隐藏/显示 DefView）从查找/轮询中移出，只在挂载完成后执行一次，避免 24H2 上反复触发 explorer 重建
- 新增壁纸模式进入/退出/重挂日志（[wallpaper]）
- 新增 pdb_sym 符号化工具（开发用）：根据 PDB 把崩溃 RVA 还原为函数名和行号

## v0.2.4（2026-08）— Win11 24H2 桌面新模型适配
- WorkerW 查找兼容双模型：经典顶层路径（Win10/23H2）+ Progman 子窗口枚举（24H2 新模型）
- 检测 Progman 的 WS_EX_NOREDIRECTIONBITMAP 判断新模型；自动维护 WorkerW 在 SHELLDLL_DefView 之下的 Z 序，并触发图标层重绘消除切换快照
- 新增 DesktopLayerWatcher：监听 WorkerW/DefView 销毁与 Explorer 重启（TaskbarCreated），视频/网页壁纸自动重新挂载
- 新增 wait_desktop_layer 轮询等待（24H2 的 WorkerW 可能延迟出现）
- 新增 desktop_probe.exe 诊断工具：打印桌面层结构，便于远程排查

## v0.2.3（2026-08）— Win11 壁纸兼容增强
- WorkerW 查找增强：枚举并打印全部 WorkerW 诊断（hwnd/vis/defview/矩形），Win11 备选放宽为“覆盖主屏幕或工作区”
- 无可用 WorkerW 时回退 Progman，不再直接失败
- 壁纸失败时用 --console 启动即可看到 [workerw] 诊断日志，便于收集反馈

## v0.2.2（2026-08）— Win11 兼容
- Win11 桌面 WorkerW 查找兼容（经典方法失效时枚举可见全屏 WorkerW）+ 桌面层诊断日志
- AMF 解码器在无 AMD 运行时（Intel/NVIDIA 笔记本）优雅跳过，不再报 Unknown error

## v0.2.1（2026-08）— 小修补
- 默认隐藏控制台窗口（--console / --debug 时显示）
- 修复输入框激活时快捷键泄漏（M 静音 / 空格暂停 / 方向键 seek）
- 音量滑块滚轮误触回滚
- 网页导航完成日志（诊断网页是否打开）

## v0.2.0（2026-08）— 二期完成：动态壁纸 + 模块化

### 新功能（M1–M7）
- M1 接口桩：HeadlessRenderer / NullAudioSink / `--headless` 无窗口回归
- M2 壁纸 UX：浮层控制面板（置顶可输入）、位置记忆、托盘图标
- M3 模块 DLL 化：渲染 plugin\2\renderer.dll、音频 plugin\2\audio.dll、同步 plugin\2\sync.dll
- M4 网页壁纸：WebView2（专用 STA 线程 + put_ParentWindow 挂桌面层），内置演示页，自动补全 https
- M5 更多硬解：AMF 插件 decoder_amf.dll（HEVC/H.264），`ME_HW_BACKEND` 可选后端
- M6 采集：WASAPI loopback 音频采集（--capture-audio）、DXGI 屏幕采集（--capture-screen）
- M7 稳定性：壁纸崩溃自动恢复 watchdog、--wallpaper-keep、崩溃日志、软硬解性能对比

### 修复
- JPG/image2 探测后 seek 复位导致不渲染
- 10bit HEVC（P010）硬解帧拷贝失败
- 切换/seek 后硬解解码器错误状态卡死（自动重建）
- 壁纸模式切换文件失败后面板消失（空转渲染）
- 控制面板输入框无法输入 / 字符被持续删除（WM_KEYUP 缺失）
- 输入框激活时快捷键泄漏（M 静音 / 空格暂停）
- 隐藏控制面板残留黑框
- WebView2 与音频 MTA/STA 冲突（0x80010106）
- 主窗口普通模式无面板（引擎创建即空转渲染）

## v0.1.0（2026-07）— 一期：可播放的媒体引擎
- 解封装/解码/渲染/音频输出/音视频同步全管线
- H.264/HEVC 软硬解、AV1 软解、图片/GIF/WebP
- 壁纸原型（WorkerW 挂载）
