#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/metrics.h"
#include "core/storage/series_store.h"
#include "core/storage/wal.h"
#include "test_framework.h"

namespace {

std::filesystem::path make_temp_path(const char* suffix) {
    static std::atomic<std::uint64_t> sequence{0};
    std::ostringstream name;
    name << "edgeguard_storage_" << std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()
         << "_" << sequence.fetch_add(1) << suffix;
    return std::filesystem::temp_directory_path() / name.str();
}

struct TempPath {
    explicit TempPath(const char* suffix) : path(make_temp_path(suffix)) {}
    ~TempPath() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::filesystem::path path;
};

eg::MetricRegistry make_wal_registry() {
    eg::MetricRegistry registry;
    eg::MetricSpec temperature;
    temperature.device = "boiler";
    temperature.name = "temp";
    temperature.area = eg::RegisterArea::HoldingRegister;
    temperature.value_type = eg::RegisterValueType::U16;
    registry.add(temperature);

    eg::MetricSpec pressure;
    pressure.device = "boiler";
    pressure.name = "pressure";
    pressure.area = eg::RegisterArea::HoldingRegister;
    pressure.value_type = eg::RegisterValueType::U16;
    registry.add(pressure);
    return registry;
}

}  // namespace

TEST_CASE(series_store_evicts_retains_and_downsamples) {
    eg::SeriesStore store(2, 60, 100);
    for (std::uint32_t i = 0; i < 150; ++i) {
        store.append(eg::Sample{0, 1000 + static_cast<std::int64_t>(i) * 100,
                                static_cast<double>(i)});
    }
    store.append(eg::Sample{1, 5000, 12.5});

    REQUIRE_EQ(store.point_count(0), static_cast<std::size_t>(100));
    const auto latest = store.latest(0);
    REQUIRE(latest.has_value());
    REQUIRE_EQ(latest->timestamp_ms, static_cast<std::int64_t>(1000 + 149 * 100));
    REQUIRE_NEAR(latest->value, 149.0, 1e-12);

    const auto all = store.query(0, 0, 20000, 1000);
    REQUIRE_EQ(all.size(), static_cast<std::size_t>(100));
    REQUIRE_EQ(all.front().timestamp_ms, static_cast<std::int64_t>(6000));

    const auto sampled = store.query(0, 0, 20000, 10);
    REQUIRE(sampled.size() <= static_cast<std::size_t>(10));
    bool has_min = false;
    bool has_max = false;
    for (const eg::StoredPoint& point : sampled) {
        if (point.value == 50.0) {
            has_min = true;
        }
        if (point.value == 149.0) {
            has_max = true;
        }
    }
    REQUIRE(has_min);
    REQUIRE(has_max);

    eg::SeriesStore retention(1, 1, 100);
    retention.append(eg::Sample{0, 0, 1.0});
    retention.append(eg::Sample{0, 2000, 2.0});
    REQUIRE_EQ(retention.point_count(0), static_cast<std::size_t>(1));
    const auto retained = retention.latest(0);
    REQUIRE(retained.has_value());
    REQUIRE_EQ(retained->timestamp_ms, static_cast<std::int64_t>(2000));
}

TEST_CASE(wal_round_trips_and_recovers_torn_tail) {
    TempPath path(".wal");
    const eg::MetricRegistry registry = make_wal_registry();
    const eg::MetricSpec* temp = registry.find("boiler", "temp");
    const eg::MetricSpec* pressure = registry.find("boiler", "pressure");
    REQUIRE(temp != nullptr);
    REQUIRE(pressure != nullptr);

    std::uint64_t missing_samples = 0;
    const eg::WalRecoveryResult missing = eg::recover_wal(
        path.path.u8string(), [&](std::string_view, eg::TimestampMs, double) {
            ++missing_samples;
        });
    REQUIRE_EQ(missing_samples, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(missing.samples, static_cast<std::uint64_t>(0));

    {
        eg::WalWriter writer(path.path.u8string(), registry, 128, 20);
        std::string error;
        REQUIRE(writer.start(error));
        REQUIRE(writer.append(eg::Sample{temp->id, 1000, 70.5}));
        REQUIRE(writer.append(eg::Sample{pressure->id, 1001, 1.15}));
        REQUIRE(writer.append(eg::Sample{temp->id, 1002, 71.5}));
        writer.stop();
        REQUIRE_EQ(writer.written(), static_cast<std::uint64_t>(3));
    }

    struct Recovered {
        std::string key;
        eg::TimestampMs timestamp;
        double value;
    };
    std::vector<Recovered> recovered;
    auto collect = [&](std::string_view key, eg::TimestampMs timestamp, double value) {
        recovered.push_back(Recovered{std::string(key), timestamp, value});
    };

    const eg::WalRecoveryResult first =
        eg::recover_wal(path.path.u8string(), collect);
    REQUIRE_EQ(first.samples, static_cast<std::uint64_t>(3));
    REQUIRE_EQ(recovered.size(), static_cast<std::size_t>(3));
    REQUIRE_EQ(recovered[0].key, std::string("boiler.temp"));
    REQUIRE_NEAR(recovered[0].value, 70.5, 1e-12);
    REQUIRE(!first.truncated);

    {
        std::ofstream garbage(path.path, std::ios::binary | std::ios::app);
        garbage << "torn-tail";
    }
    recovered.clear();
    const eg::WalRecoveryResult second =
        eg::recover_wal(path.path.u8string(), collect);
    REQUIRE_EQ(second.samples, static_cast<std::uint64_t>(3));
    REQUIRE(second.truncated);
    REQUIRE_EQ(std::filesystem::file_size(path.path), second.bytes_recovered);

    {
        eg::WalWriter writer(path.path.u8string(), registry, 128, 20);
        std::string error;
        REQUIRE(writer.start(error));
        REQUIRE(writer.append(eg::Sample{pressure->id, 2000, 1.20}));
        writer.stop();
    }
    recovered.clear();
    const eg::WalRecoveryResult third =
        eg::recover_wal(path.path.u8string(), collect);
    REQUIRE_EQ(third.samples, static_cast<std::uint64_t>(4));
}
