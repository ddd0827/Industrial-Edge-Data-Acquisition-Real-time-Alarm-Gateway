#include "protocol/modbus.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <utility>

namespace eg {
namespace {

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

std::uint16_t read_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) |
                                      static_cast<std::uint16_t>(data[1]));
}

bool is_bit_function(std::uint8_t function) {
    return function == modbus::kReadCoils || function == modbus::kReadDiscreteInputs;
}

bool is_register_function(std::uint8_t function) {
    return function == modbus::kReadHoldingRegisters ||
           function == modbus::kReadInputRegisters;
}

std::string exception_label(std::uint8_t code) {
    switch (code) {
        case 1:
            return "illegal function";
        case 2:
            return "illegal data address";
        case 3:
            return "illegal data value";
        case 4:
            return "server device failure";
        case 5:
            return "acknowledge";
        case 6:
            return "server device busy";
        case 8:
            return "memory parity error";
        case 10:
            return "gateway path unavailable";
        case 11:
            return "gateway target device failed to respond";
        default:
            return "unknown exception";
    }
}

void validate_read_function(std::uint8_t function, std::uint16_t quantity) {
    if (is_bit_function(function)) {
        if (quantity == 0 || quantity > modbus::kMaxBitQuantity) {
            throw std::invalid_argument("Modbus bit quantity must be in [1, 2000]");
        }
        return;
    }
    if (is_register_function(function)) {
        if (quantity == 0 || quantity > modbus::kMaxRegisterQuantity) {
            throw std::invalid_argument("Modbus register quantity must be in [1, 125]");
        }
        return;
    }
    throw std::invalid_argument("unsupported Modbus read function code");
}

void validate_mbap(const std::vector<std::uint8_t>& frame,
                   std::uint16_t expected_length) {
    if (frame.size() < 9) {
        throw ModbusError("Modbus frame is too short");
    }
    if (read_u16(&frame[2]) != 0) {
        throw ModbusError("invalid Modbus protocol identifier");
    }
    const std::uint16_t length = read_u16(&frame[4]);
    if (length != expected_length || frame.size() < 6U + length) {
        throw ModbusError("Modbus MBAP length mismatch");
    }
}

}  // namespace

namespace modbus {

std::vector<std::uint8_t> build_read_request(std::uint16_t transaction_id,
                                             std::uint8_t unit_id,
                                             std::uint8_t function,
                                             std::uint16_t start_address,
                                             std::uint16_t quantity) {
    validate_read_function(function, quantity);
    if (static_cast<std::uint32_t>(start_address) + quantity > 65536U) {
        throw std::invalid_argument("Modbus read range exceeds the address space");
    }

    std::vector<std::uint8_t> frame;
    frame.reserve(12);
    append_u16(frame, transaction_id);
    append_u16(frame, 0);
    append_u16(frame, 6);  // unit id + five-byte PDU
    frame.push_back(unit_id);
    frame.push_back(function);
    append_u16(frame, start_address);
    append_u16(frame, quantity);
    return frame;
}

ReadRequest parse_read_request(const std::vector<std::uint8_t>& frame) {
    if (frame.size() != 12) {
        throw ModbusError("Modbus read request must be 12 bytes");
    }
    validate_mbap(frame, 6);
    if (frame.size() != 12) {
        throw ModbusError("malformed Modbus read request");
    }

    ReadRequest request;
    request.transaction_id = read_u16(&frame[0]);
    request.unit_id = frame[6];
    request.function = frame[7];
    request.start_address = read_u16(&frame[8]);
    request.quantity = read_u16(&frame[10]);

    try {
        validate_read_function(request.function, request.quantity);
    } catch (const std::invalid_argument& ex) {
        throw ModbusError(ex.what());
    }
    if (static_cast<std::uint32_t>(request.start_address) + request.quantity > 65536U) {
        throw ModbusError("Modbus read request exceeds the address space");
    }
    return request;
}

ReadResponse parse_read_response(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < 9) {
        throw ModbusError("Modbus read response is too short");
    }
    if (read_u16(&frame[2]) != 0) {
        throw ModbusError("invalid Modbus protocol identifier");
    }
    const std::uint16_t length = read_u16(&frame[4]);
    if (length < 3 || length > 254 || frame.size() != 6U + length) {
        throw ModbusError("malformed Modbus MBAP response length");
    }

