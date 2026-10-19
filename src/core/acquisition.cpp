#include "core/acquisition.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "common/log.h"
#include "common/util.h"

namespace eg {
namespace {

double decode_register(const MetricSpec& metric,
                       const std::vector<std::uint16_t>& registers,
                       std::size_t offset) {
    double raw = 0.0;
    switch (metric.value_type) {
        case RegisterValueType::U16:
            raw = static_cast<double>(registers.at(offset));
            break;
        case RegisterValueType::I16: {
            const std::uint16_t bits = registers.at(offset);
            std::int16_t signed_value = 0;
            std::memcpy(&signed_value, &bits, sizeof(signed_value));
            raw = static_cast<double>(signed_value);
            break;
        }
        case RegisterValueType::U32: {
            const std::uint32_t value =
                (static_cast<std::uint32_t>(registers.at(offset)) << 16U) |
                static_cast<std::uint32_t>(registers.at(offset + 1));
            raw = static_cast<double>(value);
            break;
        }
        case RegisterValueType::F32: {
            const std::uint32_t bits =
                (static_cast<std::uint32_t>(registers.at(offset)) << 16U) |
                static_cast<std::uint32_t>(registers.at(offset + 1));
            float value = 0.0F;
            std::memcpy(&value, &bits, sizeof(value));
            raw = static_cast<double>(value);
            break;
        }
        case RegisterValueType::Bool:
            raw = registers.at(offset) != 0 ? 1.0 : 0.0;
            break;
    }
    return raw * metric.scale + metric.offset;
}

}  // namespace

DeviceHealthRegistry::DeviceHealthRegistry(const std::vector<DeviceSpec>& devices) {
    entries_.reserve(devices.size());
    for (const DeviceSpec& device : devices) {
        auto entry = std::make_unique<Entry>();
        entry->spec = device;
        entries_.push_back(std::move(entry));
    }
}

void DeviceHealthRegistry::record_attempt(std::size_t index, TimestampMs timestamp) {
    if (index >= entries_.size()) {
        return;
    }
    entries_[index]->last_attempt_ms.store(timestamp, std::memory_order_relaxed);
}

void DeviceHealthRegistry::record_success(std::size_t index, TimestampMs timestamp) {
    if (index >= entries_.size()) {
        return;
    }
    Entry& entry = *entries_[index];
    entry.online.store(true, std::memory_order_relaxed);
    entry.last_attempt_ms.store(timestamp, std::memory_order_relaxed);
    entry.last_sample_ms.store(timestamp, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(entry.error_mutex);
    entry.last_error.clear();
}

void DeviceHealthRegistry::record_failure(std::size_t index, TimestampMs timestamp,
                                          const std::string& error) {
    if (index >= entries_.size()) {
        return;
    }
    Entry& entry = *entries_[index];
    entry.online.store(false, std::memory_order_relaxed);
    entry.last_attempt_ms.store(timestamp, std::memory_order_relaxed);
    entry.read_errors.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(entry.error_mutex);
    entry.last_error = error;
}

std::vector<DeviceStatusSnapshot> DeviceHealthRegistry::snapshot() const {
    std::vector<DeviceStatusSnapshot> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) {
        DeviceStatusSnapshot snapshot;
        snapshot.name = entry->spec.name;
        snapshot.display_name = entry->spec.display_name;
        snapshot.host = entry->spec.host;
        snapshot.port = entry->spec.port;
        snapshot.unit_id = entry->spec.unit_id;
        snapshot.poll_interval_ms = entry->spec.poll_interval_ms;
        snapshot.online = entry->online.load(std::memory_order_relaxed);
        snapshot.last_attempt_ms =
            entry->last_attempt_ms.load(std::memory_order_relaxed);
        snapshot.last_sample_ms =
            entry->last_sample_ms.load(std::memory_order_relaxed);
        snapshot.read_errors = entry->read_errors.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(entry->error_mutex);
            snapshot.last_error = entry->last_error;
        }
        result.push_back(std::move(snapshot));
    }
    return result;
}

