// TYP-05 / CON-04: char8/16/32 types + thread_local storage class —
// end-to-end execution tests. fixture 仿 test_string.cpp 的 runSource。

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

class StorageE2E : public ::testing::Test {
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

TEST_F(StorageE2E, Char8Assignable) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            char8 c = 65;
            return c == 65 ? 0 : 1;
        }
    )", "char8.cmm"), 0);
}

TEST_F(StorageE2E, Char16Assignable) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            char16 c = 50000;
            return c == 50000 ? 0 : 1;
        }
    )", "char16.cmm"), 0);
}

TEST_F(StorageE2E, ThreadLocalGlobalReadWrite) {
    EXPECT_EQ(runSource(R"(
        thread_local int32 x = 5;
        int32 main() {
            x = x + 1;
            return x == 6 ? 0 : 1;
        }
    )", "tls.cmm"), 0);
}

TEST_F(StorageE2E, ThreadLocalOnFunctionRejected) {
    auto ast = [&]() {
        Lexer lexer("tls_fn.cmm", R"(
            thread_local int32 f() { return 1; }
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
    EXPECT_NE(analyzer.getErrors()[0].message.find("thread_local"),
              std::string::npos);
}
