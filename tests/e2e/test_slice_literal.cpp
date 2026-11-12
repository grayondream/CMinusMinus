// PAR-08: slice literal initialization `T[] s = {1,2,3}` — end-to-end tests.
// The slice is backed by a stack anonymous array; returning it is a dangling
// escape (rejected). fixture 仿 test_string.cpp 的 runSource。

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

class SliceLiteralE2E : public ::testing::Test {
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
    if (auto e = (*jit)->addIRModule(std::move(ts))) {
        ADD_FAILURE() << "addIRModule failed";
        return -1;
    }

    auto sym = (*jit)->lookup("main");
    if (!sym) {
        ADD_FAILURE() << "lookup main failed";
        return -1;
    }

    auto fn = (int (*)())(intptr_t)sym->getValue();
    return fn();
}

static bool semaRejects(const std::string& source, const std::string& filename) {
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) return true;
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    return !analyzer.getErrors().empty();
}

TEST_F(SliceLiteralE2E, InitListBacksSlice) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32[] s = {1,2,3};
            s[1] = 9;
            return s[0] + s[1] + s[2] - 13;
        }
    )", "sl_init.cdd"), 0);
}

TEST_F(SliceLiteralE2E, SliceLengthFromList) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32[] s = {1,2,3};
            if (s.len != 3) { return 1; }
            return 0;
        }
    )", "sl_len.cdd"), 0);
}

TEST_F(SliceLiteralE2E, ReturnSliceLiteralRejected) {
    EXPECT_TRUE(semaRejects(R"(
        int32[] make() {
            int32[] s = {1,2};
            return s;
        }
        int32 main() { return 0; }
    )", "sl_return.cdd"));
}

TEST_F(SliceLiteralE2E, ElementTypeMismatchRejected) {
    EXPECT_TRUE(semaRejects(R"(
        struct P { int32 x; };
        int32 main() {
            struct P[] s = {1, 2};
            return 0;
        }
    )", "sl_mismatch.cdd"));
}

TEST_F(SliceLiteralE2E, EmptySliceLiteralIsNullView) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32[] s = {};
            if (s.len != 0) { return 1; }
            return 0;
        }
    )", "sl_empty.cdd"), 0);
}

TEST_F(SliceLiteralE2E, SliceLiteralNameInAnotherFunctionDoesNotCollide) {
    // `leftover` registers `s` as a stack-backed slice literal; `pass` returns
    // a same-named parameter that is NOT stack-backed. A file-wide name set
    // would falsely reject `pass`.
    EXPECT_EQ(runSource(R"(
        int32 g[2];
        int32[] pass(int32[] s) { return s; }
        int32 leftover() { int32[] s = {1,2}; return s[0]; }
        int32 main() {
            g[0] = 4;
            int32[] r = pass(g);
            if (r[0] != 4) { return 1; }
            if (leftover() != 1) { return 2; }
            return 0;
        }
    )", "sl_no_collision.cdd"), 0);
}

TEST_F(SliceLiteralE2E, StoreSliceLiteralIntoGlobalSubobjectRejected) {
    EXPECT_TRUE(semaRejects(R"(
        struct Holder { int32[] s; };
        struct Holder g;
        int32 main() {
            g.s = (int32[]){1,2};
            return 0;
        }
    )", "sl_global_field.cdd"));
}

TEST_F(SliceLiteralE2E, StoreSliceLiteralIntoGlobalArrayElementRejected) {
    EXPECT_TRUE(semaRejects(R"(
        int32[] g[2];
        int32 main() {
            g[0] = (int32[]){1,2};
            return 0;
        }
    )", "sl_global_elem.cdd"));
}

TEST_F(SliceLiteralE2E, StoreSliceLiteralToGlobalRejected) {
    EXPECT_TRUE(semaRejects(R"(
        int32[] gs;
        int32 main() {
            gs = (int32[]){1,2};
            return 0;
        }
    )", "sl_global_store.cdd"));
}

TEST_F(SliceLiteralE2E, StringElementDeepCopyWithString) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string a = string.new("hi");
            string[] s = {a};
            if (s.len != 1) { return 1; }
            if (s[0].len() != 2) { return 2; }
            return 0;
        }
    )", "sl_string_elem.cdd"), 0);
}
