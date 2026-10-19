#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace eg {

// Bounded single-producer/single-consumer ring buffer. The producer owns
// head_ and the consumer owns tail_. Capacity is rounded up to a power of two
// so index masking is cheap; both indices are monotonically increasing.
template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(std::size_t requested_capacity) {
        std::size_t capacity = 2;
        while (capacity < requested_capacity) {
            if (capacity > (static_cast<std::size_t>(-1) >> 1)) {
                throw std::length_error("SPSC queue capacity is too large");
            }
            capacity <<= 1;
        }
        capacity_ = capacity;
        mask_ = capacity_ - 1;
        buffer_.resize(capacity_);
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    bool try_push(const T& value) {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) {
            return false;
        }
        buffer_[static_cast<std::size_t>(head & mask_)] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool try_push(T&& value) {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) {
            return false;
        }
        buffer_[static_cast<std::size_t>(head & mask_)] = std::move(value);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        out = std::move(buffer_[static_cast<std::size_t>(tail & mask_)]);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const { return capacity_; }

    std::size_t size_approx() const {
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        return head >= tail ? static_cast<std::size_t>(head - tail) : 0;
    }

    bool empty() const {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

private:
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    std::vector<T> buffer_;

    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
};

}  // namespace eg
