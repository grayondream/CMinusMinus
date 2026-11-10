// P1-04 / CT-06: 编译期求值器 CompileTimeEvaluator——字面量/算术/逻辑/
// 字符串比较与拼接、compile_time 变量与函数委托、深度上限、静默失败语义。

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "sema/CompileTimeEvaluator.h"
#include "ast/Decl.h"
#include "ast/Expr.h"
#include "ast/Stmt.h"

#include <spdlog/spdlog.h>

using ConstValue = CompileTimeEvaluator::ConstValue;

class CompileTimeEval : public ::testing::Test {
protected:
    void SetUp() override {
        spdlog::set_level(spdlog::level::off);
    }

    // 解析 `decls + int32 f() { return expr; }`，跑 sema 后提取 return 表达式。
    // analyze 后的错误数记录在 baseErrors，供「eval 静默」断言比较。
    // 致命断言失败时返回（调用方用 ASSERT_NO_FATAL_FAILURE 包裹）。
    struct Extracted {
        ExprAST* expr = nullptr;
        size_t baseErrors = 0;
        std::unique_ptr<TranslationUnitAST> tu;
        std::unique_ptr<SemanticAnalyzer> analyzer;
    };

    void extract(Extracted& ex, const std::string& decls, const std::string& exprSrc) {
        Lexer lexer("ct_eval_test.c", decls + " int32 f() { return " + exprSrc + "; }");
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        ex.tu = parser.parse();
        ex.analyzer = std::make_unique<SemanticAnalyzer>();
        ex.analyzer->analyze(*ex.tu);
        ex.baseErrors = ex.analyzer->getErrors().size();
        ASSERT_FALSE(ex.tu->declarations.empty());
        FunctionDeclAST* fn = nullptr;
        for (auto it = ex.tu->declarations.rbegin(); it != ex.tu->declarations.rend(); ++it) {
            fn = dynamic_cast<FunctionDeclAST*>(it->get());
            if (fn) break;
        }
        ASSERT_NE(fn, nullptr);
        ASSERT_FALSE(fn->body->stmts.empty());
        auto* ret = dynamic_cast<ReturnStmtAST*>(fn->body->stmts[0].get());
        ASSERT_NE(ret, nullptr);
        ASSERT_NE(ret->value, nullptr);
        ex.expr = ret->value.get();
    }
};

TEST_F(CompileTimeEval, EvalArithmetic) {
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, "", "2 + 3 * 4"));
    CompileTimeEvaluator ev(*ex.analyzer);
    auto v = ev.eval(ex.expr, *ex.expr);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->type, ConstValue::INT);
    EXPECT_EQ(v->intVal, 14);
}

// P1-06: CT-06 keeps ==/!= folding; `+` (concat) was removed with the
// language-level `str + str` operator (spec §2.5).
TEST_F(CompileTimeEval, EvalStringCompare) {
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, "", "\"a\" == \"a\""));
    CompileTimeEvaluator ev(*ex.analyzer);
    auto v = ev.eval(ex.expr, *ex.expr);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->type, ConstValue::INT);
    EXPECT_EQ(v->intVal, 1);
}

TEST_F(CompileTimeEval, EvalStringNotEqual) {
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, "", "\"a\" != \"b\""));
    CompileTimeEvaluator ev(*ex.analyzer);
    auto v = ev.eval(ex.expr, *ex.expr);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->type, ConstValue::INT);
    EXPECT_EQ(v->intVal, 1);
}

TEST_F(CompileTimeEval, EvalStringConcatRemoved) {
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, "", "\"a\" + \"b\""));
    CompileTimeEvaluator ev(*ex.analyzer);
    auto v = ev.eval(ex.expr, *ex.expr);
    EXPECT_FALSE(v.has_value());
}

TEST_F(CompileTimeEval, EvalConstexprVar) {
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, "compile_time int32 k = 5;", "k * 2"));
    CompileTimeEvaluator ev(*ex.analyzer);
    auto v = ev.eval(ex.expr, *ex.expr);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->type, ConstValue::INT);
    EXPECT_EQ(v->intVal, 10);
}

