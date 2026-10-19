#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "core/rules/ast.h"
#include "core/rules/lexer.h"

namespace eg {

class RuleParser {
public:
    explicit RuleParser(std::vector<Token> tokens);

    RuleAst parse();
    static RuleAst parse_source(std::string_view source);

private:
    std::unique_ptr<Expr> parse_expression();
    std::unique_ptr<Expr> parse_or();
    std::unique_ptr<Expr> parse_and();
    std::unique_ptr<Expr> parse_equality();
    std::unique_ptr<Expr> parse_comparison();
    std::unique_ptr<Expr> parse_additive();
    std::unique_ptr<Expr> parse_multiplicative();
    std::unique_ptr<Expr> parse_unary();
    std::unique_ptr<Expr> parse_primary();

    RuleDefinition parse_rule();
    std::uint64_t parse_duration();

    const Token& peek(std::size_t offset = 0) const;
    const Token& advance();
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);
    const Token& expect(TokenKind kind, const std::string& context);

    std::vector<Token> tokens_;
    std::size_t index_ = 0;
};

}  // namespace eg
