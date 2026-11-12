// TYP-12: slice behaviour baseline — canonical LLVM type, subscript, .len,
// array decay. Spec: docs/superpowers/specs/2026-09-30-slice-design.md
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

class SliceE2ETest : public ::testing::Test {
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

// TYP-12 Task 1: slice values cross function boundaries with one canonical
// LLVM type; the LLVM Verifier must accept the module.
TEST_F(SliceE2ETest, SlicePassingVerifierClean) {
    EXPECT_EQ(runSource(R"(
        int32 probe(int32[] s) { return 0; }
        int32 main() {
            int32[] a;
            int32[] b = a;
            return probe(a) + probe(b);
        }
    )", "test_slice_canonical.cdd"), 0);
}

// TYP-12 Task 4: writes through a decayed slice are visible to the caller
// (the slice views the original array, no copy).
TEST_F(SliceE2ETest, DecayedSliceIsZeroCopy) {
    EXPECT_EQ(runSource(R"(
        void bump(int32[] s) { s[0] = s[0] + 100; }
        int32 main() {
            int32 arr[2] = {1, 2};
            bump(arr);
            return arr[0] == 101 ? 0 : 1;
        }
    )", "test_slice_decay.cdd"), 0);
}

// TYP-12 Task 5: subscript read + write through a decayed slice.
TEST_F(SliceE2ETest, SubscriptReadWrite) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[3] = {7, 8, 9};
            int32[] s = arr;
            return s[1] == 8 ? 0 : 1;
        }
    )", "test_slice_subscript.cdd"), 0);
}

// TYP-12 Task 5: decay through plain assignment too.
TEST_F(SliceE2ETest, DecayThroughAssignment) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[3] = {7, 8, 9};
            int32[] s = arr;
            s[2] = 42;
            return arr[2] == 42 && s[0] == 7 ? 0 : 1;
        }
    )", "test_slice_assign.cdd"), 0);
}

// TYP-12 D3: an uninitialized slice variable is the empty slice {null, 0}.
TEST_F(SliceE2ETest, UninitializedSliceIsEmpty) {
    EXPECT_EQ(runSource(R"(
        int64 probe(int32[] s) { return s.len; }
        int32 main() {
            int32[] s;
            if (s.len != 0) return 1;
            return probe(s) == 0 ? 0 : 2;
        }
    )", "test_slice_zero.cdd"), 0);
}
