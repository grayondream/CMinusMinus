// TYP-11: 1-D array parameters `T name[N]` desugar to slice `T[]` (zero-copy).
// Spec: docs/superpowers/specs/2026-10-01-typ11-array-params-design.md
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

class ArrayParamsE2E : public ::testing::Test {
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
    auto jit = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit) return -1;

    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto e = (*jit)->addIRModule(std::move(ts))) return -1;

    auto entry = (*jit)->lookup("main");
    if (!entry) return -1;
    using MainFn = int (*)();
    auto fn = reinterpret_cast<MainFn>(entry->getValue());
    if (!fn) return -1;
    return fn();
}

// TYP-11: summing through an array parameter yields the correct values.
TEST_F(ArrayParamsE2E, ArrayParamSum) {
    EXPECT_EQ(runSource(R"(
        int32 APSum(int32 a[2]) { return a[0] + a[1]; }
        int32 main() {
            int32 arr[2] = {3, 4};
            return APSum(arr) == 7 ? 0 : 1;
        }
    )", "test_ap_sum.cmm"), 0);
}

// Review Focus 1: the [N] in the parameter is documentation only — any
// length array of the matching element type is accepted.
TEST_F(ArrayParamsE2E, ArrayParamAnyLengthAccepted) {
    EXPECT_EQ(runSource(R"(
        int32 APHead(int32 a[2]) { return a[0] + a[1]; }
        int32 main() {
            int32 big[5] = {10, 20, 30, 40, 50};
            return APHead(big) == 30 ? 0 : 1;
        }
    )", "test_ap_anylen.cmm"), 0);
}

// TYP-12 zero-copy semantics carry over: writes through the parameter are
// visible in the caller's original array.
TEST_F(ArrayParamsE2E, ArrayParamWriteThrough) {
    EXPECT_EQ(runSource(R"(
        void APBump(int32 a[2]) { a[0] = a[0] + 100; }
        int32 main() {
            int32 arr[2] = {1, 2};
            APBump(arr);
            return arr[0] == 101 && arr[1] == 2 ? 0 : 1;
        }
    )", "test_ap_writethrough.cmm"), 0);
}

// Review Focus 3: the dangling-return guard must not reject returning a
// (desugared) slice parameter.
TEST_F(ArrayParamsE2E, ArrayParamReturnSlice) {
    EXPECT_EQ(runSource(R"(
        int32[] APPick(int32 arr[2]) { return arr; }
        int32 main() {
            int32 a[2] = {7, 8};
            int32[] s = APPick(a);
            return s[1] == 8 && s.len == 2 ? 0 : 1;
        }
    )", "test_ap_return.cmm"), 0);
}
