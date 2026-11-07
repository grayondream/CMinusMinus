// PAR-12: 匿名类型身份验证——每个匿名定义是独立类型（C 语义）：
// 赋值/传参/返回互斥；using 命名后同型可互赋；定义即用的返回类型可用。
#include <gtest/gtest.h>

#include "codegen/CodegenContext.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "support/Log.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"

class AnonTypeE2E : public ::testing::Test {
protected:
    void SetUp() override {
        spdlog::set_level(spdlog::level::off);
        static bool init = false;
        if (!init) {
            llvm::InitializeAllTargetInfos();
            llvm::InitializeAllTargets();
            llvm::InitializeAllTargetMCs();
            llvm::InitializeAllAsmParsers();
            llvm::InitializeAllAsmPrinters();
            init = true;
        }
    }
};

static int runSource(const std::string& source, const std::string& filename) {
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast || !parser.getErrors().empty()) return -1;

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return -1;

    CodegenContext ctx;
    ctx.setSourceFile(filename);
    ast->codegen(ctx);
    ctx.finalizeDebugInfo();

    std::string ve;
    llvm::raw_string_ostream vs(ve);
    if (llvm::verifyModule(ctx.getModule(), &vs)) return -1;

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) return -1;
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit) return -1;
    if (auto e = (*jit)->addIRModule(
            llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext()))) {
        return -1;
    }
    auto sym = (*jit)->lookup("main");
    if (!sym) return -1;
    auto mainFn = sym->toPtr<int32_t(*)()>();
    return mainFn();
}

// 语义分析是否通过（供负向用例：期望编译失败）。
static bool semaOk(const std::string& source) {
    Lexer lexer("anon_neg.cmm", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast || !parser.getErrors().empty()) return false;
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    return analyzer.getErrors().empty();
}

TEST_F(AnonTypeE2E, DistinctAnonymousStructsRejectAssignment) {
    // C 语义：两次独立的匿名 struct 定义是不同类型——赋值必须拒绝。
    EXPECT_FALSE(semaOk(R"(
        struct { int32 x; } a;
        struct { int32 x; } b;
        int32 main() { a = b; return 0; }
    )"));
}

TEST_F(AnonTypeE2E, DistinctAnonymousUnionsRejectAssignment) {
    EXPECT_FALSE(semaOk(R"(
        union { int32 i; } a;
        union { int32 i; } b;
        int32 main() { a = b; return 0; }
    )"));
}

TEST_F(AnonTypeE2E, UsingNamedAnonymousStructsAreSameType) {
    // using 给匿名类型命名后，同一别名的两个变量可互赋。
    EXPECT_EQ(runSource(R"(
        using S = struct { int32 v; };
        int32 main() {
            S a = { 3 };
            S b;
            b = a;
            if (b.v != 3) return 1;
            return 0;
        }
    )", "anon_using.cmm"), 0);
}

TEST_F(AnonTypeE2E, SameDefinitionMultipleDeclaratorsShareType) {
    // 同一声明 `struct { ... } a, b;` 共享同一个匿名类型 → 可互赋。
    EXPECT_EQ(runSource(R"(
        int32 main() {
            struct { int32 v; } a, b;
            a.v = 5;
            b = a;
            if (b.v != 5) return 1;
            return 0;
        }
    )", "anon_multi_same.cmm"), 0);
}

TEST_F(AnonTypeE2E, AnonymousStructAsFieldArrayPointerNested) {
    EXPECT_EQ(runSource(R"(
        struct Outer { int32 tag; struct { int32 a; int32 b; } inner; };
        using Q = struct { int32 q; };
        int32 main() {
            struct Outer o = { 0, { 0, 0 } };
            o.inner.a = 1; o.inner.b = 2;
            if (o.inner.a != 1 || o.inner.b != 2) return 1;
            if (sizeof(o) != 12) return 2;
            struct { int32 n; } arr[2] = { { 0 }, { 0 } };
            arr[1].n = 9;
            if (arr[1].n != 9 || arr[0].n != 0) return 3;
            Q qs[2] = { { 0 }, { 0 } };
            Q* p = &qs[1];
            p->q = 4;
            if (qs[1].q != 4) return 4;
            return 0;
        }
    )", "anon_nested.cmm"), 0);
}

TEST_F(AnonTypeE2E, AnonymousReturnTypeDefinitionUses) {
    // 定义即用的匿名返回类型（同一 AST 节点内）可用。
    EXPECT_EQ(runSource(R"(
        using S = struct { int32 v; };
        S make(int32 x) { S t; t.v = x; return t; }
        int32 main() {
            S r = make(42);
            if (r.v != 42) return 1;
            return 0;
        }
    )", "anon_ret.cmm"), 0);
}

TEST_F(AnonTypeE2E, DistinctAnonymousArgsRejected) {
    // 两个不同匿名定义作实参/形参互斥。
    EXPECT_FALSE(semaOk(R"(
        struct { int32 v; } a;
        int32 f(struct { int32 v; } p) { return p.v; }
        int32 main() { return f(a); }
    )"));
}
