// FUN-09: default arguments — end-to-end execution tests.
// fixture 仿 test_string.cpp 的 runSource。

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

class DefaultArgsE2E : public ::testing::Test {
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
    if (!ast) {
        ADD_FAILURE() << "Parse failed";
        return -1;
    }

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) {
        for (auto& err : analyzer.getErrors()) {
            ADD_FAILURE() << "Semantic error: " << err.format();
        }
        return -1;
    }

    CodegenContext ctx;
    ctx.setSourceFile(filename);
    ast->codegen(ctx);
    ctx.finalizeDebugInfo();

    std::string ve;
    llvm::raw_string_ostream vs(ve);
    bool bad = llvm::verifyModule(ctx.getModule(), &vs);
    if (bad) {
        vs.flush();
        ADD_FAILURE() << "Module verification failed: " << ve;
        return -1;
    }

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        ADD_FAILURE() << "JITTargetMachineBuilder::detectHost failed";
        return -1;
    }
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb))
        .create();
    if (!jit) {
        ADD_FAILURE() << "LLJITBuilder::create failed";
        return -1;
    }

    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto err = (*jit)->addIRModule(std::move(ts))) {
        ADD_FAILURE() << "addIRModule failed";
        return -1;
    }

    auto sym = (*jit)->lookup("main");
    if (!sym) {
        ADD_FAILURE() << "main not found";
        return -1;
    }
    return ((int (*)())(intptr_t)sym->getValue())();
}

TEST_F(DefaultArgsE2E, FillsDefault) {
    EXPECT_EQ(runSource(R"(
        int32 f(int32 x, int32 y = 10) { return x + y; }
        int32 main() {
            if (f(5) != 15) return 1;
            if (f(5, 2) != 7) return 2;
            return 0;
        }
    )", "def_fill.cmm"), 0);
}

TEST_F(DefaultArgsE2E, OverloadPrefersFewerDefaults) {
    EXPECT_EQ(runSource(R"(
        int32 f(int32 x) { return 1; }
        int32 f(int32 x, int32 y = 5) { return 2; }
        int32 main() {
            if (f(1) != 1) return 1;        // one-arg overload wins
            return 0;
        }
    )", "def_over1.cmm"), 0);
}

TEST_F(DefaultArgsE2E, TooManyArgsRejected) {
    auto ast = [&]() {
        Lexer lexer("def_many.cmm", R"(
            int32 f(int32 x, int32 y = 5) { return x + y; }
            int32 main() { f(1, 2, 3); return 0; }
        )");
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        return parser.parse();
    }();
    if (!ast) {
        ADD_FAILURE() << "Parse failed";
        return;
    }
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    EXPECT_NE(analyzer.getErrors()[0].message.find("matching"),
              std::string::npos);
}

TEST_F(DefaultArgsE2E, DefaultValueMustBeConstexpr) {
    auto ast = [&]() {
        Lexer lexer("def_ct.cmm", R"(
            int32 g(int32 x) { return x; }
            int32 f(int32 a = g(1)) { return a; }
            int32 main() { return 0; }
        )");
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        return parser.parse();
    }();
    if (!ast) {
        ADD_FAILURE() << "Parse failed";
        return;
    }
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
}

TEST_F(DefaultArgsE2E, MiddleDefaultRejected) {
    auto tokens = [&]() {
        Lexer lexer("def_mid.cmm", R"(
            int32 f(int32 x = 1, int32 y) { return x + y; }
            int32 main() { return 0; }
        )");
        return lexer.tokenize();
    }();
    Parser parser(tokens);
    auto ast = parser.parse();
    // A middle default is rejected — the parser cannot treat `int32` as part
    // of the default expression, so it reports a parse error (recovery keeps
    // the AST) or sema rejects it; either way the compile must not succeed.
    bool parserError = !parser.getErrors().empty();
    bool semaError = false;
    if (ast) {
        SemanticAnalyzer analyzer;
        analyzer.analyze(*ast);
        semaError = !analyzer.getErrors().empty();
    }
    EXPECT_TRUE(parserError || semaError);
}

TEST_F(DefaultArgsE2E, FloatDefaultValueIsFloat) {
    // C4 (review): `float64 x = 5` must pass double 5.0, not int 0/5.
    EXPECT_EQ(runSource(R"(
        float64 f(float64 x = 5) { return x; }
        int32 main() {
            if (f() != 5.0) return 1;
            if (f(2.5) != 2.5) return 2;
            return 0;
        }
    )", "def_float.cmm"), 0);
}

TEST_F(DefaultArgsE2E, RedeclarationCarriesDefaults) {
    // I4 (review): prototype without defaults, then definition with defaults —
    // the redeclared defaults must be honored at call sites.
    EXPECT_EQ(runSource(R"(
        int32 f(int32 x, int32 y);
        int32 f(int32 x, int32 y = 5) { return x + y; }
        int32 main() {
            if (f(1) != 6) return 1;
            if (f(1, 2) != 3) return 2;
            return 0;
        }
    )", "def_redecl.cmm"), 0);
}

TEST_F(DefaultArgsE2E, MethodDefaultArgs) {
    EXPECT_EQ(runSource(R"(
        class C {
        public:
            int32 add(int32 a, int32 b = 3) { return a + b; }
        };
        int32 main() {
            C c;
            if (c.add(5) != 8) return 1;
            if (c.add(5, 2) != 7) return 2;
            return 0;
        }
    )", "def_method.cmm"), 0);
}
