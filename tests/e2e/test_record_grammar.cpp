// PAR-05: struct and class share the record grammar and access semantics.
#include <gtest/gtest.h>

#include "ast/Decl.h"
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

class RecordGrammarE2E : public ::testing::Test {
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

TEST_F(RecordGrammarE2E, StructWithAccessAndMethods) {
    EXPECT_EQ(runSource(R"(
        struct S {
        private:
            int32 x;
        public:
            int32 get() { return this->x; }
            void set(int32 v) { this->x = v; }
        };
        int32 main() { S s; s.set(7); return s.get() - 7; }
    )", "record_access.cmm"), 0);
}

TEST_F(RecordGrammarE2E, StructAccessOnlyWithoutMethods) {
    EXPECT_EQ(runSource(R"(
        struct S {
        private:
            int32 hidden;
        public:
            int32 open;
        };
        int32 main() { S s; s.open = 3; return s.open - 3; }
    )", "record_access_only.cmm"), 0);
}

TEST_F(RecordGrammarE2E, StructAttachedVariableKeepsMethods) {
    EXPECT_EQ(runSource(R"(
        struct S {
            int32 x;
            int32 get() { return this->x; }
        } s;
        int32 main() { s.x = 9; return s.get() - 9; }
    )", "record_attached.cmm"), 0);
}

TEST_F(RecordGrammarE2E, StructAndClassLayoutSame) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 x; int32 y; };
        class C { public: int32 x; int32 y; };
        int32 main() {
            return compile_time.size_of(S) == compile_time.size_of(C) ? 0 : 1;
        }
    )", "record_layout.cmm"), 0);
}