TEST_F(CompileTimeEval, EvalDepthLimit) {
    // 100 层嵌套括号加法：((((1+1)+1)+1)...)
    std::string expr;
    for (int i = 0; i < 100; ++i) expr += "(";
    expr += "1";
    for (int i = 0; i < 100; ++i) expr += "+1)";
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, "", expr));
    CompileTimeEvaluator ev(*ex.analyzer);
    auto v = ev.eval(ex.expr, *ex.expr);
    EXPECT_FALSE(v.has_value());
    ASSERT_GT(ex.analyzer->getErrors().size(), ex.baseErrors);
    bool found = false;
    for (const auto& d : ex.analyzer->getErrors()) {
        if (d.message.find("depth limit exceeded") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(CompileTimeEval, EvalNonConstantQuiet) {
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, "", "x + 1")); // x 未声明：sema 已有诊断，eval 不得新增
    CompileTimeEvaluator ev(*ex.analyzer);
    auto v = ev.eval(ex.expr, *ex.expr);
    EXPECT_FALSE(v.has_value());
    EXPECT_EQ(ex.analyzer->getErrors().size(), ex.baseErrors);
}

// ---- P1-04 / CT-04/05: sema 钩子与 build 配置注入 ----

static bool analyzeOk(const std::string& source) {
    Lexer lexer("ct_sema_test.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) return false;
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!parser.getErrors().empty()) return false; // parse 诊断不得被吞
    return analyzer.getErrors().empty();
}

