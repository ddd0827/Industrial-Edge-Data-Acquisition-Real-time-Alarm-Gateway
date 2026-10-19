#include "core/rules/parser.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>

#include "common/util.h"

namespace eg {
namespace {

std::unique_ptr<Expr> make_binary(BinaryOp op, std::unique_ptr<Expr> left,
                                  std::unique_ptr<Expr> right, int line, int column) {
    auto expression = std::make_unique<Expr>();
    expression->kind = Expr::Kind::Binary;
    expression->binary_op = op;
    expression->left = std::move(left);
    expression->right = std::move(right);
    expression->line = line;
    expression->column = column;
    return expression;
}

}  // namespace

RuleParser::RuleParser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {
    if (tokens_.empty() || tokens_.back().kind != TokenKind::End) {
        Token end;
        end.kind = TokenKind::End;
        tokens_.push_back(std::move(end));
    }
}

const Token& RuleParser::peek(std::size_t offset) const {
    const std::size_t position = index_ + offset;
    return position < tokens_.size() ? tokens_[position] : tokens_.back();
}

const Token& RuleParser::advance() {
    const Token& token = peek();
    if (index_ + 1 < tokens_.size()) {
        ++index_;
    }
    return token;
}

bool RuleParser::check(TokenKind kind) const {
    return peek().kind == kind;
}

bool RuleParser::match(TokenKind kind) {
    if (!check(kind)) {
        return false;
    }
    advance();
    return true;
}

const Token& RuleParser::expect(TokenKind kind, const std::string& context) {
    if (!check(kind)) {
        throw RuleError(peek().line, peek().column,
                        "expected " + std::string(token_kind_name(kind)) + " " + context +
                            ", got " + std::string(token_kind_name(peek().kind)));
    }
    return advance();
}

std::uint64_t RuleParser::parse_duration() {
    const Token& number = expect(TokenKind::Number, "for duration");
    const Token& unit = expect(TokenKind::Identifier, "after duration value (ms/s/m/h)");
    const auto parsed = parse_duration_ms(number.text + unit.text);
    if (!parsed.has_value()) {
        throw RuleError(number.line, number.column,
                        "invalid duration '" + number.text + unit.text + "'");
    }
    if (*parsed > 7ULL * 24ULL * 3600ULL * 1000ULL) {
        throw RuleError(number.line, number.column, "duration may not exceed 7 days");
    }
    return *parsed;
}

std::unique_ptr<Expr> RuleParser::parse_primary() {
    const Token& token = peek();
    if (token.kind == TokenKind::Number) {
        advance();
        auto expression = std::make_unique<Expr>();
        expression->kind = Expr::Kind::Number;
        expression->number = token.number;
        expression->line = token.line;
        expression->column = token.column;
        return expression;
    }
    if (token.kind == TokenKind::KwTrue || token.kind == TokenKind::KwFalse) {
        advance();
        auto expression = std::make_unique<Expr>();
        expression->kind = Expr::Kind::Boolean;
        expression->boolean = token.kind == TokenKind::KwTrue;
        expression->line = token.line;
        expression->column = token.column;
        return expression;
    }
    if (match(TokenKind::LParen)) {
        auto expression = parse_expression();
        expect(TokenKind::RParen, "after parenthesized expression");
        return expression;
    }
    if (token.kind == TokenKind::Identifier) {
        const Token& device = advance();
        expect(TokenKind::Dot, "after device name");
        const Token& metric = expect(TokenKind::Identifier, "after '.'");
        auto expression = std::make_unique<Expr>();
        expression->kind = Expr::Kind::Metric;
        expression->device = device.text;
        expression->metric = metric.text;
        expression->line = device.line;
        expression->column = device.column;
        return expression;
    }
    throw RuleError(token.line, token.column,
                    "expected expression, got " +
                        std::string(token_kind_name(token.kind)));
}

std::unique_ptr<Expr> RuleParser::parse_unary() {
    const Token& token = peek();
    if (token.kind == TokenKind::KwNot || token.kind == TokenKind::Minus) {
        advance();
        auto operand = parse_unary();
        auto expression = std::make_unique<Expr>();
        expression->kind = Expr::Kind::Unary;
        expression->unary_op =
            token.kind == TokenKind::KwNot ? UnaryOp::Not : UnaryOp::Negate;
        expression->left = std::move(operand);
        expression->line = token.line;
        expression->column = token.column;
        return expression;
    }
    return parse_primary();
}

std::unique_ptr<Expr> RuleParser::parse_multiplicative() {
    auto left = parse_unary();
    while (check(TokenKind::Star) || check(TokenKind::Slash)) {
        const Token op = advance();
        auto right = parse_unary();
        left = make_binary(op.kind == TokenKind::Star ? BinaryOp::Multiply
                                                       : BinaryOp::Divide,
                           std::move(left), std::move(right), op.line, op.column);
    }
    return left;
}

std::unique_ptr<Expr> RuleParser::parse_additive() {
    auto left = parse_multiplicative();
    while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
        const Token op = advance();
        auto right = parse_multiplicative();
        left = make_binary(op.kind == TokenKind::Plus ? BinaryOp::Add
                                                       : BinaryOp::Subtract,
                           std::move(left), std::move(right), op.line, op.column);
    }
    return left;
}

