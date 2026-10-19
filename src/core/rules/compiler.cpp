#include "core/rules/compiler.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "common/util.h"
#include "core/rules/lexer.h"
#include "core/rules/parser.h"

namespace eg {
namespace {

const char* type_name(ExprType type) {
    return type == ExprType::Boolean ? "bool" : "number";
}

void infer_type(Expr& expression, const MetricRegistry& registry) {
    switch (expression.kind) {
        case Expr::Kind::Number:
            expression.type = ExprType::Number;
            return;
        case Expr::Kind::Boolean:
            expression.type = ExprType::Boolean;
            return;
        case Expr::Kind::Metric: {
            const MetricSpec* metric =
                registry.find(expression.device, expression.metric);
            if (metric == nullptr) {
                throw RuleError(expression.line, expression.column,
                                "unknown metric '" + expression.device + "." +
                                    expression.metric + "'");
            }
            expression.metric_id = metric->id;
            expression.type =
                metric->is_boolean() ? ExprType::Boolean : ExprType::Number;
            return;
        }
        case Expr::Kind::Unary: {
            infer_type(*expression.left, registry);
            if (expression.unary_op == UnaryOp::Not) {
                if (expression.left->type != ExprType::Boolean) {
                    throw RuleError(expression.line, expression.column,
                                    "'not' requires a boolean operand, got " +
                                        std::string(type_name(expression.left->type)));
                }
                expression.type = ExprType::Boolean;
            } else {
                if (expression.left->type != ExprType::Number) {
                    throw RuleError(expression.line, expression.column,
                                    "unary '-' requires a number operand, got " +
                                        std::string(type_name(expression.left->type)));
                }
                expression.type = ExprType::Number;
            }
            return;
        }
        case Expr::Kind::Binary: {
            infer_type(*expression.left, registry);
            infer_type(*expression.right, registry);
            const ExprType left = expression.left->type;
            const ExprType right = expression.right->type;

            switch (expression.binary_op) {
                case BinaryOp::Add:
                case BinaryOp::Subtract:
                case BinaryOp::Multiply:
                case BinaryOp::Divide:
                    if (left != ExprType::Number || right != ExprType::Number) {
                        throw RuleError(expression.line, expression.column,
                                        "arithmetic requires number operands, got " +
                                            std::string(type_name(left)) + " and " +
                                            std::string(type_name(right)));
                    }
                    expression.type = ExprType::Number;
                    return;
                case BinaryOp::Less:
                case BinaryOp::LessEqual:
                case BinaryOp::Greater:
                case BinaryOp::GreaterEqual:
                    if (left != ExprType::Number || right != ExprType::Number) {
                        throw RuleError(expression.line, expression.column,
                                        "ordering comparisons require number operands, got " +
                                            std::string(type_name(left)) + " and " +
                                            std::string(type_name(right)));
                    }
                    expression.type = ExprType::Boolean;
                    return;
                case BinaryOp::Equal:
                case BinaryOp::NotEqual:
                    if (left != right) {
                        throw RuleError(expression.line, expression.column,
                                        "cannot compare " + std::string(type_name(left)) +
                                            " with " + type_name(right));
                    }
                    expression.type = ExprType::Boolean;
                    return;
                case BinaryOp::And:
                case BinaryOp::Or:
                    if (left != ExprType::Boolean || right != ExprType::Boolean) {
                        throw RuleError(expression.line, expression.column,
                                        "logical operator requires boolean operands, got " +
                                            std::string(type_name(left)) + " and " +
                                            std::string(type_name(right)));
                    }
                    expression.type = ExprType::Boolean;
                    return;
            }
            return;
        }
    }
}

void collect_metrics(const Expr& expression, std::vector<std::uint32_t>& metrics) {
    if (expression.kind == Expr::Kind::Metric) {
        metrics.push_back(expression.metric_id);
        return;
    }
    if (expression.left != nullptr) {
        collect_metrics(*expression.left, metrics);
    }
    if (expression.right != nullptr) {
        collect_metrics(*expression.right, metrics);
    }
}

std::size_t required_stack(const Expr& expression) {
    switch (expression.kind) {
        case Expr::Kind::Number:
        case Expr::Kind::Boolean:
        case Expr::Kind::Metric:
            return 1;
        case Expr::Kind::Unary:
            return required_stack(*expression.left);
        case Expr::Kind::Binary: {
            const std::size_t left = required_stack(*expression.left);
            const std::size_t right = required_stack(*expression.right);
            if (expression.binary_op == BinaryOp::And ||
                expression.binary_op == BinaryOp::Or) {
                return std::max(left, std::max(right, static_cast<std::size_t>(1)));
            }
            return std::max<std::size_t>(left, 1 + right);
        }
    }
    return 1;
}

class ExpressionCompiler {
public:
    explicit ExpressionCompiler(CompiledExpression& output) : output_(output) {}

