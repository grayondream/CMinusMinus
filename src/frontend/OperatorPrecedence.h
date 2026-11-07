#pragma once

#include "frontend/Token.h"

namespace cmm {

// Normative infix-operator precedence table.
//
// This is the single source of truth that mirrors the table in
// docs/spec/grammar.ebnf §9; the parser delegates to it and the unit tests in
// tests/frontend/test_operator_precedence.cpp pin it down.
//
// `precedence == 0` means the token is not an infix operator.
struct OperatorInfo {
    int precedence;
    bool rightAssociative;
};

OperatorInfo getOperatorInfo(TokenType op);

} // namespace cmm
