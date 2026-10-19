#include <cstdint>
#include <string>
#include <vector>

#include "core/metrics.h"
#include "core/rules/compiler.h"
#include "core/rules/lexer.h"
#include "core/rules/parser.h"
#include "core/rules/rule_engine.h"
#include "core/types.h"
#include "test_framework.h"

namespace {

eg::MetricRegistry make_registry() {
    eg::MetricRegistry registry;
    eg::MetricSpec temperature;
    temperature.device = "boiler";
    temperature.name = "temp";
    temperature.area = eg::RegisterArea::HoldingRegister;
    temperature.value_type = eg::RegisterValueType::U16;
    temperature.scale = 1.0;
    registry.add(temperature);

    eg::MetricSpec pump;
    pump.device = "boiler";
    pump.name = "pump";
    pump.area = eg::RegisterArea::Coil;
    pump.value_type = eg::RegisterValueType::Bool;
    registry.add(pump);
    return registry;
}

const char* kDebounceRule =
    "rule high_temp \"High temperature\" {\n"
    "  when boiler.temp > 80.0\n"
    "  for 3s\n"
    "  clear when boiler.temp < 70.0 for 2s\n"
    "  emit {\n"
    "    severity = critical\n"
    "    code = HIGH_TEMP\n"
    "    message = \"temperature too high\"\n"
    "  }\n"
    "}\n";

}  // namespace

TEST_CASE(rule_compiler_parses_and_type_checks) {
    const eg::MetricRegistry registry = make_registry();
    const std::string source =
        "rule high_temp \"High temperature\" {\n"
        "  when boiler.temp > 80.0 and not boiler.pump\n"
        "  for 3s\n"
        "  clear when boiler.temp < 70.0 for 2s\n"
        "  emit {\n"
        "    severity = critical\n"
        "    code = HIGH_TEMP\n"
        "    message = \"too hot\"\n"
        "  }\n"
        "}\n";

    eg::RuleCompiler compiler(registry);
    const eg::RuleProgram program = compiler.compile_source(source);
    REQUIRE_EQ(program.rules.size(), static_cast<std::size_t>(1));
    const eg::CompiledRule& rule = program.rules.front();
    REQUIRE_EQ(rule.name, std::string("high_temp"));
    REQUIRE_EQ(rule.title, std::string("High temperature"));
    REQUIRE_EQ(rule.severity, std::string("critical"));
    REQUIRE_EQ(rule.code, std::string("HIGH_TEMP"));
    REQUIRE_EQ(rule.for_ms, static_cast<std::uint64_t>(3000));
    REQUIRE(rule.has_clear);
    REQUIRE_EQ(rule.clear_for_ms, static_cast<std::uint64_t>(2000));
    REQUIRE_EQ(rule.referenced_metrics.size(), static_cast<std::size_t>(2));
    REQUIRE(!rule.when.code.empty());

    const eg::MetricSpec* temperature = registry.find("boiler", "temp");
    const eg::MetricSpec* pump = registry.find("boiler", "pump");
    REQUIRE(temperature != nullptr);
    REQUIRE(pump != nullptr);

    std::vector<double> latest(registry.size(), 0.0);
    std::vector<bool> available(registry.size(), true);
    latest[temperature->id] = 85.0;
    latest[pump->id] = 0.0;
    REQUIRE(eg::evaluate_condition(rule.when, latest, available));
    latest[pump->id] = 1.0;
    REQUIRE(!eg::evaluate_condition(rule.when, latest, available));

    latest[temperature->id] = 65.0;
    latest[pump->id] = 0.0;
    REQUIRE(eg::evaluate_condition(rule.clear, latest, available));
}

