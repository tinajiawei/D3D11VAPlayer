#pragma once

#include <sstream>
#include <string>

namespace me {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

namespace log_detail {

inline std::string format() { return {}; }

template <typename T, typename... Args>
std::string format(const T& first, Args&&... rest) {
    std::ostringstream oss;
    oss << first;
    ((oss << ' ' << rest), ...);
    return oss.str();
}

}  // namespace log_detail

class Log {
public:
    static void set_level(LogLevel level);
    static LogLevel level();
    static void write(LogLevel level, const char* file, int line, const std::string& message);
};

}  // namespace me

#define ME_LOG_DEBUG(...) ::me::Log::write(::me::LogLevel::Debug, __FILE__, __LINE__, ::me::log_detail::format(__VA_ARGS__))
#define ME_LOG_INFO(...)  ::me::Log::write(::me::LogLevel::Info,  __FILE__, __LINE__, ::me::log_detail::format(__VA_ARGS__))
#define ME_LOG_WARN(...)  ::me::Log::write(::me::LogLevel::Warn,  __FILE__, __LINE__, ::me::log_detail::format(__VA_ARGS__))
#define ME_LOG_ERROR(...) ::me::Log::write(::me::LogLevel::Error, __FILE__, __LINE__, ::me::log_detail::format(__VA_ARGS__))
