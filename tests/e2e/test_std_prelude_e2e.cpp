// MOD-09 / STD-23: end-to-end check that the built-in std.c binding layer lets
// user code call into libc (resolved by the JIT against the host process).
#include <gtest/gtest.h>

#include "ast/Decl.h"
#include "codegen/CodegenContext.h"
#include "driver/StdPrelude.h"
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

class StdPreludeE2E : public ::testing::Test {
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

static int runWithPrelude(const std::string& source, const std::string& filename) {
    auto ast = smc::parseStdCPrelude(source, filename);
    if (!ast) {
        ADD_FAILURE() << "user parse failed";
        return -1;
    }
    auto prelude = smc::parseStdCPrelude(smc::loadStdCPrelude({STD_DIR}), "<std.c>");
    if (!prelude) {
        ADD_FAILURE() << "prelude parse failed";
        return -1;
    }
    smc::prependDeclarations(*ast, *prelude);

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) {
        for (auto& err : analyzer.getErrors()) {
            ADD_FAILURE() << "semantic error: " << err.format();
        }
        return -1;
    }

    CodegenContext ctx;
    ctx.setSourceFile(filename);
    ast->codegen(ctx);
    ctx.finalizeDebugInfo();

    std::string ve;
    llvm::raw_string_ostream vs(ve);
    if (llvm::verifyModule(ctx.getModule(), &vs)) {
        vs.flush();
        ADD_FAILURE() << "module verification failed: " << ve;
        return -1;
    }

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        ADD_FAILURE() << "detectHost failed";
        return -1;
    }
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb))
        .create();
    if (!jit) {
        ADD_FAILURE() << "LLJIT create failed";
        return -1;
    }

    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto e = (*jit)->addIRModule(std::move(ts))) {
        ADD_FAILURE() << "addIRModule failed";
        return -1;
    }

    auto sym = (*jit)->lookup("main");
    if (!sym) {
        ADD_FAILURE() << "lookup main failed";
        return -1;
    }
    return ((int (*)())(intptr_t)sym->getValue())();
}

TEST_F(StdPreludeE2E, CallsLibcAbsThroughBindingLayer) {
    EXPECT_EQ(runWithPrelude(
        "int32 main() { return abs(-42) - 42; }", "std_prelude_abs.c"), 0);
}

TEST_F(StdPreludeE2E, CallsLibcMallocFree) {
    // malloc/free come only from the prelude.
    EXPECT_EQ(runWithPrelude(R"(
        int32 main() {
            void* p = malloc(16);
            free(p);
            return 0;
        }
    )", "std_prelude_malloc.c"), 0);
}
