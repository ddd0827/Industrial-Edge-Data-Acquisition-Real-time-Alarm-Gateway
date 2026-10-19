#include "core/sample_bus.h"

#include <stdexcept>

namespace eg {

SampleBus::SampleBus(std::size_t device_count, std::size_t per_device_capacity) {
    if (device_count == 0) {
        throw std::invalid_argument("SampleBus requires at least one device");
    }
    queues_.reserve(device_count);
    for (std::size_t i = 0; i < device_count; ++i) {
        queues_.push_back(std::make_unique<DeviceQueue>(per_device_capacity));
    }
}

bool SampleBus::publish(std::size_t device_index, const Sample& sample) {
    if (device_index >= queues_.size() || closed_.load(std::memory_order_acquire)) {
        return false;
    }
    DeviceQueue& target = *queues_[device_index];
    if (!target.queue.try_push(sample)) {
        target.dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(notify_mutex_);
        ++pending_notifications_;
    }
    notify_cv_.notify_one();
    return true;
}

bool SampleBus::wait_for_data(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(notify_mutex_);
    const bool signaled = notify_cv_.wait_for(lock, timeout, [this] {
        return pending_notifications_ > 0 || closed_.load(std::memory_order_acquire);
    });
    if (!signaled) {
        return false;
    }
    pending_notifications_ = 0;
    return true;
}

void SampleBus::close() {
    closed_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(notify_mutex_);
        // Acquire/release ordering with producers is enough for shutdown. Data
        // already in a queue remains drainable.
    }
    notify_cv_.notify_all();
}

std::size_t SampleBus::dropped(std::size_t device_index) const {
    if (device_index >= queues_.size()) {
        return 0;
    }
    return static_cast<std::size_t>(
        queues_[device_index]->dropped.load(std::memory_order_relaxed));
}

std::size_t SampleBus::queue_size(std::size_t device_index) const {
    if (device_index >= queues_.size()) {
        return 0;
    }
    return queues_[device_index]->queue.size_approx();
}

}  // namespace eg
