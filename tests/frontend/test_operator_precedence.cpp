// INF-10: binds the normative operator-precedence table (docs/spec/grammar.ebnf
// §9, implemented by cmm::getOperatorInfo) to the parser, both table-driven and
// behaviourally (by inspecting the AST shape the parser builds).
#include <gtest/gtest.h>

#include "ast/Expr.h"
#include "ast/Type.h"
#include "frontend/Lexer.h"
#include "frontend/OperatorPrecedence.h"
#include "frontend/Parser.h"

#include <spdlog/spdlog.h>

namespace {

std::vector<Token> lex(const std::string& source) {
    Lexer lexer("test.c", source);
    return lexer.tokenize();
}

// Keeps the parsed translation unit alive while a test inspects the returned
// ExprAST*, mirroring tests/frontend/test_parser.cpp.
std::unique_ptr<TranslationUnitAST> g_tu;

ExprAST* parseExpr(const std::string& source) {
    g_tu = nullptr;
    Parser parser(lex("int32 f() { return " + source + "; }"));
    g_tu = parser.parse();
    if (!g_tu || g_tu->declarations.empty()) return nullptr;
    auto* func = dynamic_cast<FunctionDeclAST*>(g_tu->declarations[0].get());
    if (!func || !func->body || func->body->stmts.empty()) return nullptr;
    auto* ret = dynamic_cast<ReturnStmtAST*>(func->body->stmts[0].get());
    return ret ? ret->value.get() : nullptr;
}

BinaryExprAST* asBinary(ExprAST* e) { return dynamic_cast<BinaryExprAST*>(e); }
AssignmentExprAST* asAssign(ExprAST* e) { return dynamic_cast<AssignmentExprAST*>(e); }

// ---------------------------------------------------------------------------
// 1. Normative table (docs/spec/grammar.ebnf §9)
// ---------------------------------------------------------------------------

struct OpCase {
    TokenType token;
    int precedence;
    bool rightAssociative;
    const char* name;
};

const std::vector<OpCase>& normativeTable() {
    static const std::vector<OpCase> table = {
        {TokenType::TOKEN_COMMA,      1, false, ","},
        {TokenType::TOKEN_ASSIGN,     2, true,  "="},
        {TokenType::TOKEN_PLUS_EQ,    2, true,  "+="},
        {TokenType::TOKEN_MINUS_EQ,   2, true,  "-="},
        {TokenType::TOKEN_STAR_EQ,    2, true,  "*="},
        {TokenType::TOKEN_SLASH_EQ,   2, true,  "/="},
        {TokenType::TOKEN_PERCENT_EQ, 2, true,  "%="},
        {TokenType::TOKEN_AMP_EQ,     2, true,  "&="},
        {TokenType::TOKEN_PIPE_EQ,    2, true,  "|="},
        {TokenType::TOKEN_CARET_EQ,   2, true,  "^="},
        {TokenType::TOKEN_LSHIFT_EQ,  2, true,  "<<="},
        {TokenType::TOKEN_RSHIFT_EQ,  2, true,  ">>="},
        {TokenType::TOKEN_QUESTION,   3, true,  "?:"},
        {TokenType::TOKEN_OR,         4, false, "||"},
        {TokenType::TOKEN_AND,        5, false, "&&"},
        {TokenType::TOKEN_BIT_OR,     6, false, "|"},
        {TokenType::TOKEN_CARET,      7, false, "^"},
        {TokenType::TOKEN_BIT_AND,    8, false, "&"},
        {TokenType::TOKEN_EQ,         9, false, "=="},
        {TokenType::TOKEN_NOT_EQ,     9, false, "!="},
        {TokenType::TOKEN_LT,        10, false, "<"},
        {TokenType::TOKEN_GT,        10, false, ">"},
        {TokenType::TOKEN_LE,        10, false, "<="},
        {TokenType::TOKEN_GE,        10, false, ">="},
        {TokenType::TOKEN_LSHIFT,    11, false, "<<"},
        {TokenType::TOKEN_RSHIFT,    11, false, ">>"},
        {TokenType::TOKEN_PLUS,      12, false, "+"},
        {TokenType::TOKEN_MINUS,     12, false, "-"},
        {TokenType::TOKEN_STAR,      13, false, "*"},
        {TokenType::TOKEN_SLASH,     13, false, "/"},
        {TokenType::TOKEN_PERCENT,   13, false, "%"},
    };
    return table;
}

class OperatorPrecedenceTest : public ::testing::Test {
protected:
    void SetUp() override { spdlog::set_level(spdlog::level::off); }
};

TEST_F(OperatorPrecedenceTest, TableMatchesNormativeSpec) {
    for (const auto& c : normativeTable()) {
        auto info = cmm::getOperatorInfo(c.token);
        EXPECT_EQ(info.precedence, c.precedence) << "precedence of " << c.name;
        EXPECT_EQ(info.rightAssociative, c.rightAssociative)
            << "associativity of " << c.name;
    }
}

TEST_F(OperatorPrecedenceTest, NonInfixTokensHaveZeroPrecedence) {
    EXPECT_EQ(cmm::getOperatorInfo(TokenType::TOKEN_PLUS_PLUS).precedence, 0);
    EXPECT_EQ(cmm::getOperatorInfo(TokenType::TOKEN_LPAREN).precedence, 0);
    EXPECT_EQ(cmm::getOperatorInfo(TokenType::TOKEN_IDENTIFIER).precedence, 0);
}

TEST_F(OperatorPrecedenceTest, CommaIsLowestPrecedenceAndLeftAssociative) {
    // `a = b, c = d` groups as `(a = b), (c = d)`.
    auto* comma = dynamic_cast<CommaExprAST*>(parseExpr("a = b, c = d"));
    ASSERT_NE(comma, nullptr);
    EXPECT_NE(asAssign(comma->left.get()), nullptr);
    EXPECT_NE(asAssign(comma->right.get()), nullptr);

    // `a, b, c` groups as `(a, b), c`.
    auto* outer = dynamic_cast<CommaExprAST*>(parseExpr("a, b, c"));
    ASSERT_NE(outer, nullptr);
    EXPECT_NE(dynamic_cast<CommaExprAST*>(outer->left.get()), nullptr);
}

TEST_F(OperatorPrecedenceTest, CommaServesAsSeparatorInCallArguments) {
    // Inside `f(...)`, `,` separates arguments rather than forming a comma expr.
    auto* call = dynamic_cast<CallExprAST*>(parseExpr("f(a, b)"));
    ASSERT_NE(call, nullptr);
    ASSERT_EQ(call->args.size(), 2u);
    EXPECT_EQ(dynamic_cast<CommaExprAST*>(call->args[0].get()), nullptr);
    EXPECT_EQ(dynamic_cast<CommaExprAST*>(call->args[1].get()), nullptr);
}

// ---------------------------------------------------------------------------
// 2. Behavioural: adjacent levels bind in the right direction
// ---------------------------------------------------------------------------

TEST_F(OperatorPrecedenceTest, AdjacentLevels) {
    struct Case {
        const char* source;
        BinaryOp outer;
        BinaryOp inner;
    };

    const std::vector<Case> cases = {
        {"a || b && c", BinaryOp::Or,     BinaryOp::And},
        {"a && b | c",  BinaryOp::And,    BinaryOp::BitOr},
        {"a | b ^ c",   BinaryOp::BitOr,  BinaryOp::BitXor},
        {"a ^ b & c",   BinaryOp::BitXor, BinaryOp::BitAnd},
        {"a & b == c",  BinaryOp::BitAnd, BinaryOp::Eq},
        {"a == b < c",  BinaryOp::Eq,     BinaryOp::Lt},
        {"a < b << c",  BinaryOp::Lt,     BinaryOp::LShift},
        {"a << b + c",  BinaryOp::LShift, BinaryOp::Add},
        {"a + b * c",   BinaryOp::Add,    BinaryOp::Mul},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.source);
        auto* outer = asBinary(parseExpr(c.source));
        ASSERT_NE(outer, nullptr);
        EXPECT_EQ(outer->op, c.outer);
        // lower precedence is the outer node; the tighter operator is on the right
        auto* inner = asBinary(outer->right.get());
        ASSERT_NE(inner, nullptr);
        EXPECT_EQ(inner->op, c.inner);
    }
}

