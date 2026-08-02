#pragma once

#include <string>
#include <string_view>

namespace me {

// 全局统一错误码：0 表示成功。
// 设计要点：所有模块只返回 Error，不抛异常、不返回裸错误码，
// 保证"错误信息"总是跟随"错误码"一起传播，便于排查。
enum class Err {
    Ok = 0,
    InvalidArgument,
    Io,               // 文件/IO 错误
    MediaOpenFailed,  // 解封装打开失败
    NoStream,         // 没有目标流（无视频/无音频）
    CodecNotFound,
    CodecOpenFailed,
    DecodeFailed,
    EndOfFile,        // 正常读到文件尾（不是错误）
    NeedMoreData,     // 解码器需要更多输入
    RenderFailed,
    AudioFailed,
    ThreadFailed,
    Unsupported,
};

class Error {
public:
    Error() = default;

    static Error success() { return {}; }
    static Error make(Err code, std::string message) {
        Error e;
        e.code_ = code;
        e.message_ = std::move(message);
        return e;
    }

    bool ok() const { return code_ == Err::Ok; }
    Err code() const { return code_; }
    const std::string& message() const { return message_; }

private:
    Err code_ = Err::Ok;
    std::string message_;
};

}  // namespace me
