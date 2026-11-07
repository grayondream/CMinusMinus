// PAR-08: C99 compound literals — end-to-end execution tests.
// `(T){...}` block-scope = stack object; file-scope = static constant.
// Direct escape (& / return / store-to-global of array/slice forms) is rejected.
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

class CompoundLiteralE2E : public ::testing::Test {
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

// A helper that reports whether compilation (sema) rejects the source.
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

TEST_F(CompoundLiteralE2E, ArrayCompoundLiteralDecaysToPointer) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32* p = (int32[3]){1,2,3};
            return p[0] + p[2] - 4;
        }
    )", "cl_array.cmm"), 0);
}

TEST_F(CompoundLiteralE2E, StructCompoundLiteralFields) {
    EXPECT_EQ(runSource(R"(
        struct P { int32 x; int32 y; };
        int32 main() {
            struct P q = (struct P){1,2};
            return q.x + q.y - 3;
        }
    )", "cl_struct.cmm"), 0);
}

TEST_F(CompoundLiteralE2E, ScalarCompoundLiteralEqualsCast) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 v = (int32){5};
            return v - 5;
        }
    )", "cl_scalar.cmm"), 0);
}

TEST_F(CompoundLiteralE2E, ByValueCopyToGlobalAllowed) {
    EXPECT_EQ(runSource(R"(
        struct P { int32 x; int32 y; };
        struct P g;
        int32 main() {
            g = (struct P){1,2};
            return g.x + g.y - 3;
        }
    )", "cl_global_copy.cmm"), 0);
}

TEST_F(CompoundLiteralE2E, ReturnCompoundLiteralRejected) {
    EXPECT_TRUE(semaRejects(R"(
        int32* make() {
            return (int32[2]){1,2};
        }
        int32 main() { return 0; }
    )", "cl_return.cmm"));
}

TEST_F(CompoundLiteralE2E, AddressOfCompoundLiteralRejected) {
    EXPECT_TRUE(semaRejects(R"(
        int32 main() {
            int32** pp = &(int32[2]){1,2};
            return 0;
        }
    )", "cl_addr.cmm"));
}

TEST_F(CompoundLiteralE2E, SliceFormCompoundLiteral) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32[] s = (int32[]){1,2,3};
            if (s.len != 3) { return 1; }
            return s[0] + s[2] - 4;
        }
    )", "cl_slice.cmm"), 0);
}

TEST_F(CompoundLiteralE2E, FileScopeStructCompoundLiteral) {
    EXPECT_EQ(runSource(R"(
        struct P { int32 x; int32 y; };
        struct P g = (struct P){1,2};
        int32 main() {
            return g.x + g.y - 3;
        }
    )", "cl_file_scope.cmm"), 0);
}

TEST_F(CompoundLiteralE2E, FileScopeArrayCompoundLiteral) {
    EXPECT_EQ(runSource(R"(
        int32* p = (int32[2]){1,2};
        int32 main() {
            return p[0] + p[1] - 3;
        }
    )", "cl_file_array.cmm"), 0);
}
