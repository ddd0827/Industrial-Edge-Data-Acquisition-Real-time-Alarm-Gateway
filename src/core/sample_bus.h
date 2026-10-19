#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "core/spsc_queue.h"
#include "core/types.h"

namespace eg {

// Decouples acquisition threads from the persistence/rule-processing stage.
// Each device owns a lock-free SPSC queue; a single consumer drains all queues.
class SampleBus {
public:
    SampleBus(std::size_t device_count, std::size_t per_device_capacity);

    bool publish(std::size_t device_index, const Sample& sample);
    bool wait_for_data(std::chrono::milliseconds timeout);
    void close();
    bool closed() const { return closed_.load(std::memory_order_acquire); }

    std::size_t dropped(std::size_t device_index) const;
    std::size_t queue_size(std::size_t device_index) const;

    template <typename Fn>
    std::size_t drain(Fn&& fn, std::size_t max_samples = 0) {
        std::size_t drained = 0;
        bool progress = true;
        while (progress && (max_samples == 0 || drained < max_samples)) {
            progress = false;
            for (auto& queue : queues_) {
                Sample sample;
                if (!queue->queue.try_pop(sample)) {
                    continue;
                }
                fn(sample);
                ++drained;
                progress = true;
                if (max_samples != 0 && drained >= max_samples) {
                    break;
                }
            }
        }
        return drained;
    }

private:
    struct DeviceQueue {
        explicit DeviceQueue(std::size_t capacity) : queue(capacity) {}
        SpscQueue<Sample> queue;
        std::atomic<std::uint64_t> dropped{0};
    };

    std::vector<std::unique_ptr<DeviceQueue>> queues_;
    mutable std::mutex notify_mutex_;
    std::condition_variable notify_cv_;
    std::uint64_t pending_notifications_ = 0;
    std::atomic<bool> closed_{false};
};

}  // namespace eg
