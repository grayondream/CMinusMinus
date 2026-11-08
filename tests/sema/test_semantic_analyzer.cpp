#include <gtest/gtest.h>
#include "sema/SemanticAnalyzer.h"
#include "sema/TemplateRegistry.h"
#include "ast/Expr.h"
#include "ast/Stmt.h"
#include "ast/Decl.h"
#include "ast/Type.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"

class SemanticAnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override {
        analyzer = std::make_unique<SemanticAnalyzer>();
        typeCtx = &TypeContext::instance();
    }

    std::unique_ptr<SemanticAnalyzer> analyzer;
    TypeContext* typeCtx;
};

TEST_F(SemanticAnalyzerTest, ValidProgramPasses) {
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<VarDeclAST>("x", typeCtx->getInt32()));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_TRUE(analyzer->getErrors().empty());
}

TEST_F(SemanticAnalyzerTest, UndeclaredVariableDetected) {
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ExprStmtAST>(
        std::make_unique<VariableExprAST>("y")));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_FALSE(analyzer->getErrors().empty());
    EXPECT_EQ(analyzer->getErrors()[0].level, Diagnostic::Level::Error);
}

TEST_F(SemanticAnalyzerTest, TypeMismatchDetected) {
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    auto lhs = std::make_unique<VariableExprAST>("x");
    auto rhs = std::make_unique<StringExprAST>("hello");
    bodyStmts.push_back(std::make_unique<ExprStmtAST>(
        std::make_unique<AssignmentExprAST>(
            AssignOp::Assign, std::move(lhs), std::move(rhs))));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    params.push_back(std::make_unique<ParamDeclAST>("x", typeCtx->getInt32()));

    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_FALSE(analyzer->getErrors().empty());
}

TEST_F(SemanticAnalyzerTest, FunctionCallArgumentMismatch) {
    auto callee = std::make_unique<VariableExprAST>("add");
    std::vector<std::unique_ptr<ExprAST>> callArgs;
    callArgs.push_back(std::make_unique<NumberExprAST>(1));
    auto callExpr = std::make_unique<CallExprAST>("add", std::move(callArgs));

    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ExprStmtAST>(std::move(callExpr)));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    params.push_back(std::make_unique<ParamDeclAST>("a", typeCtx->getInt32()));
    params.push_back(std::make_unique<ParamDeclAST>("b", typeCtx->getInt32()));

    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "add", typeCtx->getInt32(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_FALSE(analyzer->getErrors().empty());
    EXPECT_EQ(analyzer->getErrors()[0].level, Diagnostic::Level::Error);
}

TEST_F(SemanticAnalyzerTest, ValidFunctionCall) {
    std::vector<std::unique_ptr<ParamDeclAST>> callParams;
    std::vector<std::unique_ptr<ExprAST>> callArgs;
    callArgs.push_back(std::make_unique<NumberExprAST>(1));
    callArgs.push_back(std::make_unique<NumberExprAST>(2));
    auto callExpr = std::make_unique<CallExprAST>("add", std::move(callArgs));

    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ExprStmtAST>(std::move(callExpr)));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    params.push_back(std::make_unique<ParamDeclAST>("a", typeCtx->getInt32()));
    params.push_back(std::make_unique<ParamDeclAST>("b", typeCtx->getInt32()));

    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "add", typeCtx->getInt32(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_TRUE(analyzer->getErrors().empty());
}

TEST_F(SemanticAnalyzerTest, ScopeNestingWorks) {
    std::vector<std::unique_ptr<StmtAST>> innerStmts;
    innerStmts.push_back(std::make_unique<ExprStmtAST>(
        std::make_unique<VariableExprAST>("x")));
    auto innerBody = std::make_unique<CompoundStmtAST>(std::move(innerStmts));

    std::vector<std::unique_ptr<StmtAST>> outerStmts;
    outerStmts.push_back(std::make_unique<DeclStmtAST>(
        std::make_unique<VarDeclAST>("x", typeCtx->getInt32())));
    outerStmts.push_back(std::move(innerBody));
    auto outerBody = std::make_unique<CompoundStmtAST>(std::move(outerStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, outerBody));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_TRUE(analyzer->getErrors().empty());
}

