#pragma once

#include <atomic>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// windows.h (often pulled in by third-party headers) defines ERROR as a GDI
// status macro; it would otherwise overwrite the LogLevel::Error enumerator.
#ifdef ERROR
#undef ERROR
#endif

namespace eg {

enum class LogLevel { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

std::optional<LogLevel> parse_log_level(std::string_view value);
std::string_view log_level_name(LogLevel level);

class Logger {
public:
    static Logger& instance();

    void configure(LogLevel level, const std::string& file_path = {});
    bool enabled(LogLevel level) const;
    void log(LogLevel level, const std::string& message);
    void logf(LogLevel level, const char* format, ...);
    void flush();

private:
    Logger() = default;
    ~Logger();

    mutable std::mutex mutex_;
    std::atomic<LogLevel> level_{LogLevel::Info};
    std::ofstream file_;
};

}  // namespace eg

#define EG_LOGF(level, ...) ::eg::Logger::instance().logf((level), __VA_ARGS__)
#define EG_TRACE(...) EG_LOGF(::eg::LogLevel::Trace, __VA_ARGS__)
#define EG_DEBUG(...) EG_LOGF(::eg::LogLevel::Debug, __VA_ARGS__)
#define EG_INFO(...) EG_LOGF(::eg::LogLevel::Info, __VA_ARGS__)
#define EG_WARN(...) EG_LOGF(::eg::LogLevel::Warn, __VA_ARGS__)
#define EG_ERROR(...) EG_LOGF(::eg::LogLevel::Error, __VA_ARGS__)