AcquisitionEngine::AcquisitionEngine(const std::vector<DeviceSpec>& devices,
                                     const MetricRegistry& registry, SampleBus& bus,
                                     DeviceHealthRegistry& health)
    : devices_(devices), registry_(registry), bus_(bus), health_(health) {
    if (health_.size() != devices_.size()) {
        throw std::invalid_argument("device health registry and device list disagree");
    }

    workers_.reserve(devices_.size());
    for (std::size_t i = 0; i < devices_.size(); ++i) {
        auto worker = std::make_unique<Worker>();
        worker->spec = &devices_[i];
        for (const std::uint32_t metric_id : devices_[i].metric_ids) {
            const MetricSpec* metric = registry_.by_id(metric_id);
            if (metric == nullptr) {
                throw std::invalid_argument("device references an unknown metric id");
            }
            worker->metrics.push_back(metric);
        }
        worker->client = std::make_unique<ModbusTcpClient>(
            devices_[i].host, devices_[i].port, devices_[i].unit_id);
        build_plan(*worker);
        workers_.push_back(std::move(worker));
    }
}

AcquisitionEngine::~AcquisitionEngine() {
    stop();
}

void AcquisitionEngine::build_plan(Worker& worker) const {
    std::vector<const MetricSpec*> ordered = worker.metrics;
    std::sort(ordered.begin(), ordered.end(),
              [](const MetricSpec* left, const MetricSpec* right) {
                  const std::uint8_t left_function =
                      register_function_code(left->area);
                  const std::uint8_t right_function =
                      register_function_code(right->area);
                  if (left_function != right_function) {
                      return left_function < right_function;
                  }
                  return left->address < right->address;
              });

    for (const MetricSpec* metric : ordered) {
        const std::uint8_t function = register_function_code(metric->area);
        const bool bits = is_bit_area(metric->area);
        const std::uint32_t width =
            bits ? 1U : static_cast<std::uint32_t>(register_width(metric->value_type));
        const std::uint32_t address = metric->address;
        const std::uint32_t end = address + width;
        const std::uint32_t limit =
            bits ? modbus::kMaxBitQuantity : modbus::kMaxRegisterQuantity;

        bool appended = false;
        if (!worker.blocks.empty()) {
            ReadBlock& block = worker.blocks.back();
            if (block.function == function && block.bits == bits) {
                const std::uint32_t block_start = block.start;
                const std::uint32_t block_end =
                    static_cast<std::uint32_t>(block.start) + block.count;
                const std::uint32_t new_end = std::max(block_end, end);
                if (new_end - block_start <= limit) {
                    block.count = static_cast<std::uint16_t>(new_end - block_start);
                    block.metrics.push_back(metric);
                    appended = true;
                }
            }
        }

        if (!appended) {
            ReadBlock block;
            block.function = function;
            block.bits = bits;
            block.start = metric->address;
            block.count = static_cast<std::uint16_t>(width);
            block.metrics.push_back(metric);
            worker.blocks.push_back(std::move(block));
        }
    }

    EG_INFO("acquisition plan for %s: %zu metrics in %zu Modbus reads every %ums",
            worker.spec->name.c_str(), worker.metrics.size(),
            worker.blocks.size(), worker.spec->poll_interval_ms);
}

bool AcquisitionEngine::poll_once(Worker& worker, std::size_t device_index) {
    const TimestampMs timestamp = unix_ms_now();
    std::vector<Sample> samples;
    samples.reserve(worker.metrics.size());

    for (const ReadBlock& block : worker.blocks) {
        if (block.bits) {
            const std::vector<std::uint8_t> bits =
                worker.client->read_bits(block.function, block.start, block.count);
            if (bits.size() < block.count) {
                throw ModbusError("short coil response");
            }
            for (const MetricSpec* metric : block.metrics) {
                const std::size_t offset =
                    static_cast<std::size_t>(metric->address - block.start);
                if (offset >= bits.size()) {
                    throw ModbusError("coil metric falls outside response");
                }
                samples.push_back(Sample{metric->id, timestamp,
                                         bits[offset] != 0 ? 1.0 : 0.0});
            }
        } else {
            const std::vector<std::uint16_t> registers =
                worker.client->read_registers(block.function, block.start, block.count);
            for (const MetricSpec* metric : block.metrics) {
                const std::size_t offset =
                    static_cast<std::size_t>(metric->address - block.start);
                const std::size_t width = register_width(metric->value_type);
                if (offset + width > registers.size()) {
                    throw ModbusError("register metric falls outside response");
                }
                const double value = decode_register(*metric, registers, offset);
                if (!std::isfinite(value)) {
                    EG_DEBUG("skipping non-finite sample for %s",
                             metric->key.c_str());
                    continue;
                }
                samples.push_back(Sample{metric->id, timestamp, value});
            }
        }
    }

    for (const Sample& sample : samples) {
        if (!bus_.publish(device_index, sample) && !bus_.closed()) {
            EG_DEBUG("device %s sample queue is full; sample dropped",
                     worker.spec->name.c_str());
        }
    }
    return true;
}