// ---------------------------------------------------------------------------
// 3. Associativity
// ---------------------------------------------------------------------------

TEST_F(OperatorPrecedenceTest, ArithmeticIsLeftAssociative) {
    auto* outer = asBinary(parseExpr("a - b - c"));
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op, BinaryOp::Sub);
    auto* inner = asBinary(outer->left.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op, BinaryOp::Sub);
    // Right operand is the plain identifier `c`; if it were a nested Sub the
    // expression would be right-associative instead.
    ASSERT_NE(outer->right.get(), nullptr);
    EXPECT_EQ(asBinary(outer->right.get()), nullptr);
}

TEST_F(OperatorPrecedenceTest, AssignmentIsRightAssociative) {
    auto* outer = asAssign(parseExpr("a = b = c"));
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op, AssignOp::Assign);
    auto* inner = asAssign(outer->rhs.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op, AssignOp::Assign);
}

TEST_F(OperatorPrecedenceTest, CompoundAssignmentIsRightAssociative) {
    auto* outer = asAssign(parseExpr("a += b -= c"));
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op, AssignOp::AddAssign);
    auto* inner = asAssign(outer->rhs.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op, AssignOp::SubAssign);
}

// ---------------------------------------------------------------------------
// 4. Ternary, unary/postfix, parentheses
// ---------------------------------------------------------------------------

TEST_F(OperatorPrecedenceTest, TernaryBindsLooserThanLogicalOr) {
    auto* tern = dynamic_cast<TernaryExprAST*>(parseExpr("a || b ? c : d"));
    ASSERT_NE(tern, nullptr);
    auto* cond = asBinary(tern->cond.get());
    ASSERT_NE(cond, nullptr);
    EXPECT_EQ(cond->op, BinaryOp::Or);
}

TEST_F(OperatorPrecedenceTest, AssignmentBindsLooserThanTernary) {
    auto* assign = asAssign(parseExpr("a = b ? c : d"));
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->op, AssignOp::Assign);
    EXPECT_NE(dynamic_cast<TernaryExprAST*>(assign->rhs.get()), nullptr);
}

TEST_F(OperatorPrecedenceTest, UnaryBindsTighterThanMultiply) {
    auto* mul = asBinary(parseExpr("-a * b"));
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, BinaryOp::Mul);
    auto* unary = dynamic_cast<UnaryExprAST*>(mul->left.get());
    ASSERT_NE(unary, nullptr);
    EXPECT_EQ(unary->op, UnaryOp::Minus);
}

TEST_F(OperatorPrecedenceTest, ParenthesesOverridePrecedence) {
    auto* mul = asBinary(parseExpr("(a + b) * c"));
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, BinaryOp::Mul);
    auto* add = asBinary(mul->left.get());
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::Add);
}

} // namespace
