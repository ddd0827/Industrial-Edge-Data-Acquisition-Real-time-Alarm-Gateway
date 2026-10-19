#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "common/config.h"
#include "common/json.h"
#include "common/util.h"
#include "test_framework.h"

namespace {

std::filesystem::path make_temp_path(const char* suffix) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << "edgeguard_test_" << stamp << "_" << sequence.fetch_add(1) << suffix;
    return std::filesystem::temp_directory_path() / name.str();
}

struct TempFile {
    explicit TempFile(const std::string& content, const char* suffix = ".tmp")
        : path(make_temp_path(suffix)) {
        std::ofstream output(path, std::ios::binary);
        output << content;
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::filesystem::path path;
};

}  // namespace

TEST_CASE(crc32_matches_reference_vector) {
    const std::string input = "123456789";
    REQUIRE_EQ(eg::crc32(input.data(), input.size()), 0xCBF43926U);
}

TEST_CASE(duration_parser_supports_common_units) {
    REQUIRE_EQ(eg::parse_duration_ms("250ms").value(), 250U);
    REQUIRE_EQ(eg::parse_duration_ms("1.5s").value(), 1500U);
    REQUIRE_EQ(eg::parse_duration_ms("2m").value(), 120000U);
    REQUIRE_EQ(eg::parse_duration_ms("1h").value(), 3600000U);
    REQUIRE(!eg::parse_duration_ms("soon").has_value());
    REQUIRE(!eg::parse_duration_ms("-2s").has_value());
}

TEST_CASE(json_writer_escapes_and_builds_nested_values) {
    eg::JsonWriter writer;
    writer.begin_object();
    writer.key("text").value("line\nbreak \"quoted\"");
    writer.key("ok").value(true);
    writer.key("count").value(static_cast<std::int64_t>(3));
    writer.key("values").begin_array();
    writer.value(static_cast<std::int64_t>(1));
    writer.value(static_cast<std::int64_t>(2));
    writer.end_array();
    writer.key("missing").null();
    writer.end_object();

    REQUIRE_EQ(writer.str(),
               std::string("{\"text\":\"line\\nbreak \\\"quoted\\\"\",\"ok\":true,"
                           "\"count\":3,\"values\":[1,2],\"missing\":null}"));
}

TEST_CASE(config_loader_resolves_paths_and_metrics) {
    const std::string content =
        "[gateway]\n"
        "http_port=18080\n"
        "web_root=web\n"
        "wal_path=data/test.wal\n"
        "rules=rules/test.rules\n"
        "retention_seconds=120\n"
        "max_points_per_metric=128\n"
        "[logging]\n"
        "level=debug\n"
        "[simulator]\n"
        "port=15099\n"
        "[device.boiler]\n"
        "host=127.0.0.1\n"
        "port=1502\n"
        "unit_id=3\n"
        "interval_ms=100\n"
        "metrics=temp:holding:2:u16:0.5:-10:C;pump:coil:0:bool:1:0:\n";

    TempFile file(content, ".conf");
    const eg::AppConfig config = eg::load_config(file.path.u8string());

    REQUIRE_EQ(config.http_port, static_cast<std::uint16_t>(18080));
    REQUIRE_EQ(config.retention_seconds, 120U);
    REQUIRE_EQ(config.max_points_per_metric, static_cast<std::size_t>(128));
    REQUIRE_EQ(config.simulator_port, static_cast<std::uint16_t>(15099));
    REQUIRE_EQ(config.devices.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(config.metrics.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(config.log_level, eg::LogLevel::Debug);

    const eg::DeviceSpec& device = config.devices.front();
    REQUIRE_EQ(device.name, std::string("boiler"));
    REQUIRE_EQ(device.unit_id, static_cast<std::uint8_t>(3));
    REQUIRE_EQ(device.poll_interval_ms, 100U);
    REQUIRE_EQ(device.metric_ids.size(), static_cast<std::size_t>(2));

    const eg::MetricSpec* temperature = config.metrics.find("boiler", "temp");
    REQUIRE(temperature != nullptr);
    REQUIRE_NEAR(temperature->scale, 0.5, 1e-12);
    REQUIRE_NEAR(temperature->offset, -10.0, 1e-12);
    REQUIRE_EQ(temperature->unit, std::string("C"));
    REQUIRE_EQ(temperature->key, std::string("boiler.temp"));
    REQUIRE_EQ(temperature->area, eg::RegisterArea::HoldingRegister);

    const eg::MetricSpec* pump = config.metrics.find("boiler", "pump");
    REQUIRE(pump != nullptr);
    REQUIRE(pump->is_boolean());

    const std::filesystem::path base = file.path.parent_path();
    REQUIRE_EQ(config.web_root,
               (base / "web").lexically_normal().u8string());
    REQUIRE_EQ(config.wal_path,
               (base / "data" / "test.wal").lexically_normal().u8string());
}

TEST_CASE(config_loader_rejects_unknown_keys) {
    const std::string content =
        "[gateway]\n"
        "http_prot=8080\n"
        "[device.boiler]\n"
        "host=127.0.0.1\n"
        "metrics=temp:holding:0:u16:1:0:C\n";
    TempFile file(content, ".conf");
    REQUIRE_THROW(eg::load_config(file.path.u8string()), eg::ConfigError);
}

TEST_CASE(config_loader_rejects_missing_metrics) {
    const std::string content =
        "[device.boiler]\n"
        "host=127.0.0.1\n";
    TempFile file(content, ".conf");
    REQUIRE_THROW(eg::load_config(file.path.u8string()), eg::ConfigError);
}
