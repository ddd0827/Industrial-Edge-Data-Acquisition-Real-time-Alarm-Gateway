#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eg {

std::string json_escape(std::string_view value);

class JsonWriter {
public:
    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();

    JsonWriter& key(std::string_view name);

    JsonWriter& value(std::string_view text);
    JsonWriter& value(const std::string& text) {
        return value(std::string_view(text));
    }
    JsonWriter& value(const char* text);
    JsonWriter& value(bool value);
    JsonWriter& value(double value);
    JsonWriter& value(std::int64_t value);
    JsonWriter& value(std::uint64_t value);
    JsonWriter& value(std::int32_t value);
    JsonWriter& value(std::uint32_t value);
    JsonWriter& null();

    const std::string& str() const { return output_; }
    std::string take() { return std::move(output_); }

private:
    struct Frame {
        bool object = false;
        bool first = true;
        bool after_key = false;
    };

    void before_value();
    void append_number(double value);
    void append_unsigned(std::uint64_t value);
    void append_signed(std::int64_t value);

    std::string output_;
    std::vector<Frame> stack_;
    bool root_written_ = false;
};

}  // namespace eg
