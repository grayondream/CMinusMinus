// TYP-22 / TYP-19: integer promotion, usual arithmetic conversions and
// signedness-aware codegen (udiv/urem, unsigned comparisons, logical vs
// arithmetic right shift, zext when widening unsigned values).
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

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

class ArithmeticConversionE2E : public ::testing::Test {
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

static int runSource(const std::string& source, const std::string& file = "arith.cdd") {
    Lexer lexer(file, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast || !parser.getErrors().empty()) return -1;

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return -1;

    CodegenContext ctx;
    ctx.setSourceFile(file);
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

TEST_F(ArithmeticConversionE2E, UnsignedSmallValuePreservedInArithmetic) {
    // uint8 promotes to int; the value must be zero-extended (200, not -56).
    EXPECT_EQ(runSource("int32 main() { uint8 a = 200; return (a + 100) - 300; }"), 0);
}

TEST_F(ArithmeticConversionE2E, CharArithmeticPromotesToInt) {
    // char + char yields int; without promotion this would wrap to -56.
    EXPECT_EQ(runSource(
        "int32 main() { char a = 100; char b = 100; int32 c = a + b; return c - 200; }"), 0);
}

TEST_F(ArithmeticConversionE2E, MixedSignednessComparisonIsUnsigned) {
    // -1 < 1u is false (the int is converted to unsigned).
    EXPECT_EQ(runSource(
        "int32 main() { int32 a = -1; uint32 b = 1; return (a < b) ? 1 : 0; }"), 0);
}

TEST_F(ArithmeticConversionE2E, UnsignedDivisionIsUnsigned) {
    // 0xFFFFFFFF / 2 = 2147483647 (udiv, not -1/2).
    EXPECT_EQ(runSource(R"(
        int32 main() {
            uint32 p = 1;
            p = p - 2;
            return (int32)(p / 2) - 2147483647;
        }
    )"), 0);
}

TEST_F(ArithmeticConversionE2E, UnsignedModuloIsUnsigned) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            uint32 p = 1;
            p = p - 2;
            return (int32)(p % 3) - 0;
        }
    )"), 0);
}

TEST_F(ArithmeticConversionE2E, UnsignedRightShiftIsLogical) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            uint32 x = 1;
            x = x << 31;
            return (int32)(x >> 31) - 1;
        }
    )"), 0);
}

TEST_F(ArithmeticConversionE2E, SignedRightShiftIsArithmetic) {
    EXPECT_EQ(runSource("int32 main() { int32 y = -8; return (y >> 1) + 4; }"), 0);
}

TEST_F(ArithmeticConversionE2E, UnsignedWidensToSigned64WithZeroExtend) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            uint32 u = 1;
            u = u - 2;
            int64 x = u;
            return (x > 0 && (int32)(x / 2) == 2147483647) ? 0 : 1;
        }
    )"), 0);
}

TEST_F(ArithmeticConversionE2E, UnsignedIntToFloatConversion) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            uint32 u = 1;
            u = u - 2;
            float64 d = u;
            return (d > 4000000000.0) ? 0 : 1;
        }
    )"), 0);
}

TEST_F(ArithmeticConversionE2E, FloatIntCommonTypeWidens) {
    EXPECT_EQ(runSource(
        "int32 main() { float32 f = 3.5f; int32 i = 2; float64 d = f + i; "
        "return (int32)(d * 2.0) - 11; }"), 0);
}

TEST_F(ArithmeticConversionE2E, PromotionAcrossFunctionCall) {
    EXPECT_EQ(runSource(R"(
        int64 take(int64 v) { return v; }
        int32 main() {
            uint32 u = 1;
            u = u - 2;
            return (take(u) > 0) ? 0 : 1;
        }
    )"), 0);
}

// LEX-15: a literal larger than INT_MAX must not be truncated in codegen.
TEST_F(ArithmeticConversionE2E, LargeUnsignedLiteralIsNotTruncated) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            uint64 v = 4000000000ul;
            return (v / 1000000000ul) == 4 ? 0 : 1;
        }
    )"), 0);
}
