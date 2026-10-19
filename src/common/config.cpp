#include "common/config.h"

#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#include "common/util.h"

namespace eg {
namespace {

struct IniValue {
    std::string value;
    int line = 0;
};

using IniSection = std::map<std::string, IniValue>;

struct IniFile {
    std::string path;
    std::map<std::string, IniSection> sections;
    std::map<std::string, int> section_lines;
};

bool is_identifier(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(value.front());
    if (!(std::isalpha(first) != 0 || first == '_')) {
        return false;
    }
    for (unsigned char ch : value) {
        if (!(std::isalnum(ch) != 0 || ch == '_')) {
            return false;
        }
    }
    return true;
}

IniFile parse_ini(std::string& text, const std::string& path) {
    if (starts_with(text, "\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }

    IniFile ini;
    ini.path = path;

    std::istringstream stream(text);
    std::string raw;
    std::string current_section;
    int line_number = 0;

    while (std::getline(stream, raw)) {
        ++line_number;
        if (!raw.empty() && raw.back() == '\r') {
            raw.pop_back();
        }

        const std::size_t comment = raw.find('#');
        if (comment != std::string::npos) {
            raw.erase(comment);
        }
        const std::string line = trim(raw);
        if (line.empty() || line.front() == ';') {
            continue;
        }

        if (line.front() == '[') {
            if (line.back() != ']') {
                throw ConfigError(path + ":" + std::to_string(line_number) +
                                  ": malformed section header");
            }
            const std::string name = trim(std::string_view(line).substr(1, line.size() - 2));
            if (name.empty()) {
                throw ConfigError(path + ":" + std::to_string(line_number) +
                                  ": empty section name");
            }
            if (ini.sections.find(name) != ini.sections.end()) {
                throw ConfigError(path + ":" + std::to_string(line_number) +
                                  ": duplicate section [" + name + "]");
            }
            ini.sections.emplace(name, IniSection{});
            ini.section_lines.emplace(name, line_number);
            current_section = name;
            continue;
        }

        if (current_section.empty()) {
            throw ConfigError(path + ":" + std::to_string(line_number) +
                              ": key/value pair outside of a section");
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            throw ConfigError(path + ":" + std::to_string(line_number) +
                              ": expected key=value");
        }
        const std::string key = to_lower(trim(std::string_view(line).substr(0, equals)));
        std::string value = trim(std::string_view(line).substr(equals + 1));
        if (key.empty()) {
            throw ConfigError(path + ":" + std::to_string(line_number) +
                              ": empty key");
        }
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        IniSection& section = ini.sections.at(current_section);
        if (section.find(key) != section.end()) {
            throw ConfigError(path + ":" + std::to_string(line_number) +
                              ": duplicate key '" + key + "'");
        }
        section.emplace(key, IniValue{value, line_number});
    }

    return ini;
}

class Reader {
public:
    explicit Reader(IniFile file) : file_(std::move(file)) {}

    const IniSection* section(const std::string& name) const {
        const auto it = file_.sections.find(name);
        return it == file_.sections.end() ? nullptr : &it->second;
    }

    const std::map<std::string, IniSection>& sections() const { return file_.sections; }

    int section_line(const std::string& name) const {
        const auto it = file_.section_lines.find(name);
        return it == file_.section_lines.end() ? 0 : it->second;
    }

    const IniValue* optional(const std::string& section, const std::string& key) const {
        const IniSection* values = this->section(section);
        if (values == nullptr) {
            return nullptr;
        }
        const auto it = values->find(key);
        return it == values->end() ? nullptr : &it->second;
    }

    const IniValue& require(const std::string& section_name, const std::string& key) const {
        const IniValue* value = optional(section_name, key);
        if (value == nullptr) {
            fail_line(section_line(section_name),
                      "section [" + section_name + "] is missing required key '" + key + "'");
        }
        return *value;
    }

    std::string string_or(const std::string& section, const std::string& key,
                          const std::string& fallback) const {
        const IniValue* value = optional(section, key);
        return value == nullptr ? fallback : value->value;
    }

    std::uint64_t uint_or(const std::string& section, const std::string& key,
                          std::uint64_t fallback, std::uint64_t minimum,
                          std::uint64_t maximum) const {
        const IniValue* value = optional(section, key);
        if (value == nullptr) {
            return fallback;
        }
        const auto parsed = parse_i64(value->value);
        if (!parsed.has_value() || *parsed < 0 ||
            static_cast<std::uint64_t>(*parsed) < minimum ||
            static_cast<std::uint64_t>(*parsed) > maximum) {
            fail(*value, "key '" + key + "' must be an integer in [" +
                             std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
        }
        return static_cast<std::uint64_t>(*parsed);
    }

    void reject_unknown_keys(const std::string& section_name,
                             std::initializer_list<std::string_view> allowed) const {
        const IniSection* values = section(section_name);
        if (values == nullptr) {
            return;
        }
        const std::set<std::string_view> known(allowed.begin(), allowed.end());
        for (const auto& [key, value] : *values) {
            if (known.find(key) == known.end()) {
                fail(value, "unknown key '" + key + "' in section [" + section_name + "]");
            }
        }
    }

    [[noreturn]] void fail(const IniValue& value, const std::string& message) const {
        fail_line(value.line, message);
    }

    [[noreturn]] void fail_line(int line, const std::string& message) const {
        throw ConfigError(file_.path + ":" + std::to_string(line) + ": " + message);
    }

private:
    IniFile file_;
};

std::string resolve_path(const std::filesystem::path& base, const std::string& value) {
    if (value.empty()) {
        return {};
    }
    std::filesystem::path path = std::filesystem::u8path(value);
    if (path.is_relative()) {
        path = base / path;
    }
    return path.lexically_normal().u8string();
}

RegisterArea parse_area(const Reader& reader, const IniValue& line_ctx,
                        const std::string& value) {
    const std::string normalized = to_lower(value);
    if (normalized == "coil") {
        return RegisterArea::Coil;
    }
    if (normalized == "discrete") {
        return RegisterArea::DiscreteInput;
    }
    if (normalized == "holding") {
        return RegisterArea::HoldingRegister;
    }
    if (normalized == "input") {
        return RegisterArea::InputRegister;
    }
    reader.fail(line_ctx, "unknown register area '" + value +
                              "' (expected holding/input/coil/discrete)");
}

RegisterValueType parse_value_type(const Reader& reader, const IniValue& line_ctx,
                                   const std::string& value) {
    const std::string normalized = to_lower(value);
    if (normalized == "bool") {
        return RegisterValueType::Bool;
    }
    if (normalized == "u16") {
        return RegisterValueType::U16;
    }
    if (normalized == "i16") {
        return RegisterValueType::I16;
    }
    if (normalized == "u32") {
        return RegisterValueType::U32;
    }
    if (normalized == "f32") {
        return RegisterValueType::F32;
    }
    reader.fail(line_ctx, "unknown value type '" + value +
                              "' (expected bool/u16/i16/u32/f32)");
}

std::uint16_t parse_address(const Reader& reader, const IniValue& line_ctx,
                            const std::string& value) {
    const auto parsed = parse_i64(value);
    if (!parsed.has_value() || *parsed < 0 || *parsed > 65535) {
        reader.fail(line_ctx, "register address '" + value + "' must be in [0, 65535]");
    }
    return static_cast<std::uint16_t>(*parsed);
}

}  // namespace

AppConfig load_config(const std::string& path) {
    const std::filesystem::path requested = std::filesystem::u8path(path);
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(requested, ec);
    if (ec) {
        absolute = requested;
    }

    std::string text;
    std::string read_error;
    if (!read_text_file(absolute.u8string(), text, read_error)) {
        throw ConfigError("cannot load config: " + read_error);
    }

    const Reader reader(parse_ini(text, absolute.u8string()));
    const std::filesystem::path base = absolute.parent_path();

    static const std::set<std::string> kTopSections = {"gateway", "logging",
                                                       "acquisition", "simulator"};
    for (const auto& entry : reader.sections()) {
        const std::string& name = entry.first;
        if (kTopSections.find(name) != kTopSections.end()) {
            continue;
        }
        if (starts_with(name, "device.")) {
            const std::string device_name = name.substr(7);
            if (!is_identifier(device_name)) {
                reader.fail_line(reader.section_line(name),
                                 "invalid device name '" + device_name +
                                     "' (letters, digits and underscore only)");
            }
            continue;
        }
        reader.fail_line(reader.section_line(name), "unknown section [" + name + "]");
    }

    if (reader.section("gateway") != nullptr) {
        reader.reject_unknown_keys(
            "gateway", {"http_host", "http_port", "web_root", "wal_path", "rules",
                        "retention_seconds", "max_points_per_metric",
                        "per_device_queue_capacity", "batch_size", "wal_queue_capacity",
                        "wal_flush_interval_ms"});
    }
    if (reader.section("logging") != nullptr) {
        reader.reject_unknown_keys("logging", {"level", "file"});
    }
    if (reader.section("acquisition") != nullptr) {
        reader.reject_unknown_keys("acquisition", {"default_poll_interval_ms"});
    }
    if (reader.section("simulator") != nullptr) {
        reader.reject_unknown_keys("simulator", {"port"});
    }

    AppConfig config;
    config.source_path = absolute.u8string();

    config.http_host = trim(reader.string_or("gateway", "http_host", "127.0.0.1"));
    if (config.http_host.empty()) {
        throw ConfigError("gateway.http_host must not be empty");
    }
    config.http_port = static_cast<std::uint16_t>(
        reader.uint_or("gateway", "http_port", 8080, 0, 65535));
    config.retention_seconds = static_cast<std::uint32_t>(
        reader.uint_or("gateway", "retention_seconds", 3600, 1, 30ULL * 24 * 3600));
    config.max_points_per_metric = static_cast<std::size_t>(
        reader.uint_or("gateway", "max_points_per_metric", 200000, 64, 10000000));
    config.per_device_queue_capacity = static_cast<std::size_t>(
        reader.uint_or("gateway", "per_device_queue_capacity", 8192, 16, 1000000));
    config.batch_size = static_cast<std::size_t>(
        reader.uint_or("gateway", "batch_size", 256, 1, 65536));
    config.wal_queue_capacity = static_cast<std::size_t>(
        reader.uint_or("gateway", "wal_queue_capacity", 65536, 256, 10000000));
    config.wal_flush_interval_ms = static_cast<std::uint32_t>(
        reader.uint_or("gateway", "wal_flush_interval_ms", 200, 10, 60000));
    config.default_poll_interval_ms = static_cast<std::uint32_t>(
        reader.uint_or("acquisition", "default_poll_interval_ms", 500, 10, 3600000));

    config.web_root = resolve_path(base, reader.string_or("gateway", "web_root", "web"));
    config.wal_path = resolve_path(
        base, reader.string_or("gateway", "wal_path", "data/edgeguard.wal"));
    config.rules_path = resolve_path(
        base, reader.string_or("gateway", "rules", "rules/line1.rules"));
    if (config.wal_path.empty()) {
        throw ConfigError("gateway.wal_path must not be empty");
    }
    if (config.rules_path.empty()) {
        throw ConfigError("gateway.rules must not be empty");
    }

    const std::string log_level = reader.string_or("logging", "level", "info");
    const auto parsed_level = parse_log_level(log_level);
    if (!parsed_level.has_value()) {
        throw ConfigError("logging.level must be one of trace/debug/info/warn/error/off");
    }
    config.log_level = *parsed_level;
    config.log_file = resolve_path(base, reader.string_or("logging", "file", ""));

    config.simulator_port = static_cast<std::uint16_t>(
        reader.uint_or("simulator", "port", 15020, 1, 65535));

    for (const auto& entry : reader.sections()) {
        const std::string& section_name = entry.first;
        if (!starts_with(section_name, "device.")) {
            continue;
        }

        reader.reject_unknown_keys(section_name,
                                   {"host", "port", "unit_id", "interval_ms", "metrics",
                                    "display_name"});

        DeviceSpec device;
        device.name = section_name.substr(7);
        const IniValue& host = reader.require(section_name, "host");
        device.host = trim(host.value);
        if (device.host.empty()) {
            reader.fail(host, "device host must not be empty");
        }
        device.port = static_cast<std::uint16_t>(
            reader.uint_or(section_name, "port", 502, 1, 65535));
        device.unit_id = static_cast<std::uint8_t>(
            reader.uint_or(section_name, "unit_id", 1, 1, 247));
        device.poll_interval_ms = static_cast<std::uint32_t>(reader.uint_or(
            section_name, "interval_ms", config.default_poll_interval_ms, 10, 3600000));
        device.display_name = trim(reader.string_or(section_name, "display_name", ""));
        if (device.display_name.empty()) {
            device.display_name = device.name;
        }

        const IniValue& metrics = reader.require(section_name, "metrics");
        std::set<std::string> metric_names;
        for (const std::string& raw_entry : split(metrics.value, ';', false)) {
            const std::string entry_text = trim(raw_entry);
            if (entry_text.empty()) {
                continue;
            }
            const std::vector<std::string> fields = split(entry_text, ':', true);
            if (fields.size() < 6 || fields.size() > 7) {
                reader.fail(metrics,
                            "metric definition must be "
                            "name:area:address:type:scale:offset[:unit], got '" +
                                entry_text + "'");
            }

            MetricSpec spec;
            spec.device = device.name;
            spec.name = trim(fields[0]);
            if (!is_identifier(spec.name)) {
                reader.fail(metrics, "invalid metric name '" + spec.name +
                                         "' (letters, digits and underscore only)");
            }
            if (!metric_names.insert(spec.name).second) {
                reader.fail(metrics, "duplicate metric '" + spec.name + "' on device '" +
                                         device.name + "'");
            }
            spec.key = device.name + "." + spec.name;
            spec.area = parse_area(reader, metrics, trim(fields[1]));
            spec.address = parse_address(reader, metrics, trim(fields[2]));
            spec.value_type = parse_value_type(reader, metrics, trim(fields[3]));

            const auto scale = parse_double(fields[4]);
            const auto offset = parse_double(fields[5]);
            if (!scale.has_value()) {
                reader.fail(metrics, "invalid scale '" + fields[4] + "' for metric " +
                                         spec.key);
            }
            if (!offset.has_value()) {
                reader.fail(metrics, "invalid offset '" + fields[5] + "' for metric " +
                                         spec.key);
            }
            spec.scale = *scale;
            spec.offset = *offset;
            spec.unit = fields.size() == 7 ? trim(fields[6]) : "";

            if (is_bit_area(spec.area) != (spec.value_type == RegisterValueType::Bool)) {
                reader.fail(metrics, "metric " + spec.key +
                                         ": coil/discrete areas require bool, register "
                                         "areas require u16/i16/u32/f32");
            }
            if (spec.value_type == RegisterValueType::Bool &&
                (spec.scale != 1.0 || spec.offset != 0.0)) {
                reader.fail(metrics, "metric " + spec.key +
                                         ": bool metrics require scale=1 and offset=0");
            }
            const std::uint32_t width = static_cast<std::uint32_t>(
                register_width(spec.value_type));
            if (static_cast<std::uint32_t>(spec.address) + width > 65536U) {
                reader.fail(metrics, "metric " + spec.key +
                                         ": register range exceeds address space");
            }

            try {
                const std::uint32_t id = config.metrics.add(std::move(spec));
                device.metric_ids.push_back(id);
            } catch (const std::exception& ex) {
                reader.fail(metrics, ex.what());
            }
        }

        if (device.metric_ids.empty()) {
            reader.fail(metrics, "device '" + device.name + "' has no metrics");
        }
        config.devices.push_back(std::move(device));
    }

    if (config.devices.empty()) {
        throw ConfigError("configuration must define at least one [device.<name>] section");
    }

    return config;
}

}  // namespace eg
