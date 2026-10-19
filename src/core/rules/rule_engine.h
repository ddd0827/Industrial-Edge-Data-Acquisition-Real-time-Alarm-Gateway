#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <vector>

#include "core/metrics.h"
#include "core/rules/compiler.h"
#include "core/types.h"

namespace eg {

// Evaluates compiled rules against the sample stream. on_sample must be called
// from a single processing thread; active_count() may be read concurrently.
class RuleEngine {
public:
    using AlarmCallback = std::function<void(const AlarmEvent&)>;

    RuleEngine(const RuleProgram& program, const MetricRegistry& registry);

    void set_alarm_callback(AlarmCallback callback);
    void on_sample(const Sample& sample);

    std::size_t active_count() const {
        return active_count_.load(std::memory_order_relaxed);
    }

    const RuleProgram& program() const { return program_; }

private:
    struct RuntimeState {
        bool pending = false;
        TimestampMs pending_since = 0;
        bool active = false;
        bool clearing = false;
        TimestampMs clearing_since = 0;
    };

    bool references_available(const CompiledRule& rule) const;
    void evaluate_rule(std::size_t rule_index, const Sample& sample);
    void emit_alarm(std::size_t rule_index, const Sample& sample, AlarmAction action);
    bool duration_elapsed(TimestampMs now, TimestampMs since,
                          std::uint64_t duration_ms) const;

    const RuleProgram& program_;
    const MetricRegistry& registry_;
    std::vector<RuntimeState> states_;
    std::vector<double> latest_;
    std::vector<bool> available_;
    std::vector<std::vector<std::size_t>> rules_by_metric_;
    AlarmCallback callback_;
    std::atomic<std::size_t> active_count_{0};
};

}  // namespace eg
