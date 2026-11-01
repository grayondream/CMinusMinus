#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>

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

class ConstexprFunctionE2E : public ::testing::Test {
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
    bool bad = llvm::verifyModule(ctx.getModule(), &vs);
    if (bad) return -1;

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

TEST_F(ConstexprFunctionE2E, ConstexprFactorial) {
    EXPECT_EQ(runSource(R"(
constexpr int32 factorial(int32 n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int32 main() {
    constexpr int32 f5 = factorial(5);
    return f5 - 120;
}
)", "test_constexpr_factorial.c"), 0);
}

TEST_F(ConstexprFunctionE2E, ConstexprSquare) {
    EXPECT_EQ(runSource(R"(
constexpr int32 square(int32 x) {
    return x * x;
}

int32 main() {
    constexpr int32 s5 = square(5);
    return s5 - 25;
}
)", "test_constexpr_square.c"), 0);
}

TEST_F(ConstexprFunctionE2E, ConstexprForLoop) {
    EXPECT_EQ(runSource(R"(
constexpr int32 sumTo(int32 n) {
    int32 total = 0;
    for (int32 i = 0; i < n; i = i + 1) total = total + i;
    return total;
}
int32 main() { constexpr int32 x = sumTo(10); return x - 45; }
)", "test_constexpr_for.c"), 0);
}

TEST_F(ConstexprFunctionE2E, ConstexprNestedCall) {
    EXPECT_EQ(runSource(R"(
constexpr int32 inc(int32 x) { return x + 1; }
constexpr int32 twice(int32 x) { return inc(x) * 2; }
int32 main() { constexpr int32 x = twice(20); return x - 42; }
)", "test_constexpr_nested.c"), 0);
}

TEST_F(ConstexprFunctionE2E, ConstexprWhileLoop) {
    EXPECT_EQ(runSource(R"(
constexpr int32 countBits(uint32 v) {
    int32 n = 0;
    while (v != 0) {
        n = n + (int32)(v & 1);
        v = v >> 1;
    }
    return n;
}
int32 main() { constexpr int32 x = countBits(0xF0u); return x - 4; }
)", "test_constexpr_while.c"), 0);
}

TEST_F(ConstexprFunctionE2E, ConstexprDoWhileAndBreakContinue) {
    EXPECT_EQ(runSource(R"(
constexpr int32 sumOdd(int32 n) {
    int32 total = 0;
    int32 i = 0;
    do {
        i = i + 1;
        if (i > n) break;
        if ((i & 1) == 0) continue;
        total = total + i;
    } while (i < 100);
    return total;
}
int32 main() { constexpr int32 x = sumOdd(9); return x - 25; }
)", "test_constexpr_do_while.c"), 0);
}

TEST_F(ConstexprFunctionE2E, ConstexprCallbackCall) {
    EXPECT_EQ(runSource(R"(
uint64 mix(uint64 x) { return (x ^ (x >> 30)) * 3; }
constexpr uint64 apply(uint64 x) { return mix(x); }
int32 main() { constexpr uint64 v = apply(7); return v == mix(7) ? 0 : 1; }
)", "test_constexpr_callback.c"), 0);
}

TEST_F(ConstexprFunctionE2E, ConstexprCompoundAssignAndInc) {
    EXPECT_EQ(runSource(R"(
constexpr int32 calc() {
    int32 x = 1;
    x += 4;
    x *= 3;
    x -= 2;
    x++;
    ++x;
    return x;
}
int32 main() { constexpr int32 x = calc(); return x - 15; }
)", "test_constexpr_assign.c"), 0);
}