    void compile(const Expr& expression) {
        switch (expression.kind) {
            case Expr::Kind::Number:
                emit(Opcode::PushConst, 0, expression.number);
                return;
            case Expr::Kind::Boolean:
                emit(Opcode::PushConst, 0, expression.boolean ? 1.0 : 0.0);
                return;
            case Expr::Kind::Metric:
                emit(Opcode::LoadMetric, expression.metric_id);
                return;
            case Expr::Kind::Unary:
                compile(*expression.left);
                emit(expression.unary_op == UnaryOp::Not ? Opcode::Not : Opcode::Negate);
                return;
            case Expr::Kind::Binary:
                compile_binary(expression);
                return;
        }
    }

private:
    std::size_t emit(Opcode opcode, std::uint32_t operand = 0, double number = 0.0) {
        output_.code.push_back(Instruction{opcode, operand, number});
        return output_.code.size() - 1;
    }

    void compile_binary(const Expr& expression) {
        if (expression.binary_op == BinaryOp::And) {
            compile(*expression.left);
            const std::size_t jump_false = emit(Opcode::JumpIfFalse);
            compile(*expression.right);
            const std::size_t jump_end = emit(Opcode::Jump);
            output_.code[jump_false].operand =
                static_cast<std::uint32_t>(output_.code.size());
            emit(Opcode::PushConst, 0, 0.0);
            output_.code[jump_end].operand =
                static_cast<std::uint32_t>(output_.code.size());
            return;
        }
        if (expression.binary_op == BinaryOp::Or) {
            compile(*expression.left);
            const std::size_t jump_true = emit(Opcode::JumpIfTrue);
            compile(*expression.right);
            const std::size_t jump_end = emit(Opcode::Jump);
            output_.code[jump_true].operand =
                static_cast<std::uint32_t>(output_.code.size());
            emit(Opcode::PushConst, 0, 1.0);
            output_.code[jump_end].operand =
                static_cast<std::uint32_t>(output_.code.size());
            return;
        }

        compile(*expression.left);
        compile(*expression.right);
        Opcode opcode = Opcode::Add;
        switch (expression.binary_op) {
            case BinaryOp::Add:
                opcode = Opcode::Add;
                break;
            case BinaryOp::Subtract:
                opcode = Opcode::Subtract;
                break;
            case BinaryOp::Multiply:
                opcode = Opcode::Multiply;
                break;
            case BinaryOp::Divide:
                opcode = Opcode::Divide;
                break;
            case BinaryOp::Less:
                opcode = Opcode::Less;
                break;
            case BinaryOp::LessEqual:
                opcode = Opcode::LessEqual;
                break;
            case BinaryOp::Greater:
                opcode = Opcode::Greater;
                break;
            case BinaryOp::GreaterEqual:
                opcode = Opcode::GreaterEqual;
                break;
            case BinaryOp::Equal:
                opcode = Opcode::Equal;
                break;
            case BinaryOp::NotEqual:
                opcode = Opcode::NotEqual;
                break;
            case BinaryOp::And:
            case BinaryOp::Or:
                break;
        }
        emit(opcode);
    }

