// TYP-24 / SEM-02: null-pointer constants, pointer/integer comparison, and
// logical negation of pointers and integers.
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
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"

class PointerCompareE2E : public ::testing::Test {
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

static int runSource(const std::string& source) {
    Lexer lexer("ptr.cdd", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast || !parser.getErrors().empty()) return -1;

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return -1;

    CodegenContext ctx;
    ctx.setSourceFile("ptr.cdd");
    ast->codegen(ctx);
    ctx.finalizeDebugInfo();

    std::string ve;
    llvm::raw_string_ostream vs(ve);
    if (llvm::verifyModule(ctx.getModule(), &vs)) return -1;

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) return -1;
    auto jit = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit) return -1;

    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto e = (*jit)->addIRModule(std::move(ts))) return -1;

    auto sym = (*jit)->lookup("main");
    if (!sym) return -1;
    return ((int (*)())(intptr_t)sym->getValue())();
}

TEST_F(PointerCompareE2E, NonNullNotEqualToNull) {
    EXPECT_EQ(runSource("int32 main() { int32 x; int32* p = &x; return (p == null) ? 1 : 0; }"), 0);
}

TEST_F(PointerCompareE2E, NonNullNotEqualToNullOperator) {
    EXPECT_EQ(runSource("int32 main() { int32 x; int32* p = &x; return (p != null) ? 0 : 1; }"), 0);
}

TEST_F(PointerCompareE2E, NullPointerEqualsNull) {
    EXPECT_EQ(runSource("int32 main() { int32* p = null; return (p == null) ? 0 : 1; }"), 0);
}

TEST_F(PointerCompareE2E, NullOnTheLeft) {
    EXPECT_EQ(runSource("int32 main() { int32* p = null; return (0 == p) ? 0 : 1; }"), 0);
}

TEST_F(PointerCompareE2E, AssignNullThenCompare) {
    EXPECT_EQ(runSource("int32 main() { int32 x; int32* p = &x; p = null; return (p == null) ? 0 : 1; }"), 0);
}

TEST_F(PointerCompareE2E, PointerConditionalIsNotNull) {
    EXPECT_EQ(runSource("int32 main() { int32 x; int32* p = &x; if (p) return 0; return 1; }"), 0);
}

TEST_F(PointerCompareE2E, NullPointerConditionalIsFalse) {
    EXPECT_EQ(runSource("int32 main() { int32* p = null; if (p) return 1; return 0; }"), 0);
}

TEST_F(PointerCompareE2E, LogicalNotOnPointer) {
    EXPECT_EQ(runSource("int32 main() { int32* p = null; if (!p) return 0; return 1; }"), 0);
}

TEST_F(PointerCompareE2E, LogicalNotOnIntegerIsNotBitwise) {
    // `!5` must be 0 (not ~5). `!0` must be 1.
    EXPECT_EQ(runSource("int32 main() { if (!5) return 1; if (!0) return 0; return 2; }"), 0);
}

TEST_F(PointerCompareE2E, RestrictQualifiedParamAccepted) {
    EXPECT_EQ(runSource("int32 f(int32* restrict p) { return *p; } int32 main() { int32 x = 5; return f(&x) - 5; }"), 0);
}
