#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "core/metrics.h"
#include "core/types.h"

namespace eg {

class WalError : public std::runtime_error {
public:
    explicit WalError(const std::string& message) : std::runtime_error(message) {}
};

struct WalRecoveryResult {
    std::uint64_t records = 0;
    std::uint64_t samples = 0;
    std::uint64_t unknown_metrics = 0;
    std::uint64_t bytes_recovered = 0;
    bool truncated = false;
};

using WalSampleCallback =
    std::function<void(std::string_view metric_key, TimestampMs timestamp_ms,
                       double value)>;

// Replays a write-ahead log and truncates a torn/corrupt tail. The callback is
// invoked for every complete sample record whose metric has a schema entry.
WalRecoveryResult recover_wal(const std::string& path,
                              const WalSampleCallback& callback);

class WalWriter {
public:
    WalWriter(std::string path, const MetricRegistry& registry,
              std::size_t queue_capacity, std::uint32_t flush_interval_ms);
    ~WalWriter();

    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    bool start(std::string& error);
    void stop();
    bool append(const Sample& sample);
    std::uint64_t written() const;
    std::uint64_t dropped() const;
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eg
