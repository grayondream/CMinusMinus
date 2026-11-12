// TYP-09 / TYP-25 / AGG-14: explicit enum underlying types, qualified enum type
// references, constant-expression enumerator values, and C default argument
// promotions for small enum types.
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

class EnumUnderlyingE2E : public ::testing::Test {
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

static int runSource(const std::string& source, const std::string& file = "enum.cdd") {
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

struct AnalyzeResult {
    int errors;
    int warnings;
};

static AnalyzeResult analyzeSource(const std::string& source,
                                   const std::string& file = "enum.cdd") {
    Lexer lexer(file, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) return {1, 0};

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    return {static_cast<int>(analyzer.getErrors().size()),
            static_cast<int>(analyzer.getWarnings().size())};
}

TEST_F(EnumUnderlyingE2E, UInt8EnumIsOneByte) {
    EXPECT_EQ(runSource(
        "enum Color : uint8 { Red, Green, Blue };"
        "int32 main() { return sizeof(enum Color) - 1; }"), 0);
}

TEST_F(EnumUnderlyingE2E, DefaultEnumIsIntSized) {
    EXPECT_EQ(runSource("enum D { A, B }; int32 main() { return sizeof(enum D) - 4; }"), 0);
}

TEST_F(EnumUnderlyingE2E, Int8EnumNegativeValue) {
    EXPECT_EQ(runSource(
        "enum S : int8 { Neg = -1, Pos = 1 };"
        "int32 main() { enum S s = Neg; return (s == -1) ? 0 : 1; }"), 0);
}

TEST_F(EnumUnderlyingE2E, ConstantExpressionEnumeratorValues) {
    EXPECT_EQ(runSource(
        "enum F : uint16 { F0 = 1, F1 = 2, FAll = 1 + 2 + 4 };"
        "int32 main() { return FAll - 7; }"), 0);
}

TEST_F(EnumUnderlyingE2E, QualifiedEnumTypeReferenceWithoutKeyword) {
    // `cfg::Mode` (no `enum` keyword) must resolve as a type (PAR-22 / TYP-25).
    EXPECT_EQ(runSource(
        "namespace cfg { enum Mode : uint8 { Off, Auto, On }; }"
        "int32 main() { cfg::Mode m = cfg::On; return m - 2; }"), 0);
}

TEST_F(EnumUnderlyingE2E, AnonymousEnumAliasWithUnderlyingType) {
    EXPECT_EQ(runSource(
        "using Small = enum : uint16 { Lo = 1, Hi = 2 };"
        "int32 main() { Small s = Hi; return ((int32)s - 2) + (sizeof(Small) - 2); }"), 0);
}

TEST_F(EnumUnderlyingE2E, VarArgPromotesSignedSmallEnum) {
    // int8 -1 must be sign-extended to int for printf-style varargs.
    EXPECT_EQ(runSource(R"(
        extern int32 snprintf(char* s, usize n, const char* format, ...);
        enum S : int8 { Neg = -1 };
        int32 main() {
            char buf[16];
            snprintf(buf, 16, "%d", Neg);
            return (buf[0] == '-' && buf[1] == '1') ? 0 : 1;
        }
    )"), 0);
}

TEST_F(EnumUnderlyingE2E, VarArgPromotesUnsignedSmallEnum) {
    EXPECT_EQ(runSource(R"(
        extern int32 snprintf(char* s, usize n, const char* format, ...);
        enum U : uint8 { Big = 200 };
        int32 main() {
            char buf[16];
            snprintf(buf, 16, "%d", Big);
            return (buf[0] == '2' && buf[1] == '0' && buf[2] == '0') ? 0 : 1;
        }
    )"), 0);
}

TEST_F(EnumUnderlyingE2E, NegativeEnumInSwitch) {
    EXPECT_EQ(runSource(R"(
        enum S : int8 { Neg = -1, Zero = 0, Pos = 1 };
        int32 main() {
            enum S s = Neg;
            switch (s) {
                case Neg:  return 0;
                case Zero: return 1;
                case Pos:  return 2;
            }
            return 3;
        }
    )"), 0);
}

TEST_F(EnumUnderlyingE2E, FloatUnderlyingTypeIsError) {
    AnalyzeResult r = analyzeSource("enum Bad : float32 { X }; int32 main() { return 0; }");
    EXPECT_GT(r.errors, 0);
}

TEST_F(EnumUnderlyingE2E, OutOfRangeLiteralDoesNotCrash) {
    // LEX-17: an out-of-range integer literal must not throw out of the lexer.
    AnalyzeResult r = analyzeSource(
        "enum Big : int64 { X = 10000000000 }; int32 main() { return 0; }");
    EXPECT_EQ(r.errors, 0);
}
