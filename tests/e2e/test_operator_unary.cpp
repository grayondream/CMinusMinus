// SEM-17: unary operator overloading call sites — end-to-end tests.
// Declaration side already parses `operator-` / `operator~` / `operator++` /
// `operator--` / `operator!`; these tests pin the CALL side (sema lookup +
// codegen call). C++ signature alignment: prefix `operator++` takes one
// operand (free function form `V operator++(V)`), postfix takes the operand
// plus an `int32` dummy (`V operator++(V, int32)`).
// fixture 仿 test_overloading.cpp 的 runSource。

#include <gtest/gtest.h>

#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "codegen/CodegenContext.h"
#include "support/Log.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"

#include <spdlog/spdlog.h>

class OperatorUnaryE2E : public ::testing::Test {
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
    if (!ast) return -1;

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
        .setJITTargetMachineBuilder(std::move(*jtmb))
        .create();
    if (!jit) return -1;
    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto e = (*jit)->addIRModule(std::move(ts))) return -1;
    auto sym = (*jit)->lookup("main");
    if (!sym) return -1;
    auto fn = (int (*)())(intptr_t)sym->getValue();
    return fn();
}

TEST_F(OperatorUnaryE2E, UnaryMinus) {
    EXPECT_EQ(runSource(R"(
        struct V { int32 x; };
        struct V operator-(struct V a) {
            struct V r;
            r.x = -a.x;
            return r;
        }
        int32 main() {
            struct V v;
            v.x = 5;
            struct V n = -v;
            return n.x + 5;
        }
    )", "op_unary_minus.cdd"), 0);
}

TEST_F(OperatorUnaryE2E, UnaryBitNot) {
    EXPECT_EQ(runSource(R"(
        struct V { int32 x; };
        struct V operator~(struct V a) {
            struct V r;
            r.x = ~a.x;
            return r;
        }
        int32 main() {
            struct V v;
            v.x = 0;
            struct V n = ~v;
            return n.x + 1;
        }
    )", "op_unary_bitnot.cdd"), 0);
}

TEST_F(OperatorUnaryE2E, PrefixIncrement) {
    EXPECT_EQ(runSource(R"(
        struct V { int32 x; };
        struct V operator++(struct V a) {
            a.x = a.x + 1;
            return a;
        }
        int32 main() {
            struct V v;
            v.x = 1;
            struct V w = ++v;
            return w.x - 2;
        }
    )", "op_unary_preinc.cdd"), 0);
}

TEST_F(OperatorUnaryE2E, PostfixIncrementWithDummyInt) {
    EXPECT_EQ(runSource(R"(
        struct V { int32 x; };
        struct V operator++(struct V a, int32 dummy) {
            a.x = a.x + 1;
            return a;
        }
        int32 main() {
            struct V v;
            v.x = 1;
            struct V w = v++;
            return w.x - 2;
        }
    )", "op_unary_postinc.cdd"), 0);
}

TEST_F(OperatorUnaryE2E, LogicalNot) {
    EXPECT_EQ(runSource(R"(
        struct V { int32 x; };
        bool operator!(struct V a) {
            return a.x == 0;
        }
        int32 main() {
            struct V v;
            v.x = 0;
            if (!v) { return 0; }
            return 1;
        }
    )", "op_unary_not.cdd"), 0);
}

TEST_F(OperatorUnaryE2E, BuiltinUnaryStillWorks) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 x = 5;
            int32 y = -x;
            if (~0 != -1) { return 1; }
            return y + 5;
        }
    )", "op_unary_builtin.cdd"), 0);
}
