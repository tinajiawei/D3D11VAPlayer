#include "core/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <mutex>
#include <thread>

namespace me {

namespace {

std::mutex g_log_mutex;
LogLevel g_level = LogLevel::Info;

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

}  // namespace

void Log::set_level(LogLevel level) { g_level = level; }

LogLevel Log::level() { return g_level; }

void Log::write(LogLevel level, const char* file, int line, const std::string& message) {
    if (level < g_level) return;

    std::lock_guard<std::mutex> lock(g_log_mutex);

    // 可读的系统时间 + 线程 id + 文件名:行号
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);

    std::string file_part = file;
    const auto slash = file_part.find_last_of("\\/");
    if (slash != std::string::npos) file_part = file_part.substr(slash + 1);

    std::fprintf(stderr, "[%02d:%02d:%02d] [%s] [tid=%d] %s:%d | %s\n",
                 tm.tm_hour, tm.tm_min, tm.tm_sec,
                 level_name(level),
                 static_cast<int>(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffff),
                 file_part.c_str(), line, message.c_str());
}

}  // namespace me
