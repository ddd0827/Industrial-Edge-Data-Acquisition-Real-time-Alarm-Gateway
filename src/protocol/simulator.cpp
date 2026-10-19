#include "protocol/simulator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/log.h"
#include "protocol/modbus.h"

namespace eg {
namespace {

constexpr std::size_t kRegisterCount = 512;
constexpr std::size_t kCoilCount = 256;

std::uint16_t scaled_u16(double value) {
    const double clamped = std::clamp(value, 0.0, 65535.0);
    return static_cast<std::uint16_t>(clamped + 0.5);
}

std::uint16_t read_u16_at(const std::vector<std::uint8_t>& frame, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(frame[offset]) << 8U) | frame[offset + 1]);
}

bool receive_exact(TcpSocket& socket, std::uint8_t* data, std::size_t size) {
    std::size_t received = 0;
    std::string error;
    while (received < size) {
        const int count = socket.receive(data + received, size - received, error);
        if (count <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(count);
    }
    return true;
}

}  // namespace

struct SimulatorServer::Impl {
    struct UnitData {
        std::array<std::uint16_t, kRegisterCount> holding{};
        std::array<std::uint16_t, kRegisterCount> input{};
        std::array<std::uint8_t, kCoilCount> coils{};
        std::array<std::uint8_t, kCoilCount> discrete{};
    };

    explicit Impl(bool animate_in) : animate(animate_in) {
        ensure_locked(1);
        ensure_locked(2);
    }

    UnitData& ensure_locked(std::uint8_t unit) {
        const auto inserted = units.emplace(unit, UnitData{});
        UnitData& data = inserted.first->second;
        if (inserted.second) {
            initialize_unit(data, unit);
        }
        return data;
    }

    static void initialize_unit(UnitData& data, std::uint8_t unit) {
        if (unit == 1) {
            data.holding[0] = 700;
            data.holding[1] = 105;
            data.holding[2] = 320;
            data.holding[3] = 850;
            data.input[0] = 700;
            data.coils[0] = 1;
            data.discrete[0] = 1;
        } else if (unit == 2) {
            data.holding[0] = 350;
            data.holding[1] = 580;
            data.holding[2] = 1500;
            data.holding[3] = 1600;
            data.coils[0] = 1;
            data.discrete[0] = 1;
        } else {
            data.holding[0] = 500;
            data.holding[1] = 100;
            data.holding[2] = 1000;
            data.coils[0] = 1;
        }
    }

    void animate_units() {
        const double t =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                .count();
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& entry : units) {
            UnitData& data = entry.second;
            if (entry.first == 1) {
                const double temperature = 86.0 + 8.0 * std::sin(t * 0.35);
                const double pressure = 1.15 + 0.25 * std::sin(t * 0.18 + 0.7);
                const double vibration = 3.2 + 1.4 * std::sin(t * 0.6);
                data.holding[0] = scaled_u16(temperature * 10.0);
                data.holding[1] = scaled_u16(pressure * 100.0);
                data.holding[2] = scaled_u16(vibration * 100.0);
                data.holding[3] = 850;
                data.input[0] = data.holding[0];
                data.coils[0] = std::sin(t * 0.12) > -0.65 ? 1 : 0;
                data.discrete[0] = 1;
            } else if (entry.first == 2) {
                const double flow = 35.0 + 18.0 * std::sin(t * 0.11);
                const double pressure = 0.58 + 0.10 * std::sin(t * 0.21 + 1.1);
                const double rpm = 1500.0 + 180.0 * std::sin(t * 0.5);
                data.holding[0] = scaled_u16(flow * 10.0);
                data.holding[1] = scaled_u16(pressure * 100.0);
                data.holding[2] = scaled_u16(rpm);
                data.holding[3] = 1600;
                data.coils[0] = 1;
                data.discrete[0] = 1;
            } else {
                const double unit_phase = static_cast<double>(entry.first);
                data.holding[0] =
                    scaled_u16(500.0 + 100.0 * std::sin(t + unit_phase * 0.7));
                data.holding[1] =
                    scaled_u16(100.0 + 20.0 * std::sin(t * 0.4 + unit_phase));
            }
        }
    }

    mutable std::mutex mutex;
    std::unordered_map<std::uint8_t, UnitData> units;
    std::atomic<bool> running{false};
    bool animate = false;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