TEST_F(SemanticAnalyzerTest, RedefinitionDetected) {
    std::vector<std::unique_ptr<StmtAST>> stmts;
    stmts.push_back(std::make_unique<DeclStmtAST>(
        std::make_unique<VarDeclAST>("x", typeCtx->getInt32())));
    stmts.push_back(std::make_unique<DeclStmtAST>(
        std::make_unique<VarDeclAST>("x", typeCtx->getInt32())));
    auto body = std::make_unique<CompoundStmtAST>(std::move(stmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_FALSE(analyzer->getErrors().empty());
}

TEST_F(SemanticAnalyzerTest, BinaryOpTypeCheck) {
    auto lhs = std::make_unique<StringExprAST>("hello");
    auto rhs = std::make_unique<StringExprAST>("world");
    auto binExpr = std::make_unique<BinaryExprAST>(
        BinaryOp::Add, std::move(lhs), std::move(rhs));

    std::vector<std::unique_ptr<StmtAST>> stmts;
    stmts.push_back(std::make_unique<ExprStmtAST>(std::move(binExpr)));
    auto body = std::make_unique<CompoundStmtAST>(std::move(stmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_FALSE(analyzer->getErrors().empty());
}

TEST_F(SemanticAnalyzerTest, BinaryOpErrorMessageIncludesTypes) {
    auto lhs = std::make_unique<StringExprAST>("hello");
    auto rhs = std::make_unique<StringExprAST>("world");
    auto binExpr = std::make_unique<BinaryExprAST>(
        BinaryOp::Add, std::move(lhs), std::move(rhs));

    std::vector<std::unique_ptr<StmtAST>> stmts;
    stmts.push_back(std::make_unique<ExprStmtAST>(std::move(binExpr)));
    auto body = std::make_unique<CompoundStmtAST>(std::move(stmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    ASSERT_FALSE(analyzer->getErrors().empty());
    // P1-06 (FMT-03): string literals type as `str`.
    EXPECT_NE(analyzer->getErrors()[0].message.find("string_view"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, AssignmentTypeMismatchErrorMessageIncludesTypes) {
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    auto lhs = std::make_unique<VariableExprAST>("x");
    auto rhs = std::make_unique<StringExprAST>("hello");
    bodyStmts.push_back(std::make_unique<ExprStmtAST>(
        std::make_unique<AssignmentExprAST>(
            AssignOp::Assign, std::move(lhs), std::move(rhs))));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    params.push_back(std::make_unique<ParamDeclAST>("x", typeCtx->getInt32()));

    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_NE(analyzer->getErrors()[0].message.find("int"), std::string::npos);
    // P1-06 (FMT-03): string literals type as `str`.
    EXPECT_NE(analyzer->getErrors()[0].message.find("string_view"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, UndeclaredVariableErrorMessageIncludesName) {
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ExprStmtAST>(
        std::make_unique<VariableExprAST>("y")));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_NE(analyzer->getErrors()[0].message.find("y"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, ReturnTypeErrorIncludesTypes) {
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ReturnStmtAST>(
        std::make_unique<StringExprAST>("hello")));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getFloat32(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_NE(analyzer->getErrors()[0].message.find("float"), std::string::npos);
    // P1-06 (FMT-03): string literals type as `str`.
    EXPECT_NE(analyzer->getErrors()[0].message.find("string_view"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, DiagnosticHasSourceLocation) {
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ExprStmtAST>(
        std::make_unique<VariableExprAST>("y")));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_EQ(analyzer->getErrors()[0].level, Diagnostic::Level::Error);
    EXPECT_FALSE(analyzer->getErrors()[0].message.empty());
}

TEST_F(SemanticAnalyzerTest, UndeclaredFunctionErrorMessage) {
    auto callExpr = std::make_unique<CallExprAST>("nonexistent", std::vector<std::unique_ptr<ExprAST>>());

    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ExprStmtAST>(std::move(callExpr)));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_NE(analyzer->getErrors()[0].message.find("undeclared"), std::string::npos);
    EXPECT_NE(analyzer->getErrors()[0].message.find("nonexistent"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, WrongNumberOfArgumentsErrorMessage) {
    std::vector<std::unique_ptr<ExprAST>> callArgs;
    callArgs.push_back(std::make_unique<NumberExprAST>(1));
    callArgs.push_back(std::make_unique<NumberExprAST>(2));
    callArgs.push_back(std::make_unique<NumberExprAST>(3));
    auto callExpr = std::make_unique<CallExprAST>("add", std::move(callArgs));

    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ExprStmtAST>(std::move(callExpr)));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    params.push_back(std::make_unique<ParamDeclAST>("a", typeCtx->getInt32()));
    params.push_back(std::make_unique<ParamDeclAST>("b", typeCtx->getInt32()));

    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "add", typeCtx->getInt32(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_NE(analyzer->getErrors()[0].message.find("no matching function"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, RedeclarationErrorMessage) {
    std::vector<std::unique_ptr<StmtAST>> stmts;
    stmts.push_back(std::make_unique<DeclStmtAST>(
        std::make_unique<VarDeclAST>("x", typeCtx->getInt32())));
    stmts.push_back(std::make_unique<DeclStmtAST>(
        std::make_unique<VarDeclAST>("x", typeCtx->getFloat32())));
    auto body = std::make_unique<CompoundStmtAST>(std::move(stmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_NE(analyzer->getErrors()[0].message.find("redeclaration"), std::string::npos);
    EXPECT_NE(analyzer->getErrors()[0].message.find("x"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, NonVoidFunctionMustReturnValueErrorMessage) {
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ReturnStmtAST>(nullptr));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getInt32(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_NE(analyzer->getErrors()[0].message.find("must return a value"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, CastWarningFormat) {
    // Cast from int to float should produce a warning
    auto castExpr = std::make_unique<CastExprAST>(
        typeCtx->getFloat32(), std::make_unique<NumberExprAST>(42));

    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ExprStmtAST>(std::move(castExpr)));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    // int to float is compatible, so no warning expected
    // But if we cast from struct*, it should warn
}

TEST_F(SemanticAnalyzerTest, BinaryOpErrorMessageContainsOperatorSymbol) {
    auto lhs = std::make_unique<StringExprAST>("hello");
    auto rhs = std::make_unique<StringExprAST>("world");
    auto binExpr = std::make_unique<BinaryExprAST>(
        BinaryOp::Add, std::move(lhs), std::move(rhs));

    std::vector<std::unique_ptr<StmtAST>> stmts;
    stmts.push_back(std::make_unique<ExprStmtAST>(std::move(binExpr)));
    auto body = std::make_unique<CompoundStmtAST>(std::move(stmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_NE(analyzer->getErrors()[0].message.find("+"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, AssignmentErrorMessageContainsBothTypes) {
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    auto lhs = std::make_unique<VariableExprAST>("x");
    auto rhs = std::make_unique<StringExprAST>("hello");
    bodyStmts.push_back(std::make_unique<ExprStmtAST>(
        std::make_unique<AssignmentExprAST>(
            AssignOp::Assign, std::move(lhs), std::move(rhs))));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    params.push_back(std::make_unique<ParamDeclAST>("x", typeCtx->getInt32()));

    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_NE(analyzer->getErrors()[0].message.find("int"), std::string::npos);
    // P1-06 (FMT-03): string literals type as `str`.
    EXPECT_NE(analyzer->getErrors()[0].message.find("string_view"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, ReturnTypeErrorContainsFunctionName) {
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ReturnStmtAST>(
        std::make_unique<StringExprAST>("hello")));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "myFunc", typeCtx->getFloat32(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_NE(analyzer->getErrors()[0].message.find("myFunc"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, ConstAssignmentError) {
    auto constIntType = new Type(TypeKind::Int32);
    constIntType->isConst = true;

    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<DeclStmtAST>(
        std::make_unique<VarDeclAST>("x", constIntType)));
    bodyStmts.push_back(std::make_unique<ExprStmtAST>(
        std::make_unique<AssignmentExprAST>(
            AssignOp::Assign,
            std::make_unique<VariableExprAST>("x"),
            std::make_unique<NumberExprAST>(20))));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_TRUE(analyzer->getErrors().size() >= 1);
    EXPECT_NE(analyzer->getErrors()[0].message.find("const"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, ConstexprMustHaveInitializer) {
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<VarDeclAST>("x", typeCtx->getInt32(), nullptr, true));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_FALSE(analyzer->getErrors().empty());
    EXPECT_NE(analyzer->getErrors()[0].message.find("constexpr"), std::string::npos);
    EXPECT_NE(analyzer->getErrors()[0].message.find("initializer"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, ConstexprConstantFolding) {
    auto initExpr = std::make_unique<BinaryExprAST>(
        BinaryOp::Add,
        std::make_unique<NumberExprAST>(2),
        std::make_unique<BinaryExprAST>(
            BinaryOp::Mul,
            std::make_unique<NumberExprAST>(3),
            std::make_unique<NumberExprAST>(4)));

    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<VarDeclAST>("x", typeCtx->getInt32(), std::move(initExpr), true));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_TRUE(analyzer->getErrors().empty());

    auto& values = analyzer->getConstexprValues();
    ASSERT_TRUE(values.count("x"));
    EXPECT_EQ(values.at("x").intVal, 14);
}

TEST_F(SemanticAnalyzerTest, ConstexprNonConstantError) {
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<DeclStmtAST>(
        std::make_unique<VarDeclAST>("y", typeCtx->getInt32())));
    bodyStmts.push_back(std::make_unique<DeclStmtAST>(
        std::make_unique<VarDeclAST>("x", typeCtx->getInt32(),
            std::make_unique<VariableExprAST>("y"), true)));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "foo", typeCtx->getVoid(), params, body));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_FALSE(analyzer->getErrors().empty());
    EXPECT_NE(analyzer->getErrors()[0].message.find("constant expression"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, ConstexprFunctionWithValidSignature) {
    // constexpr int square(int x) { return x * x; }
    // Should pass - no errors
    auto returnExpr = std::make_unique<BinaryExprAST>(
        BinaryOp::Mul,
        std::make_unique<VariableExprAST>("x"),
        std::make_unique<VariableExprAST>("x"));
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ReturnStmtAST>(std::move(returnExpr)));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    params.push_back(std::make_unique<ParamDeclAST>("x", typeCtx->getInt32()));

    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "square", typeCtx->getInt32(), params, body, true));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_TRUE(analyzer->getErrors().empty());
}

TEST_F(SemanticAnalyzerTest, ConstexprFunctionWithNonLiteralReturnType) {
    // constexpr void bad() { return; }
    // Should emit error: constexpr function must have literal return type
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ReturnStmtAST>(nullptr));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;

    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "bad", typeCtx->getVoid(), params, body, true));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_FALSE(analyzer->getErrors().empty());
    EXPECT_NE(analyzer->getErrors()[0].message.find("literal return type"), std::string::npos);
}

TEST_F(SemanticAnalyzerTest, ConstexprFunctionWithNonLiteralParameter) {
    // constexpr int bad(string s) { return 0; }
    // Should emit error: parameter must have literal type. Scalar pointers are
    // allowed now (out-parameters); owning string is still not a literal type.
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ReturnStmtAST>(
        std::make_unique<NumberExprAST>(0, LiteralKind::Int)));
    auto body = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));

    std::vector<std::unique_ptr<ParamDeclAST>> params;
    params.push_back(std::make_unique<ParamDeclAST>("s", typeCtx->getStringType()));

    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>(
        "bad", typeCtx->getInt32(), params, body, true));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_FALSE(analyzer->getErrors().empty());
    EXPECT_NE(analyzer->getErrors()[0].message.find("literal type"), std::string::npos);
}

// ========== Overload Resolution ==========

TEST_F(SemanticAnalyzerTest, OverloadResolutionExactMatch) {
    // Declare two overloaded functions: add(int, int) and add(float, float)
    std::vector<std::unique_ptr<ParamDeclAST>> params1;
    params1.push_back(std::make_unique<ParamDeclAST>("a", typeCtx->getInt32()));
    params1.push_back(std::make_unique<ParamDeclAST>("b", typeCtx->getInt32()));
    auto body1 = std::make_unique<CompoundStmtAST>(std::vector<std::unique_ptr<StmtAST>>());
    
    std::vector<std::unique_ptr<ParamDeclAST>> params2;
    params2.push_back(std::make_unique<ParamDeclAST>("a", typeCtx->getFloat32()));
    params2.push_back(std::make_unique<ParamDeclAST>("b", typeCtx->getFloat32()));
    auto body2 = std::make_unique<CompoundStmtAST>(std::vector<std::unique_ptr<StmtAST>>());
    
    // Call with int arguments
    std::vector<std::unique_ptr<ExprAST>> callArgs;
    callArgs.push_back(std::make_unique<NumberExprAST>(1));
    callArgs.push_back(std::make_unique<NumberExprAST>(2));
    auto callExpr = std::make_unique<CallExprAST>("add", std::move(callArgs));
    
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ExprStmtAST>(std::move(callExpr)));
    auto callerBody = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));
    
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>("add", typeCtx->getInt32(), params1, body1));
    decls.push_back(std::make_unique<FunctionDeclAST>("add", typeCtx->getFloat32(), params2, body2));
    
    // Add caller function
    std::vector<std::unique_ptr<ParamDeclAST>> callerParams;
    decls.push_back(std::make_unique<FunctionDeclAST>("caller", typeCtx->getInt32(), callerParams, callerBody));
    
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_TRUE(analyzer->getErrors().empty());
}

TEST_F(SemanticAnalyzerTest, OverloadResolutionNoMatch) {
    std::vector<std::unique_ptr<ParamDeclAST>> params;
    params.push_back(std::make_unique<ParamDeclAST>("a", typeCtx->getInt32()));
    params.push_back(std::make_unique<ParamDeclAST>("b", typeCtx->getInt32()));
    auto body = std::make_unique<CompoundStmtAST>(std::vector<std::unique_ptr<StmtAST>>());
    
    // Call with float arguments (no matching overload)
    std::vector<std::unique_ptr<ExprAST>> callArgs;
    callArgs.push_back(std::make_unique<FloatExprAST>(1.0));
    callArgs.push_back(std::make_unique<FloatExprAST>(2.0));
    auto callExpr = std::make_unique<CallExprAST>("add", std::move(callArgs));
    
    std::vector<std::unique_ptr<StmtAST>> bodyStmts;
    bodyStmts.push_back(std::make_unique<ExprStmtAST>(std::move(callExpr)));
    auto callerBody = std::make_unique<CompoundStmtAST>(std::move(bodyStmts));
    
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::make_unique<FunctionDeclAST>("add", typeCtx->getInt32(), params, body));
    
    std::vector<std::unique_ptr<ParamDeclAST>> callerParams;
    decls.push_back(std::make_unique<FunctionDeclAST>("caller", typeCtx->getInt32(), callerParams, callerBody));
    
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_FALSE(analyzer->getErrors().empty());
}

// ========== Class Support ==========

TEST(ClassSupport, SemanticAnalysisValidClass) {
    std::string source = R"(
        class Foo {
            int32 x;
            void setX(int32 v) { this->x = v; }
        };
    )";
    Lexer lexer("test.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
}

TEST(ClassSupport, SemanticAnalysisInvalidBaseClass) {
    std::string source = R"(
        class Derived : public Nonexistent { public: int32 y; };
    )";
    Lexer lexer("test.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_FALSE(analyzer.getErrors().empty());
}

TEST(ClassSupport, SemanticAnalysisInheritedMethod) {
    std::string source = R"(
        class Base {
            public:
            int32 x;
            void setX(int32 v) { this->x = v; }
        };
        class Derived : public Base {
            public:
            int32 y;
        };
    )";
    Lexer lexer("test.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
}

TEST(ClassSupport, SemanticAnalysisInheritedFieldAccess) {
    std::string source = R"(
        class Base {
            public:
            int32 x;
        };
        class Derived : public Base {
            public:
            int32 y;
        };
        int32 main() {
            Derived d;
            d.x = 5;
            return d.x + d.y;
        }
    )";
    Lexer lexer("test.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
}

TEST(ClassSupport, DuplicateFunctionDefinitionIsAnError) {
    std::string source = R"(
        int32 f() { return 1; }
        int32 f() { return 2; }
    )";
    Lexer lexer("test.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_FALSE(analyzer.getErrors().empty());
}

TEST(ClassSupport, SemanticAnalysisCircularInheritance) {
    std::string source = R"(
        class A : public B { int32 x; };
        class B : public A { int32 y; };
    )";
    Lexer lexer("test.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_FALSE(analyzer.getErrors().empty());
}

// ========== print / println builtin ==========

namespace {
bool analyzeOk(const std::string& source) {
    Lexer lexer("test.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) return false;
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    return analyzer.getErrors().empty();
}
} // namespace

TEST(PrintBuiltin, BuildsTypeDirectedFormat) {
    std::string source = R"(
        int32 main() {
            print("aa {}", 1);
            return 0;
        }
    )";
    Lexer lexer("test.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty());

    auto* fn = dynamic_cast<FunctionDeclAST*>(ast->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    ASSERT_NE(fn->body, nullptr);
    ASSERT_GE(fn->body->stmts.size(), 1u);
    auto* stmt = dynamic_cast<ExprStmtAST*>(fn->body->stmts[0].get());
    ASSERT_NE(stmt, nullptr);
    auto* call = dynamic_cast<CallExprAST*>(stmt->expr.get());
    ASSERT_NE(call, nullptr);
    EXPECT_TRUE(call->isPrint);
    EXPECT_FALSE(call->printNewline);
    EXPECT_EQ(call->printCFormat, "aa %d");
}

TEST(PrintBuiltin, PrintlnAppendsNewline) {
    EXPECT_TRUE(analyzeOk("int32 main() { println(\"x={}\", 1); return 0; }"));
}

TEST(PrintBuiltin, SupportsScalarTypes) {
    EXPECT_TRUE(analyzeOk(
        "int32 main() { print(\"{} {} {} {}\", 1, 1.5, 'a', \"s\"); return 0; }"));
}

TEST(PrintBuiltin, RejectsPercentStyle) {
    // `%d` has no {} placeholder, so the extra argument is reported.
    EXPECT_FALSE(analyzeOk("int32 main() { int32 x = 1; print(\"%d\", x); return 0; }"));
}

TEST(PrintBuiltin, ReportsMissingArgument) {
    EXPECT_FALSE(analyzeOk("int32 main() { print(\"{} {}\", 1); return 0; }"));
}

TEST(PrintBuiltin, UnsupportedTypeNeedsToString) {
    EXPECT_FALSE(analyzeOk(
        "struct P { int32 x; }; int32 main() { struct P p; print(\"{}\", p); return 0; }"));
}

TEST(PrintBuiltin, UsesFreeFunctionToString) {
    EXPECT_TRUE(analyzeOk(
        "struct P { int32 x; }; const char* to_string(struct P p) { return \"P\"; } "
        "int32 main() { struct P p; print(\"{}\", p); return 0; }"));
}

TEST(PrintBuiltin, UsesMethodToString) {
    EXPECT_TRUE(analyzeOk(
        "class C { public: int32 x; public: const char* to_string() { return \"C\"; } }; "
        "int32 main() { C c; print(\"{}\", c); return 0; }"));
}

// LEX-15: literal suffix/default kinds map to fixed-width base types.
TEST_F(SemanticAnalyzerTest, LiteralKindsMapToFixedWidthTypes) {
    auto intKind = [&](LiteralKind k) {
        NumberExprAST n(42, k);
        return analyzer->getExprType(n)->kind;
    };
    EXPECT_EQ(intKind(LiteralKind::Int), TypeKind::Int32);
    EXPECT_EQ(intKind(LiteralKind::UInt), TypeKind::UInt32);
    EXPECT_EQ(intKind(LiteralKind::Long), TypeKind::Int64);
    EXPECT_EQ(intKind(LiteralKind::ULong), TypeKind::UInt64);

    auto floatKind = [&](LiteralKind k) {
        FloatExprAST f(1.5, k);
        return analyzer->getExprType(f)->kind;
    };
    EXPECT_EQ(floatKind(LiteralKind::Float16), TypeKind::Float16);
    EXPECT_EQ(floatKind(LiteralKind::Float32), TypeKind::Float32);
    EXPECT_EQ(floatKind(LiteralKind::Float64), TypeKind::Float64);
    EXPECT_EQ(floatKind(LiteralKind::Float128), TypeKind::Float128);

    // Unsuffixed defaults: integer -> int32, float -> float64.
    NumberExprAST defInt(42);
    EXPECT_EQ(analyzer->getExprType(defInt)->kind, TypeKind::Int32);
    FloatExprAST defFloat(1.5);
    EXPECT_EQ(analyzer->getExprType(defFloat)->kind, TypeKind::Float64);
}

// LEX-05: trailing-dot float literals flow through lexer -> parser -> sema.
TEST_F(SemanticAnalyzerTest, TrailingDotFloatLiteralsAreAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int32 main() { float64 x = 1.; float32 y = 1.f32; "
        "float64 z = 1.e3; return 0; }"));
}

// DEC-01/SEM-04: class members default to private; outside access is denied
// with a dedicated diagnostic (E2009), not "no member".
TEST(SemanticAnalyzerAccessTest, ClassPrivateDefaultDeniedOutside) {
    EXPECT_FALSE(analyzeOk(
        "class CA { int32 x; }; int32 main() { CA c; c.x = 1; return 0; }"));
}

TEST(SemanticAnalyzerAccessTest, PrivateAccessDiagnosticCode) {
    Lexer lexer("t.c", "class CA { int32 x; }; int32 main() { CA c; c.x = 1; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool found = false;
    for (const auto& e : analyzer.getErrors()) {
        if (e.code == DiagnosticCode::SemPrivateMemberAccess) found = true;
    }
    EXPECT_TRUE(found);
}

// In-class method bodies may reach private members through `this->`.
TEST(SemanticAnalyzerAccessTest, ClassMethodAccessesPrivateViaThis) {
    EXPECT_TRUE(analyzeOk(
        "class CC { int32 x; int32 get() { return this->x; } }; "
        "int32 main() { CC c; return 0; }"));
}

// Members in an explicit `public:` section stay accessible (no truncation).
TEST(SemanticAnalyzerAccessTest, PublicSectionMemberAccessible) {
    EXPECT_TRUE(analyzeOk(
        "class CD { private: int32 s; public: int32 o; }; "
        "int32 main() { CD c; c.o = 1; return 0; }"));
}

// struct members remain public by default (DEC-01).
TEST(SemanticAnalyzerAccessTest, StructFieldsDefaultPublic) {
    EXPECT_TRUE(analyzeOk(
        "struct SE { int32 v; }; int32 main() { SE s; s.v = 1; return 0; }"));
}

// TYP-12 Task 2: subscript on slice yields the element type and is an lvalue.
TEST(SliceSemTest, SubscriptOnSliceAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int32 pick(int32[] s) { return s[0]; } "
        "int32 main() { return 0; }"));
}

TEST(SliceSemTest, SubscriptOnIntRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32 x = 1; return x[0]; }"));
}

// Review Focus #5: nested slices parse and type-check through one layer.
// (Explicit pipeline: analyzeOk() ignores parser errors, which masked the
// fact that `[][]` did not parse at all before TYP-12.)
TEST(SliceSemTest, NestedSliceSubscriptAccepted) {
    Lexer lexer("test.c",
        "int32 pick(int32[][] grid) { return grid[0][0]; } "
        "int32 main() { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    EXPECT_TRUE(parser.getErrors().empty());
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
}

// TYP-12 Task 3: slice .len member; the only member allowed on a slice.
TEST(SliceSemTest, LenMemberAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int64 n(int32[] s) { return s.len; } "
        "int32 main() { return 0; }"));
}

TEST(SliceSemTest, LenOnNonSliceRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32 x = 1; return x.len; }"));
}

TEST(SliceSemTest, SliceOtherMemberRejected) {
    EXPECT_FALSE(analyzeOk(
        "int64 n(int32[] s) { return s.ptr; } "
        "int32 main() { return 0; }"));
}

// TYP-12 Task 4: T[N] decays to T[] at call sites (zero-copy).
TEST(SliceSemTest, ArrayDecaysToSliceParam) {
    EXPECT_TRUE(analyzeOk(
        "int32 total(int32[] s) { return 0; } "
        "int32 main() { int32 arr[3] = {1,2,3}; return total(arr); }"));
}

TEST(SliceSemTest, CrossElementDecayRejected) {
    EXPECT_FALSE(analyzeOk(
        "int64 total(float64[] s) { return 0; } "
        "int32 main() { int32 arr[3] = {1,2,3}; return total(arr); }"));
}

// TYP-12 Task 5: array->slice decay in assignment/init/return; cross-element
// slice compatibility is rejected (Review Focus #1).
TEST(SliceSemTest, ArrayToSliceAssignmentAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int32 main() { int32 arr[3] = {1,2,3}; int32[] s = arr; return 0; }"));
}

TEST(SliceSemTest, SliceToArrayRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32[] s; int32 arr[3] = {1,2,3}; arr = s; return 0; }"));
}

TEST(SliceSemTest, CrossElementSliceAssignmentRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32[] a; float64[] b = a; return 0; }"));
}

// TYP-11 D4: returning a view of a local array dangles — rejected.
TEST(SliceSemTest, DanglingLocalArrayReturnRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32[] bad() { int32 a[2] = {1,2}; return a; } "
        "int32 main() { return 0; }"));
}

// TYP-11 D2: T name[N] parameter desugars to slice T[].
TEST(SliceSemTest, ArrayParamAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int32 APSum(int32 a[2]) { return a[0] + a[1]; } "
        "int32 main() { int32 arr[2] = {3,4}; return APSum(arr); }"));
}

TEST(SliceSemTest, CrossElementArrayParamRejected) {
    EXPECT_FALSE(analyzeOk(
        "int64 APF(float64 a[2]) { return 0; } "
        "int32 main() { int32 arr[2] = {3,4}; return APF(arr); }"));
}

// Function-pointer parameters share parseParamDecl: `(*cb)(int32 a[2])`
// desugars too. (A bare function-type parameter `int32 cb(int32 a[2])` is a
// pre-existing parser gap — not pinned here.) analyzeOk ignores parse
// errors, so use the explicit pipeline.
TEST(SliceSemTest, FunctionPointerArrayParamDesugared) {
    Lexer lexer("test.c",
        "int32 APUse(int32 (*cb)(int32 a[2])) { return 0; } "
        "int32 main() { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    EXPECT_TRUE(parser.getErrors().empty());
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
}

// analyzeOk ignores parse errors — use the explicit pipeline to pin the
// parse-level rejection.
// TYP-11 2026-10-04: multi-dimensional array parameters are now supported —
// this former rejection pin became a parse-level acceptance pin (row-slice
// desugar); semantic acceptance is covered by MDPExplicitFirstDimAccepted.
TEST(SliceSemTest, MultiDimArrayParamParses) {
    Lexer lexer("test.c",
        "int32 main() { return 0; } "
        "int32 APBad(int32 a[2][3]) { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    EXPECT_TRUE(parser.getErrors().empty());
}

TEST(SliceSemTest, UnnamedArrayParamRejected) {
    Lexer lexer("test.c",
        "int32 main() { return 0; } "
        "int32 APAnon(int32[2]) { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    EXPECT_FALSE(parser.getErrors().empty());
}

// Minor 3 (review): empty brackets desugar too — `T name[]` == `T name[N]`.
TEST(SliceSemTest, EmptyBracketsArrayParamDesugared) {
    EXPECT_TRUE(analyzeOk(
        "int32 APSum2(int32 a[]) { return a[0] + a[1]; } "
        "int32 main() { int32 arr[2] = {3,4}; return APSum2(arr); }"));
}

// TYP-12 legacy: a method with a slice parameter accepts an array argument
// (zero-copy decay), mirroring plain function calls.
TEST(SliceSemTest, MethodSliceParamAcceptsArray) {
    EXPECT_TRUE(analyzeOk(
        "class MCBox { public: void bump(int32[] s) { s[0] = s[0] + 100; } }; "
        "int32 main() { MCBox b; int32 arr[2] = {1,2}; b.bump(arr); return 0; }"));
}

// Exact-match resolution must stay equivalent to the previous behaviour.
TEST(SliceSemTest, MethodExactMatchUnchanged) {
    EXPECT_TRUE(analyzeOk(
        "class MCCnt { public: int32 x; void setX(int32 v) { this->x = v; } }; "
        "int32 main() { MCCnt c; c.setX(42); return 0; }"));
}

// Cross-element decay must not slip through rank-based matching.
TEST(SliceSemTest, MethodCrossElementRejected) {
    EXPECT_FALSE(analyzeOk(
        "class MCF { public: void f(float64[] s) { } }; "
        "int32 main() { MCF m; int32 arr[2] = {1,2}; m.f(arr); return 0; }"));
}

// Resolution through the inheritance chain stores the base class's declared
// parameter types (incl. base this) for codegen mangling.
TEST(SliceSemTest, MethodInheritedSliceParamAccepted) {
    EXPECT_TRUE(analyzeOk(
        "class MCB { public: void bump(int32[] s) { s[0] = s[0] + 1; } }; "
        "class MCD : public MCB { public: int32 y; }; "
        "int32 main() { MCD d; int32 arr[2] = {1,2}; d.bump(arr); return 0; }"));
}

// Missing/invalid arguments must neither crash nor resolve.
TEST(SliceSemTest, MethodNullArgGuard) {
    EXPECT_FALSE(analyzeOk(
        "class MCG { public: void g(int32[] s) { } }; "
        "int32 main() { MCG m; m.g(); return 0; }"));
}

// Review Focus 5: an argument whose type fails to resolve (undeclared
// variable) must be disqualified by the null-argType guard, not fed to
// conversionRank. Arity matches so phase 2's loop body actually runs.
TEST(SliceSemTest, MethodNullArgTypeGuardRejected) {
    EXPECT_FALSE(analyzeOk(
        "class MCG { public: void g(int32[] s) { } }; "
        "int32 main() { MCG m; m.g(nosuch); return 0; }"));
}

// TYP-11: multi-dimensional array declarations.
TEST(SliceSemTest, MultiDimDeclAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int32 main() { int32 a[2][3] = {{1,2,3},{4,5,6}}; return a[1][2]; }"));
}

TEST(SliceSemTest, MultiDimFirstDimInferred) {
    EXPECT_TRUE(analyzeOk(
        "int32 main() { int32 a[][3] = {{1,2,3},{4,5,6}}; return a[1][2]; }"));
}

TEST(SliceSemTest, MultiDimSubscriptTypes) {
    EXPECT_TRUE(analyzeOk(
        "int32 MDSum(int32[] s) { return s[0] + s[1] + s[2]; } "
        "int32 main() { int32 a[2][3] = {{1,2,3},{4,5,6}}; return MDSum(a[1]); }"));
}

TEST(SliceSemTest, MultiDimWholeToSliceRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 MDSum(int32[] s) { return s[0]; } "
        "int32 main() { int32 a[2][3] = {{1,2,3},{4,5,6}}; return MDSum(a); }"));
}

TEST(SliceSemTest, MultiDimDanglingReturnRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32[] MDBad() { int32 a[2][3] = {{1,2,3},{4,5,6}}; return a; } "
        "int32 main() { return 0; }"));
}

TEST(SliceSemTest, MultiDimMemberFieldAccepted) {
    EXPECT_TRUE(analyzeOk(
        "class MDBox { public: int32 g[2][3]; int32 last() { return this->g[1][2]; } }; "
        "int32 main() { MDBox b; return 0; }"));
}

// Review fix: C requires every dimension except the first to be present —
// `a[2][]` must not silently become a zero-length inner array (OOB GEP).
// analyzeOk ignores parse errors, so pin the parse level explicitly.
TEST(SliceSemTest, NonFirstDimDefaultRejected) {
    Lexer lexer("test.c",
        "int32 main() { int32 a[2][] = {{1,2,3},{4,5,6}}; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    EXPECT_FALSE(parser.getErrors().empty());
}

// Review fix: a scalar (non-brace-list) initializer for an array passed sema
// before (compared against elementType) and was silently dropped or crashed.
TEST(SliceSemTest, GlobalArrayScalarInitRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 g[2] = 5; int32 main() { return 0; }"));
}

// TYP-11: multi-dimensional array parameters desugar to row slices.
// Spec: docs/superpowers/specs/2026-10-04-multidim-array-params-design.md

// D1': f(a) with a: int32[2][3], param int32 m[][3].
TEST(SliceSemTest, MDP2DParamAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int32 MDPF(int32 m[][3]) { return m[1][2]; } "
        "int32 main() { int32 a[2][3] = {{1,2,3},{4,5,6}}; return MDPF(a); }"));
}

// D3': row type mismatch is rejected at compile time.
TEST(SliceSemTest, MDPRowTypeMismatchRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 MDPF(int32 m[][3]) { return m[0][0]; } "
        "int32 main() { int32 b[2][4] = {{1,2,3,4},{5,6,7,8}}; return MDPF(b); }"));
}

