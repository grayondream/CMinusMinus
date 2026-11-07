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

class OverloadingE2E : public ::testing::Test {
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

TEST_F(OverloadingE2E, FunctionOverloading) {
    EXPECT_EQ(runSource(R"(
        int32 add(int32 a, int32 b) { return a + b; }
        int32 add(int32 a, int32 b, int32 c) { return a + b + c; }
        
        int32 main() {
            int32 r1 = add(3, 4);
            int32 r2 = add(1, 2, 3);
            return r1 + r2 - 13;
        }
    )", "test_func_overload.cmm"), 0);
}

TEST_F(OverloadingE2E, OperatorOverloading) {
    EXPECT_EQ(runSource(R"(
        struct Point { int32 x; int32 y; };
        
        struct Point operator+(struct Point a, struct Point b) {
            struct Point result;
            result.x = a.x + b.x;
            result.y = a.y + b.y;
            return result;
        }
        
        int32 main() {
            struct Point p1;
            p1.x = 1;
            p1.y = 2;
            struct Point p2;
            p2.x = 3;
            p2.y = 4;
            struct Point p3 = p1 + p2;
            return p3.x - 4;
        }
    )", "test_op_overload.cmm"), 0);
}

// TYP-23: an int32-valued expression (notably a comparison) must be usable
// where a bool parameter is expected, like assignment already allows.
TEST_F(OverloadingE2E, ComparisonResultAsBoolArgument) {
    EXPECT_EQ(runSource(R"(
        int32 truth(bool b) { return b ? 1 : 0; }
        int32 main() {
            if (truth(1 == 1) != 1) return 1;
            if (truth(1 == 2) != 0) return 2;
            if (truth(7) != 1) return 3;
            return 0;
        }
    )", "cmp_bool_arg.cmm"), 0);
}

TEST_F(OverloadingE2E, ComparisonResultPrefersInt32Overload) {
    EXPECT_EQ(runSource(R"(
        int32 pick(bool b) { return 1; }
        int32 pick(int32 x) { return 2; }
        int32 main() { return pick(1 == 1) - 2; }
    )", "cmp_bool_overload.cmm"), 0);
}
