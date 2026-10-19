#include "core/metrics.h"

#include <stdexcept>
#include <utility>

namespace eg {

std::uint8_t register_function_code(RegisterArea area) {
    return static_cast<std::uint8_t>(area);
}

bool is_bit_area(RegisterArea area) {
    return area == RegisterArea::Coil || area == RegisterArea::DiscreteInput;
}

std::size_t register_width(RegisterValueType type) {
    switch (type) {
        case RegisterValueType::Bool:
        case RegisterValueType::U16:
        case RegisterValueType::I16:
            return 1;
        case RegisterValueType::U32:
        case RegisterValueType::F32:
            return 2;
    }
    return 1;
}

std::string_view register_area_name(RegisterArea area) {
    switch (area) {
        case RegisterArea::Coil:
            return "coil";
        case RegisterArea::DiscreteInput:
            return "discrete";
        case RegisterArea::HoldingRegister:
            return "holding";
        case RegisterArea::InputRegister:
            return "input";
    }
    return "holding";
}

std::string_view register_type_name(RegisterValueType type) {
    switch (type) {
        case RegisterValueType::Bool:
            return "bool";
        case RegisterValueType::U16:
            return "u16";
        case RegisterValueType::I16:
            return "i16";
        case RegisterValueType::U32:
            return "u32";
        case RegisterValueType::F32:
            return "f32";
    }
    return "u16";
}

std::uint32_t MetricRegistry::add(MetricSpec spec) {
    if (spec.device.empty() || spec.name.empty()) {
        throw std::invalid_argument("metric device and name must not be empty");
    }
    if (spec.key.empty()) {
        spec.key = spec.device + "." + spec.name;
    }
    if (by_key_.find(spec.key) != by_key_.end()) {
        throw std::invalid_argument("duplicate metric: " + spec.key);
    }
    if (metrics_.size() >= 0xFFFFFFFFULL) {
        throw std::invalid_argument("too many metrics");
    }
    spec.id = static_cast<std::uint32_t>(metrics_.size());
    metrics_.push_back(std::move(spec));
    by_key_.emplace(metrics_.back().key, metrics_.back().id);
    return metrics_.back().id;
}

const MetricSpec* MetricRegistry::find_key(std::string_view key) const {
    const auto it = by_key_.find(std::string(key));
    if (it == by_key_.end()) {
        return nullptr;
    }
    return &metrics_[it->second];
}

const MetricSpec* MetricRegistry::find(std::string_view device,
                                       std::string_view metric) const {
    std::string key;
    key.reserve(device.size() + metric.size() + 1);
    key.append(device);
    key.push_back('.');
    key.append(metric);
    return find_key(key);
}

const MetricSpec* MetricRegistry::by_id(std::uint32_t id) const {
    if (id >= metrics_.size()) {
        return nullptr;
    }
    return &metrics_[id];
}

}  // namespace eg
