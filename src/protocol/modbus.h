#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/socket.h"

namespace eg {

class ModbusError : public std::runtime_error {
public:
    explicit ModbusError(const std::string& message) : std::runtime_error(message) {}
};

namespace modbus {

constexpr std::uint8_t kReadCoils = 1;
constexpr std::uint8_t kReadDiscreteInputs = 2;
constexpr std::uint8_t kReadHoldingRegisters = 3;
constexpr std::uint8_t kReadInputRegisters = 4;

constexpr std::uint16_t kMaxRegisterQuantity = 125;
constexpr std::uint16_t kMaxBitQuantity = 2000;

struct ReadRequest {
    std::uint16_t transaction_id = 0;
    std::uint8_t unit_id = 1;
    std::uint8_t function = 0;
    std::uint16_t start_address = 0;
    std::uint16_t quantity = 0;
};

struct ReadResponse {
    std::uint16_t transaction_id = 0;
    std::uint8_t unit_id = 1;
    std::uint8_t function = 0;
    std::vector<std::uint8_t> data;
};

std::vector<std::uint8_t> build_read_request(std::uint16_t transaction_id,
                                             std::uint8_t unit_id,
                                             std::uint8_t function,
                                             std::uint16_t start_address,
                                             std::uint16_t quantity);
ReadRequest parse_read_request(const std::vector<std::uint8_t>& frame);
ReadResponse parse_read_response(const std::vector<std::uint8_t>& frame);

std::vector<std::uint8_t> build_read_response(std::uint16_t transaction_id,
                                              std::uint8_t unit_id,
                                              std::uint8_t function,
                                              const std::vector<std::uint8_t>& data);
std::vector<std::uint8_t> build_exception_response(std::uint16_t transaction_id,
                                                   std::uint8_t unit_id,
                                                   std::uint8_t function,
                                                   std::uint8_t exception_code);

std::vector<std::uint16_t> decode_registers(const std::vector<std::uint8_t>& data);
std::vector<std::uint8_t> decode_bits(const std::vector<std::uint8_t>& data);

}  // namespace modbus

class ModbusTcpClient {
public:
    ModbusTcpClient(std::string host, std::uint16_t port, std::uint8_t unit_id);
    ~ModbusTcpClient();

    ModbusTcpClient(const ModbusTcpClient&) = delete;
    ModbusTcpClient& operator=(const ModbusTcpClient&) = delete;

    void connect(int timeout_ms = 2000);
    void close();
    bool connected() const { return socket_.valid(); }

    std::vector<std::uint16_t> read_registers(std::uint8_t function,
                                              std::uint16_t start_address,
                                              std::uint16_t quantity);
    std::vector<std::uint8_t> read_bits(std::uint8_t function,
                                        std::uint16_t start_address,
                                        std::uint16_t quantity);

private:
    std::vector<std::uint8_t> transact(std::uint8_t function,
                                       std::uint16_t start_address,
                                       std::uint16_t quantity);

    std::string host_;
    std::uint16_t port_ = 502;
    std::uint8_t unit_id_ = 1;
    std::uint16_t transaction_id_ = 0;
    int response_timeout_ms_ = 1000;
    TcpSocket socket_;
};

}  // namespace eg
