#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eg {

std::int64_t unix_ms_now();
std::int64_t steady_ms_now();

std::string trim(std::string_view value);
std::string to_lower(std::string_view value);
bool starts_with(std::string_view value, std::string_view prefix);
bool ends_with(std::string_view value, std::string_view suffix);
std::vector<std::string> split(std::string_view value, char delimiter,
                               bool keep_empty = true);
std::string join(const std::vector<std::string>& values, std::string_view separator);

std::optional<std::int64_t> parse_i64(std::string_view value, int base = 10);
std::optional<double> parse_double(std::string_view value);
std::optional<std::uint64_t> parse_duration_ms(std::string_view value);

std::string format_iso8601_utc(std::int64_t unix_ms);
std::string format_duration_ms(std::uint64_t duration_ms);

std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t previous = 0);
std::string to_hex(const std::uint8_t* data, std::size_t size);

bool read_text_file(const std::string& path, std::string& out, std::string& error);
bool read_binary_file(const std::string& path, std::vector<std::uint8_t>& out,
                      std::string& error);

// FNV-1a, used only for stable diagnostic identifiers.
std::uint64_t fnv1a64(std::string_view value);

}  // namespace eg
