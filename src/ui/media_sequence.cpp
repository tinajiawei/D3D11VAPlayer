#include "ui/media_sequence.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>

#pragma comment(lib, "shlwapi.lib")

namespace fs = std::filesystem;

namespace me {

namespace {

std::wstring to_lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return s;
}

bool has_ext(const std::wstring& ext, const wchar_t* const* exts, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (ext == exts[i]) return true;
    }
    return false;
}

bool is_video_ext(const std::wstring& ext) {
    static const wchar_t* exts[] = {
        L".mp4", L".mkv", L".mov", L".ts", L".flv", L".webm", L".avi",
        L".wmv", L".m4v", L".mpg", L".mpeg", L".3gp", L".rm", L".rmvb",
    };
    return has_ext(ext, exts, sizeof(exts) / sizeof(exts[0]));
}

bool is_image_ext(const std::wstring& ext) {
    static const wchar_t* exts[] = {
        L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".webp", L".tif", L".tiff",
    };
    return has_ext(ext, exts, sizeof(exts) / sizeof(exts[0]));
}

bool is_audio_ext(const std::wstring& ext) {
    static const wchar_t* exts[] = {
        L".mp3", L".wav", L".flac", L".opus", L".m4a", L".aac", L".ogg",
    };
    return has_ext(ext, exts, sizeof(exts) / sizeof(exts[0]));
}

bool match_type(const std::wstring& ext, SequenceType type) {
    switch (type) {
        case SequenceType::Video: return is_video_ext(ext);
        case SequenceType::Image: return is_image_ext(ext);
        case SequenceType::All: return is_video_ext(ext) || is_image_ext(ext) || is_audio_ext(ext);
    }
    return false;
}

std::string utf8_from_wide(const std::wstring& w) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<size_t>(len - 1), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

}  // namespace

void MediaSequence::rebuild(const std::wstring& current_path, SequenceType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
    index_ = -1;

    fs::path dir = fs::path(current_path).parent_path();
    if (dir.empty()) dir = fs::current_path();

    try {
        fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied);
        const fs::recursive_directory_iterator end;
        for (; it != end; ++it) {
            const fs::directory_entry& e = *it;
            if (!e.is_regular_file()) continue;
            const std::wstring ext = to_lower(e.path().extension().wstring());
            if (match_type(ext, type)) items_.push_back(e.path().wstring());
        }
    } catch (...) {
        // 目录不可读/遍历中断：保留已收集的项
    }

    std::sort(items_.begin(), items_.end(), [](const std::wstring& a, const std::wstring& b) {
        return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
    });

    const std::wstring cur = to_lower(current_path);
    for (size_t i = 0; i < items_.size(); ++i) {
        if (to_lower(items_[i]) == cur) {
            index_ = static_cast<int>(i);
            break;
        }
    }
    if (index_ < 0) {
        // 当前文件类型与过滤不符（或不在目录内）：仍放进列表，保证有上下文可跳
        items_.insert(items_.begin(), current_path);
        index_ = 0;
    }
}

bool MediaSequence::step(std::wstring& out, int delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.empty() || index_ < 0) return false;
    const int next_index = index_ + delta;
    if (next_index < 0 || next_index >= static_cast<int>(items_.size())) return false;
    index_ = next_index;
    out = items_[next_index];
    return true;
}

bool MediaSequence::next(std::wstring& out) { return step(out, +1); }
bool MediaSequence::prev(std::wstring& out) { return step(out, -1); }

SequenceInfo MediaSequence::snapshot(SequenceType type, bool auto_next) const {
    std::lock_guard<std::mutex> lock(mutex_);
    SequenceInfo info;
    info.type = static_cast<int>(type);
    info.auto_next = auto_next;
    info.count = static_cast<int>(items_.size());
    info.index = index_;
    if (index_ >= 0 && index_ < static_cast<int>(items_.size())) {
        info.current_name = utf8_from_wide(fs::path(items_[index_]).filename().wstring());
    }
    return info;
}

}  // namespace me