// D2': only the first dimension may be empty — pin the parse level
// explicitly (analyzeOk ignores parse errors).
TEST(SliceSemTest, MDPNonFirstDimRejected) {
    Lexer lexer("test.c",
        "int32 MDPF(int32 m[3][]) { return 0; } int32 main() { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    EXPECT_FALSE(parser.getErrors().empty());
}

// D2': explicit first dimension is documentation, identical to [].
TEST(SliceSemTest, MDPExplicitFirstDimAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int32 MDPG(int32 m[2][3]) { return m[1][2]; } "
        "int32 main() { int32 a[2][3] = {{1,2,3},{4,5,6}}; return MDPG(a); }"));
}

// D1': chained subscript inside the callee body.
TEST(SliceSemTest, MDPParamSubscript) {
    EXPECT_TRUE(analyzeOk(
        "int32 MDPGet(int32 m[][3]) { return m[1][2] + m[0][0]; } "
        "int32 main() { int32 a[2][3] = {{1,2,3},{4,5,6}}; return MDPGet(a); }"));
}

// VLA is a deliberate non-feature (2026-10-05): runtime-length allocation
// conflicts with the safety positioning (unbounded stack growth), C23 made
// VLAs optional, and dynamic lengths belong to Slice. Pin the dedicated
// diagnostic, not just "some parse error".
TEST(SliceSemTest, VLALocalDiagnostic) {
    Lexer lexer("test.c",
        "int32 main() { int32 n = 3; int32 a[n]; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    ASSERT_FALSE(parser.getErrors().empty());
    bool hasVLA = false;
    for (auto& e : parser.getErrors())
        if (e.message.find("variable-length") != std::string::npos)
            hasVLA = true;
    EXPECT_TRUE(hasVLA);
}

TEST(SliceSemTest, VLAParamDiagnostic) {
    Lexer lexer("test.c",
        "int32 f(int32 m[n]) { return 0; } int32 main() { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    ASSERT_FALSE(parser.getErrors().empty());
    bool hasVLA = false;
    for (auto& e : parser.getErrors())
        if (e.message.find("variable-length") != std::string::npos)
            hasVLA = true;
    EXPECT_TRUE(hasVLA);
}

// Array-assignment defect fix (review finding 2026-10-04): arrays are
// non-modifiable lvalues (C99 6.5.16). Assignment TO an array is rejected
// regardless of RHS kind; assignment FROM an array (decay) stays allowed.
TEST(SliceSemTest, AASGArrayToArrayRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32 a[3] = {1,2,3}; int32 b[3] = {4,5,6}; a = b; return 0; }"));
}

TEST(SliceSemTest, AASGArrayFromPointerRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32 a[3]; int32* p = 0; a = p; return 0; }"));
}

TEST(SliceSemTest, AASGPointerFromArrayOk) {
    EXPECT_TRUE(analyzeOk(
        "int32 main() { int32 a[3] = {1,2,3}; int32* p = 0; p = a; return 0; }"));
}

