#pragma once

#include <cstdint>
#include <string>

namespace eg {

using TimestampMs = std::int64_t;

struct Sample {
    std::uint32_t metric_id = 0;
    TimestampMs timestamp_ms = 0;
    double value = 0.0;
};

enum class AlarmAction { Raised = 0, Cleared = 1 };

struct AlarmEvent {
    TimestampMs timestamp_ms = 0;
    AlarmAction action = AlarmAction::Raised;
    std::string rule_name;
    std::string rule_title;
    std::string severity;
    std::string code;
    std::string message;
    std::string trigger_metric;
    std::string trigger_device;
    double trigger_value = 0.0;
};

inline const char* alarm_action_name(AlarmAction action) {
    return action == AlarmAction::Raised ? "raised" : "cleared";
}

}  // namespace eg