TEST_CASE(rule_compiler_reports_type_and_syntax_errors) {
    const eg::MetricRegistry registry = make_registry();
    eg::RuleCompiler compiler(registry);

    REQUIRE_THROW(compiler.compile_source(
                      "rule bad \"bad\" { when boiler.temp emit { severity = info "
                      "code = X message = \"x\" } }"),
                  eg::RuleError);

    REQUIRE_THROW(compiler.compile_source(
                      "rule bad \"bad\" { when boiler.unknown > 1.0 emit { severity "
                      "= info code = X message = \"x\" } }"),
                  eg::RuleError);

    REQUIRE_THROW(compiler.compile_source(
                      "rule bad \"bad\" { when boiler.temp > true emit { severity = "
                      "info code = X message = \"x\" } }"),
                  eg::RuleError);

    REQUIRE_THROW(compiler.compile_source(
                      "rule bad \"bad\" { when boiler.temp > 1.0 for 2 emit { "
                      "severity = info code = X message = \"x\" } }"),
                  eg::RuleError);
}

TEST_CASE(rule_engine_applies_debounce_hysteresis_and_rearms) {
    const eg::MetricRegistry registry = make_registry();
    const eg::MetricSpec* temperature = registry.find("boiler", "temp");
    REQUIRE(temperature != nullptr);

    eg::RuleCompiler compiler(registry);
    const eg::RuleProgram program = compiler.compile_source(kDebounceRule);
    eg::RuleEngine engine(program, registry);

    std::vector<eg::AlarmEvent> events;
    engine.set_alarm_callback(
        [&events](const eg::AlarmEvent& event) { events.push_back(event); });

    auto feed = [&](std::int64_t timestamp, double value) {
        engine.on_sample(eg::Sample{temperature->id, timestamp, value});
    };

    feed(0, 85.0);
    feed(1000, 85.0);
    feed(2999, 85.0);
    REQUIRE(events.empty());
    REQUIRE_EQ(engine.active_count(), static_cast<std::size_t>(0));

    feed(3000, 85.0);
    REQUIRE_EQ(events.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(events[0].action, eg::AlarmAction::Raised);
    REQUIRE_EQ(events[0].code, std::string("HIGH_TEMP"));
    REQUIRE_EQ(events[0].trigger_metric, std::string("boiler.temp"));
    REQUIRE_EQ(engine.active_count(), static_cast<std::size_t>(1));

    feed(4000, 75.0);
    feed(5000, 69.0);
    feed(6999, 69.0);
    REQUIRE_EQ(events.size(), static_cast<std::size_t>(1));
    feed(7000, 69.0);
    REQUIRE_EQ(events.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(events[1].action, eg::AlarmAction::Cleared);
    REQUIRE_EQ(engine.active_count(), static_cast<std::size_t>(0));

    feed(8000, 85.0);
    REQUIRE_EQ(events.size(), static_cast<std::size_t>(2));
    feed(11000, 85.0);
    REQUIRE_EQ(events.size(), static_cast<std::size_t>(3));
    REQUIRE_EQ(events[2].action, eg::AlarmAction::Raised);
}

TEST_CASE(rule_engine_waits_for_all_referenced_metrics) {
    const eg::MetricRegistry registry = make_registry();
    const eg::MetricSpec* temperature = registry.find("boiler", "temp");
    const eg::MetricSpec* pump = registry.find("boiler", "pump");
    REQUIRE(temperature != nullptr);
    REQUIRE(pump != nullptr);

    const std::string source =
        "rule interlock \"Interlock\" {\n"
        "  when boiler.temp > 80.0 and boiler.pump == false\n"
        "  for 0s\n"
        "  emit {\n"
        "    severity = warning\n"
        "    code = INTERLOCK\n"
        "    message = \"interlock\"\n"
        "  }\n"
        "}\n";

    eg::RuleCompiler compiler(registry);
    const eg::RuleProgram program = compiler.compile_source(source);
    eg::RuleEngine engine(program, registry);
    std::vector<eg::AlarmEvent> events;
    engine.set_alarm_callback(
        [&events](const eg::AlarmEvent& event) { events.push_back(event); });

    engine.on_sample(eg::Sample{temperature->id, 1000, 90.0});
    REQUIRE(events.empty());
    engine.on_sample(eg::Sample{pump->id, 1000, 0.0});
    REQUIRE_EQ(events.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(events[0].action, eg::AlarmAction::Raised);
}