    ReadResponse response;
    response.transaction_id = read_u16(&frame[0]);
    response.unit_id = frame[6];
    response.function = frame[7];

    if ((response.function & 0x80U) != 0) {
        if (length != 3) {
            throw ModbusError("malformed Modbus exception response");
        }
        const std::uint8_t exception_code = frame[8];
        std::ostringstream message;
        message << "Modbus exception " << static_cast<int>(exception_code) << " ("
                << exception_label(exception_code) << ")";
        throw ModbusError(message.str());
    }

    if (!is_bit_function(response.function) &&
        !is_register_function(response.function)) {
        throw ModbusError("unexpected Modbus function code in response");
    }

    const std::uint8_t byte_count = frame[8];
    if (byte_count > 250 || length != static_cast<std::uint16_t>(3U + byte_count) ||
        frame.size() != 9U + byte_count) {
        throw ModbusError("malformed Modbus response byte count");
    }
    response.data.assign(frame.begin() + 9, frame.end());
    return response;
}

std::vector<std::uint8_t> build_read_response(std::uint16_t transaction_id,
                                              std::uint8_t unit_id,
                                              std::uint8_t function,
                                              const std::vector<std::uint8_t>& data) {
    if (!is_bit_function(function) && !is_register_function(function)) {
        throw std::invalid_argument("unsupported Modbus response function code");
    }
    if (data.size() > 250) {
        throw std::invalid_argument("Modbus response payload exceeds 250 bytes");
    }

    std::vector<std::uint8_t> frame;
    frame.reserve(9 + data.size());
    append_u16(frame, transaction_id);
    append_u16(frame, 0);
    append_u16(frame, static_cast<std::uint16_t>(3 + data.size()));
    frame.push_back(unit_id);
    frame.push_back(function);
    frame.push_back(static_cast<std::uint8_t>(data.size()));
    frame.insert(frame.end(), data.begin(), data.end());
    return frame;
}

std::vector<std::uint8_t> build_exception_response(std::uint16_t transaction_id,
                                                   std::uint8_t unit_id,
                                                   std::uint8_t function,
                                                   std::uint8_t exception_code) {
    std::vector<std::uint8_t> frame;
    frame.reserve(9);
    append_u16(frame, transaction_id);
    append_u16(frame, 0);
    append_u16(frame, 3);
    frame.push_back(unit_id);
    frame.push_back(static_cast<std::uint8_t>(function | 0x80U));
    frame.push_back(exception_code);
    return frame;
}

std::vector<std::uint16_t> decode_registers(const std::vector<std::uint8_t>& data) {
    if (data.size() % 2 != 0) {
        throw ModbusError("register payload has an odd byte count");
    }
    std::vector<std::uint16_t> registers;
    registers.reserve(data.size() / 2);
    for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
        registers.push_back(read_u16(&data[i]));
    }
    return registers;
}

std::vector<std::uint8_t> decode_bits(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> bits;
    bits.reserve(data.size() * 8);
    for (const std::uint8_t byte : data) {
        for (int bit = 0; bit < 8; ++bit) {
            bits.push_back(static_cast<std::uint8_t>((byte >> bit) & 1U));
        }
    }
    return bits;
}

}  // namespace modbus

ModbusTcpClient::ModbusTcpClient(std::string host, std::uint16_t port,
                                 std::uint8_t unit_id)
    : host_(std::move(host)), port_(port), unit_id_(unit_id) {}

ModbusTcpClient::~ModbusTcpClient() {
    close();
}

void ModbusTcpClient::connect(int timeout_ms) {
    if (socket_.valid()) {
        return;
    }
    std::string error;
    socket_ = TcpSocket::connect_tcp(host_, port_, timeout_ms, error);
    if (!socket_.valid()) {
        throw ModbusError("Modbus connect " + host_ + ":" + std::to_string(port_) +
                          " failed: " + error);
    }
    if (!socket_.set_timeouts(response_timeout_ms_, response_timeout_ms_)) {
        socket_.close();
        throw ModbusError("failed to configure Modbus socket timeouts");
    }
}

