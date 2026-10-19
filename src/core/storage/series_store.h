#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "core/types.h"

namespace eg {

struct StoredPoint {
    TimestampMs timestamp_ms = 0;
    double value = 0.0;
};

// Fixed-capacity per-metric ring buffers with time-based eviction and min/max
// downsampling for chart queries. Reads and appends may run concurrently.
class SeriesStore {
public:
    SeriesStore(std::size_t metric_count, std::uint32_t retention_seconds,
                std::size_t max_points_per_metric);
    ~SeriesStore();

    SeriesStore(const SeriesStore&) = delete;
    SeriesStore& operator=(const SeriesStore&) = delete;

    void append(const Sample& sample);
    std::vector<StoredPoint> query(std::uint32_t metric_id, TimestampMs from_ms,
                                   TimestampMs to_ms,
                                   std::size_t max_points) const;
    std::optional<StoredPoint> latest(std::uint32_t metric_id) const;
    std::size_t point_count(std::uint32_t metric_id) const;
    std::size_t total_samples_appended() const;

private:
    struct Series;
    std::vector<std::unique_ptr<Series>> series_;
    std::uint32_t retention_seconds_ = 3600;
    std::size_t max_points_per_metric_ = 200000;
    std::atomic<std::uint64_t> total_appended_{0};
};

}  // namespace eg
