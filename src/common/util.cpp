#include "common/util.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace eg {
namespace {

bool is_space(unsigned char ch) {
    return std::isspace(ch) != 0;
}

std::tm to_utc_tm(std::time_t value) {
    std::tm out{};
#ifdef _WIN32
    gmtime_s(&out, &value);
#else
    gmtime_r(&value, &out);
#endif
    return out;
}

const std::array<std::uint32_t, 256>& crc_table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1U) != 0U ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
            }
            result[i] = c;
        }
        return result;
    }();
    return table;
}

}  // namespace

std::int64_t unix_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t steady_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && is_space(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && is_space(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string to_lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

std::vector<std::string> split(std::string_view value, char delimiter, bool keep_empty) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t pos = value.find(delimiter, start);
        const std::string part(value.substr(
            start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
        if (keep_empty || !part.empty()) {
            parts.push_back(part);
        }
        if (pos == std::string_view::npos) {
            break;
        }
        start = pos + 1;
    }
    return parts;
}

std::string join(const std::vector<std::string>& values, std::string_view separator) {
    std::string result;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            result.append(separator);
        }
        result.append(values[i]);
    }
    return result;
}

std::optional<std::int64_t> parse_i64(std::string_view value, int base) {
    const std::string text = trim(value);
    if (text.empty()) {
        return std::nullopt;
    }
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(text.c_str(), &end, base);
    if (end == text.c_str() || end == nullptr || *end != '\0' || errno == ERANGE) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(parsed);
}

std::optional<double> parse_double(std::string_view value) {
    const std::string text = trim(value);
    if (text.empty()) {
        return std::nullopt;
    }
    errno = 0;
    char* end = nullptr;
    const double result = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(result)) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::uint64_t> parse_duration_ms(std::string_view value) {
    std::string text = to_lower(trim(value));
    if (text.empty()) {
        return std::nullopt;
    }

    std::uint64_t multiplier = 1;
    if (ends_with(text, "ms")) {
        text.resize(text.size() - 2);
        multiplier = 1;
    } else if (ends_with(text, "s")) {
        text.resize(text.size() - 1);
        multiplier = 1000;
    } else if (ends_with(text, "m")) {
        text.resize(text.size() - 1);
        multiplier = 60ULL * 1000ULL;
    } else if (ends_with(text, "h")) {
        text.resize(text.size() - 1);
        multiplier = 60ULL * 60ULL * 1000ULL;
    }

    const auto number = parse_double(text);
    if (!number.has_value() || *number < 0.0 ||
        *number > static_cast<double>(std::numeric_limits<std::uint64_t>::max() / multiplier)) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(*number * static_cast<double>(multiplier) + 0.5);
}

std::string format_iso8601_utc(std::int64_t unix_ms) {
    std::int64_t seconds = unix_ms / 1000;
    std::int64_t millis = unix_ms % 1000;
    if (millis < 0) {
        millis += 1000;
        --seconds;
    }
    const std::time_t raw = static_cast<std::time_t>(seconds);
    const std::tm tm = to_utc_tm(raw);
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                  tm.tm_sec, static_cast<long long>(millis));
    return buffer;
}

std::string format_duration_ms(std::uint64_t duration_ms) {
    if (duration_ms < 1000) {
        return std::to_string(duration_ms) + "ms";
    }
    const std::uint64_t total_seconds = duration_ms / 1000;
    const std::uint64_t hours = total_seconds / 3600;
    const std::uint64_t minutes = (total_seconds % 3600) / 60;
    const std::uint64_t seconds = total_seconds % 60;

    std::ostringstream stream;
    if (hours > 0) {
        stream << hours << "h ";
    }
    if (hours > 0 || minutes > 0) {
        stream << minutes << "m ";
    }
    stream << seconds << "s";
    return stream.str();
}

std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t previous) {
    const auto& table = crc_table();
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = ~previous;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

std::string to_hex(const std::uint8_t* data, std::size_t size) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        result.push_back(kHex[(data[i] >> 4U) & 0x0FU]);
        result.push_back(kHex[data[i] & 0x0FU]);
    }
    return result;
}

bool read_text_file(const std::string& path, std::string& out, std::string& error) {
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    if (!input) {
        error = "cannot open file: " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        error = "failed while reading file: " + path;
        return false;
    }
    out = buffer.str();
    return true;
}

bool read_binary_file(const std::string& path, std::vector<std::uint8_t>& out,
                      std::string& error) {
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!input) {
        error = "cannot open file: " + path;
        return false;
    }
    const std::streamoff length = input.tellg();
    if (length < 0) {
        error = "cannot determine file size: " + path;
        return false;
    }
    input.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(length));
    if (length > 0) {
        input.read(reinterpret_cast<char*>(out.data()), length);
    }
    if (!input && !input.eof()) {
        error = "failed while reading file: " + path;
        return false;
    }
    return true;
}

std::uint64_t fnv1a64(std::string_view value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace eg