void ModbusTcpClient::close() {
    socket_.close();
}

std::vector<std::uint8_t> ModbusTcpClient::transact(std::uint8_t function,
                                                    std::uint16_t start_address,
                                                    std::uint16_t quantity) {
    try {
        connect();
        const std::uint16_t transaction = transaction_id_++;
        const std::vector<std::uint8_t> request =
            modbus::build_read_request(transaction, unit_id_, function, start_address,
                                       quantity);

        std::string error;
        if (!socket_.send_all(request.data(), request.size(), error)) {
            throw ModbusError("Modbus send failed: " + error);
        }

        std::array<std::uint8_t, 6> header{};
        std::size_t received = 0;
        while (received < header.size()) {
            const int count =
                socket_.receive(header.data() + received, header.size() - received, error);
            if (count <= 0) {
                throw ModbusError(count == 0 ? "Modbus peer closed the connection"
                                             : "Modbus receive failed: " + error);
            }
            received += static_cast<std::size_t>(count);
        }

        const std::uint16_t body_length =
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(header[4]) << 8U) |
                                       header[5]);
        if (body_length < 3 || body_length > 254) {
            throw ModbusError("invalid Modbus response length");
        }

        std::vector<std::uint8_t> frame(6 + static_cast<std::size_t>(body_length));
        std::copy(header.begin(), header.begin() + 6, frame.begin());
        std::size_t body_received = 0;
        while (body_received < body_length) {
            const std::size_t offset = 6 + body_received;
            const int count = socket_.receive(frame.data() + offset,
                                              frame.size() - offset, error);
            if (count <= 0) {
                throw ModbusError(count == 0 ? "Modbus peer closed the connection"
                                             : "Modbus receive failed: " + error);
            }
            body_received += static_cast<std::size_t>(count);
        }

        const modbus::ReadResponse response = modbus::parse_read_response(frame);
        if (response.transaction_id != transaction) {
            throw ModbusError("Modbus transaction id mismatch");
        }
        if (response.unit_id != unit_id_) {
            throw ModbusError("Modbus unit id mismatch");
        }
        if (response.function != function) {
            throw ModbusError("Modbus function code mismatch");
        }

        if (function == modbus::kReadCoils || function == modbus::kReadDiscreteInputs) {
            const std::size_t expected = (static_cast<std::size_t>(quantity) + 7) / 8;
            if (response.data.size() != expected) {
                throw ModbusError("Modbus bit response has an unexpected byte count");
            }
        } else {
            const std::size_t expected = static_cast<std::size_t>(quantity) * 2;
            if (response.data.size() != expected) {
                throw ModbusError("Modbus register response has an unexpected byte count");
            }
        }
        return response.data;
    } catch (...) {
        close();
        throw;
    }
}

std::vector<std::uint16_t> ModbusTcpClient::read_registers(std::uint8_t function,
                                                           std::uint16_t start_address,
                                                           std::uint16_t quantity) {
    if (function != modbus::kReadHoldingRegisters &&
        function != modbus::kReadInputRegisters) {
        throw std::invalid_argument("read_registers requires function 3 or 4");
    }
    if (quantity == 0 || quantity > modbus::kMaxRegisterQuantity) {
        throw std::invalid_argument("invalid Modbus register quantity");
    }
    const std::vector<std::uint8_t> data =
        transact(function, start_address, quantity);
    return modbus::decode_registers(data);
}

std::vector<std::uint8_t> ModbusTcpClient::read_bits(std::uint8_t function,
                                                     std::uint16_t start_address,
                                                     std::uint16_t quantity) {
    if (function != modbus::kReadCoils &&
        function != modbus::kReadDiscreteInputs) {
        throw std::invalid_argument("read_bits requires function 1 or 2");
    }
    if (quantity == 0 || quantity > modbus::kMaxBitQuantity) {
        throw std::invalid_argument("invalid Modbus bit quantity");
    }
    const std::vector<std::uint8_t> data = transact(function, start_address, quantity);
    std::vector<std::uint8_t> bits = modbus::decode_bits(data);
    bits.resize(quantity);
    return bits;
}

}  // namespace eg
