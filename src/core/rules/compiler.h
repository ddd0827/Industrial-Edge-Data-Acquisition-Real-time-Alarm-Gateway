#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/metrics.h"
#include "core/rules/ast.h"

namespace eg {

enum class Opcode {
    PushConst,
    LoadMetric,
    Add,
    Subtract,
    Multiply,
    Divide,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    NotEqual,
    Not,
    Negate,
    JumpIfFalse,
    JumpIfTrue,
    Jump,
};

struct Instruction {
    Opcode opcode = Opcode::PushConst;
    std::uint32_t operand = 0;
    double number = 0.0;
};

struct CompiledExpression {
    std::vector<Instruction> code;
    std::size_t max_stack = 1;
};

struct CompiledRule {
    std::string name;
    std::string title;
    std::string severity;
    std::string code;
    std::string message;
    std::uint64_t for_ms = 0;
    bool has_clear = false;
    std::uint64_t clear_for_ms = 0;
    CompiledExpression when;
    CompiledExpression clear;
    std::vector<std::uint32_t> referenced_metrics;
};

struct RuleProgram {
    std::vector<CompiledRule> rules;

    std::size_t instruction_count() const {
        std::size_t count = 0;
        for (const CompiledRule& rule : rules) {
            count += rule.when.code.size();
            count += rule.clear.code.size();
        }
        return count;
    }
};

class RuleCompiler {
public:
    explicit RuleCompiler(const MetricRegistry& registry);

    RuleProgram compile_file(const std::string& path) const;
    RuleProgram compile_source(std::string_view source) const;

private:
    const MetricRegistry& registry_;
};

const char* opcode_name(Opcode opcode);
std::string disassemble_expression(const CompiledExpression& expression);

bool evaluate_condition(const CompiledExpression& expression,
                        const std::vector<double>& latest_values,
                        const std::vector<bool>& available);

}  // namespace eg
