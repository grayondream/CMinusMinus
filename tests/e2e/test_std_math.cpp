// P1-09 (STD-13): std.math 基础数学端到端测试（libc 绑定 + 常量 + 位级分类）。
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

class StdMathTest : public ::testing::Test {
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

TEST_F(StdMathTest, SqrtAndSqrtf) {
    EXPECT_EQ(runWithImports(R"(
        import std.math;
        int32 main() {
            if (std::sqrt(16.0) != 4.0) return 1;
            if (std::sqrtf(9.0f) != 3.0f) return 2;
            if (std::sqrt(2.0) < 1.41 || std::sqrt(2.0) > 1.42) return 3;
            return 0;
        }
    )", "math_sqrt.cmm"), 0);
}

TEST_F(StdMathTest, AbsFloorCeilRoundTrunc) {
    EXPECT_EQ(runWithImports(R"(
        import std.math;
        int32 main() {
            if (std::fabs(-2.5) != 2.5) return 1;
            if (std::floor(3.7) != 3.0) return 2;
            if (std::ceil(3.2) != 4.0) return 3;
            if (std::round(2.5) != 3.0) return 4;
            if (std::trunc(-2.7) != -2.0) return 5;
            return 0;
        }
    )", "math_floor.cmm"), 0);
}

TEST_F(StdMathTest, PowCosSin) {
    EXPECT_EQ(runWithImports(R"(
        import std.math;
        int32 main() {
            if (std::pow(2.0, 10.0) != 1024.0) return 1;
            if (std::cos(0.0) != 1.0) return 2;
            if (std::sin(0.0) != 0.0) return 3;
            return 0;
        }
    )", "math_pow.cmm"), 0);
}

TEST_F(StdMathTest, ConstantsPIAndE) {
    EXPECT_EQ(runWithImports(R"(
        import std.math;
        int32 main() {
            if (std::PI < 3.1415 || std::PI > 3.1416) return 1;
            if (std::E < 2.7182 || std::E > 2.7183) return 2;
            return 0;
        }
    )", "math_const.cmm"), 0);
}

TEST_F(StdMathTest, IsNanIsInfIsFinite) {
    EXPECT_EQ(runWithImports(R"(
        import std.math;
        int32 main() {
            float64 nan = 0.0 / 0.0;
            float64 inf = 1.0 / 0.0;
            if (!std::isnan(nan)) return 1;
            if (std::isnan(1.5)) return 2;
            if (!std::isinf(inf)) return 3;
            if (std::isinf(1.5)) return 4;
            if (!std::is_finite(1.5)) return 5;
            if (std::is_finite(inf)) return 6;
            return 0;
        }
    )", "math_class.cmm"), 0);
}

// F1 (review): a non-extern user function whose name coincides with a libc
// name must keep its namespace mangling — importing std.math must NOT hijack
// its symbol into the libm extern declaration.
TEST_F(StdMathTest, SameNamedUserFunctionKeepsNamespaceSymbol) {
    EXPECT_EQ(runWithImports(R"(
        import std.math;
        namespace geo {
        float64 cos(float64 x) {
            return 42.0;
        }
        }
        int32 main() {
            if (geo::cos(0.0) != 42.0) return 1;
            if (std::cos(0.0) != 1.0) return 2;
            return 0;
        }
    )", "math_f1.cmm"), 0);
}

// F2 (review): a global-scope extern (prelude `malloc`) is NOT reachable as
// `std::malloc` — namespace isolation preserved (compile-time diagnostic).
TEST_F(StdMathTest, GlobalExternNotReachableViaNamespace) {
    auto ast = smc::parseStdCPrelude(R"(
        import std.math;
        int32 main() {
            void* p = std::malloc(100);
            return p != null ? 0 : 1;
        }
    )", "math_f2.cmm");
    ASSERT_NE(ast, nullptr);

    smc::ModuleSearchPaths paths{{STD_DIR}};
    std::set<std::string> loaded;
    std::vector<std::string> errors;
    smc::processImports(*ast, ".", paths, loaded, errors);
    ASSERT_TRUE(errors.empty());

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    EXPECT_NE(analyzer.getErrors()[0].message.find("undeclared"),
              std::string::npos);
}

// M2 (review): exercise the float32 math family and the f32 bitcast
// round-trip — the *f bindings were otherwise dead code in the suite.
TEST_F(StdMathTest, Float32FamilyAndF32Bitcast) {
    EXPECT_EQ(runWithImports(R"(
        import std.math;
        import std.bit;
        int32 main() {
            if (std::fabsf(-2.5f) != 2.5f) return 1;
            if (std::floorf(3.7f) != 3.0f) return 2;
            if (std::ceilf(3.2f) != 4.0f) return 3;
            if (std::roundf(2.5f) != 3.0f) return 4;
            if (std::truncf(-2.7f) != -2.0f) return 5;
            if (std::powf(2.0f, 10.0f) != 1024.0f) return 6;
            if (std::cosf(0.0f) != 1.0f) return 7;
            if (std::sinf(0.0f) != 0.0f) return 8;
            float32 f = 1.5f;
            uint32 bits = std::f32_to_bits(f);
            float32 back = std::bits_to_f32(bits);
            if (back != 1.5f) return 9;
            return 0;
        }
    )", "math_f32.cmm"), 0);
}

}  // namespace