namespace {

void send_exception(TcpSocket& socket, std::uint16_t transaction_id, std::uint8_t unit,
                    std::uint8_t function, std::uint8_t code) {
    const auto response =
        modbus::build_exception_response(transaction_id, unit, function, code);
    std::string error;
    socket.send_all(response.data(), response.size(), error);
}

void handle_client(const std::shared_ptr<SimulatorServer::Impl>& state, TcpSocket socket) {
    socket.set_timeouts(1000, 1000);
    std::string send_error;

    while (state->running.load(std::memory_order_acquire)) {
        std::array<std::uint8_t, 6> header{};
        if (!receive_exact(socket, header.data(), header.size())) {
            break;
        }
        const std::uint16_t protocol =
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(header[2]) << 8U) |
                                       header[3]);
        const std::uint16_t body_length =
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(header[4]) << 8U) |
                                       header[5]);
        if (protocol != 0 || body_length < 3 || body_length > 254) {
            break;
        }

        std::vector<std::uint8_t> frame(6U + body_length);
        std::copy(header.begin(), header.end(), frame.begin());
        if (!receive_exact(socket, frame.data() + 6, body_length)) {
            break;
        }

        const std::uint16_t transaction_id = read_u16_at(frame, 0);
        const std::uint8_t unit = frame[6];
        const std::uint8_t function = frame[7];

        if (body_length != 6) {
            send_exception(socket, transaction_id, unit, function, 3);
            continue;
        }

        const std::uint16_t start = read_u16_at(frame, 8);
        const std::uint16_t quantity = read_u16_at(frame, 10);
        if (function < modbus::kReadCoils || function > modbus::kReadInputRegisters) {
            send_exception(socket, transaction_id, unit, function, 1);
            continue;
        }

        const bool bits = function == modbus::kReadCoils ||
                          function == modbus::kReadDiscreteInputs;
        const std::uint16_t max_quantity =
            bits ? modbus::kMaxBitQuantity : modbus::kMaxRegisterQuantity;
        if (quantity == 0 || quantity > max_quantity) {
            send_exception(socket, transaction_id, unit, function, 3);
            continue;
        }
        const std::size_t limit = bits ? kCoilCount : kRegisterCount;
        if (static_cast<std::size_t>(start) + quantity > limit) {
            send_exception(socket, transaction_id, unit, function, 2);
            continue;
        }

        std::vector<std::uint8_t> payload;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            SimulatorServer::Impl::UnitData& data = state->ensure_locked(unit);
            if (bits) {
                const std::size_t byte_count = (quantity + 7U) / 8U;
                payload.assign(byte_count, 0);
                for (std::uint16_t i = 0; i < quantity; ++i) {
                    const std::uint8_t bit =
                        function == modbus::kReadCoils ? data.coils[start + i]
                                                       : data.discrete[start + i];
                    if (bit != 0) {
                        payload[i / 8U] |= static_cast<std::uint8_t>(1U << (i % 8U));
                    }
                }
            } else {
                payload.reserve(static_cast<std::size_t>(quantity) * 2U);
                for (std::uint16_t i = 0; i < quantity; ++i) {
                    const std::uint16_t value =
                        function == modbus::kReadHoldingRegisters
                            ? data.holding[start + i]
                            : data.input[start + i];
                    payload.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
                    payload.push_back(static_cast<std::uint8_t>(value & 0xFFU));
                }
            }
        }

        const std::vector<std::uint8_t> response =
            modbus::build_read_response(transaction_id, unit, function, payload);
        if (!socket.send_all(response.data(), response.size(), send_error)) {
            break;
        }
    }
    socket.close();
}

}  // namespace