TEST_F(CompileTimeEval, CompileTimeInsideNamespace) {
    // Review Focus 2: namespace 内的 compile_time 根链不被 namespacePrefix 劫持。
    EXPECT_TRUE(analyzeOk(R"(
compile_time int32 ok = compile_time.build.debug;
namespace N { int32 f() { return ok; } }
)"));
}

TEST_F(CompileTimeEval, StrEscapeDiagnosed) {
    // Review Focus 5: STR 值赋给运行时变量 → 类型错误诊断，不崩溃。
    Lexer lexer("ct_str_escape.c", "int32 main() { int32 x = compile_time.target.os; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool found = false;
    for (const auto& d : analyzer.getErrors()) {
        // 评审 M3: 须为 STR 逃逸专属诊断。
        if (d.message.find("compile-time string value cannot be used in runtime context") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(CompileTimeEval, UnknownMemberDiagnosed) {
    Lexer lexer("ct_unknown_member.c", "int32 main() { int32 x = compile_time.nope; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool found = false;
    for (const auto& d : analyzer.getErrors()) {
        if (d.message.find("unknown compile_time member 'nope'") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

// ---- P1-04 / CT-02: compile_time.static_assert ----

TEST_F(CompileTimeEval, StaticAssertOk) {
    EXPECT_TRUE(analyzeOk(R"(
compile_time.static_assert(1 == 1, "ok");
int32 main() { return 0; }
)"));
}

TEST_F(CompileTimeEval, StaticAssertFailsWithMsg) {
    Lexer lexer("ct_sa_msg.c", R"(
compile_time.static_assert(1 == 2, "must hold");
int32 main() { return 0; }
)");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool found = false;
    for (const auto& d : analyzer.getErrors()) {
        if (d.message.find("static_assert failed: must hold") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(CompileTimeEval, StaticAssertFailsNoMsg) {
    Lexer lexer("ct_sa_nomsg.c", R"(
compile_time.static_assert(1 == 2);
int32 main() { return 0; }
)");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool found = false;
    for (const auto& d : analyzer.getErrors()) {
        if (d.message.find("static_assert failed") != std::string::npos
            && d.message.find("static_assert failed:") == std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(CompileTimeEval, StaticAssertNonConstant) {
    Lexer lexer("ct_sa_nonconst.c", R"(
int32 main() { int32 x = 1; compile_time.static_assert(x == 1, "x"); return 0; }
)");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool found = false;
    for (const auto& d : analyzer.getErrors()) {
        if (d.message.find("compile_time argument must be a compile-time constant") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(CompileTimeEval, UnknownTypeArgDiagnosed) {
    Lexer lexer("ct_unknown_type.c", "int32 main() { usize s = compile_time.size_of(Nope); return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool found = false;
    for (const auto& d : analyzer.getErrors()) {
        if (d.message.find("unknown type 'Nope' in compile_time expression") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

// ---- P1-04 / CT-03: compile_time.if 条件诊断 ----

TEST_F(CompileTimeEval, CTIfNonConstantCond) {
    Lexer lexer("ct_if_nonconst.c", R"(
int32 main() { int32 x = 1; return x; }
compile_time.if (x == 1) { int32 a; } else { int32 b; }
)");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool found = false;
    for (const auto& d : analyzer.getErrors()) {
        if (d.message.find("compile_time.if condition must be a compile-time constant") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(CompileTimeEval, CTIfNonBoolCond) {
    Lexer lexer("ct_if_nonbool.c", R"(
compile_time.if ("str") { int32 a; } else { int32 b; }
int32 main() { return 0; }
)");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool found = false;
    for (const auto& d : analyzer.getErrors()) {
        if (d.message.find("compile_time.if condition must be a boolean") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

// ---- P1-04 / T7 硬化（回归 pin）----

TEST_F(CompileTimeEval, UserCompileTimeVarNotHijacked) {
    // 用户自定义 compile_time 变量：普通标识符使用不被劫持（无成员访问）。
    EXPECT_TRUE(analyzeOk(R"(
int32 compile_time = 3;
int32 main() { return compile_time + 1; }
)"));
}

TEST_F(CompileTimeEval, CTInFunctionBodyStaticAssert) {
    // 函数体内 static_assert（表达式路径）：成功与失败各一。
    EXPECT_TRUE(analyzeOk(R"(
int32 main() { compile_time.static_assert(2 > 1, "in body"); return 0; }
)"));
    Lexer lexer("ct_sa_body_fail.c", R"(
int32 main() { compile_time.static_assert(2 > 3, "body fail"); return 0; }
)");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool found = false;
    for (const auto& d : analyzer.getErrors()) {
        if (d.message.find("static_assert failed: body fail") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

// ---- P1-04 评审修复（I1/I2/I3/I4/I5）----

TEST_F(CompileTimeEval, ConstExprFnWithCTArg) {
    // I1: compile_time 函数实参含 compile_time 根 → 委托不得互递归。
    EXPECT_TRUE(analyzeOk(R"(
compile_time int32 twice(int32 x) { return x * 2; }
compile_time int32 r = twice(compile_time.size_of(int32));
int32 main() { return 0; }
)"));
}

TEST_F(CompileTimeEval, CharInCTTernary) {
    // I2: CHAR 是运行时类型，CT 常量条件下的三元不得误报 STR 逃逸。
    EXPECT_TRUE(analyzeOk(R"(
int32 main() { char c = compile_time.build.debug ? 'a' : 'b'; return 0; }
)"));
}

TEST_F(CompileTimeEval, DeadBranchTypeWrappedUseDiagnosed) {
    // I3: 毒化检查覆盖指针/数组包装层。
    auto deadBranchSource = [](const std::string& use) {
        return "compile_time.if (1 == 2) { struct Ghost { int32 x; } } "
               "else { int32 h() { return 1; } } "
               "int32 main() { " + use + " return 0; }";
    };
    for (const char* use : {"Ghost* p;", "Ghost g2[3];"}) {
        Lexer lexer("ct_dead_wrap.c", deadBranchSource(use));
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        auto ast = parser.parse();
        ASSERT_NE(ast, nullptr) << use;
        SemanticAnalyzer analyzer;
        analyzer.analyze(*ast);
        EXPECT_FALSE(analyzer.getErrors().empty()) << use;
    }
}

TEST_F(CompileTimeEval, UserCompileTimeVarMemberAccess) {
    // I4: 用户自定义 compile_time 变量的成员访问不被劫持（spec §1 消歧）。
    EXPECT_TRUE(analyzeOk(R"(
struct S { int32 target; }
int32 main() {
    S compile_time;
    compile_time.target = 1;
    return compile_time.target;
}
)"));
}

TEST_F(CompileTimeEval, CompileTimeInsideNamespaceReal) {
    // I5: compile_time 根链在 namespace 体内不被 namespacePrefix 劫持。
    EXPECT_TRUE(analyzeOk(R"(
namespace N { compile_time int32 ok = compile_time.build.debug; }
)"));
}

// P1-06 (FMT-03): the language layer has no `str + str` (spec §2.3) — the
// CT-06 literal concat went with it (Ruling: spec §2.5). Comparisons still
// fold.
TEST(CompileTimeStrOps, LiteralConcatRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { compile_time.static_assert(\"a\" + \"b\" == \"ab\"); return 0; }"));
    EXPECT_FALSE(analyzeOk(
        "int32 main() { string_view a = \"x\"; string_view b = a + \"y\"; return 0; }"));
}

TEST(CompileTimeStrOps, LiteralEqStillCT) {
    EXPECT_TRUE(analyzeOk(
        "int32 main() { compile_time.static_assert(\"a\" == \"a\"); return 0; }"));
}
