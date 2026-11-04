#include <gtest/gtest.h>
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "ast/Expr.h"
#include "ast/Symbol.h"
#include "ast/Type.h"
#include <spdlog/spdlog.h>

class ParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        spdlog::set_level(spdlog::level::off);
    }
};

class LexerTest : public ::testing::Test {
protected:
    void SetUp() override {
        spdlog::set_level(spdlog::level::off);
    }
};

static std::vector<Token> lex(const std::string& source) {
    Lexer lexer("test.c", source);
    return lexer.tokenize();
}

static auto parse(const std::string& source) {
    auto tokens = lex(source);
    Parser parser(tokens);
    return parser.parse();
}

static std::vector<Diagnostic> parseErrors(const std::string& source) {
    Lexer lexer("alias_err.c", source);
    Parser parser(lexer.tokenize());
    parser.parse();
    return parser.getErrors();
}

static std::unique_ptr<TranslationUnitAST> s_lastTU;

static auto parseExpr(const std::string& source) {
    std::string wrapped = "int32 f() { return " + source + "; }";
    s_lastTU = parse(wrapped);
    if (!s_lastTU || s_lastTU->declarations.empty()) return (ExprAST*)nullptr;
    auto func = dynamic_cast<FunctionDeclAST*>(s_lastTU->declarations[0].get());
    if (!func || !func->body || func->body->stmts.empty()) return (ExprAST*)nullptr;
    auto ret = dynamic_cast<ReturnStmtAST*>(func->body->stmts[0].get());
    if (!ret) return (ExprAST*)nullptr;
    return ret->value.get();
}

// ========== Number Literals ==========

TEST_F(ParserTest, NumberLiteral) {
    auto expr = parseExpr("42");
    ASSERT_NE(expr, nullptr);
    auto num = dynamic_cast<NumberExprAST*>(expr);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(num->value, 42);
}

TEST_F(ParserTest, ZeroLiteral) {
    auto expr = parseExpr("0");
    ASSERT_NE(expr, nullptr);
    auto num = dynamic_cast<NumberExprAST*>(expr);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(num->value, 0);
}

// LEX-15: the parser must carry the literal's suffix/default kind into the AST
// so later stages can pick the fixed-width type instead of a fixed int/i32.
TEST_F(ParserTest, LiteralKindIsCarriedIntoAst) {
    auto n = dynamic_cast<NumberExprAST*>(parseExpr("42u"));
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->literalKind, LiteralKind::UInt);

    auto l = dynamic_cast<NumberExprAST*>(parseExpr("42l"));
    ASSERT_NE(l, nullptr);
    EXPECT_EQ(l->literalKind, LiteralKind::Long);

    auto i = dynamic_cast<NumberExprAST*>(parseExpr("42"));
    ASSERT_NE(i, nullptr);
    EXPECT_EQ(i->literalKind, LiteralKind::Int);

    auto f = dynamic_cast<FloatExprAST*>(parseExpr("1.5f"));
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->literalKind, LiteralKind::Float32);

    auto d = dynamic_cast<FloatExprAST*>(parseExpr("1.5"));
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->literalKind, LiteralKind::Float64);
}

// ========== Identifiers ==========

TEST_F(ParserTest, Identifier) {
    auto expr = parseExpr("x");
    ASSERT_NE(expr, nullptr);
    auto var = dynamic_cast<VariableExprAST*>(expr);
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "x");
}

// ========== Binary Operators - Precedence ==========

TEST_F(ParserTest, Addition) {
    auto expr = parseExpr("1 + 2");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::Add);
    EXPECT_NE(dynamic_cast<NumberExprAST*>(bin->left.get()), nullptr);
    EXPECT_NE(dynamic_cast<NumberExprAST*>(bin->right.get()), nullptr);
}

TEST_F(ParserTest, Subtraction) {
    auto expr = parseExpr("5 - 3");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::Sub);
}

TEST_F(ParserTest, Multiplication) {
    auto expr = parseExpr("2 * 3");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::Mul);
}

TEST_F(ParserTest, Division) {
    auto expr = parseExpr("10 / 2");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::Div);
}

TEST_F(ParserTest, Modulo) {
    auto expr = parseExpr("7 % 3");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::Mod);
}

TEST_F(ParserTest, PrecedenceMulBeforeAdd) {
    // 1 + 2 * 3 should parse as 1 + (2 * 3)
    auto expr = parseExpr("1 + 2 * 3");
    ASSERT_NE(expr, nullptr);
    auto add = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::Add);

    auto rhs = dynamic_cast<BinaryExprAST*>(add->right.get());
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(rhs->op, BinaryOp::Mul);
}

TEST_F(ParserTest, PrecedenceAddBeforeShift) {
    // 1 << 2 + 3 should parse as 1 << (2 + 3) since + has higher prec than <<
    auto expr = parseExpr("1 << 2 + 3");
    ASSERT_NE(expr, nullptr);
    auto shift = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(shift, nullptr);
    EXPECT_EQ(shift->op, BinaryOp::LShift);

    auto rhs = dynamic_cast<BinaryExprAST*>(shift->right.get());
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(rhs->op, BinaryOp::Add);
}

TEST_F(ParserTest, PrecedenceShiftBeforeRelational) {
    // 1 < 2 << 3 should parse as 1 < (2 << 3) since << has higher prec than <
    auto expr = parseExpr("1 < 2 << 3");
    ASSERT_NE(expr, nullptr);
    auto lt = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(lt, nullptr);
    EXPECT_EQ(lt->op, BinaryOp::Lt);

    auto rhs = dynamic_cast<BinaryExprAST*>(lt->right.get());
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(rhs->op, BinaryOp::LShift);
}

TEST_F(ParserTest, ChainedAddition) {
    // 1 + 2 + 3 should parse as (1 + 2) + 3 (left-to-right)
    auto expr = parseExpr("1 + 2 + 3");
    ASSERT_NE(expr, nullptr);
    auto outer = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op, BinaryOp::Add);

    auto lhs = dynamic_cast<BinaryExprAST*>(outer->left.get());
    ASSERT_NE(lhs, nullptr);
    EXPECT_EQ(lhs->op, BinaryOp::Add);
}

TEST_F(ParserTest, SubtractionLeftAssoc) {
    // 5 - 3 - 1 should parse as (5 - 3) - 1
    auto expr = parseExpr("5 - 3 - 1");
    ASSERT_NE(expr, nullptr);
    auto outer = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op, BinaryOp::Sub);

    auto lhs = dynamic_cast<BinaryExprAST*>(outer->left.get());
    ASSERT_NE(lhs, nullptr);
    EXPECT_EQ(lhs->op, BinaryOp::Sub);
}

// ========== Relational Operators ==========

TEST_F(ParserTest, LessThan) {
    auto expr = parseExpr("1 < 2");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::Lt);
}

TEST_F(ParserTest, GreaterThan) {
    auto expr = parseExpr("2 > 1");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::Gt);
}

TEST_F(ParserTest, LessEqual) {
    auto expr = parseExpr("1 <= 2");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::Le);
}

TEST_F(ParserTest, GreaterEqual) {
    auto expr = parseExpr("2 >= 1");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::Ge);
}

// ========== Equality Operators ==========

TEST_F(ParserTest, Equal) {
    auto expr = parseExpr("1 == 2");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::Eq);
}

TEST_F(ParserTest, NotEqual) {
    auto expr = parseExpr("1 != 2");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::NotEq);
}

// ========== Logical Operators ==========

TEST_F(ParserTest, LogicalAnd) {
    auto expr = parseExpr("1 && 2");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::And);
}

TEST_F(ParserTest, LogicalOr) {
    auto expr = parseExpr("1 || 2");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::Or);
}

TEST_F(ParserTest, PrecedenceAndBeforeOr) {
    // 1 || 2 && 3 should parse as 1 || (2 && 3)
    auto expr = parseExpr("1 || 2 && 3");
    ASSERT_NE(expr, nullptr);
    auto or_bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(or_bin, nullptr);
    EXPECT_EQ(or_bin->op, BinaryOp::Or);

    auto rhs = dynamic_cast<BinaryExprAST*>(or_bin->right.get());
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(rhs->op, BinaryOp::And);
}

// ========== Bitwise Operators ==========

TEST_F(ParserTest, BitwiseAnd) {
    auto expr = parseExpr("1 & 2");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::BitAnd);
}

TEST_F(ParserTest, BitwiseOr) {
    auto expr = parseExpr("1 | 2");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::BitOr);
}

TEST_F(ParserTest, BitwiseXor) {
    auto expr = parseExpr("1 ^ 2");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::BitXor);
}

// ========== Shift Operators ==========

TEST_F(ParserTest, LeftShift) {
    auto expr = parseExpr("1 << 2");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::LShift);
}

TEST_F(ParserTest, RightShift) {
    auto expr = parseExpr("4 >> 2");
    ASSERT_NE(expr, nullptr);
    auto bin = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, BinaryOp::RShift);
}

// ========== Unary Operators ==========

TEST_F(ParserTest, UnaryNegate) {
    auto expr = parseExpr("-1");
    ASSERT_NE(expr, nullptr);
    auto unary = dynamic_cast<UnaryExprAST*>(expr);
    ASSERT_NE(unary, nullptr);
    EXPECT_EQ(unary->op, UnaryOp::Minus);
}

TEST_F(ParserTest, UnaryPlus) {
    auto expr = parseExpr("+1");
    ASSERT_NE(expr, nullptr);
    auto unary = dynamic_cast<UnaryExprAST*>(expr);
    ASSERT_NE(unary, nullptr);
    EXPECT_EQ(unary->op, UnaryOp::Plus);
}

TEST_F(ParserTest, UnaryNot) {
    auto expr = parseExpr("!1");
    ASSERT_NE(expr, nullptr);
    auto unary = dynamic_cast<UnaryExprAST*>(expr);
    ASSERT_NE(unary, nullptr);
    EXPECT_EQ(unary->op, UnaryOp::Not);
}

TEST_F(ParserTest, UnaryBitNot) {
    auto expr = parseExpr("~1");
    ASSERT_NE(expr, nullptr);
    auto unary = dynamic_cast<UnaryExprAST*>(expr);
    ASSERT_NE(unary, nullptr);
    EXPECT_EQ(unary->op, UnaryOp::BitNot);
}

TEST_F(ParserTest, UnaryDeref) {
    auto expr = parseExpr("*x");
    ASSERT_NE(expr, nullptr);
    auto unary = dynamic_cast<UnaryExprAST*>(expr);
    ASSERT_NE(unary, nullptr);
    EXPECT_EQ(unary->op, UnaryOp::Deref);
}

TEST_F(ParserTest, UnaryAddressOf) {
    auto expr = parseExpr("&x");
    ASSERT_NE(expr, nullptr);
    auto unary = dynamic_cast<UnaryExprAST*>(expr);
    ASSERT_NE(unary, nullptr);
    EXPECT_EQ(unary->op, UnaryOp::AddressOf);
}

TEST_F(ParserTest, DoubleNegate) {
    // --1 is lexed as TOKEN_MINUS_MINUS (prefix decrement), not two minuses
    auto expr = parseExpr("--1");
    ASSERT_NE(expr, nullptr);
    auto unary = dynamic_cast<UnaryExprAST*>(expr);
    ASSERT_NE(unary, nullptr);
    EXPECT_EQ(unary->op, UnaryOp::PreDec);
}

// ========== Postfix Operators ==========

TEST_F(ParserTest, PostfixIncrement) {
    auto expr = parseExpr("x++");
    ASSERT_NE(expr, nullptr);
    auto postfix = dynamic_cast<PostfixIncDecExprAST*>(expr);
    ASSERT_NE(postfix, nullptr);
    EXPECT_TRUE(postfix->isIncrement);
}

