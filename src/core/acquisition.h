#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "core/metrics.h"
#include "core/sample_bus.h"
#include "core/types.h"
#include "protocol/modbus.h"

namespace eg {

struct DeviceStatusSnapshot {
    std::string name;
    std::string display_name;
    std::string host;
    std::uint16_t port = 0;
    std::uint8_t unit_id = 0;
    std::uint32_t poll_interval_ms = 0;
    bool online = false;
    TimestampMs last_attempt_ms = 0;
    TimestampMs last_sample_ms = 0;
    std::uint64_t read_errors = 0;
    std::string last_error;
};

class DeviceHealthRegistry {
public:
    explicit DeviceHealthRegistry(const std::vector<DeviceSpec>& devices);

    void record_attempt(std::size_t index, TimestampMs timestamp);
    void record_success(std::size_t index, TimestampMs timestamp);
    void record_failure(std::size_t index, TimestampMs timestamp,
                        const std::string& error);

    std::vector<DeviceStatusSnapshot> snapshot() const;
    std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        DeviceSpec spec;
        std::atomic<bool> online{false};
        std::atomic<TimestampMs> last_attempt_ms{0};
        std::atomic<TimestampMs> last_sample_ms{0};
        std::atomic<std::uint64_t> read_errors{0};
        mutable std::mutex error_mutex;
        std::string last_error;
    };

    std::vector<std::unique_ptr<Entry>> entries_;
};

class AcquisitionEngine {
public:
    AcquisitionEngine(const std::vector<DeviceSpec>& devices,
                      const MetricRegistry& registry, SampleBus& bus,
                      DeviceHealthRegistry& health);
    ~AcquisitionEngine();

    AcquisitionEngine(const AcquisitionEngine&) = delete;
    AcquisitionEngine& operator=(const AcquisitionEngine&) = delete;

    void start();
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

private:
    struct ReadBlock {
        std::uint8_t function = 0;
        bool bits = false;
        std::uint16_t start = 0;
        std::uint16_t count = 0;
        std::vector<const MetricSpec*> metrics;
    };

    struct Worker {
        const DeviceSpec* spec = nullptr;
        std::vector<const MetricSpec*> metrics;
        std::vector<ReadBlock> blocks;
        std::unique_ptr<ModbusTcpClient> client;
        std::thread thread;
        std::uint64_t consecutive_failures = 0;
        bool ever_succeeded = false;
    };

    void build_plan(Worker& worker) const;
    bool poll_once(Worker& worker, std::size_t device_index);
    void worker_loop(std::size_t device_index);

    const std::vector<DeviceSpec>& devices_;
    const MetricRegistry& registry_;
    SampleBus& bus_;
    DeviceHealthRegistry& health_;

    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    mutable std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
};

}  // namespace eg
