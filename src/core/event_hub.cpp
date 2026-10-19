#include "core/event_hub.h"

#include <utility>

namespace eg {
namespace {
constexpr std::size_t kMaxQueuedEvents = 512;
}

std::uint64_t EventHub::subscribe() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_.load(std::memory_order_relaxed)) {
        return 0;
    }
    const std::uint64_t id = next_id_++;
    subscribers_[id] = std::make_shared<Subscriber>();
    return id;
}

void EventHub::unsubscribe(std::uint64_t id) {
    std::shared_ptr<Subscriber> subscriber;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = subscribers_.find(id);
        if (it == subscribers_.end()) {
            return;
        }
        subscriber = it->second;
        subscribers_.erase(it);
    }
    {
        std::lock_guard<std::mutex> lock(subscriber->mutex);
        subscriber->closed = true;
    }
    subscriber->cv.notify_all();
}

void EventHub::publish(StreamEvent event) {
    if (closed_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : subscribers_) {
        Subscriber& subscriber = *entry.second;
        {
            std::lock_guard<std::mutex> subscriber_lock(subscriber.mutex);
            if (subscriber.closed) {
                continue;
            }
            if (subscriber.events.size() >= kMaxQueuedEvents) {
                subscriber.events.pop_front();
            }
            subscriber.events.push_back(event);
        }
        subscriber.cv.notify_one();
    }
}

std::vector<StreamEvent> EventHub::wait_for(std::uint64_t id,
                                            std::chrono::milliseconds timeout) {
    std::shared_ptr<Subscriber> subscriber;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = subscribers_.find(id);
        if (it == subscribers_.end()) {
            return {};
        }
        subscriber = it->second;
    }

    std::vector<StreamEvent> result;
    std::unique_lock<std::mutex> lock(subscriber->mutex);
    subscriber->cv.wait_for(lock, timeout, [&subscriber] {
        return !subscriber->events.empty() || subscriber->closed;
    });
    while (!subscriber->events.empty()) {
        result.push_back(std::move(subscriber->events.front()));
        subscriber->events.pop_front();
    }
    return result;
}

bool EventHub::has_subscribers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !subscribers_.empty();
}

void EventHub::close() {
    closed_.store(true, std::memory_order_release);
    std::vector<std::shared_ptr<Subscriber>> subscribers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscribers.reserve(subscribers_.size());
        for (auto& entry : subscribers_) {
            subscribers.push_back(std::move(entry.second));
        }
        subscribers_.clear();
    }
    for (const auto& subscriber : subscribers) {
        {
            std::lock_guard<std::mutex> lock(subscriber->mutex);
            subscriber->closed = true;
        }
        subscriber->cv.notify_all();
    }
}

}  // namespace eg
