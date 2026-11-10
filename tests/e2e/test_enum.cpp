// TYP-25 / AGG-14/15: enum types and enumerator constants.
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

class EnumE2E : public ::testing::Test {
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
    Lexer lexer("enum.cmm", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast || !parser.getErrors().empty()) return -1;

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return -1;

    CodegenContext ctx;
    ctx.setSourceFile("enum.cmm");
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

TEST_F(EnumE2E, ImplicitAndExplicitValues) {
    EXPECT_EQ(runSource(
        "enum Color { RED, GREEN = 5, BLUE };\n"
        "int32 main() { return BLUE - 6; }\n"), 0);
}

TEST_F(EnumE2E, FirstValueIsZero) {
    // TYP-20: enumerators are strongly typed; converting to int is explicit.
    EXPECT_EQ(runSource(
        "enum Color { RED, GREEN };\n"
        "int32 main() { return (int32)RED; }\n"), 0);
}

TEST_F(EnumE2E, EnumeratorInArithmetic) {
    EXPECT_EQ(runSource(
        "enum E { N = 2 };\n"
        "int32 main() { return (N * 3) - 6; }\n"), 0);
}

TEST_F(EnumE2E, EnumTypedVariable) {
    EXPECT_EQ(runSource(
        "enum Color { A, B, C };\n"
        "int32 main() { enum Color c = C; return c - 2; }\n"), 0);
}

TEST_F(EnumE2E, EnumeratorIsConstexpr) {
    // TYP-20: enum -> int is explicit, but still a constant expression.
    EXPECT_EQ(runSource(
        "enum E { X = 3 };\n"
        "int32 main() { compile_time int32 k = (int32)X; return k - 3; }\n"), 0);
}

TEST_F(EnumE2E, SwitchOnEnum) {
    EXPECT_EQ(runSource(R"(
enum E { A, B, C };
int32 main() {
    enum E e = B;
    switch (e) {
        case A: return 1;
        case B: return 0;
        default: return 2;
    }
}
)"), 0);
}
