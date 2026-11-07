// P1-09 (STD-14): std.bit 纯语言位工具端到端测试。
// fixture 仿 tests/e2e/test_std_string_lib.cpp 的 runWithImports。

#include <gtest/gtest.h>

#include <cstdlib>
#include <set>
#include <string>

#include "ast/Decl.h"
#include "codegen/CodegenContext.h"
#include "driver/ModuleLoader.h"
#include "driver/StdPrelude.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "support/Log.h"

#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <spdlog/spdlog.h>

namespace {

class StdBitTest : public ::testing::Test {
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

static int runWithImports(const std::string& source, const std::string& filename) {
    auto ast = smc::parseStdCPrelude(source, filename);
    if (!ast) return -1;

    smc::ModuleSearchPaths paths{{STD_DIR}};
    std::set<std::string> loaded;
    std::vector<std::string> errors;
    smc::processImports(*ast, ".", paths, loaded, errors);
    if (!errors.empty()) return -1;

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

    auto sym = (*jit)->lookup("main");
    if (!sym) return -1;
    return ((int (*)())(intptr_t)sym->getValue())();
}

TEST_F(StdBitTest, PopCount32) {
    EXPECT_EQ(runWithImports(R"(
        import std.bit;
        int32 main() {
            if (std::popcount32(0x0Fu) != 4) return 1;
            if (std::popcount32(0xF0F0F0F0u) != 16) return 2;
            if (std::popcount32(0u) != 0) return 3;
            if (std::popcount32(0xFFFFFFFFu) != 32) return 4;
            return 0;
        }
    )", "bit_pop32.cmm"), 0);
}

TEST_F(StdBitTest, PopCount64) {
    EXPECT_EQ(runWithImports(R"(
        import std.bit;
        int32 main() {
            if (std::popcount64(0x8000000000000000ul) != 1) return 1;
            if (std::popcount64(0xFFFFFFFFul) != 32) return 2;
            if (std::popcount64(0ul) != 0) return 3;
            if (std::popcount64(0xFFFFFFFFFFFFFFFFul) != 64) return 4;
            return 0;
        }
    )", "bit_pop64.cmm"), 0);
}

TEST_F(StdBitTest, ClzCtz32) {
    EXPECT_EQ(runWithImports(R"(
        import std.bit;
        int32 main() {
            if (std::clz32(1u) != 31) return 1;
            if (std::clz32(0u) != 32) return 2;
            if (std::clz32(0x80000000u) != 0) return 3;
            if (std::ctz32(8u) != 3) return 4;
            if (std::ctz32(0u) != 32) return 5;
            if (std::ctz32(1u) != 0) return 6;
            return 0;
        }
    )", "bit_clz32.cmm"), 0);
}

TEST_F(StdBitTest, ClzCtz64) {
    EXPECT_EQ(runWithImports(R"(
        import std.bit;
        int32 main() {
            if (std::clz64(0x8000000000000000ul) != 0) return 1;
            if (std::clz64(1) != 63) return 2;
            if (std::clz64(0) != 64) return 3;
            if (std::ctz64(0x8000000000000000ul) != 63) return 4;
            if (std::ctz64(1) != 0) return 5;
            if (std::ctz64(0) != 64) return 6;
            return 0;
        }
    )", "bit_clz64.cmm"), 0);
}

TEST_F(StdBitTest, RotlRotr32) {
    EXPECT_EQ(runWithImports(R"(
        import std.bit;
        int32 main() {
            if (std::rotl32(0x80000000u, 1) != 1u) return 1;
            if (std::rotl32(1u, 1) != 2u) return 2;
            if (std::rotl32(1u, 33) != 2u) return 3;  // 取模 32
            if (std::rotr32(1u, 1) != 0x80000000u) return 4;
            return 0;
        }
    )", "bit_rot32.cmm"), 0);
}

TEST_F(StdBitTest, RotlRotr64) {
    EXPECT_EQ(runWithImports(R"(
        import std.bit;
        int32 main() {
            if (std::rotl64(0x8000000000000000ul, 1) != 1) return 1;
            if (std::rotl64(1, 63) != 0x8000000000000000ul) return 2;
            if (std::rotr64(1, 1) != 0x8000000000000000ul) return 3;
            if (std::rotr64(2, 1) != 1) return 4;
            return 0;
        }
    )", "bit_rot64.cmm"), 0);
}

TEST_F(StdBitTest, BitcastWrappers) {
    EXPECT_EQ(runWithImports(R"(
        import std.bit;
        int32 main() {
            float64 f = 1.0;
            uint64 bits = std::f64_to_bits(f);
            float64 back = std::bits_to_f64(bits);
            if (back != 1.0) return 1;
            int32 x = 12345;
            uint32 xb = std::i32_to_u32(x);
            int32 xr = std::u32_to_i32(xb);
            if (xr != 12345) return 2;
            return 0;
        }
    )", "bit_bitcast.cmm"), 0);
}

}  // namespace
