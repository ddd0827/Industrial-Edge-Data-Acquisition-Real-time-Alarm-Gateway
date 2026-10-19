#include "app/application.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/config.h"
#include "common/json.h"
#include "common/log.h"
#include "common/util.h"
#include "core/acquisition.h"
#include "core/event_hub.h"
#include "core/http/http_server.h"
#include "core/metrics.h"
#include "core/rules/compiler.h"
#include "core/rules/rule_engine.h"
#include "core/sample_bus.h"
#include "core/storage/series_store.h"
#include "core/storage/wal.h"
#include "core/types.h"
#include "protocol/simulator.h"

#ifndef EDGEGUARD_VERSION
#define EDGEGUARD_VERSION "1.0.0"
#endif

namespace eg {
namespace {

constexpr std::size_t kAlarmHistoryLimit = 500;
constexpr auto kSseHeartbeat = std::chrono::seconds(15);

const std::string* query_value(const HttpRequest& request, const char* key) {
    const auto it = request.query.find(key);
    return it == request.query.end() ? nullptr : &it->second;
}

HttpResponse json_response(int status, std::string body) {
    HttpResponse response;
    response.status = status;
    response.content_type = "application/json; charset=utf-8";
    response.body = std::move(body);
    return response;
}

HttpResponse json_error(int status, const std::string& message) {
    JsonWriter writer;
    writer.begin_object();
    writer.key("error").value(message);
    writer.end_object();
    return json_response(status, writer.take());
}

std::string alarm_to_json(const AlarmEvent& event) {
    JsonWriter writer;
    writer.begin_object();
    writer.key("ts").value(event.timestamp_ms);
    writer.key("action").value(alarm_action_name(event.action));
    writer.key("rule").value(event.rule_name);
    writer.key("title").value(event.rule_title);
    writer.key("severity").value(event.severity);
    writer.key("code").value(event.code);
    writer.key("message").value(event.message);
    writer.key("metric").value(event.trigger_metric);
    writer.key("device").value(event.trigger_device);
    writer.key("value").value(event.trigger_value);
    writer.end_object();
    return writer.take();
}

std::string sample_to_json(const MetricSpec& metric, const Sample& sample) {
    JsonWriter writer;
    writer.begin_object();
    writer.key("ts").value(sample.timestamp_ms);
    writer.key("device").value(metric.device);
    writer.key("metric").value(metric.name);
    writer.key("key").value(metric.key);
    writer.key("unit").value(metric.unit);
    writer.key("type").value(metric.is_boolean() ? "bool" : "number");
    writer.key("value").value(sample.value);
    writer.end_object();
    return writer.take();
}

std::string display_host(const std::string& host) {
    if (host.empty() || host == "0.0.0.0" || host == "::") {
        return "127.0.0.1";
    }
    return host;
}

}  // namespace

struct Application::Impl {
    explicit Impl(RunOptions opts) : options(std::move(opts)) {}
    ~Impl() { shutdown(); }

    bool initialize(std::string& error);
    int run(std::atomic<bool>& quit);
    void shutdown();

    void pipeline_loop();
    void process_sample(const Sample& sample);
    void on_alarm(const AlarmEvent& event);
    void handle_stream(const HttpRequest& request, TcpSocket& socket,
                       const std::atomic<bool>& server_stop);
    HttpResponse handle_api(const HttpRequest& request);
    HttpResponse api_health() const;
    HttpResponse api_overview() const;
    HttpResponse api_series(const HttpRequest& request) const;
    HttpResponse api_alarms(const HttpRequest& request) const;
    HttpResponse api_rules() const;
    std::uint64_t uptime_ms() const;

    RunOptions options;
    AppConfig config;

    std::unique_ptr<RuleProgram> rules;
    std::unique_ptr<DeviceHealthRegistry> health;
    std::unique_ptr<SeriesStore> store;
    std::unique_ptr<SampleBus> bus;
    std::unique_ptr<EventHub> events;
    std::unique_ptr<RuleEngine> rule_engine;
    std::unique_ptr<WalWriter> wal;
    std::unique_ptr<AcquisitionEngine> acquisition;
    std::unique_ptr<SimulatorServer> simulator;
    std::unique_ptr<HttpServer> http;

    std::thread pipeline;
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> processed_samples{0};
    std::atomic<std::uint64_t> alarm_events{0};

    mutable std::mutex alarms_mutex;
    std::deque<AlarmEvent> alarms;

