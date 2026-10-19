#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "core/sample_bus.h"
#include "core/spsc_queue.h"
#include "test_framework.h"

TEST_CASE(spsc_queue_wraps_and_reports_full) {
    eg::SpscQueue<int> queue(3);
    REQUIRE_EQ(queue.capacity(), static_cast<std::size_t>(4));

    REQUIRE(queue.try_push(1));
    REQUIRE(queue.try_push(2));
    REQUIRE(queue.try_push(3));
    REQUIRE(queue.try_push(4));
    REQUIRE(!queue.try_push(5));
    REQUIRE_EQ(queue.size_approx(), static_cast<std::size_t>(4));

    int value = 0;
    REQUIRE(queue.try_pop(value));
    REQUIRE_EQ(value, 1);
    REQUIRE(queue.try_pop(value));
    REQUIRE_EQ(value, 2);

    REQUIRE(queue.try_push(5));
    REQUIRE(queue.try_push(6));
    REQUIRE(!queue.try_push(7));

    REQUIRE(queue.try_pop(value));
    REQUIRE_EQ(value, 3);
    REQUIRE(queue.try_pop(value));
    REQUIRE_EQ(value, 4);
    REQUIRE(queue.try_pop(value));
    REQUIRE_EQ(value, 5);
    REQUIRE(queue.try_pop(value));
    REQUIRE_EQ(value, 6);
    REQUIRE(!queue.try_pop(value));
}

TEST_CASE(spsc_queue_transfers_between_threads) {
    constexpr std::uint64_t kItems = 200000;
    eg::SpscQueue<std::uint64_t> queue(256);
    std::atomic<bool> sequence_ok{true};
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kItems; ++i) {
            while (!queue.try_push(i)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        std::uint64_t expected = 0;
        std::uint64_t value = 0;
        while (expected < kItems) {
            if (!queue.try_pop(value)) {
                if (producer_done.load(std::memory_order_acquire) && queue.empty() &&
                    expected < kItems) {
                    std::this_thread::yield();
                }
                continue;
            }
            if (value != expected) {
                sequence_ok.store(false, std::memory_order_relaxed);
                return;
            }
            ++expected;
        }
    });

    producer.join();
    consumer.join();
    REQUIRE(sequence_ok.load(std::memory_order_relaxed));
}

TEST_CASE(sample_bus_drains_all_devices_and_drops_when_full) {
    eg::SampleBus bus(2, 16);
    for (std::uint32_t i = 0; i < 3; ++i) {
        REQUIRE(bus.publish(0, eg::Sample{i, 1000 + i, static_cast<double>(i)}));
    }
    REQUIRE(bus.publish(1, eg::Sample{10, 2000, 42.0}));
    REQUIRE(bus.publish(1, eg::Sample{11, 2001, 43.0}));

    std::vector<std::uint32_t> ids;
    const std::size_t count = bus.drain(
        [&](const eg::Sample& sample) { ids.push_back(sample.metric_id); });
    REQUIRE_EQ(count, static_cast<std::size_t>(5));
    REQUIRE_EQ(ids.size(), static_cast<std::size_t>(5));

    bus.close();
    REQUIRE(bus.wait_for_data(std::chrono::milliseconds(1)));
    REQUIRE(!bus.publish(0, eg::Sample{0, 1, 1.0}));

    eg::SampleBus small(1, 8);
    for (std::uint32_t i = 0; i < 32; ++i) {
        small.publish(0, eg::Sample{i, 1, static_cast<double>(i)});
    }
    REQUIRE_EQ(small.dropped(0), static_cast<std::size_t>(24));
    const std::size_t queued_before = small.queue_size(0);
    const std::size_t seen =
        small.drain([&](const eg::Sample&) {}, 0);
    REQUIRE_EQ(seen, queued_before);
}
