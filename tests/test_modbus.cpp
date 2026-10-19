#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "protocol/modbus.h"
#include "protocol/simulator.h"
#include "test_framework.h"

TEST_CASE(modbus_codec_builds_and_parses_frames) {
    const std::vector<std::uint8_t> request =
        eg::modbus::build_read_request(0x1234, 1, eg::modbus::kReadHoldingRegisters,
                                       0x0010, 2);
    const std::vector<std::uint8_t> expected = {0x12, 0x34, 0x00, 0x00, 0x00, 0x06,
                                                0x01, 0x03, 0x00, 0x10, 0x00, 0x02};
    REQUIRE(request == expected);

    const eg::modbus::ReadRequest parsed_request =
        eg::modbus::parse_read_request(request);
    REQUIRE_EQ(parsed_request.transaction_id, static_cast<std::uint16_t>(0x1234));
    REQUIRE_EQ(parsed_request.unit_id, static_cast<std::uint8_t>(1));
    REQUIRE_EQ(parsed_request.start_address, static_cast<std::uint16_t>(16));
    REQUIRE_EQ(parsed_request.quantity, static_cast<std::uint16_t>(2));

    const std::vector<std::uint8_t> payload = {0x00, 0x2A, 0x01, 0x02};
    const std::vector<std::uint8_t> response =
        eg::modbus::build_read_response(0x1234, 1, eg::modbus::kReadHoldingRegisters,
                                        payload);
    const eg::modbus::ReadResponse parsed_response =
        eg::modbus::parse_read_response(response);
    REQUIRE_EQ(parsed_response.transaction_id, static_cast<std::uint16_t>(0x1234));
    REQUIRE(parsed_response.data == payload);
    const std::vector<std::uint16_t> registers =
        eg::modbus::decode_registers(parsed_response.data);
    REQUIRE_EQ(registers.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(registers[0], static_cast<std::uint16_t>(42));
    REQUIRE_EQ(registers[1], static_cast<std::uint16_t>(258));
}

TEST_CASE(modbus_codec_handles_bits_and_exceptions) {
    const std::vector<std::uint8_t> bits =
        eg::modbus::decode_bits(std::vector<std::uint8_t>{0x05});
    REQUIRE_EQ(bits.size(), static_cast<std::size_t>(8));
    REQUIRE_EQ(bits[0], static_cast<std::uint8_t>(1));
    REQUIRE_EQ(bits[1], static_cast<std::uint8_t>(0));
    REQUIRE_EQ(bits[2], static_cast<std::uint8_t>(1));

    const std::vector<std::uint8_t> exception =
        eg::modbus::build_exception_response(7, 1, eg::modbus::kReadCoils, 2);
    REQUIRE_THROW(eg::modbus::parse_read_response(exception), eg::ModbusError);

    REQUIRE_THROW(eg::modbus::build_read_request(
                      1, 1, eg::modbus::kReadHoldingRegisters, 0, 126),
                  std::invalid_argument);
}

TEST_CASE(modbus_client_talks_to_the_simulator) {
    eg::SimulatorServer simulator(false);
    std::string error;
    REQUIRE(simulator.start("127.0.0.1", 0, error));

    simulator.set_holding_register(1, 0, 700);
    simulator.set_holding_register(1, 1, 1234);
    simulator.set_input_register(1, 0, 55);
    simulator.set_coil(1, 0, true);

    eg::ModbusTcpClient client("127.0.0.1", simulator.port(), 1);
    const std::vector<std::uint16_t> holding =
        client.read_registers(eg::modbus::kReadHoldingRegisters, 0, 2);
    REQUIRE_EQ(holding.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(holding[0], static_cast<std::uint16_t>(700));
    REQUIRE_EQ(holding[1], static_cast<std::uint16_t>(1234));

    const std::vector<std::uint16_t> input =
        client.read_registers(eg::modbus::kReadInputRegisters, 0, 1);
    REQUIRE_EQ(input.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(input[0], static_cast<std::uint16_t>(55));

    const std::vector<std::uint8_t> coils =
        client.read_bits(eg::modbus::kReadCoils, 0, 1);
    REQUIRE_EQ(coils.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(coils[0], static_cast<std::uint8_t>(1));

    REQUIRE_THROW(
        client.read_registers(eg::modbus::kReadHoldingRegisters, 600, 1),
        eg::ModbusError);

    // The client reconnects after a protocol error.
    simulator.set_holding_register(1, 1, 4321);
    const std::vector<std::uint16_t> again =
        client.read_registers(eg::modbus::kReadHoldingRegisters, 1, 1);
    REQUIRE_EQ(again[0], static_cast<std::uint16_t>(4321));

    client.close();
    simulator.stop();
}
