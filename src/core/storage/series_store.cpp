#include "core/storage/series_store.h"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <utility>

namespace eg {
namespace {

std::vector<StoredPoint> downsample(const std::vector<StoredPoint>& points,
                                    std::size_t max_points) {
    const std::size_t target = std::max<std::size_t>(2, max_points);
    if (points.size() <= target) {
        return points;
    }

    const std::size_t buckets = std::max<std::size_t>(1, target / 2);
    const std::size_t count = points.size();
    std::vector<StoredPoint> result;
    result.reserve(target);

    for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
        const std::size_t begin = (bucket * count) / buckets;
        const std::size_t end = ((bucket + 1) * count) / buckets;
        if (begin >= end) {
            continue;
        }
        std::size_t min_index = begin;
        std::size_t max_index = begin;
        for (std::size_t i = begin + 1; i < end; ++i) {
            if (points[i].value < points[min_index].value) {
                min_index = i;
            }
            if (points[i].value > points[max_index].value) {
                max_index = i;
            }
        }
        if (min_index == max_index) {
            result.push_back(points[min_index]);
        } else if (min_index < max_index) {
            result.push_back(points[min_index]);
            result.push_back(points[max_index]);
        } else {
            result.push_back(points[max_index]);
            result.push_back(points[min_index]);
        }
    }
    return result;
}

}  // namespace

struct SeriesStore::Series {
    explicit Series(std::size_t capacity)
        : buffer(std::max<std::size_t>(2, capacity)) {}

    mutable std::mutex mutex;
    std::vector<StoredPoint> buffer;
    std::size_t start = 0;
    std::size_t count = 0;
};

SeriesStore::SeriesStore(std::size_t metric_count, std::uint32_t retention_seconds,
                         std::size_t max_points_per_metric)
    : retention_seconds_(retention_seconds == 0 ? 1 : retention_seconds),
      max_points_per_metric_(std::max<std::size_t>(2, max_points_per_metric)) {
    series_.reserve(metric_count);
    for (std::size_t i = 0; i < metric_count; ++i) {
        series_.push_back(std::make_unique<Series>(max_points_per_metric_));
    }
}

SeriesStore::~SeriesStore() = default;

void SeriesStore::append(const Sample& sample) {
    if (sample.metric_id >= series_.size()) {
        return;
    }

    Series& series = *series_[sample.metric_id];
    std::lock_guard<std::mutex> lock(series.mutex);

    TimestampMs timestamp = sample.timestamp_ms;
    if (series.count > 0) {
        const std::size_t last_index =
            (series.start + series.count - 1) % series.buffer.size();
        if (timestamp < series.buffer[last_index].timestamp_ms) {
            timestamp = series.buffer[last_index].timestamp_ms;
        }
    }

    const std::int64_t cutoff =
        timestamp - static_cast<std::int64_t>(retention_seconds_) * 1000;
    while (series.count > 0 &&
           series.buffer[series.start].timestamp_ms < cutoff) {
        series.start = (series.start + 1) % series.buffer.size();
        --series.count;
    }

    const StoredPoint point{timestamp, sample.value};
    if (series.count < series.buffer.size()) {
        const std::size_t index =
            (series.start + series.count) % series.buffer.size();
        series.buffer[index] = point;
        ++series.count;
    } else {
        series.buffer[series.start] = point;
        series.start = (series.start + 1) % series.buffer.size();
    }
    total_appended_.fetch_add(1, std::memory_order_relaxed);
}

std::vector<StoredPoint> SeriesStore::query(std::uint32_t metric_id,
                                            TimestampMs from_ms, TimestampMs to_ms,
                                            std::size_t max_points) const {
    if (metric_id >= series_.size() || from_ms > to_ms) {
        return {};
    }

    const Series& series = *series_[metric_id];
    std::lock_guard<std::mutex> lock(series.mutex);

    std::vector<StoredPoint> points;
    points.reserve(series.count);
    for (std::size_t i = 0; i < series.count; ++i) {
        const StoredPoint& point =
            series.buffer[(series.start + i) % series.buffer.size()];
        if (point.timestamp_ms < from_ms) {
            continue;
        }
        if (point.timestamp_ms > to_ms) {
            break;
        }
        points.push_back(point);
    }

    if (max_points == 0 || points.size() <= max_points) {
        return points;
    }
    return downsample(points, max_points);
}

std::optional<StoredPoint> SeriesStore::latest(std::uint32_t metric_id) const {
    if (metric_id >= series_.size()) {
        return std::nullopt;
    }
    const Series& series = *series_[metric_id];
    std::lock_guard<std::mutex> lock(series.mutex);
    if (series.count == 0) {
        return std::nullopt;
    }
    const std::size_t index =
        (series.start + series.count - 1) % series.buffer.size();
    return series.buffer[index];
}

std::size_t SeriesStore::point_count(std::uint32_t metric_id) const {
    if (metric_id >= series_.size()) {
        return 0;
    }
    const Series& series = *series_[metric_id];
    std::lock_guard<std::mutex> lock(series.mutex);
    return series.count;
}

std::size_t SeriesStore::total_samples_appended() const {
    return static_cast<std::size_t>(
        total_appended_.load(std::memory_order_relaxed));
}

}  // namespace eg
