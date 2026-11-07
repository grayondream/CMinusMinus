// P0-05 / STD-01, STD-12: the minimal std.core and std.io modules.
#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "ast/Decl.h"
#include "codegen/CodegenContext.h"
#include "driver/ModuleLoader.h"
#include "driver/StdPrelude.h"
#include "sema/SemanticAnalyzer.h"
#include "support/Log.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"

#include <spdlog/spdlog.h>

namespace {

class StdModulesTest : public ::testing::Test {
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

int runWithImports(const std::string& source, const std::string& filename) {
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

// Compile (sema + codegen + verify) without executing, returning the module IR.
// Used for the terminating builtins (`assert`/`panic`), which cannot be run
// in-process because they abort.
std::string compileToIR(const std::string& source, const std::string& filename) {
    auto ast = smc::parseStdCPrelude(source, filename);
    if (!ast) return "";

    smc::ModuleSearchPaths paths{{STD_DIR}};
    std::set<std::string> loaded;
    std::vector<std::string> errors;
    smc::processImports(*ast, ".", paths, loaded, errors);
    if (!errors.empty()) return "";

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return "";

    CodegenContext ctx;
    ctx.setSourceFile(filename);
    ast->codegen(ctx);
    ctx.finalizeDebugInfo();

    std::string ve;
    llvm::raw_string_ostream vs(ve);
    if (llvm::verifyModule(ctx.getModule(), &vs)) return "";

    std::string ir;
    llvm::raw_string_ostream os(ir);
    ctx.getModule().print(os, nullptr);
    return ir;
}

} // namespace

TEST_F(StdModulesTest, ResolvesModulesFromStdDir) {
    smc::ModuleSearchPaths paths{{STD_DIR}};
    EXPECT_FALSE(smc::resolveImport("std.core", paths).empty());
    EXPECT_FALSE(smc::resolveImport("std.io", paths).empty());
}

TEST_F(StdModulesTest, CoreMinMax) {
    EXPECT_EQ(runWithImports(
        "import std.core;\nint32 main() { return std::min(3, 5) + std::max(2, 9) - 12; }\n",
        "std_core_minmax.cmm"), 0);
}

TEST_F(StdModulesTest, CoreClamp) {
    EXPECT_EQ(runWithImports(
        "import std.core;\n"
        "int32 main() { return std::clamp(42, 0, 10) - 10; }\n",
        "std_core_clamp.cmm"), 0);
}

TEST_F(StdModulesTest, CoreConstexprUse) {
    // std::min is constexpr and folds at compile time.
    EXPECT_EQ(runWithImports(
        "import std.core;\n"
        "int32 main() { constexpr int32 m = std::min(4, 7); return m - 4; }\n",
        "std_core_constexpr.cmm"), 0);
}

TEST_F(StdModulesTest, IoPrintInt) {
    EXPECT_EQ(runWithImports(
        "import std.io;\nint32 main() { std::print_int(7); return 0; }\n",
        "std_io_print_int.cmm"), 0);
}

// ----- P0-05 / STD-01: builtin assert/panic (location-aware terminators) -----

TEST_F(StdModulesTest, AssertTrueRuns) {
    // A satisfied assertion is a no-op; execution continues.
    EXPECT_EQ(runWithImports(
        "int32 main() { assert(1 + 1 == 2); return 7; }\n",
        "assert_true.cmm"), 7);
}

TEST_F(StdModulesTest, AssertFalseEmitsAbortWithLocation) {
    std::string ir = compileToIR(
        "int32 main() { int32 x = 0; assert(x); return 0; }\n",
        "assert_false.cmm");
    ASSERT_FALSE(ir.empty());
    EXPECT_NE(ir.find("@abort"), std::string::npos);
    EXPECT_NE(ir.find("assert_false.cmm:1"), std::string::npos);
}

TEST_F(StdModulesTest, PanicEmitsAbortWithLocation) {
    std::string ir = compileToIR(
        "int32 main() { panic(\"boom\"); return 0; }\n",
        "panic.cmm");
    ASSERT_FALSE(ir.empty());
    EXPECT_NE(ir.find("@abort"), std::string::npos);
    EXPECT_NE(ir.find("panic.cmm:1"), std::string::npos);
    EXPECT_NE(ir.find("panic: %s"), std::string::npos);
}

// ----- P0-05 / STD-12: file I/O round trip ------------------------------------

TEST_F(StdModulesTest, FileWriteReadRoundTrip) {
    EXPECT_EQ(runWithImports(
        "import std.io;\n"
        "int32 main() {\n"
        "    void* f = std::file_open(\"/tmp/smc_std_io_test.txt\", \"w\");\n"
        "    if (f == null) return 1;\n"
        "    char out[4];\n"
        "    out[0] = 'a'; out[1] = 'b'; out[2] = 'c'; out[3] = 0;\n"
        "    std::file_write(f, out, 3);\n"
        "    std::file_close(f);\n"
        "    void* g = std::file_open(\"/tmp/smc_std_io_test.txt\", \"r\");\n"
        "    if (g == null) return 2;\n"
        "    char in[8];\n"
        "    usize n = std::file_read(g, in, 3);\n"
        "    std::file_close(g);\n"
        "    in[n] = 0;\n"
        "    if (n != 3) return 3;\n"
        "    if (in[0] != 'a' || in[1] != 'b' || in[2] != 'c') return 4;\n"
        "    return 0;\n"
        "}\n", "std_io_file.cmm"), 0);
}

TEST_F(StdModulesTest, IoReadWriteChar) {
    EXPECT_EQ(runWithImports(
        "import std.io;\n"
        "int32 main() {\n"
        "    void* f = std::file_open(\"/tmp/smc_std_io_char.txt\", \"w\");\n"
        "    if (f == null) return 1;\n"
        "    std::file_write_char(f, 'Z');\n"
        "    std::file_close(f);\n"
        "    void* g = std::file_open(\"/tmp/smc_std_io_char.txt\", \"r\");\n"
        "    if (g == null) return 2;\n"
        "    int32 c = std::file_read_char(g);\n"
        "    std::file_close(g);\n"
        "    if (c != 'Z') return 3;\n"
        "    return 0;\n"
        "}\n", "std_io_char.cmm"), 0);
}
