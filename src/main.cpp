#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
// <windows.h> defines ERROR as a GDI macro, which would corrupt the
// LogLevel::Error enumerator when project headers are included afterwards.
#ifdef ERROR
#undef ERROR
#endif
#endif

#include "app/application.h"
#include "common/config.h"
#include "common/log.h"
#include "common/util.h"
#include "core/metrics.h"
#include "core/rules/compiler.h"
#include "core/spsc_queue.h"
#include "core/storage/series_store.h"
#include "protocol/simulator.h"

#ifndef EDGEGUARD_VERSION
#define EDGEGUARD_VERSION "1.0.0"
#endif

namespace {

std::atomic<bool> g_stop{false};

#ifdef _WIN32
BOOL WINAPI console_ctrl_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT) {
        g_stop.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}
#else
void posix_signal_handler(int) {
    g_stop.store(true, std::memory_order_relaxed);
}
#endif

void install_signal_handlers() {
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    std::signal(SIGINT, posix_signal_handler);
    std::signal(SIGTERM, posix_signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

void print_usage() {
    std::cout
        << "EdgeGuard " << EDGEGUARD_VERSION
        << " - 工业边缘数据采集与规则告警网关\n\n"
        << "Usage:\n"
        << "  edgeguard run   [--config <path>] [--with-simulator] [--duration-ms <n>]\n"
        << "  edgeguard sim   [--host <ip>] [--port <port>] [--duration-ms <n>]\n"
        << "  edgeguard check [--config <path>] [--rules <path>] [--disasm]\n"
        << "  edgeguard bench [--iterations <n>]\n"
        << "  edgeguard --version\n\n"
        << "Examples:\n"
        << "  edgeguard run --config configs/edgeguard.conf --with-simulator\n"
        << "  edgeguard check --config configs/edgeguard.conf --disasm\n"
        << "  edgeguard sim --port 15020\n";
}

bool parse_command_line_value(int& index, int argc, char** argv,
                              const std::string& name, std::string& value) {
    const std::string argument = argv[index];
    if (argument == name) {
        if (index + 1 >= argc) {
            std::cerr << "missing value for " << name << "\n";
            return false;
        }
        value = argv[++index];
        return true;
    }
    const std::string prefix = name + "=";
    if (eg::starts_with(argument, prefix)) {
        value = argument.substr(prefix.size());
        return true;
    }
    return false;
}

bool match_flag(int& index, int argc, char** argv, const std::string& name) {
    (void)argc;
    if (argv[index] == name) {
        return true;
    }
    return false;
}

int command_run(int argc, char** argv) {
    eg::RunOptions options;
    options.config_path = "configs/edgeguard.conf";

    for (int i = 2; i < argc; ++i) {
        std::string value;
        if (parse_command_line_value(i, argc, argv, "--config", value)) {
            options.config_path = value;
        } else if (match_flag(i, argc, argv, "--with-simulator")) {
            options.with_simulator = true;
        } else if (parse_command_line_value(i, argc, argv, "--duration-ms", value)) {
            const auto parsed = eg::parse_i64(value);
            if (!parsed.has_value() || *parsed < 0) {
                std::cerr << "invalid --duration-ms value\n";
                return 2;
            }
            options.duration_ms = static_cast<std::uint64_t>(*parsed);
        } else if (argv[i] == std::string("--help")) {
            print_usage();
            return 0;
        } else {
            std::cerr << "unknown option: " << argv[i] << "\n";
            return 2;
        }
    }

    eg::Application application(std::move(options));
    std::string error;
    if (!application.initialize(error)) {
        std::cerr << "initialization failed: " << error << "\n";
        return 1;
    }
    return application.run(g_stop);
}

int command_sim(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 15020;
    std::uint64_t duration_ms = 0;

    for (int i = 2; i < argc; ++i) {
        std::string value;
        if (parse_command_line_value(i, argc, argv, "--host", value)) {
            host = value;
        } else if (parse_command_line_value(i, argc, argv, "--port", value)) {
            const auto parsed = eg::parse_i64(value);
            if (!parsed.has_value() || *parsed < 1 || *parsed > 65535) {
                std::cerr << "invalid --port value\n";
                return 2;
            }
            port = static_cast<std::uint16_t>(*parsed);
        } else if (parse_command_line_value(i, argc, argv, "--duration-ms", value)) {
            const auto parsed = eg::parse_i64(value);
            if (!parsed.has_value() || *parsed < 0) {
                std::cerr << "invalid --duration-ms value\n";
                return 2;
            }
            duration_ms = static_cast<std::uint64_t>(*parsed);
        } else if (argv[i] == std::string("--help")) {
            print_usage();
            return 0;
        } else {
            std::cerr << "unknown option: " << argv[i] << "\n";
            return 2;
        }
    }

    eg::Logger::instance().configure(eg::LogLevel::Info);
    eg::SimulatorServer simulator(true);
    std::string error;
    if (!simulator.start(host, port, error)) {
        std::cerr << "cannot start simulator: " << error << "\n";
        return 1;
    }
    std::cout << "Modbus TCP simulator running on " << host << ":" << simulator.port()
              << " (units 1 and 2). Press Ctrl+C to stop.\n";

    const auto deadline =
        duration_ms > 0 ? std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(duration_ms)
                        : std::chrono::steady_clock::time_point::max();
    while (!g_stop.load(std::memory_order_acquire)) {
        if (duration_ms > 0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    simulator.stop();
    return 0;
}

int command_check(int argc, char** argv) {
    std::string config_path = "configs/edgeguard.conf";
    std::string rules_override;
    bool disassemble = false;

    for (int i = 2; i < argc; ++i) {
        std::string value;
        if (parse_command_line_value(i, argc, argv, "--config", value)) {
            config_path = value;
        } else if (parse_command_line_value(i, argc, argv, "--rules", value)) {
            rules_override = value;
        } else if (match_flag(i, argc, argv, "--disasm")) {
            disassemble = true;
        } else if (argv[i] == std::string("--help")) {
            print_usage();
            return 0;
        } else {
            std::cerr << "unknown option: " << argv[i] << "\n";
            return 2;
        }
    }

    try {
        eg::Logger::instance().configure(eg::LogLevel::Warn);
        eg::AppConfig config = eg::load_config(config_path);
        if (!rules_override.empty()) {
            config.rules_path = rules_override;
        }
        eg::RuleCompiler compiler(config.metrics);
        const eg::RuleProgram program = compiler.compile_file(config.rules_path);

        std::cout << "Config: " << config.source_path << "\n"
                  << "Devices: " << config.devices.size()
                  << ", metrics: " << config.metrics.size() << "\n"
                  << "Rules: " << program.rules.size()
                  << ", bytecode instructions: " << program.instruction_count() << "\n\n";

        for (const eg::CompiledRule& rule : program.rules) {
            std::cout << "rule " << rule.name << " [" << rule.severity << "/" << rule.code
                      << "] \"" << rule.title << "\"\n"
                      << "  for=" << rule.for_ms << "ms"
                      << " clear=" << (rule.has_clear ? "yes" : "no")
                      << " clear_for=" << rule.clear_for_ms << "ms"
                      << " refs=";
            for (std::size_t i = 0; i < rule.referenced_metrics.size(); ++i) {
                if (i != 0) {
                    std::cout << ",";
                }
                const eg::MetricSpec* metric =
                    config.metrics.by_id(rule.referenced_metrics[i]);
                std::cout << (metric == nullptr ? "?" : metric->key);
            }
            std::cout << "\n";
            if (disassemble) {
                std::cout << "  when bytecode:\n"
                          << eg::disassemble_expression(rule.when);
                if (rule.has_clear) {
                    std::cout << "  clear bytecode:\n"
                              << eg::disassemble_expression(rule.clear);
                }
            }
            std::cout << "\n";
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "rule check failed: " << ex.what() << "\n";
        return 1;
    }
}

const char* compiler_name() {
#if defined(_MSC_VER)
    return "MSVC";
#elif defined(__clang__)
    return "Clang";
#elif defined(__GNUC__)
    return "GCC";
#else
    return "unknown";
#endif
}

int command_bench(int argc, char** argv) {
    std::uint64_t iterations = 1000000;
    for (int i = 2; i < argc; ++i) {
        std::string value;
        if (parse_command_line_value(i, argc, argv, "--iterations", value)) {
            const auto parsed = eg::parse_i64(value);
            if (!parsed.has_value() || *parsed < 1000 || *parsed > 20000000) {
                std::cerr << "--iterations must be in [1000, 20000000]\n";
                return 2;
            }
            iterations = static_cast<std::uint64_t>(*parsed);
        } else if (argv[i] == std::string("--help")) {
            print_usage();
            return 0;
        } else {
            std::cerr << "unknown option: " << argv[i] << "\n";
            return 2;
        }
    }

    using Clock = std::chrono::steady_clock;
    std::cout << "EdgeGuard microbenchmarks (compiler=" << compiler_name()
              << ", iterations=" << iterations << ")\n";

    // SPSC queue producer/consumer throughput.
    {
        eg::SpscQueue<std::uint64_t> queue(65536);
        std::atomic<std::uint64_t> consumed{0};
        const auto start = Clock::now();
        std::thread producer([&] {
            for (std::uint64_t i = 0; i < iterations; ++i) {
                while (!queue.try_push(i)) {
                    std::this_thread::yield();
                }
            }
        });
        std::uint64_t value = 0;
        while (consumed.load(std::memory_order_relaxed) < iterations) {
            if (queue.try_pop(value)) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
        producer.join();
        const double seconds =
            std::chrono::duration<double>(Clock::now() - start).count();
        std::cout << std::fixed << std::setprecision(2)
                  << "SPSC queue:        " << (iterations / seconds / 1e6)
                  << " Mmsg/s\n";
    }

    // Rule VM condition evaluation throughput.
    {
        eg::MetricRegistry registry;
        eg::MetricSpec metric;
        metric.device = "bench";
        metric.name = "value";
        metric.key = "bench.value";
        metric.area = eg::RegisterArea::HoldingRegister;
        metric.value_type = eg::RegisterValueType::U16;
        registry.add(metric);

        const std::string source =
            "rule benchmark \"benchmark\" {\n"
            "  when bench.value > 42.0 and bench.value < 100.0\n"
            "  for 1s\n"
            "  emit {\n"
            "    severity = info\n"
            "    code = BENCH\n"
            "    message = \"benchmark\"\n"
            "  }\n"
            "}\n";
        eg::RuleCompiler compiler(registry);
        const eg::RuleProgram program = compiler.compile_source(source);
        const std::vector<double> latest{50.0};
        const std::vector<bool> available{true};

        std::uint64_t matches = 0;
        const auto start = Clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i) {
            if (eg::evaluate_condition(program.rules.front().when, latest, available)) {
                ++matches;
            }
        }
        const double seconds =
            std::chrono::duration<double>(Clock::now() - start).count();
        std::cout << "Rule VM condition: " << (iterations / seconds / 1e6)
                  << " Meval/s (checksum=" << matches << ")\n";
    }

    // Time-series append throughput.
    {
        eg::SeriesStore store(16, 3600, 200000);
        const eg::TimestampMs base = eg::unix_ms_now();
        const auto start = Clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i) {
            eg::Sample sample;
            sample.metric_id = static_cast<std::uint32_t>(i % 16);
            sample.timestamp_ms = base + static_cast<eg::TimestampMs>(i / 16);
            sample.value = static_cast<double>(i % 1000) * 0.1;
            store.append(sample);
        }
        const double seconds =
            std::chrono::duration<double>(Clock::now() - start).count();
        std::cout << "Series append:     " << (iterations / seconds / 1e6)
                  << " Msample/s\n";
    }

    std::cout << "Note: measure on your own hardware; do not quote these numbers "
                 "without running the benchmark.\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    install_signal_handlers();

    if (argc < 2) {
        print_usage();
        return 0;
    }
    const std::string command = argv[1];
    try {
        if (command == "--help" || command == "-h" || command == "help") {
            print_usage();
            return 0;
        }
        if (command == "--version" || command == "version") {
            std::cout << "EdgeGuard " << EDGEGUARD_VERSION << "\n";
            return 0;
        }
        if (command == "run") {
            return command_run(argc, argv);
        }
        if (command == "sim") {
            return command_sim(argc, argv);
        }
        if (command == "check") {
            return command_check(argc, argv);
        }
        if (command == "bench") {
            return command_bench(argc, argv);
        }
        std::cerr << "unknown command: " << command << "\n";
        print_usage();
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << "fatal error: " << ex.what() << "\n";
        return 1;
    }
}
