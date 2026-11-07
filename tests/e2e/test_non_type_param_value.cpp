// GEN-02: non-type template parameters usable as values — end-to-end tests.
// `template<int32 N> int32 c() { return N; }` instantiated as `c<5>()`.
// A local declaration shadowing a value parameter is rejected.
// fixture 仿 test_overloading.cpp 的 runSource。

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

class NonTypeParamE2E : public ::testing::Test {
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
    if (!ast) return -1;

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
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb))
        .create();
    if (!jit) return -1;
    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto e = (*jit)->addIRModule(std::move(ts))) return -1;
    auto sym = (*jit)->lookup("main");
    if (!sym) return -1;
    auto fn = (int (*)())(intptr_t)sym->getValue();
    return fn();
}

static bool semaRejects(const std::string& source, const std::string& filename) {
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast || !parser.getErrors().empty()) return true;
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    return !analyzer.getErrors().empty();
}

TEST_F(NonTypeParamE2E, ValueParamUsedAsReturnValue) {
    EXPECT_EQ(runSource(R"(
        template<int32 N>
        int32 c() { return N; }
        int32 main() {
            return c<5>() - 5;
        }
    )", "ntp_return.cmm"), 0);
}

TEST_F(NonTypeParamE2E, ValueParamInArithmetic) {
    EXPECT_EQ(runSource(R"(
        template<int32 N>
        int32 d() { return N * 2; }
        int32 main() {
            return d<7>() - 14;
        }
    )", "ntp_arith.cmm"), 0);
}

TEST_F(NonTypeParamE2E, ExistingArraySizeParamStillWorks) {
    EXPECT_EQ(runSource(R"(
        template<typename T, usize N>
        struct Arr { T data[N]; };
        int32 main() {
            Arr<int32, 4> a;
            a.data[0] = 3;
            a.data[3] = 9;
            return a.data[0] + a.data[3] - 12;
        }
    )", "ntp_array.cmm"), 0);
}

TEST_F(NonTypeParamE2E, LocalShadowingValueParamRejected) {
    EXPECT_TRUE(semaRejects(R"(
        template<int32 N>
        int32 e() { int32 N = 3; return N; }
        int32 main() { return e<1>(); }
    )", "ntp_shadow.cmm"));
}

TEST_F(NonTypeParamE2E, CompoundLiteralInsideTemplateBody) {
    EXPECT_EQ(runSource(R"(
        template<int32 N>
        int32 sum2() {
            int32* p = (int32[2]){N, N};
            return p[0] + p[1];
        }
        int32 main() { return sum2<3>() - 6; }
    )", "ntp_compound.cmm"), 0);
}

TEST_F(NonTypeParamE2E, PrefixedStringInsideTemplateBody) {
    EXPECT_EQ(runSource(R"(
        template<int32 N>
        int32 firstByte() {
            char8* p = u8"ab";
            if (p[0] == 'a') { return N; }
            return 0;
        }
        int32 main() { return firstByte<1>() - 1; }
    )", "ntp_prefix.cmm"), 0);
}