TEST(SliceSemTest, AASGArrayAssignDiagnostic) {
    Lexer lexer("test.c",
        "int32 main() { int32 a[3] = {1,2,3}; int32 b[3] = {4,5,6}; a = b; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool hasLvalueMsg = false;
    for (auto& e : analyzer.getErrors())
        if (e.message.find("not modifiable lvalues") != std::string::npos)
            hasLvalueMsg = true;
    EXPECT_TRUE(hasLvalueMsg);
}

// AGG-10: static members desugar to class-prefixed global symbols.
// Spec: docs/superpowers/specs/2026-10-05-static-members-design.md

TEST(SliceSemTest, SMStaticMethodQualifiedCall) {
    EXPECT_TRUE(analyzeOk(
        "class SMBox { public: static int32 make() { return 7; } }; "
        "int32 main() { return SMBox::make(); }"));
}

TEST(SliceSemTest, SMStaticVarQualifiedAccess) {
    EXPECT_TRUE(analyzeOk(
        "class SMCtr { public: static int32 count = 0; }; "
        "int32 main() { SMCtr::count = 5; return SMCtr::count; }"));
}

TEST(SliceSemTest, SMPrivateStaticExternalRejected) {
    // DS5: E2009 (member access), not a parse error — pin the message.
    Lexer lexer("test.c",
        "class SMP { private: static int32 secret = 1; }; "
        "int32 main() { return SMP::secret; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool hasE2009 = false;
    for (auto& e : analyzer.getErrors())
        if (e.message.find("private") != std::string::npos) hasE2009 = true;
    EXPECT_TRUE(hasE2009);
}

TEST(SliceSemTest, SMInstancePathRejected) {
    // DS1: static members are not instance members — pin the "no matching
    // method" diagnostic (a parse-level rejection would be wrong here).
    Lexer lexer("test.c",
        "class SMP2 { public: static int32 make() { return 1; } }; "
        "int32 main() { SMP2 obj; return obj.make(); }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool hasNoMethod = false;
    for (auto& e : analyzer.getErrors())
        if (e.message.find("no matching method") != std::string::npos) hasNoMethod = true;
    EXPECT_TRUE(hasNoMethod);
}

TEST(SliceSemTest, SMStaticInstanceSameName) {
    EXPECT_TRUE(analyzeOk(
        "class SMDual { "
        "public: int32 f(int32 x) { return x; } "
        "static int32 f() { return 42; } }; "
        "int32 main() { SMDual obj; return obj.f(1) + SMDual::f(); }"));
}

TEST(SliceSemTest, SMStaticOverload) {
    EXPECT_TRUE(analyzeOk(
        "class SMOvl { public: static int32 g(int32 x) { return x; } "
        "static int32 g(int32 x, int32 y) { return x + y; } }; "
        "int32 main() { return SMOvl::g(1) + SMOvl::g(1, 2); }"));
}

TEST(SliceSemTest, SMStaticVarNoInitZero) {
    EXPECT_TRUE(analyzeOk(
        "class SMZ { public: static int32 z; }; "
        "int32 main() { return SMZ::z; }"));
}

TEST(SliceSemTest, SMStaticNotInLayout) {
    // DS2: static variables do not occupy object layout — and the member
    // must actually exist (reference it so a silent parser drop fails).
    EXPECT_TRUE(analyzeOk(
        "class SML { public: static int32 big = 0; int32 a; }; "
        "int32 main() { return (sizeof(SML) == sizeof(int32) && SML::big == 0) ? 0 : 1; }"));
}

TEST(SliceSemTest, SMStaticCollisionRejected) {
    // Review Focus 5: hand-written SMC_v collides with the desugared
    // SMC::v — a redefinition error, never silent shadowing.
    EXPECT_FALSE(analyzeOk(
        "class SMC { public: static int32 v = 0; }; "
        "int32 SMC_v = 1; int32 main() { return 0; }"));
}

TEST(SliceSemTest, SMStaticInNamespace) {
    // Review Focus 1: declaration and access sides must agree on the ns prefix.
    EXPECT_TRUE(analyzeOk(
        "namespace smns { class SMN { public: static int32 v = 3; }; } "
        "int32 main() { return smns::SMN::v; }"));
}

// Review C2: a struct whose ONLY members are static must not fall into the
// parser's forward-declaration fallback (which dropped the definition).
TEST(SliceSemTest, SMStructStaticOnly) {
    EXPECT_TRUE(analyzeOk(
        "struct SMS { static int32 v = 4; }; "
        "int32 main() { return SMS::v; }"));
}

TEST(SliceSemTest, SMStructStaticMixed) {
    EXPECT_TRUE(analyzeOk(
        "struct SMS2 { static int32 v = 4; int32 x; }; "
        "int32 main() { SMS2 s; s.x = 1; return SMS2::v + s.x; }"));
}

// Review I1: same-name static and instance members must keep independent
// access levels. Two assertions:
//  - the PUBLIC instance member is not corrupted by the private static's
//    level (no false E2009 on obj.f(1));
//  - the static member's own private level is honored (E2009 on SMI1::f()).
TEST(SliceSemTest, SMAccessLevelIndependent) {
    EXPECT_TRUE(analyzeOk(
        "class SMI1 { "
        "public: int32 f(int32 x) { return x; } "
        "private: static int32 f() { return 42; } }; "
        "int32 main() { SMI1 obj; return obj.f(1); }"));
}

TEST(SliceSemTest, SMPrivateStaticMethodRejected) {
    Lexer lexer("test.c",
        "class SMI3 { "
        "public: int32 f(int32 x) { return x; } "
        "private: static int32 f() { return 42; } }; "
        "int32 main() { return SMI3::f(); }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool hasE2009 = false;
    for (auto& e : analyzer.getErrors())
        if (e.message.find("private") != std::string::npos) hasE2009 = true;
    EXPECT_TRUE(hasE2009);
}

// Review I2: a static method may call a sibling static method declared
// LATER in the class body (declaration order must not matter, matching
// instance methods).
TEST(SliceSemTest, SMStaticBackwardReference) {
    EXPECT_TRUE(analyzeOk(
        "class SMI2 { "
        "public: static int32 a() { return SMI2::b(); } "
        "static int32 b() { return 7; } }; "
        "int32 main() { return SMI2::a(); }"));
}

// ========== AGG-11: nested types — parser collects & registers under flat keys ==========

TEST(SliceSemTest, NTNestedStructTypeRef) {
    EXPECT_TRUE(analyzeOk(
        "class NTO { public: struct Inner { int32 x; }; }; "
        "int32 main() { NTO::Inner obj; obj.x = 3; return obj.x == 3 ? 0 : 1; }"));
}

TEST(SliceSemTest, NTEnumDoesNotEatFields) {
    // 现状缺陷：成员循环 parseType 消费内联 enum 后因无标识符 break，
    // 后续字段全部丢失。钉住修复。
    EXPECT_TRUE(analyzeOk(
        "class NTF { public: enum Color { Red, Green }; int32 x; }; "
        "int32 main() { NTF o; o.x = 1; return o.x == 1 ? 0 : 1; }"));
}

TEST(SliceSemTest, NTDeepNested) {
    EXPECT_TRUE(analyzeOk(
        "class NTD { public: struct A { struct B { int32 v; }; }; }; "
        "int32 main() { NTD::A::B obj; obj.v = 2; return obj.v == 2 ? 0 : 1; }"));
}

TEST(SliceSemTest, NTUnionNested) {
    EXPECT_TRUE(analyzeOk(
        "class NTU { public: union U { int32 a; float32 b; }; }; "
        "int32 main() { NTU::U u; u.a = 1; return u.a == 1 ? 0 : 1; }"));
}

TEST(SliceSemTest, NTFieldOfNestedType) {
    // 类体内字段用裸嵌套类型名（解析期前缀查找的自然结果）。
    EXPECT_TRUE(analyzeOk(
        "class NTFLD { public: struct Inner { int32 x; }; Inner field; }; "
        "int32 main() { NTFLD o; o.field.x = 5; return o.field.x == 5 ? 0 : 1; }"));
}

TEST(SliceSemTest, NTStructInStruct) {
    EXPECT_TRUE(analyzeOk(
        "struct NTOS { struct Inner { int32 x; }; }; "
        "int32 main() { NTOS::Inner obj; obj.x = 3; return obj.x == 3 ? 0 : 1; }"));
}

TEST(SliceSemTest, NTNestedClassMethod) {
    // 方法表在 sema 填充——Task 1 预期 RED，Task 2 转绿。
    // 注：用 class 而非 struct——本语言 struct 无方法通路（顶层同样），
    // 带方法的嵌套类型须为 class（plan 缺陷修正，Ruling 已记）。
    EXPECT_TRUE(analyzeOk(
        "class NTM { public: class S { public: int32 f(int32 x) { return x; } }; }; "
        "int32 main() { NTM::S obj; return obj.f(7) == 7 ? 0 : 1; }"));
}

TEST(SliceSemTest, NTForwardDeclPin) {
    // DS6：前向声明（含嵌套类前向）+ 定义 + 使用；枚举/union 前向解析可用。
    // 既有能力 pin——Task 1 期预期 PASS。
    EXPECT_TRUE(analyzeOk(
        "struct NF1 { int32 x; }; "
        "class NF2; class NF2 { public: int32 y; }; "
        "enum NF3 : uint8; union NF4; "
        "int32 main() { NF1 a; a.x = 1; NF2 b; b.y = 2; "
        "return (a.x == 1 && b.y == 2) ? 0 : 1; }"));
}

// ========== AGG-11 Task 2: sema traversal + classPathPrefix + enum constants + E2009 ==========

TEST(SliceSemTest, NTNestedEnumBare) {
    // 常量注册外层类作用域（DS4 修订）：Outer::Red；顶层 Mode::Red 不存在。
    EXPECT_TRUE(analyzeOk(
        "class NTE { public: enum Color { Red, Green }; int32 x; }; "
        "int32 main() { NTE::Color c = NTE::Red; NTE o; o.x = 1; "
        "return (o.x == 1 && c == 0) ? 0 : 1; }"));
}

TEST(SliceSemTest, NTNestedEnumConstantNotGlobal) {
    // Review Focus 4：类外裸名 Red 不可用（pin，预期 Task 1 期即 PASS）。
    EXPECT_FALSE(analyzeOk(
        "class NTG { public: enum Color { Red }; }; int32 main() { return Red; }"));
}

TEST(SliceSemTest, NTStaticInNestedClass) {
    EXPECT_TRUE(analyzeOk(
        "class NTS { public: struct S { static int32 v = 3; }; }; "
        "int32 main() { return NTS::S::v; }"));
}

TEST(SliceSemTest, NTStaticInNestedNsClass) {
    // Review Focus 3：ns × 类嵌套组合双侧符号一致。
    EXPECT_TRUE(analyzeOk(
        "namespace ntns { class NTN { public: struct S { static int32 v = 3; }; }; } "
        "int32 main() { return ntns::NTN::S::v; }"));
}

TEST(SliceSemTest, NTPrivateNestedTypeRejected) {
    Lexer lexer("test.c",
        "class NTP { private: struct Secret { int32 x; }; }; "
        "int32 main() { NTP::Secret s; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool hasE2009 = false;
    for (auto& e : analyzer.getErrors())
        if (e.message.find("private") != std::string::npos) hasE2009 = true;
    EXPECT_TRUE(hasE2009);
}

TEST(SliceSemTest, NTPrivateNestedTypeInternalOk) {
    EXPECT_TRUE(analyzeOk(
        "class NTP2 { private: struct Secret { int32 x; }; "
        "public: int32 probe() { Secret s; s.x = 4; return s.x; } }; "
        "int32 main() { NTP2 o; return o.probe() == 4 ? 0 : 1; }"));
}

TEST(SliceSemTest, NTCastPrivateNestedRejected) {
    // Review Focus 5：cast 目标类型检查（malloc 堆用法主通道）。
    EXPECT_FALSE(analyzeOk(
        "class NTP3 { private: struct Secret { int32 x; }; }; "
        "int32 main() { return (NTP3::Secret*)0 ? 1 : 0; }"));
}

TEST(SliceSemTest, NTStaticNestedFactory) {
    // 嵌套类 static 方法 + 实例字段组合。注意：刻意避开 self-type——
    // 嵌套类型注册发生在 decl 解析器返回之后，其体内（含限定名
    // `NTF2::S*`）无法自引用（顶层类同样存在的既有缺口，Ruling 已记）。
    EXPECT_TRUE(analyzeOk(
        "class NTF2 { public: class S { public: int32 x; "
        "static int32 answer() { return 42; } }; "
        "int32 probe() { NTF2::S s; s.x = NTF2::S::answer(); return s.x; } }; "
        "int32 main() { NTF2 o; return o.probe() == 42 ? 0 : 1; }"));
}

// ========== AGG-11 评审修复轮：C1 union bareName / C2 嵌套类前向 / I1 enum 底层类型 / I2 数组绕过 ==========

TEST(SliceSemTest, NTUnionNestedStatic) {
    // C1：union 定义分支漏记 bareName 导致 union 嵌套 static 符号双侧断裂。
    EXPECT_TRUE(analyzeOk(
        "class NTUN { public: union U { struct I { static int32 v = 3; }; }; }; "
        "int32 main() { return NTUN::U::I::v; }"));
}

TEST(SliceSemTest, NTUnionNestedNsStatic) {
    // C1 的 ns 形态：ns_U_I_v 而非 ns_ns_U_I_v。
    EXPECT_TRUE(analyzeOk(
        "namespace ntns2 { union NTUN2 { struct I { static int32 v = 3; }; }; } "
        "int32 main() { return ntns2::NTUN2::I::v; }"));
}

TEST(SliceSemTest, NTClassFwdInClass) {
    // C2：嵌套类前向声明 `class D;` 不消费分号 → 成员循环 break 吞掉后续成员。
    EXPECT_TRUE(analyzeOk(
        "class NTFC { public: class D; class D { public: int32 y; }; int32 x; }; "
        "int32 main() { NTFC o; NTFC::D d; d.y = 5; o.x = 6; "
        "return (d.y == 5 && o.x == 6) ? 0 : 1; }"));
}

TEST(SliceSemTest, NTEnumUnderlyingInClass) {
    // I1：成员位置 `enum E : 底层类型 { ... }` 未路由 → 吞成员。
    EXPECT_TRUE(analyzeOk(
        "class NTEU { public: enum Color : uint8 { Red, Green }; int32 x; }; "
        "int32 main() { NTEU o; NTEU::Color c = NTEU::Red; o.x = 1; "
        "return (c == 0 && o.x == 1) ? 0 : 1; }"));
}

TEST(SliceSemTest, NTPrivateNestedArrayRejected) {
    // I2：private 嵌套类型经数组声明绕过 E2009。
    EXPECT_FALSE(analyzeOk(
        "class NTP4 { private: struct Secret { int32 x; }; }; "
        "int32 main() { NTP4::Secret arr[3]; return 0; }"));
}

// ========== INH: inheritance — parser ==========

TEST(SliceSemTest, INHStructInheritParses) {
    // 解析级：struct 继承子句被接受且 baseClass 记录，后续成员不丢。
    Lexer lexer("test.c",
        "struct B1 { int32 x; }; struct D1 : public B1 { int32 y; };");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    EXPECT_TRUE(parser.getErrors().empty());
    // 第二个声明是 D1，baseClass == "B1"，fields 含 y。
    ASSERT_GE(ast->declarations.size(), 2u);
    auto* d1 = dynamic_cast<StructDeclAST*>(ast->declarations[1].get());
    ASSERT_NE(d1, nullptr);
    EXPECT_EQ(d1->baseClass, "B1");
    ASSERT_EQ(d1->fields.size(), 1u);
    EXPECT_EQ(d1->fields[0].name, "y");
}

TEST(SliceSemTest, INHMultiInheritRejected) {
    // INH-02：多继承给明确诊断（今天静默误解析为前向声明）。
    Lexer lexer("test.c",
        "class A2 { public: int32 x; }; class B2 { public: int32 y; }; "
        "class C2 : public A2, public B2 { public: int32 z; };");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    EXPECT_FALSE(parser.getErrors().empty());
}

TEST(SliceSemTest, INHPrivateInheritRejected) {
    // DS2：private/protected 继承诊断不支持（struct 与 class 两侧）。
    Lexer lexer("test.c",
        "class B3 { public: int32 x; }; class D3 : private B3 { public: int32 y; };");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    EXPECT_FALSE(parser.getErrors().empty());
}

TEST(SliceSemTest, INHProtectedInheritRejected) {
    Lexer lexer("test.c",
        "struct B4 { int32 x; }; struct D4 : protected B4 { int32 y; };");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    EXPECT_FALSE(parser.getErrors().empty());
}

TEST(SliceSemTest, INHClassBareInheritPins) {
    // DS2：class 缺省说明符 = public，不诊断（用户裁决，偏离 C++ 记文档）。
    // 端到端 pin（class 继承骨架今天已通，预期 Task 1 期即 PASS）。
    EXPECT_TRUE(analyzeOk(
        "class Base5 { public: int32 x; }; class D5 : Base5 { public: int32 y; }; "
        "int32 main() { D5 d; d.x = 1; d.y = 2; "
        "return (d.x == 1 && d.y == 2) ? 0 : 1; }"));
}

TEST(SliceSemTest, INHStructBareInheritParses) {
    // struct 缺省说明符同样 = public。
    Lexer lexer("test.c",
        "struct B6 { int32 x; }; struct D6 : B6 { int32 y; };");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    EXPECT_TRUE(parser.getErrors().empty());
    auto* d6 = dynamic_cast<StructDeclAST*>(ast->declarations[1].get());
    ASSERT_NE(d6, nullptr);
    EXPECT_EQ(d6->baseClass, "B6");
}

// ========== INH: inheritance — sema（方案乙） ==========

TEST(SliceSemTest, INHMethodChainSema) {
    // 沿链方法调用（不依赖方法表复制；Task 1 后 struct/class 解析已通）。
    EXPECT_TRUE(analyzeOk(
        "class B7 { public: int32 f(int32 v) { return v; } }; "
        "class D7 : public B7 { public: int32 y; }; "
        "int32 main() { D7 d; return d.f(5) == 5 ? 0 : 1; }"));
}

TEST(SliceSemTest, INHPrivateBaseMethodRejected) {
    // 方案乙行为收紧 pin：private 基方法经派生实例从类外调用 → E2009。
    // 今天（复制机制）级别丢失放行 → RED；沿链后定义类级别生效 → GREEN。
    EXPECT_FALSE(analyzeOk(
        "class B8 { private: int32 m() { return 1; } }; "
        "class D8 : public B8 { public: int32 z; }; "
        "int32 main() { D8 d; return d.m(); }"));
}

TEST(SliceSemTest, INHBaseStaticProtectedPin) {
    // INH-06: 派生类方法内经 this->/Base:: 访问基类 protected static 成员
    // 现放行（2026-10-18 放宽）；类外对象访问仍拒。
    EXPECT_TRUE(analyzeOk(
        "class Base9 { protected: static int32 ps = 7; }; "
        "class D9 : public Base9 { public: int32 get() { return Base9::ps; } }; "
        "int32 main() { D9 d; return d.get(); }"));
    EXPECT_FALSE(analyzeOk(
        "class Base9b { protected: static int32 ps = 7; }; "
        "int32 main() { return Base9b::ps; }"));
}

TEST(SliceSemTest, INHStructFieldChainSema) {
    // struct 字段沿链（Type.h StructType::base + sema walk）。
    EXPECT_TRUE(analyzeOk(
        "struct B10 { int32 x; }; struct D10 : public B10 { int32 y; }; "
        "int32 main() { D10 d; d.x = 1; d.y = 2; "
        "return (d.x == 1 && d.y == 2) ? 0 : 1; }"));
}

TEST(SliceSemTest, INHBaseUndefSema) {
    // Review Focus 5：基类未定义 → 诊断而非崩溃。
    EXPECT_FALSE(analyzeOk(
        "class Undef11; class D11 : public Undef11 { public: int32 y; }; "
        "int32 main() { D11 d; d.y = 1; return d.y; }"));
}

TEST(SliceSemTest, INHOverloadChainSema) {
    // Review Focus 3（I1 裁决）：C++ 名字隐藏语义——本类查找（含隐式转换）
    // 优先于跨跳，派生 f(float64) 遮蔽基 f(int32)，d.f(5) 命中派生；
    // 经基指针调用命中基类定义（definingClass 按定义点归属）。
    EXPECT_TRUE(analyzeOk(
        "class B13 { public: int32 f(int32 v) { return 100; } }; "
        "class D13 : public B13 { public: float64 f(float64 v) { return 0.5; } }; "
        "int32 main() { D13 d; B13* b = &d; "
        "return (d.f(5) == 0.5 && b->f(5) == 100) ? 0 : 1; }"));
}

TEST(SliceSemTest, INHStructAnonPromote) {
    // Review Focus 1：struct 继承 × 匿名内联字段（AGG-03 promote）共存。
    EXPECT_TRUE(analyzeOk(
        "struct B14 { int32 x; }; "
        "struct D14 : public B14 { union { int32 a; float32 b; }; int32 c; }; "
        "int32 main() { D14 d; d.x = 1; d.a = 2; d.c = 3; "
        "return (d.x == 1 && d.a == 2 && d.c == 3) ? 0 : 1; }"));
}

TEST(SliceSemTest, INHCircularSema) {
    // Review Focus 4：循环继承诊断不回归、不挂死。
    EXPECT_FALSE(analyzeOk(
        "class CA12 : public CB12 { public: int32 x; }; "
        "class CB12 : public CA12 { public: int32 y; }; "
        "int32 main() { return 0; }"));
}

// ========== INH 评审修复轮：C1 struct 环检测 / I1 重载遮蔽裁决 / I2 自继承 pin ==========

TEST(SliceSemTest, INHStructCircularRedefined) {
    // C1：struct 重定义形态跨类成环——今天静默接受且沿链 walk 挂死/崩溃。
    // 修复后：环检测覆盖 struct 链 → 诊断。
    EXPECT_FALSE(analyzeOk(
        "struct B31 { int32 b; }; struct A31 : B31 { int32 a; }; "
        "struct B31 : A31 { int32 c; }; "
        "int32 main() { A31 x; x.b = 1; return x.b; }"));
}

TEST(SliceSemTest, INHSelfInheritClassPin) {
    // I2：自继承 pin（class 形态，今天已诊断，防回归）。
    EXPECT_FALSE(analyzeOk(
        "class S32 : S32 { public: int32 x; }; int32 main() { return 0; }"));
}

TEST(SliceSemTest, INHSelfInheritStructPin) {
    // I2：自继承 pin（struct 形态）。
    EXPECT_FALSE(analyzeOk(
        "struct S33 : S33 { int32 x; }; int32 main() { return 0; }"));
}

// ========== Redef: 类型重复定义诊断（四类全收 + 跨种类） ==========

TEST(SliceSemTest, RedefStructRejected) {
    EXPECT_FALSE(analyzeOk(
        "struct B40 { int32 x; }; struct B40 { int32 y; }; int32 main() { return 0; }"));
}

TEST(SliceSemTest, RedefClassRejected) {
    EXPECT_FALSE(analyzeOk(
        "class B41 { public: int32 x; }; class B41 { public: int32 y; }; int32 main() { return 0; }"));
}

TEST(SliceSemTest, RedefUnionRejected) {
    EXPECT_FALSE(analyzeOk(
        "union U42 { int32 x; }; union U42 { float32 y; }; int32 main() { return 0; }"));
}

TEST(SliceSemTest, RedefEnumRejected) {
    EXPECT_FALSE(analyzeOk(
        "enum E43 { A, B }; enum E43 { C, D }; int32 main() { return 0; }"));
}

TEST(SliceSemTest, RedefCrossKindRejected) {
    // 类型名同一命名空间：struct B 后 class B 亦诊断（C++ 同规则）。
    EXPECT_FALSE(analyzeOk(
        "struct B44 { int32 x; }; class B44 { public: int32 y; }; int32 main() { return 0; }"));
}

TEST(SliceSemTest, RedefFwdThenDefPin) {
    // 前向声明 + 定义合法（占位 incomplete → 完成）。
    EXPECT_TRUE(analyzeOk(
        "struct B45; struct B45 { int32 x; }; int32 main() { B45 v; v.x = 1; return v.x; }"));
}

TEST(SliceSemTest, RedefFwdTwicePin) {
    // 前向声明可重复。
    EXPECT_TRUE(analyzeOk(
        "struct B46; struct B46; struct B46 { int32 x; }; int32 main() { B46 v; v.x = 1; return v.x; }"));
}

TEST(SliceSemTest, RedefNestedSameInnerNamePin) {
    // 不同外层类型的同名嵌套 inner（flat key 不同）不误报。
    EXPECT_TRUE(analyzeOk(
        "struct O47a { struct I47 { int32 x; }; }; "
        "struct O47b { struct I47 { int32 y; }; }; "
        "int32 main() { return 0; }"));
}

// ========== OPT/RES: P1-02 Optional/Result（Task 1 语法） ==========

TEST(SliceSemTest, OptResParseNamedOptional) {
    // `optional<T>` 显式形式必须与 `T?` 同型（spec §4.1）。
    EXPECT_TRUE(analyzeOk("int32 main() { optional<int32> o; return 0; }"));
}

TEST(SliceSemTest, OptResParseResult) {
    // 语法聚焦：Result 声明/形参/返回解析（成员访问属 Task 4，初始化属
    // Task 4/5 的聚合初始化语义）。
    EXPECT_TRUE(analyzeOk(
        "result<int32, int32> take(result<int32, int32> r) { return r; } "
        "int32 main() { result<int32, int32> x; return 0; }"));
}

TEST(SliceSemTest, OptResParseNestedArgs) {
    EXPECT_TRUE(analyzeOk(
        "int32 main() { result<int32, int32?> r; optional<optional<int32>> o; return 0; }"));
}

TEST(SliceSemTest, OptMismatchRejected) {
    // 同 kind 泛化规则今天会误放行：optional<int32> ≠ optional<float64>。
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32? a = {true, 5}; float64? b = a; return 0; }"));
}

TEST(SliceSemTest, ResMismatchRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { result<int32, int32> a; result<int32, float64> b = a; return 0; }"));
}

TEST(SliceSemTest, OptToPlainRejected) {
    // T? ↔ T 无隐式转换（spec §4.3）。现状即拒（kind 不同），转 pin。
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32? a = {true, 5}; int32 b = a; return 0; }"));
}

TEST(SliceSemTest, OptNominalEqualPin) {
    // 同实参拷贝/赋值放行（成员访问 pin 在 Task 4）。
    EXPECT_TRUE(analyzeOk(
        "int32 main() { int32? a = {true, 5}; int32? b = a; b = a; return 0; }"));
}

// ---------- OPT/RES Task 4: 伪字段访问 + 初始化列表校验 ----------

TEST(SliceSemTest, OptMemberFieldTypes) {
    EXPECT_TRUE(analyzeOk(
        "int32 main() { int32? o = {true, 5}; bool v = o.valid; int32 x = o.value; return x; }"));
}

TEST(SliceSemTest, OptUnknownMemberRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32? o = {true, 5}; int32 x = o.nope; return x; }"));
}

TEST(SliceSemTest, ResMemberFieldTypes) {
    EXPECT_TRUE(analyzeOk(
        "int32 main() { result<int32, int32> r = {false, 0, 1}; bool ok = r.ok; "
        "int32 v = r.value; int32 e = r.error; return e; }"));
}

TEST(SliceSemTest, ResUnknownMemberRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { result<int32, int32> r; int32 x = r.valid; return x; }"));
}

TEST(SliceSemTest, OptInitListArityRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32? o = {true}; return 0; }"));
}

TEST(SliceSemTest, OptInitListValueMismatchRejected) {
    // value 槽位类型错配（true/false 是 int32 字面量，{5, true} 与 {1, 1}
    // 在类型层不可区分、C 语义合法——不钉顺序，钉 value 类型）。
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32? o = {true, 5.0}; return 0; }"));
}

