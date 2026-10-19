#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "common/socket.h"

namespace eg {

// Development-only Modbus TCP slave used by the demo and the integration
// tests. It exposes deterministic register maps when animate=false.
class SimulatorServer {
public:
    explicit SimulatorServer(bool animate = true);
    ~SimulatorServer();

    SimulatorServer(const SimulatorServer&) = delete;
    SimulatorServer& operator=(const SimulatorServer&) = delete;

    bool start(const std::string& host, std::uint16_t port, std::string& error);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }
    std::uint16_t port() const { return listener_.port(); }

    void set_holding_register(std::uint8_t unit, std::uint16_t address,
                              std::uint16_t value);
    void set_input_register(std::uint8_t unit, std::uint16_t address,
                            std::uint16_t value);
    void set_coil(std::uint8_t unit, std::uint16_t address, bool value);
    std::uint16_t holding_register(std::uint8_t unit, std::uint16_t address) const;
    std::uint16_t input_register(std::uint8_t unit, std::uint16_t address) const;
    bool coil(std::uint8_t unit, std::uint16_t address) const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
    TcpListener listener_;
    std::atomic<bool> running_{false};
    bool animate_ = true;
    std::thread accept_thread_;
    std::thread generator_thread_;
};

}  // namespace eg
