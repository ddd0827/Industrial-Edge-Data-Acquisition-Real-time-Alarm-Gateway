#include "core/rules/rule_engine.h"

#include <stdexcept>
#include <utility>

namespace eg {

RuleEngine::RuleEngine(const RuleProgram& program, const MetricRegistry& registry)
    : program_(program),
      registry_(registry),
      states_(program.rules.size()),
      latest_(registry.size(), 0.0),
      available_(registry.size(), false),
      rules_by_metric_(registry.size()) {
    for (std::size_t rule_index = 0; rule_index < program.rules.size(); ++rule_index) {
        for (const std::uint32_t metric_id : program.rules[rule_index].referenced_metrics) {
            if (metric_id >= registry_.size()) {
                throw std::invalid_argument("compiled rule references an unknown metric");
            }
            rules_by_metric_[metric_id].push_back(rule_index);
        }
    }
}

void RuleEngine::set_alarm_callback(AlarmCallback callback) {
    callback_ = std::move(callback);
}

bool RuleEngine::references_available(const CompiledRule& rule) const {
    for (const std::uint32_t metric_id : rule.referenced_metrics) {
        if (!available_[metric_id]) {
            return false;
        }
    }
    return true;
}

bool RuleEngine::duration_elapsed(TimestampMs now, TimestampMs since,
                                  std::uint64_t duration_ms) const {
    if (duration_ms == 0) {
        return true;
    }
    if (now < since) {
        return false;
    }
    const std::uint64_t elapsed = static_cast<std::uint64_t>(now - since);
    return elapsed >= duration_ms;
}

void RuleEngine::emit_alarm(std::size_t rule_index, const Sample& sample,
                            AlarmAction action) {
    const CompiledRule& rule = program_.rules[rule_index];
    AlarmEvent event;
    event.timestamp_ms = sample.timestamp_ms;
    event.action = action;
    event.rule_name = rule.name;
    event.rule_title = rule.title;
    event.severity = rule.severity;
    event.code = rule.code;
    event.message = rule.message;
    event.trigger_value = sample.value;
    if (const MetricSpec* metric = registry_.by_id(sample.metric_id)) {
        event.trigger_metric = metric->key;
        event.trigger_device = metric->device;
    }

    if (action == AlarmAction::Raised) {
        active_count_.fetch_add(1, std::memory_order_relaxed);
    } else {
        const std::size_t current = active_count_.load(std::memory_order_relaxed);
        if (current > 0) {
            active_count_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    if (callback_) {
        callback_(event);
    }
}

void RuleEngine::evaluate_rule(std::size_t rule_index, const Sample& sample) {
    const CompiledRule& rule = program_.rules[rule_index];
    RuntimeState& state = states_[rule_index];

    if (!state.active) {
        const bool condition = evaluate_condition(rule.when, latest_, available_);
        if (!condition) {
            state.pending = false;
            return;
        }

        if (!state.pending || sample.timestamp_ms < state.pending_since) {
            state.pending = true;
            state.pending_since = sample.timestamp_ms;
        }
        if (duration_elapsed(sample.timestamp_ms, state.pending_since, rule.for_ms)) {
            state.active = true;
            state.pending = false;
            state.clearing = false;
            emit_alarm(rule_index, sample, AlarmAction::Raised);
        }
        return;
    }

    bool should_clear = false;
    if (rule.has_clear) {
        const bool clear = evaluate_condition(rule.clear, latest_, available_);
        if (clear) {
            if (!state.clearing || sample.timestamp_ms < state.clearing_since) {
                state.clearing = true;
                state.clearing_since = sample.timestamp_ms;
            }
            should_clear = duration_elapsed(sample.timestamp_ms, state.clearing_since,
                                            rule.clear_for_ms);
        } else {
            state.clearing = false;
        }
    } else {
        const bool condition = evaluate_condition(rule.when, latest_, available_);
        should_clear = !condition;
    }

    if (should_clear) {
        state.active = false;
        state.clearing = false;
        emit_alarm(rule_index, sample, AlarmAction::Cleared);
    }
}

void RuleEngine::on_sample(const Sample& sample) {
    if (sample.metric_id >= latest_.size()) {
        return;
    }

    latest_[sample.metric_id] = sample.value;
    available_[sample.metric_id] = true;

    for (const std::size_t rule_index : rules_by_metric_[sample.metric_id]) {
        const CompiledRule& rule = program_.rules[rule_index];
        if (!references_available(rule)) {
            continue;
        }
        evaluate_rule(rule_index, sample);
    }
}

}  // namespace eg