void AcquisitionEngine::worker_loop(std::size_t device_index) {
    Worker& worker = *workers_[device_index];
    const auto interval =
        std::chrono::milliseconds(worker.spec->poll_interval_ms);

    while (!stop_requested_.load(std::memory_order_acquire)) {
        const TimestampMs attempt_time = unix_ms_now();
        health_.record_attempt(device_index, attempt_time);

        bool success = false;
        std::string failure;
        try {
            success = poll_once(worker, device_index);
        } catch (const std::exception& ex) {
            failure = ex.what();
        }

        std::chrono::steady_clock::time_point wake_at;
        const auto now = std::chrono::steady_clock::now();

        if (success) {
            const TimestampMs completed_at = unix_ms_now();
            health_.record_success(device_index, completed_at);
            if (!worker.ever_succeeded) {
                EG_INFO("device %s is online via %s:%u (unit %u)",
                        worker.spec->name.c_str(), worker.spec->host.c_str(),
                        static_cast<unsigned>(worker.spec->port),
                        static_cast<unsigned>(worker.spec->unit_id));
                worker.ever_succeeded = true;
            } else if (worker.consecutive_failures > 0) {
                EG_INFO("device %s recovered after %llu failed polls",
                        worker.spec->name.c_str(),
                        static_cast<unsigned long long>(worker.consecutive_failures));
            }
            worker.consecutive_failures = 0;
            wake_at = now + interval;
        } else {
            ++worker.consecutive_failures;
            worker.client->close();
            health_.record_failure(device_index, unix_ms_now(), failure);

            const std::uint64_t exponent =
                std::min<std::uint64_t>(worker.consecutive_failures - 1, 4);
            std::chrono::milliseconds backoff =
                interval * static_cast<std::chrono::milliseconds::rep>(1ULL << exponent);
            const auto max_backoff = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::seconds(30));
            backoff = std::min(backoff, max_backoff);
            if (backoff < std::chrono::milliseconds(500)) {
                backoff = std::chrono::milliseconds(500);
            }
            if (worker.consecutive_failures == 1 ||
                worker.consecutive_failures % 20 == 0) {
                EG_WARN("device %s poll failed (%llu in a row): %s",
                        worker.spec->name.c_str(),
                        static_cast<unsigned long long>(worker.consecutive_failures),
                        failure.c_str());
            }
            wake_at = now + backoff;
        }

        std::unique_lock<std::mutex> lock(wait_mutex_);
        const bool stopping = wait_cv_.wait_until(lock, wake_at, [this] {
            return stop_requested_.load(std::memory_order_acquire);
        });
        if (stopping) {
            break;
        }
    }

    worker.client->close();
}

void AcquisitionEngine::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        throw std::runtime_error("acquisition engine is already running");
    }
    stop_requested_.store(false, std::memory_order_release);

    try {
        for (std::size_t i = 0; i < workers_.size(); ++i) {
            workers_[i]->thread = std::thread([this, i] { worker_loop(i); });
        }
    } catch (...) {
        stop_requested_.store(true, std::memory_order_release);
        wait_cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker->thread.joinable()) {
                worker->thread.join();
            }
        }
        running_.store(false, std::memory_order_release);
        throw;
    }

    EG_INFO("acquisition engine started with %zu device worker(s)", workers_.size());
}

void AcquisitionEngine::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(wait_mutex_);
        stop_requested_.store(true, std::memory_order_release);
    }
    wait_cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    EG_INFO("acquisition engine stopped");
}

}  // namespace eg