std::unique_ptr<Expr> RuleParser::parse_comparison() {
    auto left = parse_additive();
    while (check(TokenKind::Less) || check(TokenKind::LessEqual) ||
           check(TokenKind::Greater) || check(TokenKind::GreaterEqual)) {
        const Token op = advance();
        BinaryOp binary = BinaryOp::Less;
        switch (op.kind) {
            case TokenKind::Less:
                binary = BinaryOp::Less;
                break;
            case TokenKind::LessEqual:
                binary = BinaryOp::LessEqual;
                break;
            case TokenKind::Greater:
                binary = BinaryOp::Greater;
                break;
            case TokenKind::GreaterEqual:
                binary = BinaryOp::GreaterEqual;
                break;
            default:
                break;
        }
        auto right = parse_additive();
        left = make_binary(binary, std::move(left), std::move(right), op.line,
                           op.column);
    }
    return left;
}

std::unique_ptr<Expr> RuleParser::parse_equality() {
    auto left = parse_comparison();
    while (check(TokenKind::Equal) || check(TokenKind::NotEqual)) {
        const Token op = advance();
        auto right = parse_comparison();
        left = make_binary(op.kind == TokenKind::Equal ? BinaryOp::Equal
                                                        : BinaryOp::NotEqual,
                           std::move(left), std::move(right), op.line, op.column);
    }
    return left;
}

std::unique_ptr<Expr> RuleParser::parse_and() {
    auto left = parse_equality();
    while (check(TokenKind::KwAnd)) {
        const Token op = advance();
        auto right = parse_equality();
        left = make_binary(BinaryOp::And, std::move(left), std::move(right), op.line,
                           op.column);
    }
    return left;
}

std::unique_ptr<Expr> RuleParser::parse_or() {
    auto left = parse_and();
    while (check(TokenKind::KwOr)) {
        const Token op = advance();
        auto right = parse_and();
        left = make_binary(BinaryOp::Or, std::move(left), std::move(right), op.line,
                           op.column);
    }
    return left;
}

std::unique_ptr<Expr> RuleParser::parse_expression() {
    return parse_or();
}

