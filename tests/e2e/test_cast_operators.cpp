// LEX-11 / PAR-18 / DEC-18: explicit cast operators `static_cast<T>(x)` and
// `reinterpret_cast<T>(x)`.
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

class CastOperatorsE2E : public ::testing::Test {
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

static int runSource(const std::string& source, const std::string& file = "cast.cmm") {
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
                                   const std::string& file = "cast.cmm") {
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

TEST_F(CastOperatorsE2E, StaticCastTruncatesFloat) {
    EXPECT_EQ(runSource("int32 main() { int32 x = static_cast<int32>(3.7); return x - 3; }"), 0);
}

TEST_F(CastOperatorsE2E, StaticCastWidensInteger) {
    EXPECT_EQ(runSource(
        "int32 main() { int64 x = static_cast<int64>(41); return (int32)x - 41; }"), 0);
}

TEST_F(CastOperatorsE2E, StaticCastPointerToBase) {
    EXPECT_EQ(runSource(R"(
        struct Base { int32 b; };
        struct Derived { int32 b; int32 d; };
        int32 main() {
            struct Derived d;
            d.b = 1; d.d = 2;
            struct Derived* dp = &d;
            struct Base* bp = static_cast<struct Base*>(dp);
            return bp->b - 1;
        }
    )"), 0);
}

TEST_F(CastOperatorsE2E, ReinterpretCastFloatBits) {
    // 1.0f has bit pattern 0x3F800000 == 1065353216.
    EXPECT_EQ(runSource(
        "int32 main() { float32 f = 1.0f; int32 bits = reinterpret_cast<int32>(f);"
        " return (bits == 1065353216) ? 0 : 1; }"), 0);
}

TEST_F(CastOperatorsE2E, ReinterpretCastIntToFloat) {
    EXPECT_EQ(runSource(
        "int32 main() { int32 bits = 1065353216; float32 f = reinterpret_cast<float32>(bits);"
        " return (f == 1.0f) ? 0 : 1; }"), 0);
}

TEST_F(CastOperatorsE2E, ReinterpretCastPointerRoundTrip) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 v = 7;
            int32* p = &v;
            int64 addr = reinterpret_cast<int64>(p);
            int32* q = reinterpret_cast<int32*>(addr);
            return (q == p) ? 0 : 1;
        }
    )"), 0);
}

TEST_F(CastOperatorsE2E, StaticCastIntToPointerIsRejected) {
    AnalyzeResult r = analyzeSource(
        "int32 main() { int32 x = 5; int32* p = static_cast<int32*>(x); return 0; }");
    EXPECT_GT(r.errors, 0);
}

TEST_F(CastOperatorsE2E, StaticCastToAggregateIsRejected) {
    AnalyzeResult r = analyzeSource(
        "struct S { int32 a; };"
        "int32 main() { int32 x = 5; struct S s = static_cast<struct S>(x); return 0; }");
    EXPECT_GT(r.errors, 0);
}

TEST_F(CastOperatorsE2E, ReinterpretCastIntToPointerIsAllowed) {
    AnalyzeResult r = analyzeSource(
        "int32 main() { int32 x = 5; int32* p = reinterpret_cast<int32*>(x); return 0; }");
    EXPECT_EQ(r.errors, 0);
}

TEST_F(CastOperatorsE2E, CStyleCastStillWarnsButCompiles) {
    AnalyzeResult r = analyzeSource(
        "struct S { int32 a; };"
        "int32 main() { int32 x = 5; struct S s = (struct S)x; return 0; }");
    EXPECT_EQ(r.errors, 0);
    EXPECT_GT(r.warnings, 0);
}

TEST_F(CastOperatorsE2E, StaticCastToFunctionPointerTypeId) {
    EXPECT_EQ(runSource(R"(
        int32 inc(int32 x) { return x + 1; }
        int32 main() {
            int32 (*src)(int32) = inc;
            int32 (*dst)(int32) = static_cast<int32(*)(int32)>(src);
            return dst(1) - 2;
        }
    )"), 0);
}

TEST_F(CastOperatorsE2E, ReinterpretCastToPointerToArrayTypeId) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a[2] = {3, 4};
            int64 addr = reinterpret_cast<int64>(&a);
            int32 (*p)[2] = reinterpret_cast<int32(*)[2]>(addr);
            return (*p)[1] - 4;
        }
    )"), 0);
}
