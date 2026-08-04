#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace me {

// 文件夹序列播放（提前加入的功能）：打开一个媒体后，扫描它所在目录（含子文件夹），
// 按类型过滤出视频/图片/全部，提供上一个/下一个；线程安全（渲染线程读快照，主线程重建）。
enum class SequenceType { Video = 0, Image = 1, All = 2 };

struct SequenceInfo {
    int count = 0;
    int index = -1;
    int type = 0;          // SequenceType 的 int 值
    bool auto_next = false;
    std::string current_name;  // 当前文件名字（UTF-8，面板显示用）
};

class MediaSequence {
public:
    // 以 current_path 所在目录为根，递归扫描并按 type 过滤；current 不在列表时插入到最前。
    void rebuild(const std::wstring& current_path, SequenceType type);

    // 成功返回 true 并输出目标路径；到边界返回 false（不循环）。
    bool next(std::wstring& out);
    bool prev(std::wstring& out);

    SequenceInfo snapshot(SequenceType type, bool auto_next) const;

private:
    bool step(std::wstring& out, int delta);

    mutable std::mutex mutex_;
    std::vector<std::wstring> items_;
    int index_ = -1;
};

}  // namespace me