    std::uint64_t recovered_samples = 0;
    std::chrono::steady_clock::time_point started_monotonic{};
    bool initialized = false;
    bool ran = false;
    bool shutdown_done = false;
    std::mutex shutdown_mutex;
};

std::uint64_t Application::Impl::uptime_ms() const {
    if (started_monotonic.time_since_epoch().count() == 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_monotonic)
            .count());
}

bool Application::Impl::initialize(std::string& error) {
    if (initialized) {
        error = "application is already initialized";
        return false;
    }

    try {
        config = load_config(options.config_path);
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }

    Logger::instance().configure(config.log_level, config.log_file);
    EG_INFO("EdgeGuard %s starting", EDGEGUARD_VERSION);
    EG_INFO("config=%s metrics=%zu devices=%zu", config.source_path.c_str(),
            config.metrics.size(), config.devices.size());

    try {
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::u8path(config.web_root), ec)) {
            EG_WARN("web root does not exist: %s", config.web_root.c_str());
        }

        RuleCompiler compiler(config.metrics);
        auto compiled = std::make_unique<RuleProgram>(
            compiler.compile_file(config.rules_path));
        if (compiled->rules.empty()) {
            throw std::runtime_error("rule file does not contain any rules");
        }
        rules = std::move(compiled);

        for (const CompiledRule& rule : rules->rules) {
            if (rule.referenced_metrics.empty()) {
                throw std::runtime_error("rule '" + rule.name +
                                         "' does not reference any metric");
            }
            EG_INFO("rule %s severity=%s code=%s when_ops=%zu clear_ops=%zu refs=%zu",
                    rule.name.c_str(), rule.severity.c_str(), rule.code.c_str(),
                    rule.when.code.size(), rule.clear.code.size(),
                    rule.referenced_metrics.size());
        }
        EG_INFO("compiled %zu rules into %zu bytecode instructions", rules->rules.size(),
                rules->instruction_count());

        health = std::make_unique<DeviceHealthRegistry>(config.devices);
        store = std::make_unique<SeriesStore>(config.metrics.size(),
                                             config.retention_seconds,
                                             config.max_points_per_metric);
        events = std::make_unique<EventHub>();

        const TimestampMs cutoff =
            unix_ms_now() - static_cast<TimestampMs>(config.retention_seconds) * 1000;
        std::uint64_t replayed = 0;
        const WalRecoveryResult recovery =
            recover_wal(config.wal_path, [&](std::string_view key,
                                             TimestampMs timestamp, double value) {
                const MetricSpec* metric = config.metrics.find_key(key);
                if (metric == nullptr || timestamp < cutoff) {
                    return;
                }
                store->append(Sample{metric->id, timestamp, value});
                ++replayed;
            });
        recovered_samples = replayed;
        EG_INFO("WAL recovery: %llu samples, %llu records, %llu bytes%s",
                static_cast<unsigned long long>(recovery.samples),
                static_cast<unsigned long long>(recovery.records),
                static_cast<unsigned long long>(recovery.bytes_recovered),
                recovery.truncated ? " (torn tail truncated)" : "");

        bus = std::make_unique<SampleBus>(config.devices.size(),
                                          config.per_device_queue_capacity);
        rule_engine = std::make_unique<RuleEngine>(*rules, config.metrics);
        rule_engine->set_alarm_callback(
            [this](const AlarmEvent& event) { on_alarm(event); });
        wal = std::make_unique<WalWriter>(config.wal_path, config.metrics,
                                          config.wal_queue_capacity,
                                          config.wal_flush_interval_ms);
        acquisition = std::make_unique<AcquisitionEngine>(
            config.devices, config.metrics, *bus, *health);

        http = std::make_unique<HttpServer>(
            config.http_host, config.http_port, config.web_root,
            [this](const HttpRequest& request) { return handle_api(request); },
            [this](const HttpRequest& request, TcpSocket& socket,
                   const std::atomic<bool>& server_stop) {
                handle_stream(request, socket, server_stop);
            });

        if (options.with_simulator) {
            simulator = std::make_unique<SimulatorServer>(true);
            for (const DeviceSpec& device : config.devices) {
                const std::string host = to_lower(device.host);
                if (device.port != config.simulator_port ||
                    (host != "127.0.0.1" && host != "localhost" && host != "::1")) {
                    EG_WARN("device %s points at %s:%u, not the in-process simulator "
                            "127.0.0.1:%u",
                            device.name.c_str(), device.host.c_str(),
                            static_cast<unsigned>(device.port),
                            static_cast<unsigned>(config.simulator_port));
                }
            }
            EG_INFO("in-process Modbus simulator enabled on port %u",
                    static_cast<unsigned>(config.simulator_port));
        }

        initialized = true;
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