TEST_F(ParserTest, PostfixDecrement) {
    auto expr = parseExpr("x--");
    ASSERT_NE(expr, nullptr);
    auto postfix = dynamic_cast<PostfixIncDecExprAST*>(expr);
    ASSERT_NE(postfix, nullptr);
    EXPECT_FALSE(postfix->isIncrement);
}

TEST_F(ParserTest, ArrayAccess) {
    auto expr = parseExpr("arr[0]");
    ASSERT_NE(expr, nullptr);
    auto arr = dynamic_cast<ArrayAccessExprAST*>(expr);
    ASSERT_NE(arr, nullptr);
    EXPECT_NE(dynamic_cast<VariableExprAST*>(arr->array.get()), nullptr);
    EXPECT_NE(dynamic_cast<NumberExprAST*>(arr->index.get()), nullptr);
}

TEST_F(ParserTest, MemberAccessDot) {
    auto expr = parseExpr("s.field");
    ASSERT_NE(expr, nullptr);
    auto member = dynamic_cast<MemberAccessExprAST*>(expr);
    ASSERT_NE(member, nullptr);
    EXPECT_EQ(member->accessKind, MemberAccessKind::Dot);
    EXPECT_EQ(member->memberName, "field");
}

TEST_F(ParserTest, MemberAccessArrow) {
    auto expr = parseExpr("p->field");
    ASSERT_NE(expr, nullptr);
    auto member = dynamic_cast<MemberAccessExprAST*>(expr);
    ASSERT_NE(member, nullptr);
    EXPECT_EQ(member->accessKind, MemberAccessKind::Arrow);
    EXPECT_EQ(member->memberName, "field");
}

// ========== Function Calls ==========

TEST_F(ParserTest, FunctionCallNoArgs) {
    auto expr = parseExpr("foo()");
    ASSERT_NE(expr, nullptr);
    auto call = dynamic_cast<CallExprAST*>(expr);
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->callee, "foo");
    EXPECT_EQ(call->args.size(), 0u);
}

TEST_F(ParserTest, FunctionCallOneArg) {
    auto expr = parseExpr("foo(1)");
    ASSERT_NE(expr, nullptr);
    auto call = dynamic_cast<CallExprAST*>(expr);
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->callee, "foo");
    EXPECT_EQ(call->args.size(), 1u);
}

TEST_F(ParserTest, FunctionCallMultipleArgs) {
    auto expr = parseExpr("foo(1, 2, 3)");
    ASSERT_NE(expr, nullptr);
    auto call = dynamic_cast<CallExprAST*>(expr);
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->callee, "foo");
    EXPECT_EQ(call->args.size(), 3u);
}

// ========== Parenthesized Expressions ==========

TEST_F(ParserTest, ParenthesizedExpr) {
    auto expr = parseExpr("(42)");
    ASSERT_NE(expr, nullptr);
    auto num = dynamic_cast<NumberExprAST*>(expr);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(num->value, 42);
}

TEST_F(ParserTest, ParenthesizedOverridesPrecedence) {
    auto expr = parseExpr("(1 + 2) * 3");
    ASSERT_NE(expr, nullptr);
    auto mul = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, BinaryOp::Mul);

    auto lhs = dynamic_cast<BinaryExprAST*>(mul->left.get());
    ASSERT_NE(lhs, nullptr);
    EXPECT_EQ(lhs->op, BinaryOp::Add);
}

// ========== Assignment Operators ==========

TEST_F(ParserTest, SimpleAssignment) {
    auto expr = parseExpr("x = 1");
    ASSERT_NE(expr, nullptr);
    auto assign = dynamic_cast<AssignmentExprAST*>(expr);
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->op, AssignOp::Assign);
    EXPECT_NE(dynamic_cast<VariableExprAST*>(assign->lhs.get()), nullptr);
    EXPECT_NE(dynamic_cast<NumberExprAST*>(assign->rhs.get()), nullptr);
}

TEST_F(ParserTest, AddAssignment) {
    auto expr = parseExpr("x += 1");
    ASSERT_NE(expr, nullptr);
    auto assign = dynamic_cast<AssignmentExprAST*>(expr);
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->op, AssignOp::AddAssign);
}

TEST_F(ParserTest, SubAssignment) {
    auto expr = parseExpr("x -= 1");
    ASSERT_NE(expr, nullptr);
    auto assign = dynamic_cast<AssignmentExprAST*>(expr);
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->op, AssignOp::SubAssign);
}

TEST_F(ParserTest, MulAssignment) {
    auto expr = parseExpr("x *= 2");
    ASSERT_NE(expr, nullptr);
    auto assign = dynamic_cast<AssignmentExprAST*>(expr);
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->op, AssignOp::MulAssign);
}

TEST_F(ParserTest, DivAssignment) {
    auto expr = parseExpr("x /= 2");
    ASSERT_NE(expr, nullptr);
    auto assign = dynamic_cast<AssignmentExprAST*>(expr);
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->op, AssignOp::DivAssign);
}

TEST_F(ParserTest, AssignmentRightAssoc) {
    // x = y = 1 should parse as x = (y = 1)
    auto expr = parseExpr("x = y = 1");
    ASSERT_NE(expr, nullptr);
    auto outer = dynamic_cast<AssignmentExprAST*>(expr);
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op, AssignOp::Assign);

    auto inner = dynamic_cast<AssignmentExprAST*>(outer->rhs.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op, AssignOp::Assign);
}

// ========== Comma Operator ==========

TEST_F(ParserTest, CommaExpr) {
    // Comma is handled at statement level, not in parseExpr
    // This test verifies that parseExpr stops at comma
    auto expr = parseExpr("1");
    ASSERT_NE(expr, nullptr);
    auto num = dynamic_cast<NumberExprAST*>(expr);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(num->value, 1);
}

// ========== Ternary Operator ==========

TEST_F(ParserTest, TernaryExpr) {
    auto expr = parseExpr("1 ? 2 : 3");
    ASSERT_NE(expr, nullptr);
    auto ternary = dynamic_cast<TernaryExprAST*>(expr);
    ASSERT_NE(ternary, nullptr);
    EXPECT_NE(dynamic_cast<NumberExprAST*>(ternary->cond.get()), nullptr);
    EXPECT_NE(dynamic_cast<NumberExprAST*>(ternary->then.get()), nullptr);
    EXPECT_NE(dynamic_cast<NumberExprAST*>(ternary->elseExpr.get()), nullptr);
}

// ========== Cast Expressions ==========

TEST_F(ParserTest, CastToInt) {
    auto expr = parseExpr("(int32)1");
    ASSERT_NE(expr, nullptr);
    auto cast = dynamic_cast<CastExprAST*>(expr);
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->castType->kind, TypeKind::Int32);
}

TEST_F(ParserTest, CastToFloat) {
    auto expr = parseExpr("(float32)1");
    ASSERT_NE(expr, nullptr);
    auto cast = dynamic_cast<CastExprAST*>(expr);
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->castType->kind, TypeKind::Float32);
}

// ========== Sizeof ==========

TEST_F(LexerTest, FloatSuffixIsPartOfTheLiteral) {
    auto tokens = lex("3.14f 2.0F 1.5L");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_FLOAT);
    EXPECT_EQ(tokens[1].type, TokenType::TOKEN_FLOAT);
    EXPECT_EQ(tokens[2].type, TokenType::TOKEN_FLOAT);
    EXPECT_NEAR(std::get<double>(tokens[0].value), 3.14, 1e-9);
}

TEST_F(ParserTest, MultipleDeclarators) {
    auto tu = parse("int32 a = 1, b = 2, c = 3;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1);
    auto* multi = dynamic_cast<MultiVarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(multi, nullptr);
    EXPECT_EQ(multi->decls.size(), 3);
}

TEST_F(ParserTest, SingleDeclaratorIsNotWrapped) {
    auto tu = parse("int32 a = 1;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1);
    EXPECT_NE(dynamic_cast<VarDeclAST*>(tu->declarations[0].get()), nullptr);
}

TEST_F(ParserTest, SizeofType) {
    auto expr = parseExpr("sizeof(int32)");
    ASSERT_NE(expr, nullptr);
    auto sz = dynamic_cast<SizeofExprAST*>(expr);
    ASSERT_NE(sz, nullptr);
    EXPECT_EQ(sz->sizeofType->kind, TypeKind::Int32);
}

TEST_F(ParserTest, SizeofExpr) {
    auto expr = parseExpr("sizeof x");
    ASSERT_NE(expr, nullptr);
    auto sz = dynamic_cast<SizeofExprAST*>(expr);
    ASSERT_NE(sz, nullptr);
    EXPECT_EQ(sz->sizeofType, nullptr);
    EXPECT_NE(sz->expr, nullptr);
}

TEST_F(ParserTest, SizeofExprWithParens) {
    auto expr = parseExpr("sizeof(x)");
    ASSERT_NE(expr, nullptr);
    auto sz = dynamic_cast<SizeofExprAST*>(expr);
    ASSERT_NE(sz, nullptr);
    EXPECT_EQ(sz->sizeofType, nullptr);
    EXPECT_NE(sz->expr, nullptr);
}

TEST_F(ParserTest, SizeofArithmeticExprWithParens) {
    auto expr = parseExpr("sizeof(1 + 2)");
    ASSERT_NE(expr, nullptr);
    auto sz = dynamic_cast<SizeofExprAST*>(expr);
    ASSERT_NE(sz, nullptr);
    EXPECT_EQ(sz->sizeofType, nullptr);
    EXPECT_NE(sz->expr, nullptr);
}

// ========== Complex Expressions ==========

TEST_F(ParserTest, ComplexExpr1) {
    // 1 + 2 * 3 - 4 / 2
    auto expr = parseExpr("1 + 2 * 3 - 4 / 2");
    ASSERT_NE(expr, nullptr);
    // Should parse as: (1 + (2 * 3)) - (4 / 2)
    auto sub = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(sub, nullptr);
    EXPECT_EQ(sub->op, BinaryOp::Sub);
}

TEST_F(ParserTest, ComplexExpr2) {
    // (a + b) * (c - d)
    auto expr = parseExpr("(a + b) * (c - d)");
    ASSERT_NE(expr, nullptr);
    auto mul = dynamic_cast<BinaryExprAST*>(expr);
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, BinaryOp::Mul);
}

TEST_F(ParserTest, NestedFunctionCall) {
    // foo(bar(1))
    auto expr = parseExpr("foo(bar(1))");
    ASSERT_NE(expr, nullptr);
    auto outer = dynamic_cast<CallExprAST*>(expr);
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->callee, "foo");
    EXPECT_EQ(outer->args.size(), 1u);
    auto inner = dynamic_cast<CallExprAST*>(outer->args[0].get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->callee, "bar");
}

// ========== Function Overloading ==========

TEST_F(ParserTest, FunctionOverloading) {
    auto tu = parse(R"(
        int32 add_int_int(int32 a, int32 b) { return a + b; }
        float32 add_float_float(float32 a, float32 b) { return a + b; }
    )");
    ASSERT_NE(tu, nullptr);
    EXPECT_EQ(tu->declarations.size(), 2);
    
    auto* func1 = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    auto* func2 = dynamic_cast<FunctionDeclAST*>(tu->declarations[1].get());
    ASSERT_NE(func1, nullptr);
    ASSERT_NE(func2, nullptr);
    EXPECT_EQ(func1->name, "add_int_int");
    EXPECT_EQ(func2->name, "add_float_float");
}