    CompiledExpression& output_;
};

CompiledRule compile_rule(const RuleDefinition& definition,
                          const MetricRegistry& registry) {
    infer_type(*definition.when, registry);
    if (definition.when->type != ExprType::Boolean) {
        throw RuleError(definition.when->line, definition.when->column,
                        "rule condition must evaluate to bool, got " +
                            std::string(type_name(definition.when->type)));
    }
    if (definition.has_clear && definition.clear_when != nullptr) {
        infer_type(*definition.clear_when, registry);
        if (definition.clear_when->type != ExprType::Boolean) {
            throw RuleError(definition.clear_when->line, definition.clear_when->column,
                            "clear condition must evaluate to bool, got " +
                                std::string(type_name(definition.clear_when->type)));
        }
    }

    CompiledRule rule;
    rule.name = definition.name;
    rule.title = definition.title;
    rule.severity = definition.severity;
    rule.code = definition.code;
    rule.message = definition.message;
    rule.for_ms = definition.for_ms;
    rule.has_clear = definition.has_clear;
    rule.clear_for_ms = definition.clear_for_ms;

    rule.when.max_stack = std::max<std::size_t>(1, required_stack(*definition.when));
    ExpressionCompiler when_compiler(rule.when);
    when_compiler.compile(*definition.when);

    if (rule.has_clear && definition.clear_when != nullptr) {
        rule.clear.max_stack =
            std::max<std::size_t>(1, required_stack(*definition.clear_when));
        ExpressionCompiler clear_compiler(rule.clear);
        clear_compiler.compile(*definition.clear_when);
    }

    collect_metrics(*definition.when, rule.referenced_metrics);
    if (definition.has_clear && definition.clear_when != nullptr) {
        collect_metrics(*definition.clear_when, rule.referenced_metrics);
    }
    std::sort(rule.referenced_metrics.begin(), rule.referenced_metrics.end());
    rule.referenced_metrics.erase(
        std::unique(rule.referenced_metrics.begin(), rule.referenced_metrics.end()),
        rule.referenced_metrics.end());
    if (rule.referenced_metrics.empty()) {
        throw RuleError(definition.line, definition.column,
                        "rule '" + rule.name + "' does not reference any metric");
    }
    return rule;
}

}  // namespace

const char* opcode_name(Opcode opcode) {
    switch (opcode) {
        case Opcode::PushConst:
            return "PUSH_CONST";
        case Opcode::LoadMetric:
            return "LOAD_METRIC";
        case Opcode::Add:
            return "ADD";
        case Opcode::Subtract:
            return "SUB";
        case Opcode::Multiply:
            return "MUL";
        case Opcode::Divide:
            return "DIV";
        case Opcode::Less:
            return "LT";
        case Opcode::LessEqual:
            return "LE";
        case Opcode::Greater:
            return "GT";
        case Opcode::GreaterEqual:
            return "GE";
        case Opcode::Equal:
            return "EQ";
        case Opcode::NotEqual:
            return "NE";
        case Opcode::Not:
            return "NOT";
        case Opcode::Negate:
            return "NEG";
        case Opcode::JumpIfFalse:
            return "JIF_FALSE";
        case Opcode::JumpIfTrue:
            return "JIT_TRUE";
        case Opcode::Jump:
            return "JUMP";
    }
    return "UNKNOWN";
}

std::string disassemble_expression(const CompiledExpression& expression) {
    std::ostringstream stream;
    stream << "max_stack=" << expression.max_stack << "\n";
    for (std::size_t pc = 0; pc < expression.code.size(); ++pc) {
        const Instruction& instruction = expression.code[pc];
        stream << "  " << pc << "  " << opcode_name(instruction.opcode);
        switch (instruction.opcode) {
            case Opcode::PushConst:
                stream << " " << instruction.number;
                break;
            case Opcode::LoadMetric:
                stream << " metric#" << instruction.operand;
                break;
            case Opcode::JumpIfFalse:
            case Opcode::JumpIfTrue:
            case Opcode::Jump:
                stream << " -> " << instruction.operand;
                break;
            default:
                break;
        }
        stream << "\n";
    }
    return stream.str();
}

bool evaluate_condition(const CompiledExpression& expression,
                        const std::vector<double>& latest_values,
                        const std::vector<bool>& available) {
    std::vector<double> stack;
    stack.reserve(expression.max_stack + 2);
    std::size_t pc = 0;

    auto pop = [&](double& out) -> bool {
        if (stack.empty()) {
            return false;
        }
        out = stack.back();
        stack.pop_back();
        return true;
    };

    while (pc < expression.code.size()) {
        const Instruction& instruction = expression.code[pc];
        switch (instruction.opcode) {
            case Opcode::PushConst:
                stack.push_back(instruction.number);
                ++pc;
                break;
            case Opcode::LoadMetric:
                if (instruction.operand >= latest_values.size() ||
                    instruction.operand >= available.size() ||
                    !available[instruction.operand]) {
                    return false;
                }
                stack.push_back(latest_values[instruction.operand]);
                ++pc;
                break;
            case Opcode::Add:
            case Opcode::Subtract:
            case Opcode::Multiply:
            case Opcode::Divide:
            case Opcode::Less:
            case Opcode::LessEqual:
            case Opcode::Greater:
            case Opcode::GreaterEqual:
            case Opcode::Equal:
            case Opcode::NotEqual: {
                double right = 0.0;
                double left = 0.0;
                if (!pop(right) || !pop(left)) {
                    return false;
                }
                double result = 0.0;
                switch (instruction.opcode) {
                    case Opcode::Add:
                        result = left + right;
                        break;
                    case Opcode::Subtract:
                        result = left - right;
                        break;
                    case Opcode::Multiply:
                        result = left * right;
                        break;
                    case Opcode::Divide:
                        if (right == 0.0) {
                            return false;
                        }
                        result = left / right;
                        break;
                    case Opcode::Less:
                        result = left < right ? 1.0 : 0.0;
                        break;
                    case Opcode::LessEqual:
                        result = left <= right ? 1.0 : 0.0;
                        break;
                    case Opcode::Greater:
                        result = left > right ? 1.0 : 0.0;
                        break;
                    case Opcode::GreaterEqual:
                        result = left >= right ? 1.0 : 0.0;
                        break;
                    case Opcode::Equal:
                        result = left == right ? 1.0 : 0.0;
                        break;
                    case Opcode::NotEqual:
                        result = left != right ? 1.0 : 0.0;
                        break;
                    default:
                        break;
                }
                stack.push_back(result);
                ++pc;
                break;
            }
            case Opcode::Not: {
                double value = 0.0;
                if (!pop(value)) {
                    return false;
                }
                stack.push_back(value == 0.0 ? 1.0 : 0.0);
                ++pc;
                break;
            }
            case Opcode::Negate: {
                double value = 0.0;
                if (!pop(value)) {
                    return false;
                }
                stack.push_back(-value);
                ++pc;
                break;
            }
            case Opcode::JumpIfFalse:
            case Opcode::JumpIfTrue: {
                double value = 0.0;
                if (!pop(value)) {
                    return false;
                }
                if (instruction.operand > expression.code.size()) {
                    return false;
                }
                const bool condition =
                    instruction.opcode == Opcode::JumpIfTrue ? value != 0.0
                                                              : value == 0.0;
                pc = condition ? instruction.operand : pc + 1;
                break;
            }
            case Opcode::Jump:
                if (instruction.operand > expression.code.size()) {
                    return false;
                }
                pc = instruction.operand;
                break;
        }
    }
    return stack.size() == 1 && stack.back() != 0.0;
}

RuleCompiler::RuleCompiler(const MetricRegistry& registry) : registry_(registry) {}

RuleProgram RuleCompiler::compile_source(std::string_view source) const {
    RuleAst ast = RuleParser::parse_source(source);
    RuleProgram program;
    program.rules.reserve(ast.rules.size());
    for (const RuleDefinition& definition : ast.rules) {
        program.rules.push_back(compile_rule(definition, registry_));
    }
    return program;
}

RuleProgram RuleCompiler::compile_file(const std::string& path) const {
    std::string source;
    std::string error;
    if (!read_text_file(path, source, error)) {
        throw std::runtime_error("cannot read rule file: " + error);
    }
    try {
        return compile_source(source);
    } catch (const RuleError& ex) {
        throw std::runtime_error(path + ": " + ex.what());
    }
}

}  // namespace eg
