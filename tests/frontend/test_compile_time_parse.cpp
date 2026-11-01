// P1-04 / CT-01: `compile_time` 顶层声明解析——`compile_time.if` 条件编译
// 与 `compile_time.static_assert` parse 成专有 DeclAST 节点（非关键字，
// 成员名集合内前缀特判）。

#include <gtest/gtest.h>

#include <string>

#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "ast/Decl.h"
#include "ast/Expr.h"
#include "ast/Stmt.h"

#include <spdlog/spdlog.h>

static std::unique_ptr<TranslationUnitAST> parse(const std::string& source) {
    Lexer lexer("compile_time_parse_test.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    return parser.parse();
}

class CompileTimeParse : public ::testing::Test {
protected:
    void SetUp() override {
        spdlog::set_level(spdlog::level::off);
    }
};

TEST_F(CompileTimeParse, CompileTimeIfParses) {
    auto tu = parse("compile_time.if (1) { int32 a; } else { int32 b; }");
    ASSERT_NE(tu, nullptr);
    ASSERT_FALSE(tu->declarations.empty());
    auto* ctIf = dynamic_cast<CompileTimeIfDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(ctIf, nullptr);
    ASSERT_NE(ctIf->cond, nullptr);
    ASSERT_EQ(ctIf->thenDecls.size(), 1u);
    ASSERT_EQ(ctIf->elseDecls.size(), 1u);
}

TEST_F(CompileTimeParse, CompileTimeIfElseOptional) {
    auto tu = parse("compile_time.if (1) { int32 a; }");
    ASSERT_NE(tu, nullptr);
    auto* ctIf = dynamic_cast<CompileTimeIfDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(ctIf, nullptr);
    EXPECT_TRUE(ctIf->elseDecls.empty());
}

TEST_F(CompileTimeParse, CompileTimeAssertParses) {
    auto tu = parse("compile_time.static_assert(1 == 1, \"x\");");
    ASSERT_NE(tu, nullptr);
    ASSERT_FALSE(tu->declarations.empty());
    auto* ctAssert = dynamic_cast<CompileTimeAssertDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(ctAssert, nullptr);
    ASSERT_NE(ctAssert->call, nullptr);
    auto* call = dynamic_cast<MethodCallExprAST*>(ctAssert->call.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->methodName, "static_assert");
    ASSERT_EQ(call->args.size(), 2u);
}

TEST_F(CompileTimeParse, BranchDeclsParse) {
    auto tu = parse("compile_time.if (1) { struct S { int32 x; } int32 f() { return 0; } }");
    ASSERT_NE(tu, nullptr);
    auto* ctIf = dynamic_cast<CompileTimeIfDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(ctIf, nullptr);
    ASSERT_EQ(ctIf->thenDecls.size(), 2u);
    EXPECT_NE(dynamic_cast<StructDeclAST*>(ctIf->thenDecls[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<FunctionDeclAST*>(ctIf->thenDecls[1].get()), nullptr);
}

// CTX-01: multi-type-argument queries must type-parse every type argument.
TEST_F(CompileTimeParse, IsSameWrapsBothTypeArgs) {
    auto tu = parse("bool same = compile_time.is_same(int32, int64);");
    ASSERT_NE(tu, nullptr);
    ASSERT_FALSE(tu->declarations.empty());
    auto* var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    auto* call = dynamic_cast<MethodCallExprAST*>(var->initExpr.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->methodName, "is_same");
    ASSERT_EQ(call->args.size(), 2u);
    EXPECT_NE(dynamic_cast<SizeofExprAST*>(call->args[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<SizeofExprAST*>(call->args[1].get()), nullptr);
}

// CTX-01: only the first argument is a type; the rest are ordinary expressions.
TEST_F(CompileTimeParse, AssertSizeFirstTypeRestExpr) {
    auto tu = parse("int32 x = compile_time.assert_size(int32, 4);");
    ASSERT_NE(tu, nullptr);
    ASSERT_FALSE(tu->declarations.empty());
    auto* var = dynamic_cast<VarDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(var, nullptr);
    auto* call = dynamic_cast<MethodCallExprAST*>(var->initExpr.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->methodName, "assert_size");
    ASSERT_EQ(call->args.size(), 2u);
    EXPECT_NE(dynamic_cast<SizeofExprAST*>(call->args[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<NumberExprAST*>(call->args[1].get()), nullptr);
}

// CTX-01 / E2: compile_time.repeat parses as a statement with a body.
TEST_F(CompileTimeParse, RepeatParses) {
    auto tu = parse("int32 f() { compile_time.repeat(3) { int32 x = 1; } return 0; }");
    ASSERT_NE(tu, nullptr);
    ASSERT_FALSE(tu->declarations.empty());
    auto* fn = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    ASSERT_NE(fn->body, nullptr);
    ASSERT_FALSE(fn->body->stmts.empty());
    auto* repeat = dynamic_cast<CompileTimeRepeatStmtAST*>(fn->body->stmts[0].get());
    ASSERT_NE(repeat, nullptr);
    EXPECT_NE(dynamic_cast<NumberExprAST*>(repeat->count.get()), nullptr);
    ASSERT_NE(repeat->body, nullptr);
    EXPECT_EQ(repeat->body->stmts.size(), 1u);
}

// CTX-01: diagnostic-only members take no type arguments.
TEST_F(CompileTimeParse, FailTakesExpressionArg) {
    auto tu = parse("int32 f() { compile_time.fail(\"boom\"); return 0; }");
    ASSERT_NE(tu, nullptr);
    ASSERT_FALSE(tu->declarations.empty());
    auto* fn = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    ASSERT_NE(fn->body, nullptr);
    ASSERT_FALSE(fn->body->stmts.empty());
    auto* stmt = dynamic_cast<ExprStmtAST*>(fn->body->stmts[0].get());
    ASSERT_NE(stmt, nullptr);
    auto* call = dynamic_cast<MethodCallExprAST*>(stmt->expr.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->methodName, "fail");
    ASSERT_EQ(call->args.size(), 1u);
    EXPECT_NE(dynamic_cast<StringExprAST*>(call->args[0].get()), nullptr);
}
