// PAR-14 / TYP-24: alignof/offsetof keyword forms + nullptr literal.
// end-to-end execution tests. fixture 仿 test_string.cpp 的 runSource。

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

class ExprLiteralsE2E : public ::testing::Test {
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
    if (!ast) {
        ADD_FAILURE() << "Parse failed";
        return -1;
    }

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) {
        for (auto& err : analyzer.getErrors()) {
            ADD_FAILURE() << "Semantic error: " << err.format();
        }
        return -1;
    }

    CodegenContext ctx;
    ctx.setSourceFile(filename);
    ast->codegen(ctx);
    ctx.finalizeDebugInfo();

    std::string ve;
    llvm::raw_string_ostream vs(ve);
    bool bad = llvm::verifyModule(ctx.getModule(), &vs);
    if (bad) {
        vs.flush();
        ADD_FAILURE() << "Module verification failed: " << ve;
        return -1;
    }

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        ADD_FAILURE() << "JITTargetMachineBuilder::detectHost failed";
        return -1;
    }
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb))
        .create();
    if (!jit) {
        ADD_FAILURE() << "LLJITBuilder::create failed";
        return -1;
    }

    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto err = (*jit)->addIRModule(std::move(ts))) {
        ADD_FAILURE() << "addIRModule failed";
        return -1;
    }

    auto sym = (*jit)->lookup("main");
    if (!sym) {
        ADD_FAILURE() << "main not found";
        return -1;
    }
    return ((int (*)())(intptr_t)sym->getValue())();
}

TEST_F(ExprLiteralsE2E, AlignofMatchesCompileTime) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            if (alignof(int32) != compile_time.align_of(int32)) return 1;
            if (alignof(float64) != 8) return 2;
            return 0;
        }
    )", "alignof.cdd"), 0);
}

TEST_F(ExprLiteralsE2E, OffsetofSecondField) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 a; int32 b; };
        int32 main() {
            if (offsetof(S, b) != 4) return 1;
            if (offsetof(S, a) != 0) return 2;
            return 0;
        }
    )", "offsetof.cdd"), 0);
}

TEST_F(ExprLiteralsE2E, OffsetofValueUsedInComputation) {
    // C3 (review): offsetof must return a real value used in computation —
    // not silently drop the expression (the old e2e was false-green because
    // the failed codegen produced a skipped `if`).
    EXPECT_EQ(runSource(R"(
        struct S { int32 a; int32 b; float64 c; };
        int32 main() {
            // a@0, b@4, c@8
            usize total = offsetof(S, a) + offsetof(S, b) + offsetof(S, c);
            return total == 12 ? 0 : 1;
        }
    )", "offsetof_use.cdd"), 0);
}

TEST_F(ExprLiteralsE2E, NullPointerIsNullPointerConstant) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32* p = null;
            if (p != null) return 1;
            if (p != 0) return 2;
            return 0;
        }
    )", "nullptr.cdd"), 0);
}

TEST_F(ExprLiteralsE2E, NullPointerInBoolContext) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32* p = null;
            if (!p) return 0;
            return 1;
        }
    )", "nullptr_bool.cdd"), 0);
}

TEST_F(ExprLiteralsE2E, ShortFloatTypeKeywords) {
    EXPECT_EQ(runSource(R"(
        f16 a = 1.0f16;
        f32 b = 2.5f32;
        f64 c = 3.0;
        f128 d = 1.0f128;
        int32 main() {
            return (int32)a + (int32)b + (int32)c + (int32)d - 7;
        }
    )", "f16_keywords.cdd"), 0);
}
