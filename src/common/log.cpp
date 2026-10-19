#include "common/log.h"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <sstream>
#include <thread>
#include <vector>

#include "common/util.h"

namespace eg {
namespace {

const char* level_tag(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO ";
        case LogLevel::Warn:
            return "WARN ";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Off:
            return "OFF  ";
    }
    return "INFO ";
}

std::string thread_tag() {
    std::ostringstream stream;
    stream << std::hex << std::hash<std::thread::id>{}(std::this_thread::get_id());
    std::string value = stream.str();
    if (value.size() > 8) {
        value.resize(8);
    }
    return value;
}

}  // namespace

std::optional<LogLevel> parse_log_level(std::string_view value) {
    const std::string normalized = to_lower(trim(value));
    if (normalized == "trace") {
        return LogLevel::Trace;
    }
    if (normalized == "debug") {
        return LogLevel::Debug;
    }
    if (normalized == "info") {
        return LogLevel::Info;
    }
    if (normalized == "warn" || normalized == "warning") {
        return LogLevel::Warn;
    }
    if (normalized == "error") {
        return LogLevel::Error;
    }
    if (normalized == "off") {
        return LogLevel::Off;
    }
    return std::nullopt;
}

std::string_view log_level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
        case LogLevel::Off:
            return "off";
    }
    return "info";
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void Logger::configure(LogLevel level, const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_.store(level, std::memory_order_relaxed);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
    if (file_path.empty()) {
        return;
    }

    const std::filesystem::path path = std::filesystem::u8path(file_path);
    if (path.has_parent_path() && !path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    file_.open(path, std::ios::out | std::ios::app);
    if (!file_) {
        std::fprintf(stderr, "[EdgeGuard] warning: cannot open log file: %s\n",
                     file_path.c_str());
    }
}

bool Logger::enabled(LogLevel level) const {
    return level >= level_.load(std::memory_order_relaxed);
}

void Logger::log(LogLevel level, const std::string& message) {
    if (!enabled(level)) {
        return;
    }

    std::ostringstream line;
    line << format_iso8601_utc(unix_ms_now()) << " [" << level_tag(level) << "] ["
         << thread_tag() << "] " << message;

    std::lock_guard<std::mutex> lock(mutex_);
    const std::string rendered = line.str();
    std::FILE* console = level >= LogLevel::Warn ? stderr : stdout;
    std::fwrite(rendered.data(), 1, rendered.size(), console);
    std::fputc('\n', console);
    std::fflush(console);

    if (file_.is_open()) {
        file_ << rendered << '\n';
        file_.flush();
    }
}

void Logger::logf(LogLevel level, const char* format, ...) {
    if (!enabled(level)) {
        return;
    }

    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int needed = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);

    if (needed < 0) {
        va_end(args);
        log(level, "log formatting error");
        return;
    }

    std::vector<char> buffer(static_cast<std::size_t>(needed) + 1);
    std::vsnprintf(buffer.data(), buffer.size(), format, args);
    va_end(args);
    log(level, std::string(buffer.data(), static_cast<std::size_t>(needed)));
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fflush(stdout);
    std::fflush(stderr);
    if (file_.is_open()) {
        file_.flush();
    }
}

}  // namespace eg
