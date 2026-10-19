#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace eg {

struct StreamEvent {
    std::string type;
    std::string data;
};

// Bounded fan-out for Server-Sent Events. Slow clients drop the oldest events
// rather than blocking the acquisition pipeline.
class EventHub {
public:
    std::uint64_t subscribe();
    void unsubscribe(std::uint64_t id);
    void publish(StreamEvent event);
    std::vector<StreamEvent> wait_for(std::uint64_t id,
                                      std::chrono::milliseconds timeout);
    bool closed() const { return closed_.load(std::memory_order_acquire); }
    bool has_subscribers() const;
    void close();

private:
    struct Subscriber {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<StreamEvent> events;
        bool closed = false;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Subscriber>> subscribers_;
    std::uint64_t next_id_ = 1;
    std::atomic<bool> closed_{false};
};

}  // namespace eg
