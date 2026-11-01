// Flexible array member (C99) — layout/sizeof/access/copy e2e (AGG-04).
#include <gtest/gtest.h>
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "codegen/CodegenContext.h"
#include "driver/StdPrelude.h"
#include "support/Log.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include <spdlog/spdlog.h>

class FlexArrayE2E : public ::testing::Test {
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
    auto ast = smc::parseStdCPrelude(source, filename);
    if (!ast) {
        ADD_FAILURE() << "Parse failed";
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
    if (llvm::verifyModule(ctx.getModule(), &vs)) {
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
        .setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit) {
        ADD_FAILURE() << "LLJITBuilder::create failed";
        return -1;
    }
    if (auto e = (*jit)->addIRModule(
            llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext()))) {
        ADD_FAILURE() << "addIRModule failed";
        return -1;
    }
    auto sym = (*jit)->lookup("main");
    if (!sym) {
        ADD_FAILURE() << "lookup main failed";
        return -1;
    }
    auto mainFn = sym->toPtr<int32_t(*)()>();
    return mainFn();
}

TEST_F(FlexArrayE2E, SizeofAndOffsetMatchGcc) {
    // gcc 实测基线: sizeof(S8)=1 offsetof(d)=1 | sizeof(S32)=4 offsetof(d)=4
    EXPECT_EQ(runSource(R"(
        struct S8  { char c; char d[]; };
        struct S32 { char c; int32 d[]; };
        int32 main() {
            if (sizeof(struct S8)  != 1) { return 1; }
            if (sizeof(struct S32) != 4) { return 2; }
            if (compile_time.offset_of(struct S32, "d") != 4) { return 3; }
            return 0;
        }
    )", "fam_sizeof.c"), 0);
}

TEST_F(FlexArrayE2E, MallocIdiomReadWrite) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 n; int32 data[]; };
        int32 main() {
            S* s = (S*)malloc(sizeof(S) + 3 * 4);
            s->n = 3;
            s->data[0] = 10; s->data[1] = 20; s->data[2] = 30;
            if (s->data[2] != 30) { return 1; }
            free(s);
            return 0;
        }
    )", "fam_malloc.c"), 0);
}

TEST_F(FlexArrayE2E, ValueCopyCopiesFixedPartOnly) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 n; int32 data[]; };
        int32 main() {
            S* a = (S*)malloc(sizeof(S) + 2 * 4);
            a->n = 7; a->data[0] = 1;
            S b = *a;                // 只拷固定部分：b.n==7
            if (b.n != 7) { return 1; }
            free(a);
            return 0;
        }
    )", "fam_copy.c"), 0);
}

TEST_F(FlexArrayE2E, MaterializedLayoutsMatchGcc) {
    // gcc 实测: packed sizeof=1 offsetof=1; align(16) sizeof=16 offsetof=4
    EXPECT_EQ(runSource(R"(
        [[packed]] struct SP { char c; int32 d[]; };
        [[align(16)]] struct SA { char c; int32 data[]; };
        struct BF { int32 a : 3; int32 b : 5; int32 rest[]; };
        int32 main() {
            if (sizeof(struct SP) != 1) { return 1; }
            if (compile_time.offset_of(struct SP, "d") != 1) { return 2; }
            if (sizeof(struct SA) != 16) { return 3; }
            if (compile_time.offset_of(struct SA, "data") != 4) { return 4; }
            if (sizeof(struct BF) != 4) { return 5; }
            SP* sp = (SP*)malloc(sizeof(SP) + 3 * 4);
            sp->d[0] = 5; sp->d[1] = 6; sp->d[2] = 7;
            if (sp->d[2] != 7) { return 6; }
            free(sp);
            return 0;
        }
    )", "fam_materialized.c"), 0);
}