TEST_F(ParserTest, OperatorOverloading) {
    auto tu = parse(R"(
        struct Point { int32 x; int32 y; };
        struct Point operator+(struct Point a, struct Point b) {
            struct Point result;
            result.x = a.x + b.x;
            result.y = a.y + b.y;
            return result;
        }
    )");
    ASSERT_NE(tu, nullptr);
    EXPECT_GE(tu->declarations.size(), 2);
    
    // Find the operator function
    for (auto& decl : tu->declarations) {
        if (auto* func = dynamic_cast<FunctionDeclAST*>(decl.get())) {
            if (func->name.find("operator") != std::string::npos) {
                EXPECT_EQ(func->name, "operator+");
                EXPECT_EQ(func->params.size(), 2);
                return;
            }
        }
    }
    FAIL() << "operator+ function not found";
}

// ========== Lexer Token Tests ==========

TEST_F(LexerTest, TokenizeDoubleEqual) {
    auto tokens = lex("==");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_EQ);
}

TEST_F(LexerTest, TokenizeNotEqual) {
    auto tokens = lex("!=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_NOT_EQ);
}

TEST_F(LexerTest, TokenizeLessEqual) {
    auto tokens = lex("<=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_LE);
}

TEST_F(LexerTest, TokenizeGreaterEqual) {
    auto tokens = lex(">=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_GE);
}

TEST_F(LexerTest, TokenizePlusPlus) {
    auto tokens = lex("++");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_PLUS_PLUS);
}

TEST_F(LexerTest, TokenizeMinusMinus) {
    auto tokens = lex("--");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_MINUS_MINUS);
}

TEST_F(LexerTest, TokenizeShiftLeft) {
    auto tokens = lex("<<");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_LSHIFT);
}

TEST_F(LexerTest, TokenizeShiftRight) {
    auto tokens = lex(">>");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_RSHIFT);
}

TEST_F(LexerTest, TokenizeCaret) {
    auto tokens = lex("^");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_CARET);
}

TEST_F(LexerTest, TokenizeTilde) {
    auto tokens = lex("~");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_TILDE);
}

TEST_F(LexerTest, TokenizeQuestion) {
    auto tokens = lex("?");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_QUESTION);
}

TEST_F(LexerTest, TokenizeColon) {
    auto tokens = lex(":");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_COLON);
}

TEST_F(LexerTest, TokenizeArrow) {
    auto tokens = lex("->");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_ARROW);
}

TEST_F(LexerTest, TokenizeOperatorKeyword) {
    auto tokens = lex("operator+");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_OPERATOR);
    EXPECT_EQ(tokens[0].lexeme, "operator");
    EXPECT_EQ(tokens[1].type, TokenType::TOKEN_PLUS);
}

