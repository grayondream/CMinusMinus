#include "frontend/OperatorPrecedence.h"

namespace cmm {

// Precedence levels (higher binds tighter), matching Parser::parseExpr and
// docs/spec/grammar.ebnf §9. Level 3 (?:) and the comma are parsed specially
// but carry their level here so the table stays complete.
OperatorInfo getOperatorInfo(TokenType op) {
    switch (op) {
        case TokenType::TOKEN_COMMA:        return {1, false};

        case TokenType::TOKEN_ASSIGN:
        case TokenType::TOKEN_PLUS_EQ:
        case TokenType::TOKEN_MINUS_EQ:
        case TokenType::TOKEN_STAR_EQ:
        case TokenType::TOKEN_SLASH_EQ:
        case TokenType::TOKEN_PERCENT_EQ:
        case TokenType::TOKEN_AMP_EQ:
        case TokenType::TOKEN_PIPE_EQ:
        case TokenType::TOKEN_CARET_EQ:
        case TokenType::TOKEN_LSHIFT_EQ:
        case TokenType::TOKEN_RSHIFT_EQ:    return {2, true};

        case TokenType::TOKEN_QUESTION:     return {3, true};

        case TokenType::TOKEN_OR:           return {4, false};
        case TokenType::TOKEN_AND:          return {5, false};
        case TokenType::TOKEN_BIT_OR:       return {6, false};
        case TokenType::TOKEN_CARET:        return {7, false};
        case TokenType::TOKEN_BIT_AND:      return {8, false};

        case TokenType::TOKEN_EQ:
        case TokenType::TOKEN_NOT_EQ:       return {9, false};

        case TokenType::TOKEN_LT:
        case TokenType::TOKEN_GT:
        case TokenType::TOKEN_LE:
        case TokenType::TOKEN_GE:           return {10, false};

        case TokenType::TOKEN_LSHIFT:
        case TokenType::TOKEN_RSHIFT:       return {11, false};

        case TokenType::TOKEN_PLUS:
        case TokenType::TOKEN_MINUS:        return {12, false};

        case TokenType::TOKEN_STAR:
        case TokenType::TOKEN_SLASH:
        case TokenType::TOKEN_PERCENT:      return {13, false};

        default:                            return {0, false};
    }
}

} // namespace cmm
