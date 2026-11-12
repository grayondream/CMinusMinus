// 全局变量 codegen：无初始化器的全局变量必须是零初始化定义（tentative
// definition 语义），而非 external 声明。含全局 slice 的 D3 零初始化（TYP-12）。
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

class GlobalsE2E : public ::testing::Test {
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

// An uninitialized global is a zero-initialized definition, not an
// external declaration (the old bug: `int32 g;` emitted no storage).
TEST_F(GlobalsE2E, UninitializedGlobalZeroInitialized) {
    EXPECT_EQ(runSource(R"(
        int32 g;
        int32 main() { return g; }
    )", "test_global_zero.cdd"), 0);
}

// An initialized global keeps its value.
TEST_F(GlobalsE2E, InitializedGlobalValue) {
    EXPECT_EQ(runSource(R"(
        int32 g = 5;
        int32 main() { return g - 5; }
    )", "test_global_init.cdd"), 0);
}

// Writes to a global are visible across calls.
TEST_F(GlobalsE2E, GlobalWriteVisible) {
    EXPECT_EQ(runSource(R"(
        int32 counter;
        void bump() { counter = counter + 1; }
        int32 main() {
            bump();
            bump();
            return counter == 2 ? 0 : 1;
        }
    )", "test_global_write.cdd"), 0);
}

// TYP-12 D3 via the global fix: an uninitialized global slice is the
// empty slice {null, 0} (restored from TYP-12 Task 6).
TEST_F(GlobalsE2E, GlobalSliceZeroInitialized) {
    EXPECT_EQ(runSource(R"(
        int32[] g;
        int32 main() { return g.len == 0 ? 0 : 1; }
    )", "test_global_slice.cdd"), 0);
}
