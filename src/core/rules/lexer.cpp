#include "core/rules/lexer.h"

#include <cctype>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "common/util.h"

namespace eg {
namespace {

bool is_identifier_start(char ch) {
    const auto value = static_cast<unsigned char>(ch);
    return std::isalpha(value) != 0 || ch == '_';
}

bool is_identifier_char(char ch) {
    const auto value = static_cast<unsigned char>(ch);
    return std::isalnum(value) != 0 || ch == '_';
}

TokenKind keyword_kind(const std::string& text) {
    static const std::unordered_map<std::string, TokenKind> keywords = {
        {"rule", TokenKind::KwRule},   {"when", TokenKind::KwWhen},
        {"for", TokenKind::KwFor},     {"clear", TokenKind::KwClear},
        {"emit", TokenKind::KwEmit},   {"and", TokenKind::KwAnd},
        {"or", TokenKind::KwOr},       {"not", TokenKind::KwNot},
        {"true", TokenKind::KwTrue},   {"false", TokenKind::KwFalse},
    };
    const auto it = keywords.find(text);
    return it == keywords.end() ? TokenKind::Identifier : it->second;
}

}  // namespace

RuleError::RuleError(int line, int column, const std::string& message)
    : std::runtime_error("line " + std::to_string(line) + ", column " +
                         std::to_string(column) + ": " + message),
      line_(line),
      column_(column) {}

std::string_view token_kind_name(TokenKind kind) {
    switch (kind) {
        case TokenKind::End:
            return "end of input";
        case TokenKind::Identifier:
            return "identifier";
        case TokenKind::Number:
            return "number";
        case TokenKind::String:
            return "string";
        case TokenKind::KwRule:
            return "'rule'";
        case TokenKind::KwWhen:
            return "'when'";
        case TokenKind::KwFor:
            return "'for'";
        case TokenKind::KwClear:
            return "'clear'";
        case TokenKind::KwEmit:
            return "'emit'";
        case TokenKind::KwAnd:
            return "'and'";
        case TokenKind::KwOr:
            return "'or'";
        case TokenKind::KwNot:
            return "'not'";
        case TokenKind::KwTrue:
            return "'true'";
        case TokenKind::KwFalse:
            return "'false'";
        case TokenKind::LBrace:
            return "'{'";
        case TokenKind::RBrace:
            return "'}'";
        case TokenKind::LParen:
            return "'('";
        case TokenKind::RParen:
            return "')'";
        case TokenKind::Dot:
            return "'.'";
        case TokenKind::Assign:
            return "'='";
        case TokenKind::Equal:
            return "'=='";
        case TokenKind::NotEqual:
            return "'!='";
        case TokenKind::Less:
            return "'<'";
        case TokenKind::LessEqual:
            return "'<='";
        case TokenKind::Greater:
            return "'>'";
        case TokenKind::GreaterEqual:
            return "'>='";
        case TokenKind::Plus:
            return "'+'";
        case TokenKind::Minus:
            return "'-'";
        case TokenKind::Star:
            return "'*'";
        case TokenKind::Slash:
            return "'/'";
        case TokenKind::Semicolon:
            return "';'";
        case TokenKind::Comma:
            return "','";
    }
    return "token";
}

std::vector<Token> lex_rule_source(std::string_view source) {
    std::vector<Token> tokens;
    std::size_t position = 0;
    int line = 1;
    int column = 1;

    auto peek = [&](std::size_t offset = 0) -> char {
        return position + offset < source.size() ? source[position + offset] : '\0';
    };
    auto advance = [&]() -> char {
        if (position >= source.size()) {
            return '\0';
        }
        const char ch = source[position++];
        if (ch == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
        return ch;
    };

    while (position < source.size()) {
        const char ch = peek();
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            advance();
            continue;
        }
        if (ch == '#') {
            while (position < source.size() && peek() != '\n') {
                advance();
            }
            continue;
        }

        Token token;
        token.line = line;
        token.column = column;

        if (is_identifier_start(ch)) {
            const std::size_t start = position;
            advance();
            while (is_identifier_char(peek())) {
                advance();
            }
            token.text = std::string(source.substr(start, position - start));
            token.kind = keyword_kind(token.text);
            tokens.push_back(std::move(token));
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            const std::size_t start = position;
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                advance();
            }
            if (peek() == '.' &&
                std::isdigit(static_cast<unsigned char>(peek(1))) != 0) {
                advance();
                while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                    advance();
                }
            }
            if ((peek() == 'e' || peek() == 'E')) {
                advance();
                if (peek() == '+' || peek() == '-') {
                    advance();
                }
                while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                    advance();
                }
            }
            token.text = std::string(source.substr(start, position - start));
            const auto parsed = parse_double(token.text);
            if (!parsed.has_value()) {
                throw RuleError(token.line, token.column,
                                "invalid numeric literal '" + token.text + "'");
            }
            token.kind = TokenKind::Number;
            token.number = *parsed;
            tokens.push_back(std::move(token));
            continue;
        }

        if (ch == '"') {
            advance();
            std::string text;
            bool terminated = false;
            while (position < source.size()) {
                const char next = advance();
                if (next == '"') {
                    terminated = true;
                    break;
                }
                if (next == '\n') {
                    throw RuleError(token.line, token.column,
                                    "unterminated string literal");
                }
                if (next == '\\') {
                    if (position >= source.size()) {
                        break;
                    }
                    const char escaped = advance();
                    switch (escaped) {
                        case 'n':
                            text.push_back('\n');
                            break;
                        case 't':
                            text.push_back('\t');
                            break;
                        case 'r':
                            text.push_back('\r');
                            break;
                        case '\\':
                            text.push_back('\\');
                            break;
                        case '"':
                            text.push_back('"');
                            break;
                        default:
                            throw RuleError(token.line, token.column,
                                            "unsupported escape sequence '\\" +
                                                std::string(1, escaped) + "'");
                    }
                    continue;
                }
                text.push_back(next);
            }
            if (!terminated) {
                throw RuleError(token.line, token.column, "unterminated string literal");
            }
            token.kind = TokenKind::String;
            token.text = std::move(text);
            tokens.push_back(std::move(token));
            continue;
        }

        advance();
        switch (ch) {
            case '{':
                token.kind = TokenKind::LBrace;
                break;
            case '}':
                token.kind = TokenKind::RBrace;
                break;
            case '(':
                token.kind = TokenKind::LParen;
                break;
            case ')':
                token.kind = TokenKind::RParen;
                break;
            case '.':
                token.kind = TokenKind::Dot;
                break;
            case '+':
                token.kind = TokenKind::Plus;
                break;
            case '-':
                token.kind = TokenKind::Minus;
                break;
            case '*':
                token.kind = TokenKind::Star;
                break;
            case '/':
                token.kind = TokenKind::Slash;
                break;
            case ';':
                token.kind = TokenKind::Semicolon;
                break;
            case ',':
                token.kind = TokenKind::Comma;
                break;
            case '=':
                if (peek() == '=') {
                    advance();
                    token.kind = TokenKind::Equal;
                } else {
                    token.kind = TokenKind::Assign;
                }
                break;
            case '!':
                if (peek() == '=') {
                    advance();
                    token.kind = TokenKind::NotEqual;
                } else {
                    throw RuleError(token.line, token.column,
                                    "expected '!=' after '!'");
                }
                break;
            case '<':
                if (peek() == '=') {
                    advance();
                    token.kind = TokenKind::LessEqual;
                } else {
                    token.kind = TokenKind::Less;
                }
                break;
            case '>':
                if (peek() == '=') {
                    advance();
                    token.kind = TokenKind::GreaterEqual;
                } else {
                    token.kind = TokenKind::Greater;
                }
                break;
            default:
                throw RuleError(token.line, token.column,
                                "unexpected character '" + std::string(1, ch) + "'");
        }
        tokens.push_back(std::move(token));
    }

    Token end;
    end.kind = TokenKind::End;
    end.line = line;
    end.column = column;
    tokens.push_back(std::move(end));
    return tokens;
}

}  // namespace eg
