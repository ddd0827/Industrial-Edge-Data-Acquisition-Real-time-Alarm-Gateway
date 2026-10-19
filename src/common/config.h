#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/log.h"
#include "core/metrics.h"

namespace eg {

class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& message) : std::runtime_error(message) {}
};

struct DeviceSpec {
    std::string name;
    std::string display_name;
    std::string host;
    std::uint16_t port = 502;
    std::uint8_t unit_id = 1;
    std::uint32_t poll_interval_ms = 500;
    std::vector<std::uint32_t> metric_ids;
};

struct AppConfig {
    std::string source_path;

    std::string http_host = "127.0.0.1";
    std::uint16_t http_port = 8080;
    std::string web_root = "web";
    std::string wal_path = "data/edgeguard.wal";
    std::string rules_path = "rules/line1.rules";

    LogLevel log_level = LogLevel::Info;
    std::string log_file;

    std::uint32_t retention_seconds = 3600;
    std::size_t max_points_per_metric = 200000;
    std::size_t per_device_queue_capacity = 8192;
    std::size_t batch_size = 256;
    std::size_t wal_queue_capacity = 65536;
    std::uint32_t wal_flush_interval_ms = 200;
    std::uint32_t default_poll_interval_ms = 500;

    std::uint16_t simulator_port = 15020;

    MetricRegistry metrics;
    std::vector<DeviceSpec> devices;
};

// Loads and validates an INI-style gateway configuration. Relative paths are
// resolved against the directory containing the configuration file.
AppConfig load_config(const std::string& path);

}  // namespace eg