RuleDefinition RuleParser::parse_rule() {
    const Token& rule_token = expect(TokenKind::KwRule, "at start of rule");
    RuleDefinition rule;
    rule.line = rule_token.line;
    rule.column = rule_token.column;
    rule.name = expect(TokenKind::Identifier, "after 'rule'").text;
    const Token& title = expect(TokenKind::String, "after rule name");
    rule.title = title.text;
    if (rule.title.empty()) {
        throw RuleError(title.line, title.column, "rule title must not be empty");
    }

    expect(TokenKind::LBrace, "after rule title");
    expect(TokenKind::KwWhen, "at start of rule body");
    rule.when = parse_expression();
    rule.has_for = match(TokenKind::KwFor);
    if (rule.has_for) {
        rule.for_ms = parse_duration();
    }

    rule.has_clear = match(TokenKind::KwClear);
    if (rule.has_clear) {
        expect(TokenKind::KwWhen, "after 'clear'");
        rule.clear_when = parse_expression();
        rule.has_clear_for = match(TokenKind::KwFor);
        if (rule.has_clear_for) {
            rule.clear_for_ms = parse_duration();
        }
    }

    const Token& emit_token = expect(TokenKind::KwEmit, "before rule metadata");
    expect(TokenKind::LBrace, "after 'emit'");

    bool severity_set = false;
    bool code_set = false;
    bool message_set = false;
    while (!check(TokenKind::RBrace)) {
        if (check(TokenKind::End)) {
            throw RuleError(peek().line, peek().column,
                            "unterminated emit block");
        }
        const Token& key_token = expect(TokenKind::Identifier, "in emit block");
        const std::string key = to_lower(key_token.text);
        if (key != "severity" && key != "code" && key != "message") {
            throw RuleError(key_token.line, key_token.column,
                            "unknown emit property '" + key_token.text +
                                "' (expected severity/code/message)");
        }
        expect(TokenKind::Assign, "after emit property name");
        if (!check(TokenKind::Identifier) && !check(TokenKind::String)) {
            throw RuleError(peek().line, peek().column,
                            "expected a string or identifier for '" + key + "'");
        }
        const Token& value = advance();
        if (value.text.empty()) {
            throw RuleError(value.line, value.column,
                            "emit property '" + key + "' must not be empty");
        }

        if (key == "severity") {
            if (severity_set) {
                throw RuleError(key_token.line, key_token.column,
                                "duplicate emit property 'severity'");
            }
            rule.severity = to_lower(value.text);
            severity_set = true;
        } else if (key == "code") {
            if (code_set) {
                throw RuleError(key_token.line, key_token.column,
                                "duplicate emit property 'code'");
            }
            rule.code = value.text;
            code_set = true;
        } else {
            if (message_set) {
                throw RuleError(key_token.line, key_token.column,
                                "duplicate emit property 'message'");
            }
            rule.message = value.text;
            message_set = true;
        }

        if (check(TokenKind::Semicolon) || check(TokenKind::Comma)) {
            advance();
        }
    }
    expect(TokenKind::RBrace, "at end of emit block");
    expect(TokenKind::RBrace, "at end of rule body");

    if (!severity_set || !code_set || !message_set) {
        throw RuleError(emit_token.line, emit_token.column,
                        "rule '" + rule.name +
                            "' emit block must define severity, code and message");
    }

    return rule;
}

RuleAst RuleParser::parse() {
    RuleAst ast;
    std::set<std::string> names;
    while (!check(TokenKind::End)) {
        if (match(TokenKind::Semicolon)) {
            continue;
        }
        if (!check(TokenKind::KwRule)) {
            throw RuleError(peek().line, peek().column,
                            "expected 'rule', got " +
                                std::string(token_kind_name(peek().kind)));
        }
        RuleDefinition rule = parse_rule();
        if (!names.insert(rule.name).second) {
            throw RuleError(rule.line, rule.column,
                            "duplicate rule name '" + rule.name + "'");
        }
        ast.rules.push_back(std::move(rule));
        match(TokenKind::Semicolon);
    }

    if (ast.rules.empty()) {
        throw RuleError(1, 1, "rule file does not contain any rules");
    }
    return ast;
}

RuleAst RuleParser::parse_source(std::string_view source) {
    return RuleParser(lex_rule_source(source)).parse();
}

}  // namespace eg
