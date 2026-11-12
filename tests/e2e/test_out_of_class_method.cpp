// PAR-03: out-of-class method definitions `RetType Class::method(params) { ... }`.
// The declaration lives in the class body; the definition provides the body.
// fixture 仿 test_class_codegen.cpp 的 runSource。

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

class OutOfClassMethodE2E : public ::testing::Test {
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
    if (!ast || !parser.getErrors().empty()) return true;
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    return !analyzer.getErrors().empty();
}

TEST_F(OutOfClassMethodE2E, DefinitionOutsideClassBody) {
    EXPECT_EQ(runSource(R"(
        class S {
        public:
            int32 v;
            int32 get();
        };
        int32 S::get() { return this->v; }
        int32 main() {
            S s;
            s.v = 5;
            return s.get() - 5;
        }
    )", "ooc_basic.cdd"), 0);
}

TEST_F(OutOfClassMethodE2E, DefinitionWithParameters) {
    EXPECT_EQ(runSource(R"(
        class S {
        public:
            int32 v;
            int32 add(int32 a, int32 b);
        };
        int32 S::add(int32 a, int32 b) { return this->v + a + b; }
        int32 main() {
            S s;
            s.v = 1;
            return s.add(2, 3) - 6;
        }
    )", "ooc_params.cdd"), 0);
}

TEST_F(OutOfClassMethodE2E, DefinitionCanAccessPrivateMember) {
    EXPECT_EQ(runSource(R"(
        class C {
            int32 secret;
        public:
            void set(int32 v) { this->secret = v; }
            int32 get();
        };
        int32 C::get() { return this->secret; }
        int32 main() {
            C c;
            c.set(7);
            return c.get() - 7;
        }
    )", "ooc_private.cdd"), 0);
}

TEST_F(OutOfClassMethodE2E, SignatureMismatchRejected) {
    EXPECT_TRUE(semaRejects(R"(
        class S {
        public:
            int32 v;
            int32 get();
        };
        int32 S::get(int32 x) { return x; }
        int32 main() { return 0; }
    )", "ooc_mismatch.cdd"));
}

TEST_F(OutOfClassMethodE2E, UndeclaredMethodRejected) {
    EXPECT_TRUE(semaRejects(R"(
        class S { public: int32 v; };
        int32 S::nope() { return 0; }
        int32 main() { return 0; }
    )", "ooc_undeclared.cdd"));
}

TEST_F(OutOfClassMethodE2E, UnknownClassRejected) {
    EXPECT_TRUE(semaRejects(R"(
        int32 X::f() { return 0; }
        int32 main() { return 0; }
    )", "ooc_unknown.cdd"));
}
