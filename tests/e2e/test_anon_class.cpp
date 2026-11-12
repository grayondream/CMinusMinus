// PAR-12: 匿名 class 定义全通路——字段/方法/访问段注册与解析。
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
#include "llvm/Support/TargetSelect.h"

class AnonClassE2E : public ::testing::Test {
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
    llvm::raw_string_ostream os(ve);
    if (llvm::verifyModule(ctx.getModule(), &os)) return -2;

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) return -3;
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit) return -4;
    if (auto e = (*jit)->addIRModule(
            llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext()))) {
        return -5;
    }
    auto sym = (*jit)->lookup("main");
    if (!sym) return -6;
    auto mainFn = sym->toPtr<int32_t(*)()>();
    return mainFn();
}

TEST_F(AnonClassE2E, FieldsMethodsAndAccess) {
    EXPECT_EQ(runSource(R"(
        class { public: int32 v; int32 get() { return this->v; } } c;
        int32 main() {
            c.v = 5;
            if (c.get() != 5) return 1;
            return 0;
        }
    )", "anon_cls_fm.cdd"), 0);
}

TEST_F(AnonClassE2E, PrivateFieldRejectedOutside) {
    // 匿名 class 默认 private：类外直接访问 private 字段报 E2009。
    auto run = []() {
        Lexer lexer("anon_cls_priv.cdd",
            "class { int32 secret; } c;\n"
            "int32 main() { c.secret = 1; return 0; }\n");
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        auto ast = parser.parse();
        if (!ast || !parser.getErrors().empty()) return -1;
        SemanticAnalyzer analyzer;
        analyzer.analyze(*ast);
        return analyzer.getErrors().empty() ? 0 : -1;
    };
    EXPECT_EQ(run(), -1);
}

TEST_F(AnonClassE2E, MultiVarDeclarators) {
    EXPECT_EQ(runSource(R"(
        class { public: int32 a; int32 b; } x, y;
        int32 main() {
            x.a = 1; y.b = 2;
            if (x.a != 1 || y.b != 2) return 1;
            return 0;
        }
    )", "anon_cls_multi.cdd"), 0);
}

TEST_F(AnonClassE2E, LocalAnonymousClass) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            class { public: int32 v; } c;
            c.v = 7;
            if (c.v != 7) return 1;
            return 0;
        }
    )", "anon_cls_local.cdd"), 0);
}
