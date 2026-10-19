#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace eg {

struct RunOptions {
    std::string config_path;
    bool with_simulator = false;
    std::uint64_t duration_ms = 0;
};

class Application {
public:
    explicit Application(RunOptions options);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool initialize(std::string& error);
    int run(std::atomic<bool>& quit);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eg