TEST(SliceSemTest, ResInitListArityRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { result<int32, int32> r = {false, 0}; return 0; }"));
}

TEST(SliceSemTest, OptMemberWritablePin) {
    EXPECT_TRUE(analyzeOk(
        "int32 main() { int32? o = {true, 5}; o.value = 6; o.valid = false; return 0; }"));
}

// ========== OPT/RES 评审修复（2026-10-06，C1/I1/I2/I4/I5） ==========

TEST(SliceSemTest, OptComparisonRejected) {
    // 评审 I1：Optional 上 ==/</&& 曾崩编译器（ICmp 打在 struct 上）。
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32? a = {true, 5}; int32? b = {true, 5}; bool r = a == b; return 0; }"));
}

TEST(SliceSemTest, OptLtRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32? a = {true, 5}; int32? b = {false, 0}; bool r = a < b; return 0; }"));
}

TEST(SliceSemTest, OptLogicalAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int32 main() { int32? a = {true, 5}; int32? b = {false, 0}; bool r = a && b; return 0; }"));
}

TEST(SliceSemTest, OptTypedefArgPin) {
    // 评审 I2：init-list 校验须剥别名。
    EXPECT_TRUE(analyzeOk(
        "using MyInt = int32; int32 main() { optional<MyInt> o = {true, 5}; return 0; }"));
}

TEST(SliceSemTest, OptTypedefValuePin) {
    EXPECT_TRUE(analyzeOk(
        "using MyInt = int32; int32 main() { MyInt x = 5; int32? p = {true, x}; return x; }"));
}

TEST(SliceSemTest, OptTernaryBranchAllowed) {
    // 三元根因修复后撤销 I4 拒绝：同型 Optional 分支按值 phi（e2e 执行见
    // TernaryE2E.OptionalBranch）。
    EXPECT_TRUE(analyzeOk(
        "int32 main() { int32? a = {true, 5}; int32? b = {false, 0}; "
        "int32? c = a.valid ? a : b; return 0; }"));
}

TEST(SliceSemTest, OptRvalueAssignRejected) {
    // 评审 I5：rvalue 伪字段赋值曾生成 store 到非指针。
    EXPECT_FALSE(analyzeOk(
        "int32? make(int32 v) { int32? t = {v != 0, 7}; return t; } "
        "int32 main() { make(1).value = 5; return 0; }"));
}

TEST(SliceSemTest, TernaryIncompatibleBranchesRejected) {
    // 三元根因轮：非算术对且不兼容的分支须诊断（旧 getCommonType 静默取左）。
    EXPECT_FALSE(analyzeOk(
        "struct S { int32 x; };\n"
        "int32 main() { S s = {1}; int32 t = 1; S c = t ? s : 5; return 0; }"));
}

// ========== P1-03 / GEN-03: 类模板实例化（Tpl* 前缀防 TypeContext 碰撞） ==========

TEST(TplSemTest, ClassTemplateFieldAccessOk) {
    TemplateRegistry::instance().resetForTesting();
    EXPECT_TRUE(analyzeOk(
        "template<typename T> struct Box { T value; };\n"
        "int32 main() { Box<int32> b; b.value = 3; return 0; }"));
}

TEST(TplSemTest, ClassTemplateTypeMismatch) {
    TemplateRegistry::instance().resetForTesting();
    // 实例点检查：T=int32 后给 `b.value` 赋聚合类型是类型错误（GEN-06）。
    // （float→int 赋值在 C 语义下合法窄化，不能用作失配用例。）
    EXPECT_FALSE(analyzeOk(
        "template<typename T> struct Box { T value; };\n"
        "int32 main() { Box<int32> b; Box<float64> c; b.value = c; return 0; }"));
}

