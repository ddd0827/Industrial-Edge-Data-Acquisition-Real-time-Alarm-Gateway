#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace eg {

enum class RegisterArea : std::uint8_t {
    Coil = 1,
    DiscreteInput = 2,
    HoldingRegister = 3,
    InputRegister = 4,
};

enum class RegisterValueType : std::uint8_t {
    Bool,
    U16,
    I16,
    U32,
    F32,
};

std::uint8_t register_function_code(RegisterArea area);
bool is_bit_area(RegisterArea area);
std::size_t register_width(RegisterValueType type);
std::string_view register_area_name(RegisterArea area);
std::string_view register_type_name(RegisterValueType type);

struct MetricSpec {
    std::uint32_t id = 0;
    std::string device;
    std::string name;
    std::string key;  // device.name, used by the rule DSL
    std::string unit;
    RegisterArea area = RegisterArea::HoldingRegister;
    std::uint16_t address = 0;
    RegisterValueType value_type = RegisterValueType::U16;
    double scale = 1.0;
    double offset = 0.0;

    bool is_boolean() const { return value_type == RegisterValueType::Bool; }
};

class MetricRegistry {
public:
    std::uint32_t add(MetricSpec spec);
    const MetricSpec* find_key(std::string_view key) const;
    const MetricSpec* find(std::string_view device, std::string_view metric) const;
    const MetricSpec* by_id(std::uint32_t id) const;
    const std::vector<MetricSpec>& all() const { return metrics_; }
    std::size_t size() const { return metrics_.size(); }

private:
    std::vector<MetricSpec> metrics_;
    std::unordered_map<std::string, std::uint32_t> by_key_;
};

}  // namespace eg