int Application::Impl::run(std::atomic<bool>& quit) {
    if (!initialized || ran) {
        EG_ERROR("application is not initialized or has already run");
        return 1;
    }
    ran = true;
    stopping.store(false, std::memory_order_release);
    started_monotonic = std::chrono::steady_clock::now();

    std::string error;
    if (!wal->start(error)) {
        EG_ERROR("cannot start WAL writer: %s", error.c_str());
        shutdown();
        return 1;
    }

    pipeline = std::thread([this] { pipeline_loop(); });

    if (options.with_simulator) {
        if (!simulator->start("127.0.0.1", config.simulator_port, error)) {
            EG_ERROR("cannot start in-process Modbus simulator: %s", error.c_str());
            shutdown();
            return 1;
        }
    }

    try {
        acquisition->start();
    } catch (const std::exception& ex) {
        EG_ERROR("cannot start acquisition engine: %s", ex.what());
        shutdown();
        return 1;
    }

    if (!http->start(error)) {
        EG_ERROR("cannot start HTTP server: %s", error.c_str());
        shutdown();
        return 1;
    }

    running.store(true, std::memory_order_release);
    const std::uint16_t port = http->port();
    EG_INFO("EdgeGuard ready: %zu devices, %zu rules, dashboard http://%s:%u/",
            config.devices.size(), rules->rules.size(),
            display_host(config.http_host).c_str(),
            static_cast<unsigned>(port));
    if (options.with_simulator) {
        EG_INFO("demo data is served by the embedded Modbus simulator on 127.0.0.1:%u",
                static_cast<unsigned>(config.simulator_port));
    }

    const auto deadline =
        options.duration_ms > 0
            ? std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(options.duration_ms)
            : std::chrono::steady_clock::time_point::max();

    while (!quit.load(std::memory_order_acquire) &&
           !stopping.load(std::memory_order_acquire)) {
        if (options.duration_ms > 0 &&
            std::chrono::steady_clock::now() >= deadline) {
            EG_INFO("run duration reached");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (quit.load(std::memory_order_acquire)) {
        EG_INFO("shutdown signal received");
    }
    shutdown();
    return 0;
}

void Application::Impl::shutdown() {
    std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex);
    if (shutdown_done) {
        return;
    }
    shutdown_done = true;
    stopping.store(true, std::memory_order_release);

    if (acquisition != nullptr) {
        acquisition->stop();
    }
    if (simulator != nullptr) {
        simulator->stop();
    }
    if (bus != nullptr) {
        bus->close();
    }
    if (pipeline.joinable()) {
        pipeline.join();
    }
    if (http != nullptr) {
        http->stop();
    }
    if (events != nullptr) {
        events->close();
    }
    if (wal != nullptr) {
        wal->stop();
    }
    running.store(false, std::memory_order_release);

    if (ran) {
        const std::uint64_t uptime = uptime_ms();
        EG_INFO("EdgeGuard stopped after %s: %llu samples processed, %llu alarm events, "
                "%llu WAL samples written, %llu WAL drops",
                format_duration_ms(uptime).c_str(),
                static_cast<unsigned long long>(
                    processed_samples.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    alarm_events.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(wal == nullptr ? 0 : wal->written()),
                static_cast<unsigned long long>(wal == nullptr ? 0 : wal->dropped()));
        Logger::instance().flush();
    }
}

void Application::Impl::pipeline_loop() {
    auto consume = [this](const Sample& sample) {
        try {
            process_sample(sample);
        } catch (const std::exception& ex) {
            EG_ERROR("sample pipeline error: %s", ex.what());
        }
    };

    while (true) {
        bus->wait_for_data(std::chrono::milliseconds(100));
        std::size_t drained = bus->drain(consume, config.batch_size);
        while (drained == config.batch_size && config.batch_size != 0) {
            drained = bus->drain(consume, config.batch_size);
        }
        if (bus->closed()) {
            std::size_t remaining = 0;
            do {
                remaining = bus->drain(consume, 0);
            } while (remaining > 0);
            break;
        }
    }
}

void Application::Impl::process_sample(const Sample& sample) {
    store->append(sample);
    wal->append(sample);
    rule_engine->on_sample(sample);
    processed_samples.fetch_add(1, std::memory_order_relaxed);

    if (events->has_subscribers()) {
        if (const MetricSpec* metric = config.metrics.by_id(sample.metric_id)) {
            events->publish(StreamEvent{"sample", sample_to_json(*metric, sample)});
        }
    }
}

void Application::Impl::on_alarm(const AlarmEvent& event) {
    {
        std::lock_guard<std::mutex> lock(alarms_mutex);
        alarms.push_back(event);
        while (alarms.size() > kAlarmHistoryLimit) {
            alarms.pop_front();
        }
    }
    alarm_events.fetch_add(1, std::memory_order_relaxed);

    if (events != nullptr && events->has_subscribers()) {
        events->publish(StreamEvent{"alarm", alarm_to_json(event)});
    }

    if (event.action == AlarmAction::Raised) {
        EG_WARN("ALARM [%s/%s] %s: %s (metric=%s value=%.4f)", event.severity.c_str(),
                event.code.c_str(), event.rule_name.c_str(), event.message.c_str(),
                event.trigger_metric.c_str(), event.trigger_value);
    } else {
        EG_INFO("ALARM CLEARED [%s] %s: %s", event.code.c_str(),
                event.rule_name.c_str(), event.message.c_str());
    }
}

HttpResponse Application::Impl::api_health() const {
    const auto snapshots = health->snapshot();
    std::size_t online = 0;
    for (const DeviceStatusSnapshot& snapshot : snapshots) {
        if (snapshot.online) {
            ++online;
        }
    }
    std::size_t queued = 0;
    for (std::size_t i = 0; i < config.devices.size(); ++i) {
        queued += bus->queue_size(i);
    }

    JsonWriter writer;
    writer.begin_object();
    writer.key("status").value("ok");
    writer.key("version").value(EDGEGUARD_VERSION);
    writer.key("uptime_ms").value(static_cast<std::uint64_t>(uptime_ms()));
    writer.key("server_time_ms").value(unix_ms_now());
    writer.key("devices").value(static_cast<std::uint64_t>(config.devices.size()));
    writer.key("online_devices").value(static_cast<std::uint64_t>(online));
    writer.key("rules").value(static_cast<std::uint64_t>(rules->rules.size()));
    writer.key("active_alarms").value(
        static_cast<std::uint64_t>(rule_engine->active_count()));
    writer.key("processed_samples").value(
        processed_samples.load(std::memory_order_relaxed));
    writer.key("recovered_samples").value(recovered_samples);
    writer.key("total_store_samples").value(
        static_cast<std::uint64_t>(store->total_samples_appended()));
    writer.key("queue_depth").value(static_cast<std::uint64_t>(queued));
    writer.key("wal_written").value(
        static_cast<std::uint64_t>(wal->written()));
    writer.key("wal_dropped").value(
        static_cast<std::uint64_t>(wal->dropped()));
    writer.end_object();
    return json_response(200, writer.take());
}

HttpResponse Application::Impl::api_overview() const {
    const auto snapshots = health->snapshot();
    std::size_t online = 0;
    for (const DeviceStatusSnapshot& snapshot : snapshots) {
        if (snapshot.online) {
            ++online;
        }
    }

    JsonWriter writer;
    writer.begin_object();
    writer.key("ts").value(unix_ms_now());
    writer.key("uptime_ms").value(static_cast<std::uint64_t>(uptime_ms()));
    writer.key("online_devices").value(static_cast<std::uint64_t>(online));
    writer.key("active_alarms").value(
        static_cast<std::uint64_t>(rule_engine->active_count()));

    writer.key("devices").begin_array();
    for (std::size_t i = 0; i < config.devices.size(); ++i) {
        const DeviceSpec& device = config.devices[i];
        const DeviceStatusSnapshot& status = snapshots[i];
        writer.begin_object();
        writer.key("name").value(device.name);
        writer.key("display_name").value(device.display_name);
        writer.key("host").value(device.host);
        writer.key("port").value(static_cast<std::uint32_t>(device.port));
        writer.key("unit_id").value(static_cast<std::uint32_t>(device.unit_id));
        writer.key("poll_interval_ms").value(device.poll_interval_ms);
        writer.key("online").value(status.online);
        writer.key("last_attempt_ms").value(status.last_attempt_ms);
        writer.key("last_sample_ms").value(status.last_sample_ms);
        writer.key("read_errors").value(
            static_cast<std::uint64_t>(status.read_errors));
        writer.key("last_error").value(status.last_error);
        writer.key("queue_depth").value(
            static_cast<std::uint64_t>(bus->queue_size(i)));
        writer.key("dropped").value(
            static_cast<std::uint64_t>(bus->dropped(i)));

        writer.key("metrics").begin_array();
        for (const std::uint32_t metric_id : device.metric_ids) {
            const MetricSpec* metric = config.metrics.by_id(metric_id);
            if (metric == nullptr) {
                continue;
            }
            const auto latest = store->latest(metric_id);
            writer.begin_object();
            writer.key("name").value(metric->name);
            writer.key("key").value(metric->key);
            writer.key("unit").value(metric->unit);
            writer.key("type").value(metric->is_boolean() ? "bool" : "number");
            writer.key("value");
            if (latest.has_value()) {
                writer.value(latest->value);
            } else {
                writer.null();
            }
            writer.key("ts");
            if (latest.has_value()) {
                writer.value(latest->timestamp_ms);
            } else {
                writer.null();
            }
            writer.end_object();
        }
        writer.end_array();
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    return json_response(200, writer.take());
}

HttpResponse Application::Impl::api_series(const HttpRequest& request) const {
    const std::string* device = query_value(request, "device");
    const std::string* metric_name = query_value(request, "metric");
    if (device == nullptr || metric_name == nullptr) {
        return json_error(400, "query parameters 'device' and 'metric' are required");
    }
    const MetricSpec* metric = config.metrics.find(*device, *metric_name);
    if (metric == nullptr) {
        return json_error(404, "unknown metric");
    }

    const TimestampMs now = unix_ms_now();
    TimestampMs to_ms = now;
    if (const std::string* value = query_value(request, "to")) {
        const auto parsed = parse_i64(*value);
        if (!parsed.has_value()) {
            return json_error(400, "invalid 'to' timestamp");
        }
        to_ms = *parsed;
    }

    std::int64_t minutes = 5;
    if (const std::string* value = query_value(request, "minutes")) {
        const auto parsed = parse_i64(*value);
        if (!parsed.has_value() || *parsed < 1 || *parsed > 1440) {
            return json_error(400, "'minutes' must be in [1, 1440]");
        }
        minutes = *parsed;
    }

    TimestampMs from_ms = to_ms - minutes * 60 * 1000;
    if (const std::string* value = query_value(request, "from")) {
        const auto parsed = parse_i64(*value);
        if (!parsed.has_value()) {
            return json_error(400, "invalid 'from' timestamp");
        }
        from_ms = *parsed;
    }
    if (from_ms > to_ms) {
        return json_error(400, "'from' must be less than or equal to 'to'");
    }

    std::size_t max_points = 400;
    if (const std::string* value = query_value(request, "max_points")) {
        const auto parsed = parse_i64(*value);
        if (!parsed.has_value() || *parsed < 2 || *parsed > 5000) {
            return json_error(400, "'max_points' must be in [2, 5000]");
        }
        max_points = static_cast<std::size_t>(*parsed);
    }

    const auto points = store->query(metric->id, from_ms, to_ms, max_points);

    JsonWriter writer;
    writer.begin_object();
    writer.key("device").value(metric->device);
    writer.key("metric").value(metric->name);
    writer.key("key").value(metric->key);
    writer.key("unit").value(metric->unit);
    writer.key("type").value(metric->is_boolean() ? "bool" : "number");
    writer.key("from").value(from_ms);
    writer.key("to").value(to_ms);
    writer.key("max_points").value(static_cast<std::uint64_t>(max_points));
    writer.key("points").begin_array();
    for (const StoredPoint& point : points) {
        writer.begin_object();
        writer.key("t").value(point.timestamp_ms);
        writer.key("v").value(point.value);
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    return json_response(200, writer.take());
}

HttpResponse Application::Impl::api_alarms(const HttpRequest& request) const {
    std::size_t limit = 50;
    if (const std::string* value = query_value(request, "limit")) {
        const auto parsed = parse_i64(*value);
        if (!parsed.has_value() || *parsed < 1 || *parsed > 500) {
            return json_error(400, "'limit' must be in [1, 500]");
        }
        limit = static_cast<std::size_t>(*parsed);
    }

    std::vector<AlarmEvent> selected;
    {
        std::lock_guard<std::mutex> lock(alarms_mutex);
        selected.reserve(std::min(limit, alarms.size()));
        for (auto it = alarms.rbegin(); it != alarms.rend() && selected.size() < limit;
             ++it) {
            selected.push_back(*it);
        }
    }

    JsonWriter writer;
    writer.begin_object();
    writer.key("active").value(
        static_cast<std::uint64_t>(rule_engine->active_count()));
    writer.key("alarms").begin_array();
    for (const AlarmEvent& event : selected) {
        writer.begin_object();
        writer.key("ts").value(event.timestamp_ms);
        writer.key("action").value(alarm_action_name(event.action));
        writer.key("rule").value(event.rule_name);
        writer.key("title").value(event.rule_title);
        writer.key("severity").value(event.severity);
        writer.key("code").value(event.code);
        writer.key("message").value(event.message);
        writer.key("metric").value(event.trigger_metric);
        writer.key("device").value(event.trigger_device);
        writer.key("value").value(event.trigger_value);
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    return json_response(200, writer.take());
}

HttpResponse Application::Impl::api_rules() const {
    JsonWriter writer;
    writer.begin_object();
    writer.key("count").value(static_cast<std::uint64_t>(rules->rules.size()));
    writer.key("rules").begin_array();
    for (const CompiledRule& rule : rules->rules) {
        writer.begin_object();
        writer.key("name").value(rule.name);
        writer.key("title").value(rule.title);
        writer.key("severity").value(rule.severity);
        writer.key("code").value(rule.code);
        writer.key("message").value(rule.message);
        writer.key("for_ms").value(rule.for_ms);
        writer.key("has_clear").value(rule.has_clear);
        writer.key("clear_for_ms").value(rule.clear_for_ms);
        writer.key("metrics").begin_array();
        for (const std::uint32_t metric_id : rule.referenced_metrics) {
            if (const MetricSpec* metric = config.metrics.by_id(metric_id)) {
                writer.value(metric->key);
            }
        }
        writer.end_array();
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    return json_response(200, writer.take());
}

HttpResponse Application::Impl::handle_api(const HttpRequest& request) {
    if (request.path == "/api/v1/health") {
        return api_health();
    }
    if (request.path == "/api/v1/overview") {
        return api_overview();
    }
    if (request.path == "/api/v1/series") {
        return api_series(request);
    }
    if (request.path == "/api/v1/alarms") {
        return api_alarms(request);
    }
    if (request.path == "/api/v1/rules") {
        return api_rules();
    }
    return json_error(404, "unknown API endpoint");
}

void Application::Impl::handle_stream(const HttpRequest& request,
                                      TcpSocket& socket,
                                      const std::atomic<bool>& server_stop) {
    (void)request;
    socket.set_timeouts(2000, 2000);
    const std::string headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache, no-store\r\n"
        "Connection: keep-alive\r\n"
        "X-Accel-Buffering: no\r\n"
        "\r\n"
        "retry: 3000\n\n";
    std::string send_error;
    if (!socket.send_all(headers.data(), headers.size(), send_error)) {
        return;
    }

    const std::uint64_t subscriber_id = events->subscribe();
    if (subscriber_id == 0) {
        return;
    }

    struct SubscriptionGuard {
        EventHub* hub;
        std::uint64_t id;
        ~SubscriptionGuard() { hub->unsubscribe(id); }
    } guard{events.get(), subscriber_id};

    std::string ready =
        "event: ready\ndata: {\"ts\":" + std::to_string(unix_ms_now()) + "}\n\n";
    if (!socket.send_all(ready.data(), ready.size(), send_error)) {
        return;
    }

    auto last_heartbeat = std::chrono::steady_clock::now();
    while (!stopping.load(std::memory_order_acquire) &&
           !server_stop.load(std::memory_order_acquire) && !events->closed()) {
        const std::vector<StreamEvent> pending =
            events->wait_for(subscriber_id, std::chrono::milliseconds(500));
        for (const StreamEvent& event : pending) {
            std::string wire = "event: ";
            wire.append(event.type);
            wire.append("\ndata: ");
            wire.append(event.data);
            wire.append("\n\n");
            if (!socket.send_all(wire.data(), wire.size(), send_error)) {
                return;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_heartbeat >= kSseHeartbeat) {
            const std::string heartbeat =
                ": heartbeat " + std::to_string(unix_ms_now()) + "\n\n";
            if (!socket.send_all(heartbeat.data(), heartbeat.size(), send_error)) {
                return;
            }
            last_heartbeat = now;
        }
    }
}

Application::Application(RunOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

Application::~Application() = default;

bool Application::initialize(std::string& error) {
    return impl_->initialize(error);
}

int Application::run(std::atomic<bool>& quit) {
    return impl_->run(quit);
}

}  // namespace eg