TEST(TplSemTest, InstanceErrorDualLoc) {
    TemplateRegistry::instance().resetForTesting();
    // 实例体内错误：诊断消息须含 `in instantiation of`（双 loc 的消息侧）。
    // 方法承载类型须用 class（该语言 struct 无方法——解析器只在 class
    // 路径解析方法）。
    Lexer lexer("tpl.c",
        "template<typename T> class B { T x; T bad() { return oops; } };\n"
        "int32 main() { B<int32> b; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    bool found = false;
    for (auto& e : analyzer.getErrors()) {
        if (e.message.find("in instantiation of") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(TplSemTest, BareTemplateNameRejected) {
    TemplateRegistry::instance().resetForTesting();
    // 裸模板名（无实参）在类型位置须报错，不静默解析。
    EXPECT_FALSE(analyzeOk(
        "template<typename T> struct Box { T value; };\n"
        "int32 main() { Box b; return 0; }"));
}

// ========== P1-03 / GEN-03/06: 函数模板显式与推导（TplFn* 前缀） ==========

// 完整管线检查：parse 诊断 + sema 诊断都须为空（表达式位置的模板调用
// 若投机解析失败会回退成比较表达式并产生 parse 错误，analyzeOk 会漏报）。
static bool analyzeFullyOk(const std::string& source) {
    Lexer lexer("t.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast || !parser.getErrors().empty()) return false;
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    return analyzer.getErrors().empty();
}

TEST(TplFnSemTest, ExplicitFuncTemplateOk) {
    TemplateRegistry::instance().resetForTesting();
    EXPECT_TRUE(analyzeFullyOk(
        "template<typename T> T tmax(T a, T b) { return a; }\n"
        "int32 main() { return tmax<int32>(3, 4); }"));
}

TEST(TplFnSemTest, DeducedFuncTemplateOk) {
    TemplateRegistry::instance().resetForTesting();
    EXPECT_TRUE(analyzeFullyOk(
        "template<typename T> T tmax(T a, T b) { return a; }\n"
        "int32 main() { return tmax(3, 4); }"));
}

TEST(TplFnSemTest, PointerParamDeduction) {
    TemplateRegistry::instance().resetForTesting();
    // T* 形参按去指针类型推导（Review Focus 钉死项）。
    EXPECT_TRUE(analyzeFullyOk(
        "template<typename T> T* pick(T* a) { return a; }\n"
        "int32 main() { int32 v = 5; int32* p = pick(&v); return *p; }"));
}

TEST(TplFnSemTest, ConflictingDeduction) {
    TemplateRegistry::instance().resetForTesting();
    Lexer lexer("t.c",
        "template<typename T> T tmax(T a, T b) { return a; }\n"
        "int32 main() { return tmax(3, 0.5); }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    bool found = false;
    for (auto& e : analyzer.getErrors()) {
        if (e.message.find("conflicting deduction for 'T'") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(TplFnSemTest, Undeducible) {
    TemplateRegistry::instance().resetForTesting();
    Lexer lexer("t.c",
        "template<typename T> T make() { T x; return x; }\n"
        "int32 main() { return make(); }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    bool found = false;
    for (auto& e : analyzer.getErrors()) {
        if (e.message.find("cannot deduce template argument") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

// ========== P1-03 / GEN-02: 非类型参数与别名模板（TplArr* 前缀） ==========

TEST(TplArrSemTest, NonTypeParamArrayOk) {
    TemplateRegistry::instance().resetForTesting();
    EXPECT_TRUE(analyzeFullyOk(
        "template<typename T, usize N> struct Arr { T data[N]; };\n"
        "int32 main() { Arr<int32, 4> a; a.data[0] = 1; return 0; }"));
}

TEST(TplArrSemTest, AliasEquivalent) {
    TemplateRegistry::instance().resetForTesting();
    EXPECT_TRUE(analyzeFullyOk(
        "template<typename T, usize N> struct Arr { T data[N]; };\n"
        "template<typename T> using Vec = Arr<T, 8>;\n"
        "int32 main() { Vec<int32> v; Arr<int32, 8> a = v; v = a; return 0; }"));
}

TEST(TplArrSemTest, NonIntegralValueParamRejected) {
    TemplateRegistry::instance().resetForTesting();
    Lexer lexer("t.c",
        "template<typename T, usize N> struct Arr { T data[N]; };\n"
        "int32 main() { Arr<int32, 0.5> a; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    bool found = false;
    for (auto& d : parser.getErrors()) {
        if (d.message.find("non-type template argument must be an integer constant") !=
            std::string::npos)
            found = true;
    }
    if (!ast) found = true; // parse 整体失败也计拒绝
    ASSERT_TRUE(found);
}

// ========== P1-03 / PAR-17: this 表达式（TplThis* 前缀） ==========

TEST(TplThisSemTest, ThisInStaticRejected) {
    TemplateRegistry::instance().resetForTesting();
    Lexer lexer("t.c",
        "class A { public: static int32 f() { return this.v; } };\n"
        "int32 main() { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    bool found = false;
    for (auto& e : analyzer.getErrors()) {
        if (e.message.find("'this' is not valid in a static method") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}

// ========== P1-03 / INH-05: CRTP 递归值字段拒绝（TplCrtp* 前缀） ==========

TEST(TplCrtpSemTest, CrtpRecursiveValueFieldRejected) {
    TemplateRegistry::instance().resetForTesting();
    Lexer lexer("t.c",
        "template<typename D> class Base2 { D next; };\n"
        "class Derived2 : Base2<Derived2> { public: int32 v; };\n"
        "int32 main() { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    bool found = false;
    for (auto& e : analyzer.getErrors()) {
        if (e.message.find("recursive instantiation") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

// ========== P1-03 评审修复轮（fix-first） ==========

TEST(TplFixSemTest, ExplicitArgsSuppressDeduction) {
    // I1：显式实参定位后推导跳过（plan 钉死规则）——字面量不再冲突。
    TemplateRegistry::instance().resetForTesting();
    EXPECT_TRUE(analyzeFullyOk(
        "template<typename T> T tmax(T a, T b) { return a; }\n"
        "int32 main() { return tmax<float64>(3, 4); }"));
}

TEST(TplFixSemTest, DeepNestingDepthLimit) {
    // I2：真实 parser→sema 路径上的深度上限（100 层须诊断，64 内合法）。
    TemplateRegistry::instance().resetForTesting();
    std::string src = "template<typename T> struct Box { T value; };\n";
    std::string inner = "int32";
    for (int i = 0; i < 100; ++i) inner = "Box<" + inner + ">";
    src += "int32 main() { " + inner + " b; return 0; }";
    Lexer lexer("t.c", src);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    bool found = false;
    for (auto& e : analyzer.getErrors()) {
        if (e.message.find("template instantiation depth limit exceeded") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}

// P1-06 / T1: the capitalized builtin spellings are no longer types — parse
// yields a template-instance placeholder (P1-03), and sema rejects it.
TEST_F(SemanticAnalyzerTest, UppercaseBuiltinNoLongerAType) {
    EXPECT_FALSE(analyzeFullyOk("Optional<int32> x;"));
    EXPECT_FALSE(analyzeFullyOk("Result<int32, bool> r;"));
}

// P1-06 / T2: a string literal's type is `str` (FMT-03 retype).
TEST_F(SemanticAnalyzerTest, StringLiteralIsStr) {
    Lexer lexer("t.c", "string_view s = \"hi\";");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
    auto* var = dynamic_cast<VarDeclAST*>(ast->declarations[0].get());
    ASSERT_NE(var, nullptr);
    ASSERT_NE(var->initExpr, nullptr);
    ASSERT_NE(var->initExpr->type, nullptr);
    EXPECT_EQ(var->initExpr->type->kind, TypeKind::Str);
}

// P1-06 / T3: str builtin method diagnostics.
TEST_F(SemanticAnalyzerTest, StrMethodDiagnostics) {
    auto check = [&](const std::string& src, const std::string& needle) {
        Lexer lexer("t.c", src);
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        auto ast = parser.parse();
        ASSERT_NE(ast, nullptr);
        SemanticAnalyzer analyzer;
        analyzer.analyze(*ast);
        bool found = false;
        for (const auto& d : analyzer.getErrors()) {
            if (d.message.find(needle) != std::string::npos) found = true;
        }
        EXPECT_TRUE(found) << "missing '" << needle << "' for: " << src;
    };
    check("int32 main() { string_view s = \"a\"; return s.bogus(); }",
          "no member named 'bogus' in string_view");
    check("int32 main() { string_view s = \"a\"; return s.len(1); }",
          "takes no arguments");
    check("int32 main() { string_view s = \"a\"; return str_from_c(42); }",
          "str_from_c");
}

// P1-06 / T3: str_from_c accepts char* only (no implicit str -> str_from_c).
TEST_F(SemanticAnalyzerTest, StrFromCAcceptsCharPointer) {
    EXPECT_TRUE(analyzeFullyOk(
        "int32 main() { const char* p = \"abc\"; string_view s = str_from_c(p); return 0; }"));
    EXPECT_TRUE(analyzeFullyOk(
        "int32 main() { string_view s = str_from_c(\"abc\"); return 0; }"));
}

// P1-06 / T4: no implicit heap allocation into `string` (DEC-20).
TEST_F(SemanticAnalyzerTest, StringNoImplicitAllocation) {
    auto check = [&](const std::string& src, const std::string& needle) {
        Lexer lexer("t.c", src);
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        auto ast = parser.parse();
        ASSERT_NE(ast, nullptr);
        SemanticAnalyzer analyzer;
        analyzer.analyze(*ast);
        bool found = false;
        for (const auto& d : analyzer.getErrors()) {
            if (d.message.find(needle) != std::string::npos) found = true;
        }
        EXPECT_TRUE(found) << "missing '" << needle << "' for: " << src;
    };
    check("int32 main() { string s = \"a\"; return 0; }", "string.new");
    check("int32 main() { char* p; string s = p; return 0; }", "string.new");
}

// P1-06 / T5 (FMT-04): UTF-8 validation at the literal point (E2015).
TEST_F(SemanticAnalyzerTest, Utf8LiteralValidation) {
    auto e2015 = [&](const std::string& src) {
        Lexer lexer("t.c", src);
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        auto ast = parser.parse();
        if (!ast) return false;
        SemanticAnalyzer analyzer;
        analyzer.analyze(*ast);
        bool found = false;
        for (const auto& d : analyzer.getErrors()) {
            if (diagnosticId(d.code) == "E2015") found = true;
        }
        return found;
    };
    // RFC 3629 subset: overlong, lone continuation, surrogate, out of range.
    EXPECT_TRUE(e2015("string_view s = \"\xC0\x80\";"));       // overlong NUL (C0 80)
    EXPECT_TRUE(e2015("string_view s = \"\x80\";"));           // lone continuation byte
    EXPECT_TRUE(e2015("string_view s = \"\xED\xA0\x80\";"));   // U+D800 surrogate
    EXPECT_TRUE(e2015("string_view s = \"\xF5\x80\x80\x80\";"));// > U+10FFFF
    EXPECT_TRUE(e2015("string_view s = r\"(\xFF)\";"));        // raw string is checked too
    EXPECT_FALSE(e2015("string_view s = \"\xE4\xB8\xAD\xF0\x9F\x8E\x89\";")); // 中🎉 valid
}

// P1-06 / T5: str_from_c validates a literal argument statically.
TEST_F(SemanticAnalyzerTest, StrFromCLiteralValidation) {
    Lexer lexer("t.c", "int32 main() { string_view s = str_from_c(\"\xFF\"); return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    bool found = false;
    for (const auto& d : analyzer.getErrors()) {
        if (diagnosticId(d.code) == "E2015") found = true;
    }
    EXPECT_TRUE(found);
}

// P1-06 / T6: DEC-18 semantics are TYPE-POSITION-ONLY — `str`/`string`/
// `optional`/`result` stay ordinary identifiers in value positions (same
// precedent as LegacyKeywordNamesAreUsableAsIdentifiers), and the builtins
// still recognize them in type positions.
TEST_F(SemanticAnalyzerTest, TypeNamesRemainUsableAsIdentifiers) {
    // DEC-18 hard rename precedent: the old `str` spelling is now an ordinary
    // identifier; the new `string_view` name is also TYPE-POSITION-ONLY.
    EXPECT_TRUE(analyzeFullyOk("int32 main() { int32 str = 1; return str; }"));
    EXPECT_TRUE(analyzeFullyOk("int32 main() { int32 string_view = 1; return string_view; }"));
    EXPECT_TRUE(analyzeFullyOk("int32 main() { int32 string = 1; return string; }"));
    EXPECT_TRUE(analyzeFullyOk("string_view g = \"x\"; int32 main() { string l = string.new(\"y\"); return 0; }"));
}

// P1-06 / T6: the empty literal is a valid (zero-length) str.
TEST_F(SemanticAnalyzerTest, EmptyLiteralIsStr) {
    EXPECT_TRUE(analyzeFullyOk("int32 main() { string_view s = \"\"; if (s.len() != 0) { return 1; } return 0; }"));
}

// 终审 C1（修复轮）：char* -> str 不得经初始化/return 隐式转换（spec §1.4：
// 仅经 str_from_c）。双向规则曾放行并产生垃圾 len。
TEST_F(SemanticAnalyzerTest, CharPointerToStrStillExplicit) {
    EXPECT_FALSE(analyzeFullyOk(
        "int32 main() { char* p = \"abc\"; string_view s = p; return 0; }"));
    EXPECT_FALSE(analyzeFullyOk(
        "string_view f(char* p) { return p; }\nint32 main() { return 0; }"));
}

// P1-09 / FMT-11: print/format spec failures carry stable codes E2020-E2022.
TEST_F(SemanticAnalyzerTest, FormatSpecDiagnosticCodes) {
    auto diagOf = [](const std::string& src) {
        Lexer lexer("t.c", src);
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        auto ast = parser.parse();
        if (!ast) return std::string("PARSE_FAIL");
        SemanticAnalyzer analyzer;
        analyzer.analyze(*ast);
        for (const auto& d : analyzer.getErrors()) {
            std::string id = diagnosticId(d.code);
            if (id == "E2020" || id == "E2021" || id == "E2022") return id;
        }
        return std::string("NONE");
    };

    // E2020: placeholder/argument count mismatch.
    EXPECT_EQ(diagOf("int32 main() { println(\"{} {}\", 1); return 0; }"), "E2020");
    EXPECT_EQ(diagOf("int32 main() { println(\"{}\", 1, 2); return 0; }"), "E2020");
    // E2021: spec does not match the argument kind.
    EXPECT_EQ(diagOf("int32 main() { println(\"{:x}\", 1.5); return 0; }"), "E2021");
    EXPECT_EQ(diagOf("int32 main() { println(\"{:.2}\", 1); return 0; }"), "E2021");
    // E2022: malformed or excluded spec text.
    EXPECT_EQ(diagOf("int32 main() { println(\"{:d}\", 1); return 0; }"), "E2022");
    EXPECT_EQ(diagOf("int32 main() { println(\"{:z}\", 1); return 0; }"), "E2022");
    // Legal specs still pass.
    EXPECT_TRUE(analyzeFullyOk(
        "int32 main() { println(\"{:x} {:>8} {:02} {:b}\", 255, 42, 5, 5); return 0; }"));
}

// P1-09 / FMT-07/09/10/11: builtin format() -> string, shared spec pipeline.
static std::string formatDiagId(const std::string& src) {
    Lexer lexer("t.c", src);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) return "PARSE_FAIL";
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    for (const auto& d : analyzer.getErrors()) {
        std::string id = diagnosticId(d.code);
        if (id == "E2020" || id == "E2021" || id == "E2022") return id;
    }
    return "NONE";
}

TEST_F(SemanticAnalyzerTest, BuiltinFormatSema) {
    // Literal format string with valid args.
    EXPECT_TRUE(analyzeFullyOk(
        "int32 main() { string s = format(\"x={}\", 1); return 0; }"));
    // Dynamic format string: legal, no compile-time spec checks (FMT-09).
    EXPECT_TRUE(analyzeFullyOk(
        "int32 main() { string_view f = \"{}\"; string s = format(f, 1); return 0; }"));
    // Custom types lower through to_string (FMT-10).
    EXPECT_TRUE(analyzeFullyOk(
        "class C { public: const char* to_string() { return \"C\"; } };"
        "int32 main() { C c; string s = format(\"{}\", c); return 0; }"));
    // A class without to_string cannot be formatted.
    EXPECT_FALSE(analyzeFullyOk(
        "class D { public: int32 x; };"
        "int32 main() { D d; string s = format(\"{}\", d); return 0; }"));
    // First argument must be str-typed.
    EXPECT_FALSE(analyzeFullyOk(
        "int32 main() { string s = format(1, 2); return 0; }"));
    // User-defined format shadows the builtin.
    EXPECT_TRUE(analyzeFullyOk(
        "string format(string_view f, int32 v) { return string.new(f); }"
        "int32 main() { string s = format(\"{}\", 1); return 0; }"));
}

TEST_F(SemanticAnalyzerTest, BuiltinFormatDiagnosticCodes) {
    // E2020: placeholder/argument count mismatch (literal fmt only).
    EXPECT_EQ(formatDiagId(
        "int32 main() { string s = format(\"{} {}\", 1); return 0; }"), "E2020");
    EXPECT_EQ(formatDiagId(
        "int32 main() { string s = format(\"{}\", 1, 2); return 0; }"), "E2020");
    // E2021: spec vs argument kind.
    EXPECT_EQ(formatDiagId(
        "int32 main() { string s = format(\"{:x}\", 1.5); return 0; }"), "E2021");
    EXPECT_EQ(formatDiagId(
        "int32 main() { string s = format(\"{:s}\", 1); return 0; }"), "E2021");
    // E2022: malformed or excluded spec text.
    EXPECT_EQ(formatDiagId(
        "int32 main() { string s = format(\"{:d}\", 1); return 0; }"), "E2022");
    EXPECT_EQ(formatDiagId(
        "int32 main() { string s = format(\"{:z}\", 1); return 0; }"), "E2022");
}

// P1-09 / STD-10: find/rfind/sub primitive type checks.
TEST_F(SemanticAnalyzerTest, StrFindRFindSubSema) {
    EXPECT_TRUE(analyzeFullyOk(
        "int32 main() { string_view s = \"hello\"; isize i = s.find(\"ll\"); return 0; }"));
    EXPECT_TRUE(analyzeFullyOk(
        "int32 main() { string_view s = \"a/b/c\"; isize i = s.rfind(\"/\"); return 0; }"));
    EXPECT_TRUE(analyzeFullyOk(
        "int32 main() { string_view s = \"hello\"; string_view t = s.sub(1, 3); return 0; }"));
    // Arity / type errors.
    EXPECT_FALSE(analyzeFullyOk(
        "int32 main() { string_view s = \"hello\"; s.find(); return 0; }"));
    EXPECT_FALSE(analyzeFullyOk(
        "int32 main() { string_view s = \"hello\"; s.find(1); return 0; }"));
    EXPECT_FALSE(analyzeFullyOk(
        "int32 main() { string_view s = \"hello\"; s.rfind(\"a\", 2); return 0; }"));
    EXPECT_FALSE(analyzeFullyOk(
        "int32 main() { string_view s = \"hello\"; s.sub(1.5, 2); return 0; }"));
    EXPECT_FALSE(analyzeFullyOk(
        "int32 main() { string_view s = \"hello\"; s.sub(1); return 0; }"));
}

// P1-09 / spec 偏差 1/2: builtin split/split_destroy.
TEST_F(SemanticAnalyzerTest, BuiltinSplitSema) {
    EXPECT_TRUE(analyzeFullyOk(
        "int32 main() { string_view[] parts = split(\"a,b\", \",\"); return 0; }"));
    EXPECT_TRUE(analyzeFullyOk(
        "int32 main() { string_view[] parts = split(\"a,b\", \",\"); split_destroy(parts); return 0; }"));
    // 非 str 实参
    EXPECT_FALSE(analyzeFullyOk(
        "int32 main() { string_view[] parts = split(1, \",\"); return 0; }"));
    EXPECT_FALSE(analyzeFullyOk(
        "int32 main() { string_view[] parts = split(\"a\", 2); return 0; }"));
    // 字面空 sep → 编译期拒绝
    EXPECT_FALSE(analyzeFullyOk(
        "int32 main() { string_view[] parts = split(\"a\", \"\"); return 0; }"));
    // 实参个数
    EXPECT_FALSE(analyzeFullyOk(
        "int32 main() { string_view[] parts = split(\"a\"); return 0; }"));
    // user-defined 优先
    EXPECT_TRUE(analyzeFullyOk(
        "string_view split(string_view s, string_view sep) { return s; }"
        "int32 main() { return 0; }"));
}

// I1: split_destroy 只接受 []str（split 从不产生 []string；放宽会配合数组
// decay 对栈上 string 数组指针 free）。
TEST_F(SemanticAnalyzerTest, SplitDestroyRejectsStringSlices) {
    EXPECT_FALSE(analyzeFullyOk(
        "int32 main() { string arr[2]; string[] a = arr; split_destroy(a); return 0; }"));
}

// ========== P1-08 / DEC-09: inline asm 语义 ==========

TEST_F(SemanticAnalyzerTest, InlineAsmValid) {
    std::string source = "int32 f() { int32 x; asm(\"mov $7, %0\" : \"=r\"(x)); return x; }";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    analyzer->analyze(*ast);
    EXPECT_TRUE(analyzer->getErrors().empty());
}

TEST_F(SemanticAnalyzerTest, InlineAsmOutputMustBeLValue) {
    std::string source = "int32 f() { asm(\"\" : \"=r\"(1 + 2)); return 0; }";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    analyzer->analyze(*ast);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_EQ(analyzer->getErrors()[0].code, DiagnosticCode::SemAsmOperand);
}

TEST_F(SemanticAnalyzerTest, InlineAsmOperandMustBeScalar) {
    std::string source = "struct P { int32 x; }; int32 f() { struct P p; asm(\"\" : \"=r\"(p)); return 0; }";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    analyzer->analyze(*ast);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_EQ(analyzer->getErrors()[0].code, DiagnosticCode::SemAsmOperand);
}

TEST_F(SemanticAnalyzerTest, InlineAsmEmptyConstraintRejected) {
    std::string source = "int32 f() { int32 x; asm(\"\" : \"\"(x)); return 0; }";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    analyzer->analyze(*ast);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_EQ(analyzer->getErrors()[0].code, DiagnosticCode::SemAsmOperand);
}

TEST_F(SemanticAnalyzerTest, InlineAsmTemplateBadEscape) {
    std::string source = "int32 f() { asm(\"mov %z, %0\"); return 0; }";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    analyzer->analyze(*ast);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_EQ(analyzer->getErrors()[0].code, DiagnosticCode::SemAsmTemplate);
}

TEST_F(SemanticAnalyzerTest, InlineAsmPercentEscapeOk) {
    std::string source = "int32 f() { int32 x; asm(\"mov %%eax, %0\" : \"=r\"(x)); return x; }";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    analyzer->analyze(*ast);
    EXPECT_TRUE(analyzer->getErrors().empty());
}

TEST_F(SemanticAnalyzerTest, InlineAsmNonLValueInputOk) {
    std::string source = "int32 f(int32 x) { int32 y; asm(\"mov %1, %0\" : \"=r\"(y) : \"r\"(x + 1)); return y; }";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    analyzer->analyze(*ast);
    EXPECT_TRUE(analyzer->getErrors().empty());
}

// 最终评审 C1：内存类输出约束（m/o/V）在 LLVM 21 的返回-值输出模型下会触发
// SelectionDAGBuilder 崩溃（SEGV）——必须在 sema 拒绝，编译器不得因 GCC 合法
// 源码崩溃。
TEST_F(SemanticAnalyzerTest, InlineAsmMemoryOutputRejected) {
    std::string source = "int32 f() { int32 x; asm(\"movl $42, %0\" : \"=m\"(x)); return 0; }";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    analyzer->analyze(*ast);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_EQ(analyzer->getErrors()[0].code, DiagnosticCode::SemAsmOperand);
}

TEST_F(SemanticAnalyzerTest, InlineAsmReadWriteMemoryOutputRejected) {
    std::string source = "int32 f() { int32 x; asm(\"movl $42, %0\" : \"+m\"(x)); return 0; }";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    analyzer->analyze(*ast);
    ASSERT_FALSE(analyzer->getErrors().empty());
    EXPECT_EQ(analyzer->getErrors()[0].code, DiagnosticCode::SemAsmOperand);
}

// Found issue（I4 review）: `true`/`false` 是布尔字面量（spec keywords.md），
// 必须能解析到 bool 形参（当前被解析成 int 字面量 → 重载解析不匹配 bool）。
TEST_F(SemanticAnalyzerTest, BoolLiteralMatchesBoolParam) {
    EXPECT_TRUE(analyzeOk(
        "bool f(bool flag) { return flag; }\n"
        "int32 main() { return f(true) ? 0 : 1; }"));
    // bool 字面量在整数上下文仍可用（整数族加宽，行为不变）。
    EXPECT_TRUE(analyzeOk(
        "int32 g(int32 x) { return x; }\n"
        "int32 main() { return g(true); }"));
    EXPECT_TRUE(analyzeOk(
        "int32 main() { bool b = true; int32 x = false; return (int32)b + x; }"));
}

// ========== Designated Initializers + Flexible Array (PAR-13/AGG-04) ==========

static std::vector<std::string> semaErrorsOf(const std::string& source) {
    Lexer lexer("t.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) return {"parse failed"};
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    std::vector<std::string> out;
    for (auto& d : analyzer.getErrors()) out.push_back(d.message);
    return out;
}

static bool hasMsg(const std::vector<std::string>& msgs, const std::string& needle) {
    for (auto& m : msgs) if (m.find(needle) != std::string::npos) return true;
    return false;
}

TEST(SemaDesignatedInitTest, UnknownFieldReportsError) {
    auto errs = semaErrorsOf(
        "struct Point { int32 x; int32 y; };\n"
        "int32 main() { Point p = { .nosuch = 1 }; return 0; }");
    EXPECT_TRUE(hasMsg(errs, "nosuch")) << "expected 'no member' error";
}

TEST(SemaDesignatedInitTest, OutOfBoundsIndexReportsError) {
    auto errs = semaErrorsOf(
        "int32 main() { int32 a[2] = { [5] = 1 }; return 0; }");
    EXPECT_TRUE(hasMsg(errs, "5")) << "expected out-of-bounds index error";
}

TEST(SemaDesignatedInitTest, ArraySizeInferredByMaxDesignator) {
    // C99: { [2] = 5, 6 } → 大小 max(2+1, 2) = 4。手建 AST（visit(ArrayDeclAST)
    // 就地改写 node.size），检查推断结果。不能用 sizeof(局部数组) 于
    // compile_time.static_assert——CT 求值器只接受 compile_time.size_of(类型名)。
    auto analyzer = std::make_unique<SemanticAnalyzer>();
    std::vector<std::unique_ptr<ExprAST>> elems;
    elems.push_back(std::make_unique<NumberExprAST>(5));
    elems.push_back(std::make_unique<NumberExprAST>(6));
    auto il = std::make_unique<InitializerListExprAST>(std::move(elems));
    il->designators.resize(2);
    Designator d;
    d.kind = Designator::Kind::Index;
    d.indexExpr = std::make_unique<NumberExprAST>(2);
    il->designators[0].push_back(std::move(d));

    auto arrDecl = std::make_unique<ArrayDeclAST>("a", TypeContext::instance().getInt32(), 0, std::move(il));
    ArrayDeclAST* raw = arrDecl.get();
    std::vector<std::unique_ptr<DeclAST>> decls;
    decls.push_back(std::move(arrDecl));
    TranslationUnitAST tu(std::move(decls));
    analyzer->analyze(tu);
    EXPECT_TRUE(analyzer->getErrors().empty());
    EXPECT_EQ(raw->size, 4);
}

TEST(SemaDesignatedInitTest, OptionalPseudoFieldDesignatorRejected) {
    auto errs = semaErrorsOf(
        "int32 main() { int32? o = { .value = 1, true }; return 0; }");
    EXPECT_TRUE(hasMsg(errs, "designator cannot target pseudo-field"))
        << "expected pseudo-field designator rejection";
}

TEST(SemaDesignatedInitTest, BaseFieldPrivateAccessReportsE2009) {
    auto errs = semaErrorsOf(
        "class B { private: int32 bf; public: int32 pb; };\n"
        "class D : B { public: int32 x; };\n"
        "int32 main() { D d = { .bf = 1 }; return 0; }");
    EXPECT_TRUE(hasMsg(errs, "bf")) << "expected E2009 on private base field";
}

TEST(SemaFlexArrayTest, RejectsFlexArrayNotLast) {
    auto errs = semaErrorsOf("struct S { int32 n; int32 data[]; int32 m; };");
    EXPECT_TRUE(hasMsg(errs, "flexible array"));
}

TEST(SemaFlexArrayTest, RejectsFlexArrayInClassAndUnion) {
    auto errs1 = semaErrorsOf("class C { int32 n; int32 data[]; };");
    EXPECT_TRUE(hasMsg(errs1, "flexible array")) << "class FAM rejected";
    auto errs2 = semaErrorsOf("union U { int32 n; int32 data[]; };");
    EXPECT_TRUE(hasMsg(errs2, "flexible array")) << "union FAM rejected";
}

TEST(SemaFlexArrayTest, RejectsInitOfFlexArray) {
    auto errs = semaErrorsOf(
        "struct S { int32 n; int32 data[]; };\n"
        "int32 main() { S s = { 1, 2, 3 }; return 0; }");
    EXPECT_TRUE(hasMsg(errs, "flexible array member cannot be initialized"));
}

// INH-06: protected base members are reachable from a DERIVED class's methods
// (via this->), while class-external access stays denied.
TEST(SemanticAnalyzerAccessTest, ProtectedBaseFieldAccessibleFromDerivedMethod) {
    EXPECT_TRUE(analyzeOk(
        "class PB { protected: int32 x; };\n"
        "class PD : public PB { int32 get() { return this->x; } };\n"
        "int32 main() { PD d; return 0; }"));
}

TEST(SemanticAnalyzerAccessTest, ProtectedBaseMethodAccessibleFromDerivedMethod) {
    EXPECT_TRUE(analyzeOk(
        "class PM { protected: int32 f() { return 1; } };\n"
        "class PDM : public PM { int32 g() { return this->f(); } };\n"
        "int32 main() { PDM d; return 0; }"));
}

TEST(SemanticAnalyzerAccessTest, ProtectedStillDeniedOutside) {
    EXPECT_FALSE(analyzeOk(
        "class PO { protected: int32 x; };\n"
        "int32 main() { PO o; o.x = 1; return 0; }"));
}

TEST(SemanticAnalyzerAccessTest, PrivateBaseMemberStillRejectedFromDerived) {
    EXPECT_FALSE(analyzeOk(
        "class PV { private: int32 x; };\n"
        "class PVD : public PV { int32 get() { return this->x; } };\n"
        "int32 main() { PVD d; return 0; }"));
}

TEST(SemanticAnalyzerAccessTest, PrivateMemberAliasDeniedOutside) {
    Lexer lexer("t.c", "class C { using T = int32; }; int32 main() { C::T v = 1; return v; }");
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    bool found = false;
    for (const auto& e : analyzer.getErrors())
        if (e.code == DiagnosticCode::SemPrivateMemberAccess) found = true;
    EXPECT_TRUE(found);
}

TEST(SemanticAnalyzerAccessTest, PrivateMemberAliasInsideClassOk) {
    EXPECT_TRUE(analyzeOk(
        "class C { using T = int32; public: T get() { T v = 2; return v; } }; "
        "int32 main() { C c; return c.get() - 2; }"));
}

TEST(SemanticAnalyzerAccessTest, PrivateMemberAliasPointerDeniedOutside) {
    Lexer lexer("t.c", "class C { using T = int32; }; int32 main() { C::T* p = null; return 0; }");
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    bool found = false;
    for (const auto& e : analyzer.getErrors())
        if (e.code == DiagnosticCode::SemPrivateMemberAccess) found = true;
    EXPECT_TRUE(found);
}

// ========== PAR-11 lambda capture semantics (Task 2) ==========

TEST(LambdaSemaGaps, RejectsUncapturedOuterLocal) {
    auto errs = semaErrorsOf(R"(
        int32 main() {
            int32 x = 1;
            auto outer = [&x]() -> int32 {
                auto inner = []() -> int32 { return x; };
                return inner();
            };
            return outer();
        }
    )");
    EXPECT_FALSE(errs.empty());
    EXPECT_TRUE(hasMsg(errs, "not captured"))
        << "expected an uncaptured-variable diagnostic";
}

TEST(LambdaSemaGaps, CaptureInitializerInfersType) {
    EXPECT_TRUE(analyzeOk(R"(
        int32 main() {
            auto f = [x = 41]() -> int32 { return x + 1; };
            return f() == 42 ? 0 : 1;
        }
    )"));
}

TEST(LambdaSemaGaps, CaptureInitializerByRefRequiresLValue) {
    auto errs = semaErrorsOf(R"(
        int32 main() {
            auto f = [&x = 1]() -> int32 { return x; };
            return 0;
        }
    )");
    EXPECT_FALSE(errs.empty());
    EXPECT_TRUE(hasMsg(errs, "lvalue") || hasMsg(errs, "左值"))
        << "expected an lvalue diagnostic for a by-ref init capture";
}

TEST(LambdaSemaGaps, ThisImplicitMemberResolves) {
    auto errs = semaErrorsOf(R"(
        class C {
        public:
            int32 v;
            int32 f() {
                auto g = [this]() -> int32 { return v; };
                return g();
            }
        };
    )");
    EXPECT_TRUE(errs.empty()) << (errs.empty() ? "" : errs.front());
}

TEST(LambdaSemaGaps, ThisCaptureOutsideMethodRejected) {
    auto errs = semaErrorsOf(R"(
        int32 main() {
            auto f = [this]() -> int32 { return 1; };
            return 0;
        }
    )");
    EXPECT_FALSE(errs.empty());
}

TEST(LambdaSemaGaps, CaptureAllDoesNotCaptureGlobals) {
    std::string source = R"(
        int32 g = 5;
        int32 main() {
            auto f = [=]() -> int32 { return g; };
            return f() == 5 ? 0 : 1;
        }
    )";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty());

    auto* fn = dynamic_cast<FunctionDeclAST*>(ast->declarations[1].get());
    ASSERT_NE(fn, nullptr);
    ASSERT_NE(fn->body, nullptr);
    auto* ds = dynamic_cast<DeclStmtAST*>(fn->body->stmts[0].get());
    ASSERT_NE(ds, nullptr);
    auto* vd = dynamic_cast<VarDeclAST*>(ds->decl.get());
    ASSERT_NE(vd, nullptr);
    auto* lam = dynamic_cast<LambdaExprAST*>(vd->initExpr.get());
    ASSERT_NE(lam, nullptr);
    EXPECT_TRUE(lam->captures.empty());
    EXPECT_EQ(lam->closureType, nullptr);
    EXPECT_EQ(lam->type->kind, TypeKind::Pointer);
}

TEST(LambdaSemaGaps, CaptureAllPreservesExplicitOverrides) {
    std::string source = R"(
        int32 main() {
            int32 a = 1; int32 b = 2;
            auto byVal = [=, &a, b]() -> int32 { return a + b; };
            auto byRef = [&, a, &b]() -> int32 { return a + b; };
            return byVal() + byRef() - 6;
        }
    )";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());

    auto* fn = dynamic_cast<FunctionDeclAST*>(ast->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    ASSERT_NE(fn->body, nullptr);
    auto lambdaAt = [&](size_t stmtIndex) -> LambdaExprAST* {
        auto* ds = dynamic_cast<DeclStmtAST*>(fn->body->stmts.at(stmtIndex).get());
        if (!ds) return nullptr;
        auto* vd = dynamic_cast<VarDeclAST*>(ds->decl.get());
        if (!vd) return nullptr;
        return dynamic_cast<LambdaExprAST*>(vd->initExpr.get());
    };

    auto* byVal = lambdaAt(2);
    ASSERT_NE(byVal, nullptr);
    ASSERT_EQ(byVal->captures.size(), 2u);
    std::unordered_map<std::string, bool> byValRef;
    for (auto& c : byVal->captures) byValRef[c.name] = c.byRef;
    EXPECT_TRUE(byValRef["a"]);
    EXPECT_FALSE(byValRef["b"]);

    auto* byRef = lambdaAt(3);
    ASSERT_NE(byRef, nullptr);
    ASSERT_EQ(byRef->captures.size(), 2u);
    std::unordered_map<std::string, bool> byRefRef;
    for (auto& c : byRef->captures) byRefRef[c.name] = c.byRef;
    EXPECT_FALSE(byRefRef["a"]);
    EXPECT_TRUE(byRefRef["b"]);
}

TEST(LambdaSemaGaps, CaptureAllDiscoversArrayAccess) {
    EXPECT_TRUE(analyzeOk(R"(
        int32 main() {
            int32 a[2] = { 1, 2 };
            auto f = [&]() -> int32 { return a[0]; };
            return f() - 1;
        }
    )"));
}

TEST(LambdaSemaGaps, CaptureAllDiscoversCast) {
    EXPECT_TRUE(analyzeOk(R"(
        int32 main() {
            int32 x = 1;
            auto f = [=]() -> int32 { return (int32)x + 1; };
            return f() - 2;
        }
    )"));
}

TEST(LambdaSemaGaps, CaptureAllDiscoversComma) {
    EXPECT_TRUE(analyzeOk(R"(
        int32 main() {
            int32 x = 2;
            auto f = [=]() -> int32 { return (0, x); };
            return f() - 2;
        }
    )"));
}

TEST(LambdaSemaGaps, CaptureAllDiscoversPostfixIncDec) {
    EXPECT_TRUE(analyzeOk(R"(
        int32 main() {
            int32 x = 2;
            auto f = [=]() { x++; };
            f();
            return x - 2;
        }
    )"));
}

TEST(LambdaSemaGaps, CaptureAllDiscoversAsmOperand) {
    EXPECT_TRUE(analyzeOk(R"(
        int32 main() {
            int32 x = 1;
            auto f = [=]() { asm("" : : "r"(x)); };
            f();
            return 0;
        }
    )"));
}

TEST(LambdaSemaGaps, CaptureAllCapturesLocalFunctionPointer) {
    std::string source = R"(
        int32 inc(int32 x) { return x + 1; }
        int32 main() {
            int32 (*fp)(int32) = inc;
            auto g = [=](int32 x) -> int32 { return fp(x); };
            return g(1) - 2;
        }
    )";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty());

    auto* fn = dynamic_cast<FunctionDeclAST*>(ast->declarations[1].get());
    ASSERT_NE(fn, nullptr);
    ASSERT_NE(fn->body, nullptr);
    auto* ds = dynamic_cast<DeclStmtAST*>(fn->body->stmts[1].get());
    ASSERT_NE(ds, nullptr);
    auto* vd = dynamic_cast<VarDeclAST*>(ds->decl.get());
    ASSERT_NE(vd, nullptr);
    auto* lam = dynamic_cast<LambdaExprAST*>(vd->initExpr.get());
    ASSERT_NE(lam, nullptr);
    bool capturedFp = false;
    for (auto& c : lam->captures) {
        if (c.name == "fp") capturedFp = true;
    }
    EXPECT_TRUE(capturedFp) << "[=] must capture the local function pointer";
}

TEST(LambdaSemaGaps, NoCaptureRejectsLocalFunctionPointerCallee) {
    auto errs = semaErrorsOf(R"(
        int32 inc(int32 x) { return x + 1; }
        int32 main() {
            int32 (*fp)(int32) = inc;
            auto g = [](int32 x) -> int32 { return fp(x); };
            return g(1) - 2;
        }
    )");
    EXPECT_FALSE(errs.empty());
    EXPECT_TRUE(hasMsg(errs, "not captured"))
        << "expected a not-captured diagnostic for the callee";
}

TEST(LambdaSemaGaps, RejectsThreeLevelUncapturedThroughEmptyLambdas) {
    auto errs = semaErrorsOf(R"(
        int32 main() {
            int32 x = 1;
            auto outer = []() -> int32 {
                auto middle = []() -> int32 {
                    auto inner = [=]() -> int32 { return x; };
                    return inner();
                };
                return middle();
            };
            return outer() - 1;
        }
    )");
    EXPECT_FALSE(errs.empty());
    EXPECT_TRUE(hasMsg(errs, "not captured"))
        << "expected the intervening lambda to reject the capture";
}

TEST(LambdaSemaGaps, RejectsExplicitCaptureThroughNonCapturingLambda) {
    auto errs = semaErrorsOf(R"(
        int32 main() {
            int32 x = 1;
            auto outer = []() -> int32 {
                auto middle = []() -> int32 {
                    auto inner = [&x]() -> int32 { return x; };
                    return inner();
                };
                return middle();
            };
            return outer() - 1;
        }
    )");
    EXPECT_FALSE(errs.empty());
    EXPECT_TRUE(hasMsg(errs, "not captured"))
        << "expected the intervening lambda to reject the explicit capture";
}

TEST(LambdaSemaGaps, ThisMemberBeatsGlobalField) {
    std::string source = R"(
        int32 v = 7;
        class GlobalShadowC {
        public:
            int32 v;
            int32 f() {
                auto g = [this]() -> int32 { return v; };
                return g();
            }
        };
        int32 main() {
            GlobalShadowC c;
            c.v = 3;
            return c.f() - 3;
        }
    )";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty())
        << (analyzer.getErrors().empty() ? ""
                                         : analyzer.getErrors().front().message);
    auto* cls = dynamic_cast<StructDeclAST*>(ast->declarations[1].get());
    ASSERT_NE(cls, nullptr);
    ASSERT_FALSE(cls->methods.empty());
    auto* method = cls->methods[0].get();
    ASSERT_NE(method, nullptr);
    ASSERT_NE(method->body, nullptr);
    auto* ds = dynamic_cast<DeclStmtAST*>(method->body->stmts[0].get());
    ASSERT_NE(ds, nullptr);
    auto* vd = dynamic_cast<VarDeclAST*>(ds->decl.get());
    ASSERT_NE(vd, nullptr);
    auto* lam = dynamic_cast<LambdaExprAST*>(vd->initExpr.get());
    ASSERT_NE(lam, nullptr);
    auto* ret = dynamic_cast<ReturnStmtAST*>(lam->body->stmts[0].get());
    ASSERT_NE(ret, nullptr);
    auto* ref = dynamic_cast<VariableExprAST*>(ret->value.get());
    ASSERT_NE(ref, nullptr);
    EXPECT_NE(ref->implicitThisAccess, nullptr)
        << "member must win over the same-named global inside [this]";
}

// ========== PAR-11 lambda return-type inference (Task 3) ==========

TEST(LambdaSemaGaps, ReturnInferenceSeesBodyLocal) {
    auto errs = semaErrorsOf(R"(
        int32 main() {
            auto f = []() {
                int32 y = 1;
                return y;
            };
            return f() == 1 ? 0 : 1;
        }
    )");
    EXPECT_TRUE(errs.empty()) << (errs.empty() ? "" : errs.front());
}

TEST(LambdaSemaGaps, ReturnInferenceCoversIfBranch) {
    auto errs = semaErrorsOf(R"(
        int32 main() {
            int32 c = 1;
            auto f = [c](int32 x) {
                if (x > 0) { return c + x; }
                return c;
            };
            return f(1) == 2 ? 0 : 1;
        }
    )");
    EXPECT_TRUE(errs.empty()) << (errs.empty() ? "" : errs.front());
}

TEST(LambdaSemaGaps, ReturnInferenceCoversOnlyBranch) {
    auto errs = semaErrorsOf(R"(
        int32 main() {
            auto f = [](int32 x) {
                if (x > 0) { return 1; }
            };
            return f(1) == 1 ? 0 : 1;
        }
    )");
    EXPECT_TRUE(errs.empty()) << (errs.empty() ? "" : errs.front());
}

TEST(LambdaSemaGaps, ReturnInferenceCoversSwitchCase) {
    auto errs = semaErrorsOf(R"(
        int32 main() {
            auto f = [](int32 x) {
                switch (x) {
                    case 1: return 10;
                    default: return 20;
                }
            };
            return f(1) == 10 ? 0 : 1;
        }
    )");
    EXPECT_TRUE(errs.empty()) << (errs.empty() ? "" : errs.front());
}

TEST(LambdaSemaGaps, ReturnInferenceVoidWithoutValueReturn) {
    auto errs = semaErrorsOf(R"(
        int32 main() {
            auto f = []() { int32 x = 1; };
            f();
            return 0;
        }
    )");
    EXPECT_TRUE(errs.empty()) << (errs.empty() ? "" : errs.front());
}

TEST(LambdaSemaGaps, ReturnInferenceVoidReturnType) {
    std::string source = R"(
        int32 main() {
            auto f = []() { int32 x = 1; };
            f();
            return 0;
        }
    )";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty())
        << (analyzer.getErrors().empty() ? ""
                                         : analyzer.getErrors().front().message);
    auto* fn = dynamic_cast<FunctionDeclAST*>(ast->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    auto* ds = dynamic_cast<DeclStmtAST*>(fn->body->stmts[0].get());
    ASSERT_NE(ds, nullptr);
    auto* vd = dynamic_cast<VarDeclAST*>(ds->decl.get());
    ASSERT_NE(vd, nullptr);
    auto* lam = dynamic_cast<LambdaExprAST*>(vd->initExpr.get());
    ASSERT_NE(lam, nullptr);
    ASSERT_NE(lam->returnType, nullptr);
    EXPECT_EQ(lam->returnType->kind, TypeKind::Void);
}

TEST(LambdaSemaGaps, UnexplicitReturnIsNotAnalyzedTwice) {
    // Task 2 ruling: an uncaptured outer variable referenced from a return must
    // produce exactly one diagnostic. The removed pre-scan used to analyze the
    // return expression a second time.
    auto errs = semaErrorsOf(R"(
        int32 main() {
            int32 x = 1;
            auto f = []() { return x; };
            return f() == 1 ? 0 : 1;
        }
    )");
    int count = 0;
    for (const auto& m : errs) {
        if (m.find("not captured") != std::string::npos) ++count;
    }
    EXPECT_EQ(count, 1) << "expected exactly one uncaptured diagnostic";
}

TEST(LambdaSemaGaps, VoidReturnBeforeValueReturnDiagnosedAtEarlierReturn) {
    std::string source =
        "int32 main() {\n"                   // 1
        "    auto f = [](int32 x) {\n"       // 2
        "        if (x > 0) { return; }\n"   // 3
        "        return 5;\n"                // 4
        "    };\n"                           // 5
        "    return f(1);\n"                 // 6
        "}\n";                               // 7
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    int count = 0;
    int line = -1;
    for (const auto& d : analyzer.getErrors()) {
        if (d.message.find("without a value") != std::string::npos ||
            d.message.find("non-void") != std::string::npos) {
            ++count;
            line = d.line;
        }
    }
    ASSERT_EQ(count, 1) << "expected a single diagnostic for the earlier void return";
    EXPECT_EQ(line, 3) << "diagnostic must point at the earlier value-less return";
}

TEST(LambdaSemaGaps, NestedLambdaInferenceIndependent) {
    std::string source = R"(
        int32 main() {
            auto outer = []() {
                auto inner = []() { return 7; };
                int64 big = 9;
                if (big > 0) { return big; }
                return 1;
            };
            return outer() == 9 ? 0 : 1;
        }
    )";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty())
        << (analyzer.getErrors().empty() ? ""
                                         : analyzer.getErrors().front().message);

    auto* fn = dynamic_cast<FunctionDeclAST*>(ast->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    ASSERT_NE(fn->body, nullptr);
    auto* outerDecl = dynamic_cast<DeclStmtAST*>(fn->body->stmts[0].get());
    ASSERT_NE(outerDecl, nullptr);
    auto* outerVar = dynamic_cast<VarDeclAST*>(outerDecl->decl.get());
    ASSERT_NE(outerVar, nullptr);
    auto* outer = dynamic_cast<LambdaExprAST*>(outerVar->initExpr.get());
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(outer->body, nullptr);
    auto* innerDecl = dynamic_cast<DeclStmtAST*>(outer->body->stmts[0].get());
    ASSERT_NE(innerDecl, nullptr);
    auto* innerVar = dynamic_cast<VarDeclAST*>(innerDecl->decl.get());
    ASSERT_NE(innerVar, nullptr);
    auto* inner = dynamic_cast<LambdaExprAST*>(innerVar->initExpr.get());
    ASSERT_NE(inner, nullptr);

    // Inner infers int32; outer infers int64 from its first value return and
    // must not be contaminated by the inner lambda's int32 return.
    ASSERT_NE(inner->returnType, nullptr);
    EXPECT_EQ(inner->returnType->kind, TypeKind::Int32);
    ASSERT_NE(outer->returnType, nullptr);
    EXPECT_EQ(outer->returnType->kind, TypeKind::Int64);
}

TEST(LambdaSemaGaps, FirstValueReturnDeterminesInferredType) {
    std::string source = R"(
        int32 main() {
            auto f = []() {
                if (true) { return (int64)1; }
                return 2;
            };
            return f() == 1 ? 0 : 1;
        }
    )";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty())
        << (analyzer.getErrors().empty() ? ""
                                         : analyzer.getErrors().front().message);
    auto* fn = dynamic_cast<FunctionDeclAST*>(ast->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    auto* ds = dynamic_cast<DeclStmtAST*>(fn->body->stmts[0].get());
    ASSERT_NE(ds, nullptr);
    auto* vd = dynamic_cast<VarDeclAST*>(ds->decl.get());
    ASSERT_NE(vd, nullptr);
    auto* lam = dynamic_cast<LambdaExprAST*>(vd->initExpr.get());
    ASSERT_NE(lam, nullptr);
    ASSERT_NE(lam->returnType, nullptr);
    EXPECT_EQ(lam->returnType->kind, TypeKind::Int64)
        << "the first value return must determine the inferred type";
}

TEST(LambdaSemaGaps, VoidExpressionReturnStaysVoid) {
    std::string source = R"(
        void g() {}
        int32 main() {
            auto a = [](int32 x) {
                if (x > 0) { return; }
                return g();
            };
            auto b = [](int32 x) {
                if (x > 0) { return g(); }
                return;
            };
            a(1);
            b(1);
            return 0;
        }
    )";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty())
        << (analyzer.getErrors().empty() ? ""
                                         : analyzer.getErrors().front().message);

    auto* fn = dynamic_cast<FunctionDeclAST*>(ast->declarations[1].get());
    ASSERT_NE(fn, nullptr);
    ASSERT_NE(fn->body, nullptr);
    for (size_t i = 0; i < 2; ++i) {
        auto* ds = dynamic_cast<DeclStmtAST*>(fn->body->stmts[i].get());
        ASSERT_NE(ds, nullptr) << "statement " << i;
        auto* vd = dynamic_cast<VarDeclAST*>(ds->decl.get());
        ASSERT_NE(vd, nullptr) << "statement " << i;
        auto* lam = dynamic_cast<LambdaExprAST*>(vd->initExpr.get());
        ASSERT_NE(lam, nullptr) << "statement " << i;
        ASSERT_NE(lam->returnType, nullptr) << "lambda " << i;
        EXPECT_EQ(lam->returnType->kind, TypeKind::Void)
            << "`return voidExpr;` must not make lambda " << i << " non-void";
    }
}

TEST(LambdaSemaGaps, AllVoidReturnsDiagnosedWhenValueReturnAppears) {
    std::string source =
        "int32 main() {\n"                       // 1
        "    auto f = [](int32 x) {\n"           // 2
        "        if (x > 0) { return; }\n"       // 3
        "        if (x < 0) { return; }\n"       // 4
        "        if (x == 0) { return 5; }\n"    // 5
        "        return;\n"                      // 6
        "    };\n"                               // 7
        "    return f(1);\n"                     // 8
        "}\n";                                   // 9
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    std::vector<int> lines;
    for (const auto& d : analyzer.getErrors()) {
        if (d.message.find("must return a value") != std::string::npos)
            lines.push_back(d.line);
    }
    EXPECT_EQ(lines, (std::vector<int>{3, 4, 6}))
        << "every value-less return must be diagnosed once, including later ones";
}

TEST(LambdaSemaGaps, ExplicitReturnTypeStillAuthoritative) {
    auto errs = semaErrorsOf(R"(
        int32 main() {
            auto f = []() -> void { return 1; };
            f();
            return 0;
        }
    )");
    EXPECT_FALSE(errs.empty());
    EXPECT_TRUE(hasMsg(errs, "return type mismatch") ||
                hasMsg(errs, "类型不匹配") ||
                hasMsg(errs, "non-void"))
        << "expected explicit void return type to reject a value return";
}

TEST(LambdaSemaGaps, FileScopeCaptureInitializerDiagnosed) {
    auto errs = semaErrorsOf(R"(
        int32 k = 5;
        auto f = [x = k + 1]() -> int32 { return x; };
    )");
    EXPECT_FALSE(errs.empty()) << "expected a diagnostic for a capture initializer "
                                  "in a global initializer";
    EXPECT_TRUE(hasMsg(errs, "enclosing function scope") ||
                hasMsg(errs, "global initializer"))
        << "expected the global-scope capture diagnostic";
}

// ========== PAR-11 final review regressions ==========

TEST(LambdaSemaGaps, VoidCaptureInitializerRejected) {
    auto errs = semaErrorsOf(R"(
        void g() {}
        int32 main() {
            auto f = [x = g()]() -> int32 { return 1; };
            return f();
        }
    )");
    EXPECT_FALSE(errs.empty());
    EXPECT_TRUE(hasMsg(errs, "must have a value type"))
        << "expected a value-type diagnostic for a void capture initializer";
}

TEST(LambdaSemaGaps, InferredArrayReturnDecaysToPointer) {
    std::string source = R"(
        int32 main() {
            auto f = [](int32 c) {
                if (c) { int32 a[3] = {1, 2, 3}; return a; }
                return 0;
            };
            return f(1)[0] - 1;
        }
    )";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty())
        << (analyzer.getErrors().empty() ? ""
                                         : analyzer.getErrors().front().message);
    auto* fn = dynamic_cast<FunctionDeclAST*>(ast->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    auto* ds = dynamic_cast<DeclStmtAST*>(fn->body->stmts[0].get());
    ASSERT_NE(ds, nullptr);
    auto* vd = dynamic_cast<VarDeclAST*>(ds->decl.get());
    ASSERT_NE(vd, nullptr);
    auto* lam = dynamic_cast<LambdaExprAST*>(vd->initExpr.get());
    ASSERT_NE(lam, nullptr);
    ASSERT_NE(lam->returnType, nullptr);
    EXPECT_EQ(lam->returnType->kind, TypeKind::Pointer)
        << "an inferred array return must decay to a pointer";
    ASSERT_NE(lam->returnType->base, nullptr);
    EXPECT_EQ(lam->returnType->base->kind, TypeKind::Int32);
}

TEST(LambdaSemaGaps, ExplicitCaptureRedeclaredByBodyLocalRejected) {
    auto errs = semaErrorsOf(R"(
        int32 main() {
            int32 x = 1;
            auto f = [x]() -> int32 { int32 x = 5; return x; };
            return f();
        }
    )");
    EXPECT_FALSE(errs.empty());
    EXPECT_TRUE(hasMsg(errs, "redeclaration of lambda capture"))
        << "expected the body-local to be rejected as a capture redeclaration";
}

TEST(LambdaSemaGaps, CaptureAllDiscoversSwitch) {
    EXPECT_TRUE(analyzeOk(R"(
        int32 main() {
            int32 x = 2;
            auto f = [=]() -> int32 {
                switch (x) {
                    case 1: return 1;
                    default: return x;
                }
            };
            return f() - 2;
        }
    )"));
}

TEST(LambdaSemaGaps, CaptureAllDiscoversDefer) {
    EXPECT_TRUE(analyzeOk(R"(
        void cleanup(int32 v) {}
        int32 main() {
            int32 x = 1;
            auto f = [=]() { defer cleanup(x); };
            f();
            return 0;
        }
    )"));
}

TEST(LambdaSemaGaps, CaptureAllDiscoversCompileTimeIf) {
    EXPECT_TRUE(analyzeOk(R"(
        int32 main() {
            int32 x = 1;
            auto f = [=]() -> int32 {
                compile_time.if (true) { return x; } else { return 0; }
            };
            return f() - 1;
        }
    )"));
}

TEST(LambdaSemaGaps, CaptureAllDiscoversCompileTimeRepeat) {
    EXPECT_TRUE(analyzeOk(R"(
        int32 main() {
            int32 x = 1;
            int32 total = 0;
            auto f = [&]() { compile_time.repeat(2) { total = total + x; } };
            f();
            return total - 4;
        }
    )"));
}

TEST(LambdaSemaGaps, CaptureAllDiscoversMultiVarInitializer) {
    EXPECT_TRUE(analyzeOk(R"(
        int32 main() {
            int32 x = 1;
            int32 y = 2;
            auto f = [=]() -> int32 {
                int32 a = x, b = y;
                return a + b;
            };
            return f() - 3;
        }
    )"));
}

TEST(LambdaSemaGaps, CaptureAllDiscoversFieldUseInsideSwitch) {
    EXPECT_TRUE(analyzeOk(R"(
        class SwitchThisC {
        public:
            int32 v;
            int32 f() {
                auto g = [=]() -> int32 {
                    switch (v) {
                        case 0: return 10;
                        default: return v + 1;
                    }
                };
                return g();
            }
        };
    )"));
}

TEST(LambdaSemaGaps, NestedInitCaptureDoesNotCaptureOuterName) {
    std::string source = R"(
        int32 main() {
            int32 z = 3;
            int32 (*fp)(int32) = [=](int32 x) -> int32 {
                auto inner = [z = 5]() -> int32 { return z; };
                return inner() + x;
            };
            return fp(1) + z - 9;
        }
    )";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty())
        << (analyzer.getErrors().empty() ? ""
                                         : analyzer.getErrors().front().message);
    auto* fn = dynamic_cast<FunctionDeclAST*>(ast->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    auto* ds = dynamic_cast<DeclStmtAST*>(fn->body->stmts[1].get());
    ASSERT_NE(ds, nullptr);
    auto* vd = dynamic_cast<VarDeclAST*>(ds->decl.get());
    ASSERT_NE(vd, nullptr);
    auto* lam = dynamic_cast<LambdaExprAST*>(vd->initExpr.get());
    ASSERT_NE(lam, nullptr);
    EXPECT_TRUE(lam->captures.empty())
        << "an inner init-capture LHS is not an outer reference";
    ASSERT_NE(lam->type, nullptr);
    EXPECT_EQ(lam->type->kind, TypeKind::Pointer)
        << "the outer lambda must stay convertible to a function pointer";
}

TEST(LambdaSemaGaps, StaticLocalBeatsSameNamedField) {
    std::string source = R"(
        class StaticShadowC {
        public:
            int32 x;
            int32 f() {
                static int32 x = 5;
                auto g = [=]() -> int32 { return x; };
                return g();
            }
        };
    )";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty())
        << (analyzer.getErrors().empty() ? ""
                                         : analyzer.getErrors().front().message);
    auto* cls = dynamic_cast<StructDeclAST*>(ast->declarations[0].get());
    ASSERT_NE(cls, nullptr);
    ASSERT_FALSE(cls->methods.empty());
    auto* method = cls->methods[0].get();
    ASSERT_NE(method, nullptr);
    auto* ds = dynamic_cast<DeclStmtAST*>(method->body->stmts[1].get());
    ASSERT_NE(ds, nullptr);
    auto* vd = dynamic_cast<VarDeclAST*>(ds->decl.get());
    ASSERT_NE(vd, nullptr);
    auto* lam = dynamic_cast<LambdaExprAST*>(vd->initExpr.get());
    ASSERT_NE(lam, nullptr);
    auto* ret = dynamic_cast<ReturnStmtAST*>(lam->body->stmts[0].get());
    ASSERT_NE(ret, nullptr);
    auto* ref = dynamic_cast<VariableExprAST*>(ret->value.get());
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->implicitThisAccess, nullptr)
        << "a block-scope static local must win over the same-named field";
}

TEST(LambdaSemaGaps, EnumeratorLosesToSameNamedField) {
    std::string source = R"(
        enum Color { Red = 1, Green = 2 };
        class EnumShadowC {
        public:
            int32 Red;
            int32 f() {
                auto g = [this]() -> int32 { return Red; };
                return g();
            }
        };
    )";
    Lexer lexer("t.c", source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty())
        << (analyzer.getErrors().empty() ? ""
                                         : analyzer.getErrors().front().message);
    StructDeclAST* cls = nullptr;
    for (auto& d : ast->declarations) {
        if (auto* candidate = dynamic_cast<StructDeclAST*>(d.get())) {
            cls = candidate;
            break;
        }
    }
    ASSERT_NE(cls, nullptr);
    ASSERT_FALSE(cls->methods.empty());
    auto* method = cls->methods[0].get();
    ASSERT_NE(method, nullptr);
    auto* ds = dynamic_cast<DeclStmtAST*>(method->body->stmts[0].get());
    ASSERT_NE(ds, nullptr);
    auto* vd = dynamic_cast<VarDeclAST*>(ds->decl.get());
    ASSERT_NE(vd, nullptr);
    auto* lam = dynamic_cast<LambdaExprAST*>(vd->initExpr.get());
    ASSERT_NE(lam, nullptr);
    auto* ret = dynamic_cast<ReturnStmtAST*>(lam->body->stmts[0].get());
    ASSERT_NE(ret, nullptr);
    auto* ref = dynamic_cast<VariableExprAST*>(ret->value.get());
    ASSERT_NE(ref, nullptr);
    EXPECT_FALSE(ref->isEnumConstant)
        << "a namespace-scope enumerator must lose to the same-named field";
    EXPECT_NE(ref->implicitThisAccess, nullptr);
}

TEST(LambdaSemaGaps, ThisByReferenceRejected) {
    auto errs = semaErrorsOf(R"(
        class C { public: int32 v; int32 f() { auto g = [&this]() { return v; }; return g(); } };
    )");
    EXPECT_FALSE(errs.empty());
    EXPECT_TRUE(hasMsg(errs, "cannot be captured by reference"));
}

TEST(LambdaSemaGaps, ThisCaptureInitializerRejected) {
    auto errs = semaErrorsOf(R"(
        class C { public: int32 v; int32 f() { auto g = [this = 1]() { return v; }; return g(); } };
    )");
    EXPECT_FALSE(errs.empty());
    EXPECT_TRUE(hasMsg(errs, "cannot have a capture initializer"));
}

TEST(LambdaSemaGaps, DuplicateCaptureRejected) {
    auto errs = semaErrorsOf(R"(
        int32 main() {
            int32 x = 1;
            auto f = [x, x]() -> int32 { return x; };
            return f() - 1;
        }
    )");
    EXPECT_FALSE(errs.empty());
    EXPECT_TRUE(hasMsg(errs, "duplicate capture"));
}
