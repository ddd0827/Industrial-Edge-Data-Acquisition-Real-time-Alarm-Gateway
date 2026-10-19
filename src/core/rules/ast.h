#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace eg {

enum class ExprType { Number, Boolean };

enum class UnaryOp { Negate, Not };

enum class BinaryOp {
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
    And,
    Or,
};

struct Expr {
    enum class Kind { Number, Boolean, Metric, Unary, Binary };

    Kind kind = Kind::Number;
    ExprType type = ExprType::Number;
    double number = 0.0;
    bool boolean = false;
    std::string device;
    std::string metric;
    std::uint32_t metric_id = 0;
    UnaryOp unary_op = UnaryOp::Negate;
    BinaryOp binary_op = BinaryOp::Add;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
    int line = 0;
    int column = 0;
};

struct RuleDefinition {
    std::string name;
    std::string title;
    std::unique_ptr<Expr> when;
    bool has_for = false;
    std::uint64_t for_ms = 0;
    std::unique_ptr<Expr> clear_when;
    bool has_clear = false;
    bool has_clear_for = false;
    std::uint64_t clear_for_ms = 0;
    std::string severity;
    std::string code;
    std::string message;
    int line = 0;
    int column = 0;
};

struct RuleAst {
    std::vector<RuleDefinition> rules;
};

}  // namespace eg
