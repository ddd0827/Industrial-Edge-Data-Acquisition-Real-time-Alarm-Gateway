#include "common/json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace eg {

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"':
                out.append("\\\"");
                break;
            case '\\':
                out.append("\\\\");
                break;
            case '\b':
                out.append("\\b");
                break;
            case '\f':
                out.append("\\f");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                if (ch < 0x20U) {
                    char escape[7]{};
                    std::snprintf(escape, sizeof(escape), "\\u%04X",
                                  static_cast<unsigned int>(ch));
                    out.append(escape);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return out;
}

void JsonWriter::before_value() {
    if (stack_.empty()) {
        if (root_written_) {
            throw std::logic_error("JSON writer already has a root value");
        }
        root_written_ = true;
        return;
    }

    Frame& parent = stack_.back();
    if (parent.object) {
        if (!parent.after_key) {
            throw std::logic_error("JSON object value must follow a key");
        }
        parent.after_key = false;
    } else {
        if (!parent.first) {
            output_.push_back(',');
        }
        parent.first = false;
    }
}

JsonWriter& JsonWriter::begin_object() {
    before_value();
    output_.push_back('{');
    stack_.push_back(Frame{true, true, false});
    return *this;
}

JsonWriter& JsonWriter::end_object() {
    if (stack_.empty() || !stack_.back().object || stack_.back().after_key) {
        throw std::logic_error("unbalanced JSON object end");
    }
    stack_.pop_back();
    output_.push_back('}');
    return *this;
}

JsonWriter& JsonWriter::begin_array() {
    before_value();
    output_.push_back('[');
    stack_.push_back(Frame{false, true, false});
    return *this;
}

JsonWriter& JsonWriter::end_array() {
    if (stack_.empty() || stack_.back().object) {
        throw std::logic_error("unbalanced JSON array end");
    }
    stack_.pop_back();
    output_.push_back(']');
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
    if (stack_.empty() || !stack_.back().object || stack_.back().after_key) {
        throw std::logic_error("JSON key outside of an object");
    }
    Frame& frame = stack_.back();
    if (!frame.first) {
        output_.push_back(',');
    }
    frame.first = false;
    frame.after_key = true;
    output_.push_back('"');
    output_.append(json_escape(name));
    output_.append("\":");
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view text) {
    before_value();
    output_.push_back('"');
    output_.append(json_escape(text));
    output_.push_back('"');
    return *this;
}

JsonWriter& JsonWriter::value(const char* text) {
    return value(std::string_view(text == nullptr ? "" : text));
}

JsonWriter& JsonWriter::value(bool value_in) {
    before_value();
    output_.append(value_in ? "true" : "false");
    return *this;
}

void JsonWriter::append_number(double value_in) {
    if (!std::isfinite(value_in)) {
        output_.append("null");
        return;
    }
    char buffer[64]{};
    const int written = std::snprintf(buffer, sizeof(buffer), "%.15g", value_in);
    if (written <= 0) {
        output_.append("null");
        return;
    }
    const std::size_t length =
        std::min<std::size_t>(static_cast<std::size_t>(written), sizeof(buffer) - 1);
    output_.append(buffer, length);
}

JsonWriter& JsonWriter::value(double value_in) {
    before_value();
    append_number(value_in);
    return *this;
}

void JsonWriter::append_unsigned(std::uint64_t value_in) {
    output_.append(std::to_string(value_in));
}

void JsonWriter::append_signed(std::int64_t value_in) {
    output_.append(std::to_string(value_in));
}

JsonWriter& JsonWriter::value(std::int64_t value_in) {
    before_value();
    append_signed(value_in);
    return *this;
}

JsonWriter& JsonWriter::value(std::uint64_t value_in) {
    before_value();
    append_unsigned(value_in);
    return *this;
}

JsonWriter& JsonWriter::value(std::int32_t value_in) {
    return value(static_cast<std::int64_t>(value_in));
}

JsonWriter& JsonWriter::value(std::uint32_t value_in) {
    return value(static_cast<std::uint64_t>(value_in));
}

JsonWriter& JsonWriter::null() {
    before_value();
    output_.append("null");
    return *this;
}

}  // namespace eg