SimulatorServer::SimulatorServer(bool animate) : animate_(animate) {}

SimulatorServer::~SimulatorServer() {
    stop();
}

bool SimulatorServer::start(const std::string& host, std::uint16_t port,
                            std::string& error) {
    if (running_.load(std::memory_order_acquire)) {
        error = "simulator is already running";
        return false;
    }

    impl_ = std::make_shared<Impl>(animate_);
    impl_->started = std::chrono::steady_clock::now();
    impl_->running.store(true, std::memory_order_release);

    if (!listener_.listen_tcp(host, port, 32, error)) {
        impl_->running.store(false, std::memory_order_release);
        impl_.reset();
        return false;
    }

    running_.store(true, std::memory_order_release);
    accept_thread_ = std::thread([this, state = impl_] {
        while (state->running.load(std::memory_order_acquire)) {
            std::string accept_error;
            auto client = listener_.accept_for(200, accept_error);
            if (!client.has_value()) {
                if (!accept_error.empty() && state->running.load()) {
                    EG_DEBUG("simulator accept: %s", accept_error.c_str());
                }
                continue;
            }
            TcpSocket socket = std::move(*client);
            socket.set_timeouts(1000, 1000);
            std::thread([state, socket = std::move(socket)]() mutable {
                handle_client(state, std::move(socket));
            }).detach();
        }
    });

    if (animate_) {
        generator_thread_ = std::thread([state = impl_] {
            while (state->running.load(std::memory_order_acquire)) {
                state->animate_units();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }

    EG_INFO("Modbus TCP simulator listening on %s:%u (units 1 and 2, animate=%s)",
            host.c_str(), static_cast<unsigned>(listener_.port()),
            animate_ ? "true" : "false");
    return true;
}

void SimulatorServer::stop() {
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (impl_ != nullptr) {
        impl_->running.store(false, std::memory_order_release);
    }
    if (!was_running) {
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        if (generator_thread_.joinable()) {
            generator_thread_.join();
        }
        return;
    }

    // The accept loop uses a finite select timeout, so it observes the stop
    // flag without closing the listener from another thread.
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    listener_.close();
    if (generator_thread_.joinable()) {
        generator_thread_.join();
    }
    impl_.reset();
    EG_INFO("Modbus TCP simulator stopped");
}

void SimulatorServer::set_holding_register(std::uint8_t unit, std::uint16_t address,
                                           std::uint16_t value) {
    if (impl_ == nullptr || address >= kRegisterCount) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->ensure_locked(unit).holding[address] = value;
}

void SimulatorServer::set_input_register(std::uint8_t unit, std::uint16_t address,
                                         std::uint16_t value) {
    if (impl_ == nullptr || address >= kRegisterCount) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->ensure_locked(unit).input[address] = value;
}

void SimulatorServer::set_coil(std::uint8_t unit, std::uint16_t address, bool value) {
    if (impl_ == nullptr || address >= kCoilCount) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->ensure_locked(unit).coils[address] = value ? 1 : 0;
}

std::uint16_t SimulatorServer::holding_register(std::uint8_t unit,
                                                std::uint16_t address) const {
    if (impl_ == nullptr || address >= kRegisterCount) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->units.find(unit);
    return it == impl_->units.end() ? 0 : it->second.holding[address];
}

std::uint16_t SimulatorServer::input_register(std::uint8_t unit,
                                              std::uint16_t address) const {
    if (impl_ == nullptr || address >= kRegisterCount) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->units.find(unit);
    return it == impl_->units.end() ? 0 : it->second.input[address];
}

bool SimulatorServer::coil(std::uint8_t unit, std::uint16_t address) const {
    if (impl_ == nullptr || address >= kCoilCount) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->units.find(unit);
    return it != impl_->units.end() && it->second.coils[address] != 0;
}

}  // namespace eg