TEST_F(LexerTest, TokenizeAssign) {
    auto tokens = lex("=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_ASSIGN);
}

TEST_F(LexerTest, TokenizePlusEqual) {
    auto tokens = lex("+=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_PLUS_EQ);
}

TEST_F(LexerTest, TokenizeMinusEqual) {
    auto tokens = lex("-=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_MINUS_EQ);
}

TEST_F(LexerTest, TokenizeStarEqual) {
    auto tokens = lex("*=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_STAR_EQ);
}

TEST_F(LexerTest, TokenizeSlashEqual) {
    auto tokens = lex("/=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_SLASH_EQ);
}

TEST_F(LexerTest, TokenizePercentEqual) {
    auto tokens = lex("%=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_PERCENT_EQ);
}

TEST_F(LexerTest, TokenizeAmpEqual) {
    auto tokens = lex("&=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_AMP_EQ);
}

TEST_F(LexerTest, TokenizePipeEqual) {
    auto tokens = lex("|=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_PIPE_EQ);
}

TEST_F(LexerTest, TokenizeCaretEqual) {
    auto tokens = lex("^=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_CARET_EQ);
}

TEST_F(LexerTest, TokenizeShiftLeftEqual) {
    auto tokens = lex("<<=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_LSHIFT_EQ);
}

TEST_F(LexerTest, TokenizeShiftRightEqual) {
    auto tokens = lex(">>=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TOKEN_RSHIFT_EQ);
}

TEST_F(LexerTest, RecognizesConstexpr) {
    Lexer lexer("test.c", "constexpr");
    Token tok = lexer.nextToken();
    EXPECT_EQ(tok.type, TokenType::TOKEN_CONSTEXPR);
    EXPECT_EQ(tok.lexeme, "constexpr");
}

// ========== Statement Parser Tests ==========

class ParserStmtTest : public ::testing::Test {
protected:
    void SetUp() override {
        spdlog::set_level(spdlog::level::off);
    }

    static std::vector<Token> lex(const std::string& source) {
        Lexer lexer("test.c", source);
        return lexer.tokenize();
    }

    static auto parse(const std::string& source) {
        auto tokens = lex(source);
        Parser parser(tokens);
        return parser.parse();
    }

    static std::unique_ptr<TranslationUnitAST> s_lastTU;

    static auto parseStmt(const std::string& source) {
        s_lastTU = parse(source);
        if (!s_lastTU || s_lastTU->declarations.empty()) return (StmtAST*)nullptr;
        auto func = dynamic_cast<FunctionDeclAST*>(s_lastTU->declarations[0].get());
        if (!func || !func->body || func->body->stmts.empty()) return (StmtAST*)nullptr;
        return func->body->stmts[0].get();
    }
};

std::unique_ptr<TranslationUnitAST> ParserStmtTest::s_lastTU;

TEST_F(ParserStmtTest, ReturnStatement) {
    auto stmt = parseStmt("int32 f() { return 42; }");
    ASSERT_NE(stmt, nullptr);
    auto ret = dynamic_cast<ReturnStmtAST*>(stmt);
    ASSERT_NE(ret, nullptr);
    EXPECT_NE(ret->value, nullptr);
}

TEST_F(ParserStmtTest, IfStatement) {
    auto stmt = parseStmt("int32 f() { if (1) { } }");
    ASSERT_NE(stmt, nullptr);
    auto ifStmt = dynamic_cast<IfStmtAST*>(stmt);
    ASSERT_NE(ifStmt, nullptr);
    EXPECT_NE(ifStmt->cond, nullptr);
    EXPECT_NE(ifStmt->thenStmt, nullptr);
    EXPECT_EQ(ifStmt->elseStmt, nullptr);
}

TEST_F(ParserStmtTest, IfElseStatement) {
    auto stmt = parseStmt("int32 f() { if (1) { } else { } }");
    ASSERT_NE(stmt, nullptr);
    auto ifStmt = dynamic_cast<IfStmtAST*>(stmt);
    ASSERT_NE(ifStmt, nullptr);
    EXPECT_NE(ifStmt->cond, nullptr);
    EXPECT_NE(ifStmt->thenStmt, nullptr);
    EXPECT_NE(ifStmt->elseStmt, nullptr);
}

TEST_F(ParserStmtTest, WhileStatement) {
    auto stmt = parseStmt("int32 f() { while (1) { } }");
    ASSERT_NE(stmt, nullptr);
    auto whileStmt = dynamic_cast<WhileStmtAST*>(stmt);
    ASSERT_NE(whileStmt, nullptr);
    EXPECT_NE(whileStmt->cond, nullptr);
    EXPECT_NE(whileStmt->body, nullptr);
}

TEST_F(ParserStmtTest, SwitchStatement) {
    auto stmt = parseStmt(
        "int32 f() { switch (x) { case 1: break; case 2: case 3: break; default: break; } }");
    ASSERT_NE(stmt, nullptr);
    auto sw = dynamic_cast<SwitchStmtAST*>(stmt);
    ASSERT_NE(sw, nullptr);
    EXPECT_NE(sw->cond, nullptr);
    ASSERT_EQ(sw->cases.size(), 4u);
    ASSERT_EQ(sw->caseLabels.size(), 4u);
    EXPECT_NE(sw->caseLabels[0], nullptr);
    EXPECT_NE(sw->caseLabels[1], nullptr);
    EXPECT_NE(sw->caseLabels[2], nullptr);
    EXPECT_EQ(sw->caseLabels[3], nullptr);  // default
}

TEST_F(ParserStmtTest, DoWhileStatement) {
    auto stmt = parseStmt("int32 f() { do { } while (1); }");
    ASSERT_NE(stmt, nullptr);
    auto doWhile = dynamic_cast<DoWhileStmtAST*>(stmt);
    ASSERT_NE(doWhile, nullptr);
    EXPECT_NE(doWhile->body, nullptr);
    EXPECT_NE(doWhile->cond, nullptr);
}

TEST_F(ParserStmtTest, ForStatement) {
    auto stmt = parseStmt("int32 f() { for (i = 0; i < 10; i = i + 1) { } }");
    ASSERT_NE(stmt, nullptr);
    auto forStmt = dynamic_cast<ForStmtAST*>(stmt);
    ASSERT_NE(forStmt, nullptr);
    EXPECT_NE(forStmt->init, nullptr);
    EXPECT_NE(forStmt->cond, nullptr);
    EXPECT_NE(forStmt->inc, nullptr);
    EXPECT_NE(forStmt->body, nullptr);
}

TEST_F(ParserStmtTest, ForEmptyInit) {
    auto stmt = parseStmt("int32 f() { for (; 1; ) { } }");
    ASSERT_NE(stmt, nullptr);
    auto forStmt = dynamic_cast<ForStmtAST*>(stmt);
    ASSERT_NE(forStmt, nullptr);
    EXPECT_EQ(forStmt->init, nullptr);
    EXPECT_NE(forStmt->cond, nullptr);
    EXPECT_EQ(forStmt->inc, nullptr);
}

TEST_F(ParserStmtTest, BreakStatement) {
    auto stmt = parseStmt("int32 f() { while(1) { break; } }");
    ASSERT_NE(stmt, nullptr);
    auto whileStmt = dynamic_cast<WhileStmtAST*>(stmt);
    ASSERT_NE(whileStmt, nullptr);
    auto body = dynamic_cast<CompoundStmtAST*>(whileStmt->body.get());
    ASSERT_NE(body, nullptr);
    ASSERT_FALSE(body->stmts.empty());
    auto breakStmt = dynamic_cast<BreakStmtAST*>(body->stmts[0].get());
    ASSERT_NE(breakStmt, nullptr);
}

TEST_F(ParserStmtTest, ContinueStatement) {
    auto stmt = parseStmt("int32 f() { while(1) { continue; } }");
    ASSERT_NE(stmt, nullptr);
    auto whileStmt = dynamic_cast<WhileStmtAST*>(stmt);
    ASSERT_NE(whileStmt, nullptr);
    auto body = dynamic_cast<CompoundStmtAST*>(whileStmt->body.get());
    ASSERT_NE(body, nullptr);
    ASSERT_FALSE(body->stmts.empty());
    auto continueStmt = dynamic_cast<ContinueStmtAST*>(body->stmts[0].get());
    ASSERT_NE(continueStmt, nullptr);
}

TEST_F(ParserStmtTest, ExprStatement) {
    auto stmt = parseStmt("int32 f() { x; }");
    ASSERT_NE(stmt, nullptr);
    auto exprStmt = dynamic_cast<ExprStmtAST*>(stmt);
    ASSERT_NE(exprStmt, nullptr);
    EXPECT_NE(exprStmt->expr, nullptr);
}

TEST_F(ParserStmtTest, CompoundStatement) {
    auto stmt = parseStmt("int32 f() { { return 1; } }");
    ASSERT_NE(stmt, nullptr);
    auto compound = dynamic_cast<CompoundStmtAST*>(stmt);
    ASSERT_NE(compound, nullptr);
    EXPECT_FALSE(compound->stmts.empty());
}

TEST_F(ParserStmtTest, NestedIf) {
    auto stmt = parseStmt("int32 f() { if (1) { if (2) { } } }");
    ASSERT_NE(stmt, nullptr);
    auto outer = dynamic_cast<IfStmtAST*>(stmt);
    ASSERT_NE(outer, nullptr);
    auto outerBody = dynamic_cast<CompoundStmtAST*>(outer->thenStmt.get());
    ASSERT_NE(outerBody, nullptr);
    ASSERT_FALSE(outerBody->stmts.empty());
    auto inner = dynamic_cast<IfStmtAST*>(outerBody->stmts[0].get());
    ASSERT_NE(inner, nullptr);
    EXPECT_NE(inner->cond, nullptr);
}

// ========== Declaration Parser Tests ==========

class ParserDeclTest : public ::testing::Test {
protected:
    void SetUp() override {
        spdlog::set_level(spdlog::level::off);
    }

    static std::vector<Token> lex(const std::string& source) {
        Lexer lexer("test.c", source);
        return lexer.tokenize();
    }

    static auto parse(const std::string& source) {
        auto tokens = lex(source);
        Parser parser(tokens);
        return parser.parse();
    }
};

// ========== Variable Declarations ==========

TEST_F(ParserDeclTest, SimpleVarDecl) {
    auto tu = parse("int32 x;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "x");
    EXPECT_EQ(var->type->kind, TypeKind::Int32);
    EXPECT_EQ(var->initExpr, nullptr);
}

TEST_F(ParserDeclTest, VarDeclWithInit) {
    auto tu = parse("int32 x = 42;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "x");
    EXPECT_EQ(var->type->kind, TypeKind::Int32);
    ASSERT_NE(var->initExpr, nullptr);
    auto num = dynamic_cast<NumberExprAST*>(var->initExpr.get());
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(num->value, 42);
}

TEST_F(ParserDeclTest, FloatVarDecl) {
    auto tu = parse("float32 f;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "f");
    EXPECT_EQ(var->type->kind, TypeKind::Float32);
}

TEST_F(ParserDeclTest, DoubleVarDecl) {
    auto tu = parse("float64 d = 3.14;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "d");
    EXPECT_EQ(var->type->kind, TypeKind::Float64);
}

TEST_F(ParserDeclTest, Float16AndFloat128Types) {
    auto tu = parse("float16 h; float128 q;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 2u);
    auto h = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    auto q = dynamic_cast<VarDeclAST*>(tu->declarations[1].get());
    ASSERT_NE(h, nullptr);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(h->name, "h");
    EXPECT_EQ(h->type->kind, TypeKind::Float16);
    EXPECT_EQ(q->name, "q");
    EXPECT_EQ(q->type->kind, TypeKind::Float128);
}

TEST_F(ParserDeclTest, ShortFloatTypeKeywords) {
    auto tu = parse("f16 a; f32 b; f64 c; f128 d;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 4u);
    auto a = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    auto b = dynamic_cast<VarDeclAST*>(tu->declarations[1].get());
    auto c = dynamic_cast<VarDeclAST*>(tu->declarations[2].get());
    auto d = dynamic_cast<VarDeclAST*>(tu->declarations[3].get());
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(a->type->kind, TypeKind::Float16);
    EXPECT_EQ(b->type->kind, TypeKind::Float32);
    EXPECT_EQ(c->type->kind, TypeKind::Float64);
    EXPECT_EQ(d->type->kind, TypeKind::Float128);
}

TEST_F(ParserDeclTest, PointerPostfixRestrict) {
    auto tu = parse("int32* restrict p; int32* const restrict q;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 2u);
    auto p = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    auto q = dynamic_cast<VarDeclAST*>(tu->declarations[1].get());
    ASSERT_NE(p, nullptr);
    ASSERT_NE(q, nullptr);
    ASSERT_EQ(p->type->kind, TypeKind::Pointer);
    EXPECT_TRUE(p->type->isRestrict);
    EXPECT_FALSE(p->type->isConst);
    ASSERT_EQ(q->type->kind, TypeKind::Pointer);
    EXPECT_TRUE(q->type->isRestrict);
    EXPECT_TRUE(q->type->isConst);
}

TEST_F(ParserDeclTest, OperatorSubscriptAndCallDeclarations) {
    auto tu = parse("int32 operator[](int32 a, int32 i) { return 0; } "
                    "int32 operator()(int32 a) { return 0; }");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 2u);
    auto sub = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    auto call = dynamic_cast<FunctionDeclAST*>(tu->declarations[1].get());
    ASSERT_NE(sub, nullptr);
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(sub->name, "operator[]");
    EXPECT_EQ(call->name, "operator()");
}

TEST_F(ParserDeclTest, CompoundLiteralMultiDimShape) {
    auto tu = parse("int32 main() { int32* p = (int32[2][3]){{1,2,3},{4,5,6}}; return 0; }");
    ASSERT_NE(tu, nullptr);
    auto fn = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    auto declStmt = dynamic_cast<DeclStmtAST*>(fn->body->stmts[0].get());
    ASSERT_NE(declStmt, nullptr);
    auto decl = dynamic_cast<VarDeclAST*>(declStmt->decl.get());
    ASSERT_NE(decl, nullptr);
    auto cl = dynamic_cast<CompoundLiteralExprAST*>(decl->initExpr.get());
    ASSERT_NE(cl, nullptr);
    ASSERT_EQ(cl->literalType->kind, TypeKind::Array);
    auto* outer = static_cast<ArrayType*>(cl->literalType);
    // `int32[2][3]` = array[2] of array[3] (C declarator order).
    EXPECT_EQ(outer->size, 2);
    ASSERT_EQ(outer->elementType->kind, TypeKind::Array);
    EXPECT_EQ(static_cast<ArrayType*>(outer->elementType)->size, 3);
}

TEST_F(ParserDeclTest, CompoundLiteralExpression) {
    auto tu = parse("int32 main() { int32* p = (int32[2]){1,2}; return p[0]; }");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto fn = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    // The initializer expression for `p` should be a compound literal.
    ASSERT_NE(fn->body, nullptr);
    ASSERT_EQ(fn->body->stmts.size(), 2u);
    auto declStmt = dynamic_cast<DeclStmtAST*>(fn->body->stmts[0].get());
    ASSERT_NE(declStmt, nullptr);
    auto decl = dynamic_cast<VarDeclAST*>(declStmt->decl.get());
    ASSERT_NE(decl, nullptr);
    auto cl = dynamic_cast<CompoundLiteralExprAST*>(decl->initExpr.get());
    ASSERT_NE(cl, nullptr);
    ASSERT_EQ(cl->literalType->kind, TypeKind::Array);
    ASSERT_NE(cl->initList, nullptr);
    ASSERT_EQ(cl->initList->initializers.size(), 2u);
}

TEST_F(ParserDeclTest, CharVarDecl) {
    auto tu = parse("char c;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "c");
    EXPECT_EQ(var->type->kind, TypeKind::Char);
}

TEST_F(ParserDeclTest, VoidPointerDecl) {
    auto tu = parse("void* p;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "p");
    EXPECT_EQ(var->type->kind, TypeKind::Pointer);
    EXPECT_EQ(var->type->base->kind, TypeKind::Void);
}

TEST_F(ParserDeclTest, IntPointerDecl) {
    auto tu = parse("int32* ptr;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "ptr");
    EXPECT_EQ(var->type->kind, TypeKind::Pointer);
    EXPECT_EQ(var->type->base->kind, TypeKind::Int32);
}

TEST_F(ParserDeclTest, DoublePointerDecl) {
    auto tu = parse("int32** pp;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "pp");
    EXPECT_EQ(var->type->kind, TypeKind::Pointer);
    EXPECT_EQ(var->type->base->kind, TypeKind::Pointer);
    EXPECT_EQ(var->type->base->base->kind, TypeKind::Int32);
}

// ========== Array Declarations ==========

TEST_F(ParserDeclTest, ArrayDecl) {
    auto tu = parse("int32 arr[10];");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto arr = dynamic_cast<ArrayDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->name, "arr");
    EXPECT_EQ(arr->elementType->kind, TypeKind::Int32);
    EXPECT_EQ(arr->size, 10);
}

TEST_F(ParserDeclTest, ArrayDeclWithInit) {
    auto tu = parse("int32 arr[5] = {1, 2, 3};");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto arr = dynamic_cast<ArrayDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->name, "arr");
    EXPECT_EQ(arr->size, 5);
    EXPECT_NE(arr->initExpr, nullptr);
}

// ========== Function Declarations ==========

TEST_F(ParserDeclTest, FunctionDeclNoParams) {
    auto tu = parse("int32 main() { return 0; }");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto func = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->name, "main");
    EXPECT_EQ(func->returnType->kind, TypeKind::Int32);
    EXPECT_TRUE(func->params.empty());
    EXPECT_NE(func->body, nullptr);
}

TEST_F(ParserDeclTest, FunctionDeclWithParams) {
    auto tu = parse("int32 add(int32 a, int32 b) { return a + b; }");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto func = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->name, "add");
    EXPECT_EQ(func->returnType->kind, TypeKind::Int32);
    EXPECT_EQ(func->params.size(), 2u);
    EXPECT_EQ(func->params[0]->name, "a");
    EXPECT_EQ(func->params[0]->type->kind, TypeKind::Int32);
    EXPECT_EQ(func->params[1]->name, "b");
    EXPECT_EQ(func->params[1]->type->kind, TypeKind::Int32);
}

TEST_F(ParserDeclTest, FunctionDeclVoidReturn) {
    auto tu = parse("void print(void) { }");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto func = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->name, "print");
    EXPECT_EQ(func->returnType->kind, TypeKind::Void);
}

TEST_F(ParserDeclTest, FunctionDeclForwardDecl) {
    auto tu = parse("int32 foo(int32 x);");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto func = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->name, "foo");
    EXPECT_EQ(func->returnType->kind, TypeKind::Int32);
    EXPECT_EQ(func->params.size(), 1u);
    EXPECT_EQ(func->body, nullptr);
}

TEST_F(ParserDeclTest, FunctionDeclPointerParam) {
    auto tu = parse("void swap(int32* a, int32* b) { }");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto func = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->params.size(), 2u);
    EXPECT_EQ(func->params[0]->type->kind, TypeKind::Pointer);
    EXPECT_EQ(func->params[0]->type->base->kind, TypeKind::Int32);
    EXPECT_EQ(func->params[1]->type->kind, TypeKind::Pointer);
    EXPECT_EQ(func->params[1]->type->base->kind, TypeKind::Int32);
}

// ========== Struct Declarations ==========

TEST_F(ParserDeclTest, StructDecl) {
    auto tu = parse("struct Point { int32 x; int32 y; };");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto strct = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(strct, nullptr);
    EXPECT_EQ(strct->name, "Point");
    EXPECT_EQ(strct->fields.size(), 2u);
    EXPECT_EQ(strct->fields[0].name, "x");
    EXPECT_EQ(strct->fields[0].type->kind, TypeKind::Int32);
    EXPECT_EQ(strct->fields[1].name, "y");
    EXPECT_EQ(strct->fields[1].type->kind, TypeKind::Int32);
}

TEST_F(ParserDeclTest, StructVarDecl) {
    auto tu = parse("struct Point p;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "p");
    EXPECT_EQ(var->type->kind, TypeKind::Struct);
}

TEST_F(ParserDeclTest, StructForwardDecl) {
    auto tu = parse("struct Node;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto strct = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(strct, nullptr);
    EXPECT_EQ(strct->name, "Node");
    EXPECT_TRUE(strct->fields.empty());
}

TEST_F(ParserDeclTest, StructWithPointerField) {
    auto tu = parse("struct Node { int32 data; struct Node* next; };");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto strct = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(strct, nullptr);
    EXPECT_EQ(strct->fields.size(), 2u);
    EXPECT_EQ(strct->fields[1].type->kind, TypeKind::Pointer);
    EXPECT_EQ(strct->fields[1].type->base->kind, TypeKind::Struct);
}

// ========== Union Declarations ==========

TEST_F(ParserDeclTest, UnionDecl) {
    auto tu = parse("union Data { int32 i; float32 f; };");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto un = dynamic_cast<UnionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(un, nullptr);
    EXPECT_EQ(un->name, "Data");
    EXPECT_EQ(un->members.size(), 2u);
    EXPECT_EQ(un->members[0].name, "i");
    EXPECT_EQ(un->members[0].type->kind, TypeKind::Int32);
    EXPECT_EQ(un->members[1].name, "f");
    EXPECT_EQ(un->members[1].type->kind, TypeKind::Float32);
}

TEST_F(ParserDeclTest, UnionVarDecl) {
    auto tu = parse("union Data d;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "d");
    EXPECT_EQ(var->type->kind, TypeKind::Union);
}

TEST_F(ParserDeclTest, StructArrayMember) {
    auto tu = parse("struct S { int32 n; char name[20]; };");
    ASSERT_NE(tu, nullptr);
    auto strct = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(strct, nullptr);
    ASSERT_EQ(strct->fields.size(), 2u);
    ASSERT_EQ(strct->fields[1].type->kind, TypeKind::Array);
    auto* arr = static_cast<ArrayType*>(strct->fields[1].type);
    EXPECT_EQ(arr->size, 20);
    EXPECT_EQ(arr->elementType->kind, TypeKind::Char);
}

TEST_F(ParserDeclTest, UnionArrayMember) {
    auto tu = parse("union U { int32 i; char buf[8]; };");
    ASSERT_NE(tu, nullptr);
    auto un = dynamic_cast<UnionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(un, nullptr);
    ASSERT_EQ(un->members.size(), 2u);
    ASSERT_EQ(un->members[1].type->kind, TypeKind::Array);
    auto* arr = static_cast<ArrayType*>(un->members[1].type);
    EXPECT_EQ(arr->size, 8);
    EXPECT_EQ(arr->elementType->kind, TypeKind::Char);
}

TEST_F(ParserDeclTest, FunctionPointerDeclarator) {
    auto tu = parse("int32 (*fp)(int32, int32);");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "fp");
    ASSERT_EQ(var->type->kind, TypeKind::Pointer);
    ASSERT_NE(var->type->base, nullptr);
    EXPECT_EQ(var->type->base->kind, TypeKind::Function);
}

// ========== Enum Declarations ==========

TEST_F(ParserDeclTest, EnumDecl) {
    auto tu = parse("enum Color { RED, GREEN, BLUE };");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto en = dynamic_cast<EnumDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(en->name, "Color");
    EXPECT_EQ(en->values.size(), 3u);
    EXPECT_EQ(en->values[0].first, "RED");
    EXPECT_EQ(en->values[0].second, 0);
    EXPECT_EQ(en->values[1].first, "GREEN");
    EXPECT_EQ(en->values[1].second, 1);
    EXPECT_EQ(en->values[2].first, "BLUE");
    EXPECT_EQ(en->values[2].second, 2);
}

TEST_F(ParserDeclTest, EnumDeclWithValues) {
    auto tu = parse("enum Status { OK = 200, NOT_FOUND = 404 };");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto en = dynamic_cast<EnumDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(en->name, "Status");
    EXPECT_EQ(en->values.size(), 2u);
    EXPECT_EQ(en->values[0].first, "OK");
    EXPECT_EQ(en->values[0].second, 200);
    EXPECT_EQ(en->values[1].first, "NOT_FOUND");
    EXPECT_EQ(en->values[1].second, 404);
}

TEST_F(ParserDeclTest, EnumVarDecl) {
    auto tu = parse("enum Color c;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "c");
    EXPECT_EQ(var->type->kind, TypeKind::Enum);
}

// ========== using Alias Declarations ==========

TEST_F(ParserDeclTest, UsingDecl) {
    auto tu = parse("using Integer = int32;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto u = dynamic_cast<UsingDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->name, "Integer");
    EXPECT_EQ(u->aliasedType->kind, TypeKind::Int32);
}

TEST_F(ParserDeclTest, UsingAnonStruct) {
    auto tu = parse("using Point = struct { int32 x; int32 y; };");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto u = dynamic_cast<UsingDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->name, "Point");
    EXPECT_EQ(u->aliasedType->kind, TypeKind::Struct);
}

TEST_F(ParserDeclTest, UsingUsage) {
    auto tu = parse("using Integer = int32; Integer x = 5;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 2u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[1].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "x");
    EXPECT_EQ(var->type->kind, TypeKind::Typedef);
}

// ========== Multiple Declarations ==========

TEST_F(ParserDeclTest, MultipleVarDecls) {
    auto tu = parse("int32 x; float32 y; char z;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 3u);
    EXPECT_NE(dynamic_cast<VarDeclAST*>(tu->declarations[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<VarDeclAST*>(tu->declarations[1].get()), nullptr);
    EXPECT_NE(dynamic_cast<VarDeclAST*>(tu->declarations[2].get()), nullptr);
}

TEST_F(ParserDeclTest, MixedDeclarations) {
    auto tu = parse("int32 x; int32 add(int32 a, int32 b) { return a + b; } float32 y;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 3u);
    EXPECT_NE(dynamic_cast<VarDeclAST*>(tu->declarations[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<FunctionDeclAST*>(tu->declarations[1].get()), nullptr);
    EXPECT_NE(dynamic_cast<VarDeclAST*>(tu->declarations[2].get()), nullptr);
}

// ========== Const and Volatile Qualifiers ==========

TEST_F(ParserDeclTest, ConstVarDecl) {
    auto tu = parse("const int32 x = 10;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_TRUE(var->type->isConst);
    EXPECT_FALSE(var->isConstexpr);
}

TEST_F(ParserDeclTest, VolatileVarDecl) {
    auto tu = parse("volatile int32 x;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_TRUE(var->type->isVolatile);
}

TEST_F(ParserDeclTest, ConstPointerDecl) {
    auto tu = parse("const int32* p;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->type->kind, TypeKind::Pointer);
    EXPECT_TRUE(var->type->base->isConst);
    EXPECT_FALSE(var->type->isConst);
}

// ========== Additional Edge Case Tests ==========

TEST_F(ParserDeclTest, FunctionDeclVarArgs) {
    auto tu = parse("int32 printf(const char* fmt, ...);");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto func = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->name, "printf");
    EXPECT_EQ(func->params.size(), 1u);
    EXPECT_EQ(func->params[0]->name, "fmt");
    EXPECT_EQ(func->params[0]->type->kind, TypeKind::Pointer);
    EXPECT_EQ(func->body, nullptr);
}

TEST_F(ParserDeclTest, FunctionDeclVoidParams) {
    auto tu = parse("void foo(void) { }");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto func = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->name, "foo");
    EXPECT_EQ(func->returnType->kind, TypeKind::Void);
    EXPECT_TRUE(func->params.empty());
    EXPECT_NE(func->body, nullptr);
}

TEST_F(ParserDeclTest, NestedStructDecl) {
    auto tu = parse("struct Outer { struct Inner { int32 x; } inner; int32 y; };");
    ASSERT_NE(tu, nullptr);
    ASSERT_GE(tu->declarations.size(), 1u);
    auto strct = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(strct, nullptr);
    EXPECT_EQ(strct->name, "Outer");
    EXPECT_EQ(strct->fields.size(), 2u);
    EXPECT_EQ(strct->fields[0].name, "inner");
    EXPECT_EQ(strct->fields[0].type->kind, TypeKind::Struct);
    EXPECT_EQ(strct->fields[1].name, "y");
    EXPECT_EQ(strct->fields[1].type->kind, TypeKind::Int32);
}

TEST_F(ParserDeclTest, EnumForwardDecl) {
    auto tu = parse("enum Color;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto en = dynamic_cast<EnumDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(en->name, "Color");
    EXPECT_TRUE(en->values.empty());
}

TEST_F(ParserDeclTest, EnumDeclWithMixedValues) {
    auto tu = parse("enum Flags { NONE = 0, READ = 1, WRITE = 2, EXEC = 4 };");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto en = dynamic_cast<EnumDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(en->values.size(), 4u);
    EXPECT_EQ(en->values[0].first, "NONE");
    EXPECT_EQ(en->values[0].second, 0);
    EXPECT_EQ(en->values[1].first, "READ");
    EXPECT_EQ(en->values[1].second, 1);
    EXPECT_EQ(en->values[2].first, "WRITE");
    EXPECT_EQ(en->values[2].second, 2);
    EXPECT_EQ(en->values[3].first, "EXEC");
    EXPECT_EQ(en->values[3].second, 4);
}

TEST_F(ParserDeclTest, ForwardStructDecl) {
    auto tu = parse("struct Node; struct Node { int32 data; Node* next; };");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 2u);
    auto fwd = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(fwd, nullptr);
    EXPECT_TRUE(fwd->isForwardDecl);
    EXPECT_EQ(fwd->name, "Node");
}

TEST_F(ParserDeclTest, ConstVolatileDecl) {
    auto tu = parse("const volatile int32 x;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_TRUE(var->type->isConst);
    EXPECT_TRUE(var->type->isVolatile);
}

TEST_F(ParserDeclTest, MultiplePointersDecl) {
    auto tu = parse("int32** const p;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->type->kind, TypeKind::Pointer);
    EXPECT_TRUE(var->type->isConst);
    EXPECT_EQ(var->type->base->kind, TypeKind::Pointer);
    EXPECT_FALSE(var->type->base->isConst);
    EXPECT_EQ(var->type->base->base->kind, TypeKind::Int32);
}

TEST_F(ParserDeclTest, ArrayOfPointersDecl) {
    auto tu = parse("int32* arr[10];");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto arr = dynamic_cast<ArrayDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->name, "arr");
    EXPECT_EQ(arr->size, 10);
    EXPECT_EQ(arr->elementType->kind, TypeKind::Pointer);
    EXPECT_EQ(arr->elementType->base->kind, TypeKind::Int32);
}

TEST_F(ParserDeclTest, StructWithArrayField) {
    auto tu = parse("struct Buffer { int32 size; int32* data; };");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto strct = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(strct, nullptr);
    EXPECT_EQ(strct->fields.size(), 2u);
    EXPECT_EQ(strct->fields[0].name, "size");
    EXPECT_EQ(strct->fields[0].type->kind, TypeKind::Int32);
    EXPECT_EQ(strct->fields[1].name, "data");
    EXPECT_EQ(strct->fields[1].type->kind, TypeKind::Pointer);
}

TEST_F(ParserDeclTest, FunctionDeclPointerReturn) {
    auto tu = parse("int32* getPointer(void) { return 0; }");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto func = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->returnType->kind, TypeKind::Pointer);
    EXPECT_EQ(func->returnType->base->kind, TypeKind::Int32);
    EXPECT_NE(func->body, nullptr);
}

TEST_F(ParserDeclTest, UsingPointer) {
    auto tu = parse("using IntPtr = int32*; IntPtr p;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 2u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[1].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "p");
    EXPECT_EQ(var->type->kind, TypeKind::Typedef);
    auto u = dynamic_cast<UsingDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->aliasedType->kind, TypeKind::Pointer);
}

TEST_F(ParserDeclTest, UsingArray) {
    auto tu = parse("using IntArr = int32[10]; IntArr a;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 2u);
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[1].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "a");
    EXPECT_EQ(var->type->kind, TypeKind::Typedef);
    auto u = dynamic_cast<UsingDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(u, nullptr);
    ASSERT_EQ(u->aliasedType->kind, TypeKind::Array);
    EXPECT_EQ(static_cast<ArrayType*>(u->aliasedType)->size, 10);
}

TEST_F(ParserDeclTest, UsingArrayAliasKeepsDimensions) {
    auto tu = parse("using IntArr = int32[4];");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto u = dynamic_cast<UsingDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(u, nullptr);
    ASSERT_EQ(u->aliasedType->kind, TypeKind::Array);
    EXPECT_EQ(static_cast<ArrayType*>(u->aliasedType)->size, 4);
}

TEST_F(ParserDeclTest, TypedefRejectedWithHint) {
    auto errs = parseErrors("typedef int32 T;");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("'typedef' has been removed"), std::string::npos);
    EXPECT_NE(errs[0].message.find("using Name = Type;"), std::string::npos);
}

TEST_F(ParserDeclTest, TypeRejectedWithHint) {
    auto errs = parseErrors("type T = int32;");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("'type' has been removed"), std::string::npos);
    EXPECT_NE(errs[0].message.find("using Name = Type;"), std::string::npos);
}

TEST_F(ParserDeclTest, TypedefInBlockRejected) {
    auto errs = parseErrors("int32 main() { typedef int32 T; return 0; }");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("using Name = Type;"), std::string::npos);
}

TEST_F(ParserDeclTest, TypeInBlockRejected) {
    auto errs = parseErrors("int32 main() { type T = int32; return 0; }");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("using Name = Type;"), std::string::npos);
}

TEST_F(ParserDeclTest, TypedefInStructBodyRejected) {
    auto errs = parseErrors("struct S { typedef int32 T; int32 x; };");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("using Name = Type;"), std::string::npos);
}

TEST_F(ParserDeclTest, TypeInClassBodyRejected) {
    auto errs = parseErrors("class C { public: typedef int32 T; int32 x; };");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("using Name = Type;"), std::string::npos);
}

TEST_F(ParserDeclTest, AliasRejectionDoesNotCascadeInTranslationUnit) {
    auto errs = parseErrors("typedef int32 T; int32 x;");
    ASSERT_EQ(errs.size(), 1u);
}

TEST_F(ParserDeclTest, AliasRejectionDoesNotCascadeInNamespace) {
    auto errs = parseErrors("namespace A { typedef int32 T; int32 x; }");
    ASSERT_EQ(errs.size(), 1u);
}

TEST_F(ParserDeclTest, UsingInBlockParses) {
    EXPECT_TRUE(parseErrors("int32 main() { using T = int32; T x = 1; return x; }").empty());
}

TEST_F(ParserDeclTest, UsingBlockScopeEndsAtBlock) {
    EXPECT_FALSE(parseErrors("int32 main() { { using T = int32; } T x = 1; }").empty());
}

TEST_F(ParserDeclTest, UsingBlockShadowRestoresOuter) {
    auto tu = parse("using T = int32; int32 main() { { using T = float64; } T y = 2; return y; }");
    ASSERT_NE(tu, nullptr);
    auto* fn = dynamic_cast<FunctionDeclAST*>(tu->declarations[1].get());
    ASSERT_NE(fn, nullptr);
    auto* block = dynamic_cast<CompoundStmtAST*>(fn->body.get());
    ASSERT_NE(block, nullptr);
    auto* declStmt = dynamic_cast<DeclStmtAST*>(block->stmts[1].get());
    ASSERT_NE(declStmt, nullptr);
    auto* var = dynamic_cast<VarDeclAST*>(declStmt->decl.get());
    ASSERT_NE(var, nullptr);
    ASSERT_NE(var->type, nullptr);
    ASSERT_EQ(var->type->kind, TypeKind::Typedef);
    auto* td = static_cast<TypedefType*>(var->type);
    EXPECT_EQ(td->aliasedType->kind, TypeKind::Int32);
}

TEST_F(ParserDeclTest, ClassMemberUsingAlias) {
    auto tu = parse("class C { public: using T = int32; };");
    ASSERT_NE(tu, nullptr);
    auto* cls = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(cls, nullptr);
    ASSERT_EQ(cls->nestedTypes.size(), 1u);
    auto* u = dynamic_cast<UsingDeclAST*>(cls->nestedTypes[0].get());
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->name, "C_T");
    EXPECT_EQ(u->bareName, "T");
    EXPECT_FALSE(u->isBlockScope);
}

TEST_F(ParserDeclTest, ConstexprVarDecl) {
    auto tu = parse("constexpr int32 x = 42;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto* varDecl = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(varDecl, nullptr);
    EXPECT_TRUE(varDecl->isConstexpr);
    EXPECT_EQ(varDecl->name, "x");
}

TEST_F(ParserDeclTest, ConstexprWithInit) {
    auto tu = parse("constexpr int32 y = 20;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto* varDecl = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(varDecl, nullptr);
    EXPECT_TRUE(varDecl->isConstexpr);
    EXPECT_TRUE(varDecl->type->isConst);
}

TEST_F(ParserDeclTest, ConstexprFunctionDeclaration) {
    auto tu = parse("constexpr int32 square(int32 x) { return x * x; }");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto& func = dynamic_cast<FunctionDeclAST&>(*tu->declarations[0]);
    EXPECT_TRUE(func.isConstexpr);
    EXPECT_EQ(func.name, "square");
}

// ========== Error Message Tests ==========

class ParserErrorTest : public ::testing::Test {
protected:
    void SetUp() override {
        spdlog::set_level(spdlog::level::off);
    }

    static std::vector<Token> lex(const std::string& source) {
        Lexer lexer("test.c", source);
        return lexer.tokenize();
    }

    static auto parseWithErrors(const std::string& source) {
        auto tokens = lex(source);
        Parser parser(tokens);
        auto tu = parser.parse();
        return std::make_pair(std::move(tu), parser.getErrors());
    }
};

TEST_F(ParserErrorTest, MissingSemicolonAfterReturnReportsError) {
    auto [tu, errors] = parseWithErrors("int32 f() { return 1 }");
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors[0].message.find("expected ';'"), std::string::npos);
}

TEST_F(ParserErrorTest, MissingClosingParenReportsError) {
    auto [tu, errors] = parseWithErrors("int32 f() { if (1 { } }");
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& err : errors) {
        if (err.message.find("expected ')'") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserErrorTest, MissingOpeningBraceReportsError) {
    auto [tu, errors] = parseWithErrors("int32 f() return 1; }");
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& err : errors) {
        if (err.message.find("expected '{'") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserErrorTest, ErrorHasFileAndLineInfo) {
    auto [tu, errors] = parseWithErrors("int32 f() { return 1 }");
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors[0].file, "test.c");
    EXPECT_GT(errors[0].line, 0);
    EXPECT_GT(errors[0].column, 0);
}

TEST_F(ParserErrorTest, MultipleErrorsReported) {
    auto [tu, errors] = parseWithErrors("int32 f() { return 1 } int32 g() { return 2 }");
    EXPECT_GE(errors.size(), 1u);
}

TEST_F(ParserErrorTest, ErrorFormatContainsFileLineColumn) {
    auto [tu, errors] = parseWithErrors("int32 f() { return 1 }");
    ASSERT_FALSE(errors.empty());
    std::string formatted = errors[0].format();
    EXPECT_NE(formatted.find("error:"), std::string::npos);
    EXPECT_NE(formatted.find("test.c:"), std::string::npos);
}

TEST_F(ParserErrorTest, MissingClosingBraceReportsError) {
    auto [tu, errors] = parseWithErrors("int32 f() { return 1;");
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& err : errors) {
        if (err.message.find("expected '}'") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserErrorTest, MissingWhileAfterDoReportsError) {
    auto [tu, errors] = parseWithErrors("int32 f() { do { } 1; }");
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& err : errors) {
        if (err.message.find("expected 'while'") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserErrorTest, MissingSemicolonAfterBreakReportsError) {
    auto [tu, errors] = parseWithErrors("int32 f() { while(1) { break } }");
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& err : errors) {
        if (err.message.find("expected ';' after 'break'") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserErrorTest, UnexpectedTokenAfterSemicolon) {
    auto [tu, errors] = parseWithErrors("int32 f() { return 1 ; }");
    ASSERT_TRUE(errors.empty());
}

TEST_F(ParserErrorTest, MissingSemicolonAfterContinueReportsError) {
    auto [tu, errors] = parseWithErrors("int32 f() { while(1) { continue } }");
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& err : errors) {
        if (err.message.find("expected ';' after 'continue'") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}


TEST_F(ParserErrorTest, MissingClosingParenInFuncCallReportsError) {
    auto [tu, errors] = parseWithErrors("int32 f() { foo(1; }");
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& err : errors) {
        if (err.message.find("expected ')'") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserErrorTest, MissingClosingBracketReportsError) {
    auto [tu, errors] = parseWithErrors("int32 f() { int32 arr[10; }");
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& err : errors) {
        if (err.message.find("expected ']'") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserErrorTest, MultipleErrorsCollected) {
    auto [tu, errors] = parseWithErrors("int32 f() { return 1 } int32 g() { return 2 }");
    EXPECT_GE(errors.size(), 1u);
}

TEST_F(ParserErrorTest, ErrorHasCorrectFile) {
    auto [tu, errors] = parseWithErrors("int32 f() { return 1 }");
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors[0].file, "test.c");
}

TEST_F(ParserErrorTest, ErrorHasPositiveLine) {
    auto [tu, errors] = parseWithErrors("int32 f() { return 1 }");
    ASSERT_FALSE(errors.empty());
    EXPECT_GT(errors[0].line, 0);
}

TEST_F(ParserErrorTest, ErrorHasPositiveColumn) {
    auto [tu, errors] = parseWithErrors("int32 f() { return 1 }");
    ASSERT_FALSE(errors.empty());
    EXPECT_GT(errors[0].column, 0);
}

TEST_F(ParserErrorTest, FormatShowsSeverityPrefix) {
    auto [tu, errors] = parseWithErrors("int32 f() { return 1 }");
    ASSERT_FALSE(errors.empty());
    std::string formatted = errors[0].format();
    EXPECT_NE(formatted.find("error:"), std::string::npos);
}

TEST_F(ParserErrorTest, FormatShowsFileLocation) {
    auto [tu, errors] = parseWithErrors("int32 f() { return 1 }");
    ASSERT_FALSE(errors.empty());
    std::string formatted = errors[0].format();
    EXPECT_NE(formatted.find("test.c:"), std::string::npos);
}

TEST_F(ParserErrorTest, MissingEqualsInArrayInitReportsError) {
    auto [tu, errors] = parseWithErrors("int32 f() { int32 arr[10] {1,2,3}; }");
    ASSERT_FALSE(errors.empty());
}

TEST_F(ParserErrorTest, UnexpectedTokenInExpression) {
    // 裸 `return;` 现已合法（void 早退），改用真正非法 token 测表达式错误恢复。
    auto [tu, errors] = parseWithErrors("int32 f() { return ) ; }");
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& err : errors) {
        if (err.message.find("expected expression") != std::string::npos) {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);
}

TEST_F(ParserErrorTest, BareReturnParses) {
    // FUN-02 / PAR-02：void 函数裸 `return;` 早退——解析器放行（sema 检查
    // 函数返回类型，非 void 报 "must return a value"）。
    auto [tu, errors] = parseWithErrors("void f() { if (1) return; return; }");
    ASSERT_TRUE(errors.empty());
}

TEST_F(ParserErrorTest, NestedCompoundStmtError) {
    auto [tu, errors] = parseWithErrors("int32 f() { { return 1 } }");
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& err : errors) {
        if (err.message.find("expected ';'") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

// ========== Class Declaration Tests ==========

class ParserClassTest : public ::testing::Test {
protected:
    void SetUp() override {
        spdlog::set_level(spdlog::level::off);
    }

    static std::vector<Token> lex(const std::string& source) {
        Lexer lexer("test.c", source);
        return lexer.tokenize();
    }

    static auto parse(const std::string& source) {
        auto tokens = lex(source);
        Parser parser(tokens);
        return parser.parse();
    }
};

TEST_F(ParserClassTest, ClassWithMethods) {
    std::string source = R"(
        class Foo {
            int32 x;
            void setX(int32 v) { this->x = v; }
            int32 getX() { return this->x; }
        };
    )";
    auto tokens = lex(source);
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    ASSERT_EQ(ast->declarations.size(), 1);

    auto& classDecl = dynamic_cast<StructDeclAST&>(*ast->declarations[0]);
    EXPECT_EQ(classDecl.name, "Foo");
    EXPECT_EQ(classDecl.fields.size(), 1);
    EXPECT_EQ(classDecl.methods.size(), 2);
}

TEST_F(ParserClassTest, ClassInheritance) {
    std::string source = R"(
        class Base { int32 x; };
        class Derived : public Base { int32 y; };
    )";
    auto tokens = lex(source);
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    ASSERT_EQ(ast->declarations.size(), 2);

    auto& derivedDecl = dynamic_cast<StructDeclAST&>(*ast->declarations[1]);
    EXPECT_EQ(derivedDecl.name, "Derived");
    EXPECT_EQ(derivedDecl.baseClass, "Base");
}

TEST_F(ParserErrorTest, GotoIsRejected) {
    // P0-01 / NG-01: goto is not part of the language; `goto` lexes as an
    // identifier, so this must be a parse error (never a valid statement).
    auto [tu, errors] = parseWithErrors("int32 f() { goto label; }");
    EXPECT_FALSE(errors.empty());
}

TEST_F(ParserErrorTest, PreprocessorDirectiveIsRejected) {
    // P0-01 / NG-02: there is no preprocessor.
    auto [tu, errors] = parseWithErrors("#include <stdio.h>\nint32 main() { return 0; }");
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& err : errors) {
        if (err.message.find("preprocessor") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

// TYP: `int`/`float`/`double` are no longer type keywords. A statement that
// tries to declare with `int` must not silently parse as a declaration.
TEST_F(ParserErrorTest, LegacyIntKeywordIsNotAType) {
    auto [tu, errors] = parseWithErrors("int32 main() { int x = 1; return 0; }");
    EXPECT_FALSE(errors.empty());
}

// The same words are ordinary identifiers now, so they may name variables.
TEST_F(ParserErrorTest, LegacyKeywordNamesAreUsableAsIdentifiers) {
    auto [tu, errors] = parseWithErrors("int32 main() { int32 int = 5; return int; }");
    EXPECT_TRUE(errors.empty());
}

// P1-06 / T1: builtin polymorphic type names are lowercase (`optional`,
// `result`); the old capitalized spellings are no longer types (DEC-18
// hard-rename precedent).
TEST_F(ParserTest, OptionalLowercaseParses) {
    auto tu = parse("optional<int32> x;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1);
    auto* var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->type->kind, TypeKind::Optional);
}

TEST_F(ParserTest, ResultLowercaseParses) {
    auto tu = parse("result<int32, bool> r;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1);
    auto* var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->type->kind, TypeKind::Result);
}

// P1-06 / T2: `string` (dynamic) builtin type.
TEST_F(ParserTest, StringTypeParses) {
    auto tu = parse("string s;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1);
    auto* var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->type->kind, TypeKind::String);
}

// P1-06 / T2 (I4/D3): `str` 硬改名 `string_view`（DEC-18 先例）——
// 新名解析为 string_view 类型（内部 TypeKind::Str，仅内部名不动）。
TEST_F(ParserTest, StringViewTypeParses) {
    auto tu = parse("string_view s;");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1);
    auto* var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->type->kind, TypeKind::Str);
}

// P1-06 / T2 (I4/D3): 旧名 `str` 不再是类型名——类型位置不再被识别。
// （注意：顶层未知语句会被静默吞掉——P1-08 遗留；故用函数体内的
// 局部声明作载体：str 若仍是类型则 `str v2;` 应解析为声明，改名后
// 只能按表达式解析而报 `expected ';'`。）
TEST_F(ParserTest, StrTypeNowUnknown) {
    auto tokens = lex("int32 main() { str v2; return 0; }");
    Parser parser(tokens);
    auto tu = parser.parse();
    EXPECT_FALSE(parser.getErrors().empty());
}

// ========== P1-08 / PAR-16: inline asm ==========

// 取函数体第一条语句的表达式（asm 经 ExprStmtAST 作为语句出现）。
static ExprAST* parseStmtExpr(const std::string& body) {
    std::string src = "int32 f() { " + body + " return 0; }";
    s_lastTU = parse(src);
    if (!s_lastTU || s_lastTU->declarations.empty()) return nullptr;
    auto func = dynamic_cast<FunctionDeclAST*>(s_lastTU->declarations[0].get());
    if (!func || !func->body || func->body->stmts.empty()) return nullptr;
    auto est = dynamic_cast<ExprStmtAST*>(func->body->stmts[0].get());
    return est ? est->expr.get() : nullptr;
}

TEST_F(ParserTest, AsmNoOperands) {
    auto a = dynamic_cast<AsmExprAST*>(parseStmtExpr("asm(\"nop\");"));
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->asmTemplate, "nop");
    EXPECT_FALSE(a->isVolatile);
    EXPECT_TRUE(a->outputs.empty() && a->inputs.empty() && a->clobbers.empty());
}

TEST_F(ParserTest, AsmVolatileWithOutput) {
    auto a = dynamic_cast<AsmExprAST*>(
        parseStmtExpr("asm volatile(\"mov $7, %0\" : \"=r\"(out));"));
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->isVolatile);
    ASSERT_EQ(a->outputs.size(), 1u);
    EXPECT_EQ(a->outputs[0].constraint, "=r");
}

TEST_F(ParserTest, AsmReadWriteAndInput) {
    auto a = dynamic_cast<AsmExprAST*>(
        parseStmtExpr("asm(\"add %1, %0\" : \"+r\"(x) : \"r\"(y));"));
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(a->outputs.size(), 1u);
    ASSERT_EQ(a->inputs.size(), 1u);
    EXPECT_EQ(a->outputs[0].constraint, "+r");
    EXPECT_EQ(a->inputs[0].constraint, "r");
}

TEST_F(ParserTest, AsmMultipleOutputsAndClobbers) {
    auto a = dynamic_cast<AsmExprAST*>(
        parseStmtExpr("asm(\"\" : \"=r\"(a), \"=r\"(b) : : \"memory\", \"cc\");"));
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(a->outputs.size(), 2u);
    ASSERT_EQ(a->clobbers.size(), 2u);
    EXPECT_EQ(a->clobbers[0], "memory");
    EXPECT_EQ(a->clobbers[1], "cc");
}

TEST_F(ParserTest, AsmEmptyOutputSection) {
    auto a = dynamic_cast<AsmExprAST*>(
        parseStmtExpr("asm(\"\" : : \"r\"(x) : \"memory\");"));
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->outputs.empty());
    ASSERT_EQ(a->inputs.size(), 1u);
}

TEST_F(ParserTest, AsmTemplateMustBeLiteral) {
    // 非字面量模板：GCC 要求字符串字面量，解析层报错。
    // 注：Parser::parse() 只有一个 return（始终返回非空 TU），故不断言 nullptr，只断言有诊断。
    auto tokens = lex("int32 f() { asm(nop); return 0; }");
    Parser parser(tokens);
    auto tu = parser.parse();
    EXPECT_NE(tu, nullptr);
    EXPECT_FALSE(parser.getErrors().empty());
}

// ========== Designated Initializers + Flexible Array Member (PAR-13/AGG-04) ==========

TEST(ParserDesignatedInitTest, ParsesDesignators) {
    auto expr = parseExpr("{ .x = 1, 2, [1] = 3 }");
    ASSERT_NE(expr, nullptr);
    auto il = dynamic_cast<InitializerListExprAST*>(expr);
    ASSERT_NE(il, nullptr);
    ASSERT_EQ(il->initializers.size(), 3u);
    ASSERT_EQ(il->designators.size(), 3u);
    ASSERT_EQ(il->designators[0].size(), 1u);
    EXPECT_EQ(il->designators[0][0].kind, Designator::Kind::Field);
    EXPECT_EQ(il->designators[0][0].field, "x");
    EXPECT_TRUE(il->designators[1].empty());
    ASSERT_EQ(il->designators[2].size(), 1u);
    EXPECT_EQ(il->designators[2][0].kind, Designator::Kind::Index);
}

TEST(ParserDesignatedInitTest, ParsesNestedDesignators) {
    auto expr = parseExpr("{ .a.b = 2 }");
    ASSERT_NE(expr, nullptr);
    auto il = dynamic_cast<InitializerListExprAST*>(expr);
    ASSERT_NE(il, nullptr);
    ASSERT_EQ(il->designators.size(), 1u);
    ASSERT_EQ(il->designators[0].size(), 2u);
    EXPECT_EQ(il->designators[0][0].kind, Designator::Kind::Field);
    EXPECT_EQ(il->designators[0][0].field, "a");
    EXPECT_EQ(il->designators[0][1].field, "b");
}

TEST(ParserDesignatedInitTest, MemberEmptyDimIsFlexibleArray) {
    auto tu = parse("struct S { int32 n; int32 data[]; };");
    ASSERT_NE(tu, nullptr);
    StructDeclAST* sd = nullptr;
    for (auto& d : tu->declarations) {
        if (auto* s = dynamic_cast<StructDeclAST*>(d.get())) { sd = s; break; }
    }
    ASSERT_NE(sd, nullptr);
    ASSERT_EQ(sd->fields.size(), 2u);
    EXPECT_TRUE(sd->fields[1].flexibleArray);
    ASSERT_NE(sd->fields[1].type, nullptr);
    ASSERT_EQ(sd->fields[1].type->kind, TypeKind::Array);
    EXPECT_EQ(static_cast<ArrayType*>(sd->fields[1].type)->size, 0);
}

// ========== PAR-12: 匿名类型定义（synthetic identity） ==========

TEST(ParserAnonTypeTest, AnonymousStructGetsUniqueSyntheticName) {
    // 两次独立的 `struct { int32 x; }` 顶层匿名定义 → 两个不同的合成名类型，
    // 名字以 __anon_ 开头且互不相同（C 语义：每次匿名定义 = 独立类型）。
    auto tu = parse(
        "struct { int32 x; } a;\n"
        "struct { int32 x; } b;\n");
    ASSERT_NE(tu, nullptr);
    Type* ta = nullptr;
    Type* tb = nullptr;
    for (auto& d : tu->declarations) {
        if (auto* v = dynamic_cast<VarDeclAST*>(d.get())) {
            if (v->name == "a") ta = v->type;
            if (v->name == "b") tb = v->type;
        }
    }
    ASSERT_NE(ta, nullptr);
    ASSERT_NE(tb, nullptr);
    ASSERT_EQ(ta->kind, TypeKind::Struct);
    ASSERT_EQ(tb->kind, TypeKind::Struct);
    auto* sa = static_cast<StructType*>(ta);
    auto* sb = static_cast<StructType*>(tb);
    EXPECT_NE(sa->name, "");
    EXPECT_NE(sb->name, "");
    EXPECT_NE(sa->name, sb->name);
    EXPECT_TRUE(sa->name.rfind("__anon_", 0) == 0);
    EXPECT_FALSE(typesEqual(ta, tb));
}

TEST(ParserAnonTypeTest, AnonymousUnionGetsUniqueSyntheticName) {
    auto tu = parse("union { int32 i; float32 f; } u;");
    ASSERT_NE(tu, nullptr);
    Type* tu_t = nullptr;
    for (auto& d : tu->declarations) {
        if (auto* v = dynamic_cast<VarDeclAST*>(d.get())) {
            if (v->name == "u") tu_t = v->type;
        }
    }
    ASSERT_NE(tu_t, nullptr);
    ASSERT_EQ(tu_t->kind, TypeKind::Union);
    auto* u = static_cast<UnionType*>(tu_t);
    EXPECT_NE(u->name, "");
    EXPECT_TRUE(u->name.rfind("__anon_", 0) == 0);
}

TEST(ParserAnonTypeTest, AnonymousMemberPromotionStillDetected) {
    // AGG-03: 匿名 struct 成员提升的判定仍须命中（名字以 __anon_ 开头）。
    auto tu = parse(
        "struct Outer { int32 tag; struct { int32 a; int32 b; }; };\n");
    ASSERT_NE(tu, nullptr);
    StructDeclAST* sd = nullptr;
    for (auto& d : tu->declarations) {
        if (auto* s = dynamic_cast<StructDeclAST*>(d.get())) { sd = s; break; }
    }
    ASSERT_NE(sd, nullptr);
    // tag + 提升的 a + b
    ASSERT_EQ(sd->fields.size(), 3u);
    EXPECT_EQ(sd->fields[0].name, "tag");
    EXPECT_EQ(sd->fields[1].name, "a");
    EXPECT_EQ(sd->fields[2].name, "b");
}

// ========== PAR-11: lambda 解析 ==========

TEST(ParserLambdaTest, ParsesNoCaptureLambda) {
    auto expr = parseExpr("[](int32 x) -> int32 { return x; }");
    ASSERT_NE(expr, nullptr);
    auto lam = dynamic_cast<LambdaExprAST*>(expr);
    ASSERT_NE(lam, nullptr);
    EXPECT_TRUE(lam->captures.empty());
    EXPECT_FALSE(lam->captureAll);
    ASSERT_EQ(lam->params.size(), 1u);
    EXPECT_EQ(lam->params[0]->name, "x");
    EXPECT_EQ(lam->params[0]->type->kind, TypeKind::Int32);
    EXPECT_NE(lam->returnType, nullptr);
    EXPECT_TRUE(lam->hasExplicitReturnType);
    EXPECT_NE(lam->body, nullptr);
}

TEST(ParserLambdaTest, ParsesCaptureListByValueAndRef) {
    auto expr = parseExpr("[a, &b](int32 x) { return x; }");
    ASSERT_NE(expr, nullptr);
    auto lam = dynamic_cast<LambdaExprAST*>(expr);
    ASSERT_NE(lam, nullptr);
    ASSERT_EQ(lam->captures.size(), 2u);
    EXPECT_EQ(lam->captures[0].name, "a");
    EXPECT_FALSE(lam->captures[0].byRef);
    EXPECT_EQ(lam->captures[1].name, "b");
    EXPECT_TRUE(lam->captures[1].byRef);
    EXPECT_FALSE(lam->hasExplicitReturnType);  // 返回类型省略 → 推断
}

TEST(ParserLambdaTest, ParsesCaptureAll) {
    auto expr = parseExpr("[=](int32 x) { return x + 1; }");
    ASSERT_NE(expr, nullptr);
    auto lam = dynamic_cast<LambdaExprAST*>(expr);
    ASSERT_NE(lam, nullptr);
    EXPECT_TRUE(lam->captureAll);
}

TEST(ParserLambdaTest, ParsesCaptureAllByRef) {
    auto expr = parseExpr("[&](int32 x) { return x; }");
    auto lam = dynamic_cast<LambdaExprAST*>(expr);
    ASSERT_NE(lam, nullptr);
    EXPECT_TRUE(lam->captureAll);
    EXPECT_TRUE(lam->captureAllByRef);
}

TEST(ParserLambdaTest, ParsesMixedCaptureDefaults) {
    auto byVal = dynamic_cast<LambdaExprAST*>(
        parseExpr("[=, &a, b](int32 x) { return x; }"));
    ASSERT_NE(byVal, nullptr);
    EXPECT_TRUE(byVal->captureAll);
    EXPECT_FALSE(byVal->captureAllByRef);
    ASSERT_EQ(byVal->captures.size(), 2u);
    EXPECT_TRUE(byVal->captures[0].byRef);
    EXPECT_FALSE(byVal->captures[1].byRef);

    auto byRef = dynamic_cast<LambdaExprAST*>(
        parseExpr("[&, a, &b](int32 x) { return x; }"));
    ASSERT_NE(byRef, nullptr);
    EXPECT_TRUE(byRef->captureAll);
    EXPECT_TRUE(byRef->captureAllByRef);
    ASSERT_EQ(byRef->captures.size(), 2u);
    EXPECT_FALSE(byRef->captures[0].byRef);
    EXPECT_TRUE(byRef->captures[1].byRef);
}

TEST(ParserLambdaTest, ParsesCaptureInitializers) {
    auto expr = parseExpr("[x = 1 + 2, &y = z](int32 a) { return a; }");
    auto lam = dynamic_cast<LambdaExprAST*>(expr);
    ASSERT_NE(lam, nullptr);
    ASSERT_EQ(lam->captures.size(), 2u);
    EXPECT_EQ(lam->captures[0].name, "x");
    EXPECT_FALSE(lam->captures[0].byRef);
    ASSERT_NE(lam->captures[0].init, nullptr);
    EXPECT_EQ(lam->captures[1].name, "y");
    EXPECT_TRUE(lam->captures[1].byRef);
    ASSERT_NE(lam->captures[1].init, nullptr);
}

TEST(ParserLambdaTest, ParsesThisCapture) {
    auto expr = parseExpr("[this](int32 x) { return x; }");
    auto lam = dynamic_cast<LambdaExprAST*>(expr);
    ASSERT_NE(lam, nullptr);
    ASSERT_EQ(lam->captures.size(), 1u);
    EXPECT_EQ(lam->captures[0].name, "this");
    EXPECT_FALSE(lam->captures[0].byRef);
}

TEST(ParserLambdaTest, LambdaVsSubscriptDisambiguated) {
    // 数组下标仍走 ArrayAccessExprAST。
    auto expr = parseExpr("arr[2]");
    ASSERT_NE(expr, nullptr);
    EXPECT_NE(dynamic_cast<ArrayAccessExprAST*>(expr), nullptr);
    // 表达式起始 `[` 是 lambda（下标是后缀）。
    auto lam = parseExpr("[](int32 x) { return x; }");
    EXPECT_NE(dynamic_cast<LambdaExprAST*>(lam), nullptr);
}

// TYP-05: char8/char16/char32 are type keywords mapping to the unsigned
// fixed-width integers (aliases, not nominal types — design decision).
TEST_F(ParserTest, Char8DeclaresUInt8) {
    auto tu = parse("char8 c;");
    ASSERT_NE(tu, nullptr);
    ASSERT_FALSE(tu->declarations.empty());
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->type, TypeContext::instance().getUInt8());
}

TEST_F(ParserTest, Char16DeclaresUInt16) {
    auto tu = parse("char16 c;");
    ASSERT_NE(tu, nullptr);
    ASSERT_FALSE(tu->declarations.empty());
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->type, TypeContext::instance().getUInt16());
}

TEST_F(ParserTest, Char32DeclaresUInt32) {
    auto tu = parse("char32 c;");
    ASSERT_NE(tu, nullptr);
    ASSERT_FALSE(tu->declarations.empty());
    auto var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->type, TypeContext::instance().getUInt32());
}

// CON-01: `atomic T` produces a distinct qualified type (clone), and does NOT
// poison the shared scalar singleton — a later plain `int32` is unaffected.
TEST_F(ParserTest, AtomicQualifierClonesType) {
    auto tu = parse("atomic int32 a; int32 b;");
    ASSERT_NE(tu, nullptr);
    ASSERT_GE(tu->declarations.size(), 2u);
    auto av = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    auto bv = dynamic_cast<VarDeclAST*>(tu->declarations[1].get());
    ASSERT_NE(av, nullptr);
    ASSERT_NE(bv, nullptr);
    EXPECT_TRUE(av->type->isAtomic);
    EXPECT_NE(av->type, TypeContext::instance().getInt32());
    EXPECT_FALSE(bv->type->isAtomic);
    EXPECT_EQ(bv->type, TypeContext::instance().getInt32());
}

// FUN-09: default parameter `T name = const-expr` parses; only trailing
// parameters may carry a default (enforced at sema).
TEST_F(ParserTest, DefaultArgParses) {
    auto tu = parse("int32 f(int32 x, int32 y = 5) { return x + y; }");
    ASSERT_NE(tu, nullptr);
    ASSERT_FALSE(tu->declarations.empty());
    auto fn = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    ASSERT_EQ(fn->params.size(), 2u);
    EXPECT_FALSE(fn->params[0]->hasDefault);
    EXPECT_TRUE(fn->params[1]->hasDefault);
    EXPECT_NE(fn->params[1]->defaultValue, nullptr);
}

// Unknown-type declarations must be diagnosed, not silently dropped (a
// `nosuchtype int32 y;` currently compiles with zero diagnostics and no `y`).
TEST_F(ParserTest, UnknownTypeDeclDiagnosed) {
    auto tokens = lex("nosuchtype int32 y = 5;");
    Parser parser(tokens);
    parser.parse();
    ASSERT_FALSE(parser.getErrors().empty());
    bool hasUnknown = false;
    for (auto& e : parser.getErrors()) {
        if (e.message.find("unknown type") != std::string::npos &&
            e.message.find("nosuchtype") != std::string::npos) {
            hasUnknown = true;
        }
    }
    EXPECT_TRUE(hasUnknown);
}

// Template-argument parse failure must not double-report: `Box<int32, nosuch>`
// already diagnosed the bad template argument; no extra "unknown type 'nosuch'".
TEST_F(ParserTest, UnknownTypeNotDoubleReported) {
    auto tokens = lex(
        "template<typename T> struct Box { T v; };\n"
        "Box<int32, nosuchtype> b;\n");
    Parser parser(tokens);
    parser.parse();
    // The template-argument failure produces at least one error; counting
    // "unknown type 'nosuchtype'" occurrences must be at most one overall.
    size_t unknownTypeCount = 0;
    for (auto& e : parser.getErrors()) {
        if (e.message.find("unknown type 'nosuchtype'") != std::string::npos) {
            ++unknownTypeCount;
        }
    }
    EXPECT_LE(unknownTypeCount, 1u);
}

// PAR-14: `alignof(T)` / `offsetof(T, member)` keyword forms parse.
TEST_F(ParserTest, AlignofKeywordExpr) {
    auto expr = parseExpr("alignof(int32)");
    ASSERT_NE(expr, nullptr);
    auto sized = dynamic_cast<SizeofExprAST*>(expr);
    ASSERT_NE(sized, nullptr);
    EXPECT_TRUE(sized->isAlignOf);
}

TEST_F(ParserTest, OffsetofKeywordExpr) {
    auto tu = parse("struct S { int32 a; int32 b; };\n"
                    "int32 f() { return offsetof(S, b); }");
    ASSERT_NE(tu, nullptr);
    ASSERT_FALSE(tu->declarations.empty());
    auto fn = dynamic_cast<FunctionDeclAST*>(tu->declarations[1].get());
    ASSERT_NE(fn, nullptr);
}

TEST_F(ParserTest, ModuleMustBeFirstDeclaration) {
    auto errs = parseErrors("int32 x = 1;\nmodule late;\n");
    bool found = false;
    for (const auto& e : errs) {
        if (e.message.find("module declaration must appear before other declarations") !=
            std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserTest, DuplicateModuleDeclarationRejected) {
    auto errs = parseErrors("module a;\nmodule b;\n");
    bool found = false;
    for (const auto& e : errs) {
        if (e.message.find("duplicate module declaration") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(ParserTest, ModuleWithoutNameRejected) {
    auto errs = parseErrors("module ;\n");
    bool found = false;
    for (const auto& e : errs) {
        if (e.message.find("expected module name") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}
