#pragma once

#include <windows.h>

namespace me {

// 找到桌面壁纸层 WorkerW（Win10 经典 + Win11 兼容）：
// - Win10：Progman 发送 0x052C 后，含 SHELLDLL_DefView 的窗口的兄弟 WorkerW 是壁纸层；
// - Win11：该结构可能不可用，改为枚举"可见且覆盖主工作区"的 WorkerW。
// 找不到返回 nullptr。
HWND find_desktop_workerw();

}  // namespace me