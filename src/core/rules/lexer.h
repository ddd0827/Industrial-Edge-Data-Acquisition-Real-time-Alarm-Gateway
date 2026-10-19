#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace eg {

enum class TokenKind {
    End,
    Identifier,
    Number,
    String,
    KwRule,
    KwWhen,
    KwFor,
    KwClear,
    KwEmit,
    KwAnd,
    KwOr,
    KwNot,
    KwTrue,
    KwFalse,
    LBrace,
    RBrace,
    LParen,
    RParen,
    Dot,
    Assign,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Plus,
    Minus,
    Star,
    Slash,
    Semicolon,
    Comma,
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    double number = 0.0;
    int line = 1;
    int column = 1;
};

class RuleError : public std::runtime_error {
public:
    RuleError(int line, int column, const std::string& message);

    int line() const { return line_; }
    int column() const { return column_; }

private:
    int line_ = 0;
    int column_ = 0;
};

std::string_view token_kind_name(TokenKind kind);
std::vector<Token> lex_rule_source(std::string_view source);

}  // namespace eg
