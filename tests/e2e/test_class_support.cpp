#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>

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

class ClassE2ETest : public ::testing::Test {
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
    bool bad = llvm::verifyModule(ctx.getModule(), &vs);
    if (bad) return -1;

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

TEST_F(ClassE2ETest, BasicClass) {
    EXPECT_EQ(runSource(R"(
        class Foo {
            public:
            int32 x;
            void setX(int32 v) { this->x = v; }
            int32 getX() { return this->x; }
        };
        
        int32 main() {
            Foo f;
            f.setX(42);
            return f.getX();
        }
    )", "test_class.cdd"), 42);
}

TEST_F(ClassE2ETest, ClassInheritance) {
    EXPECT_EQ(runSource(R"(
        class Base {
            public:
            int32 x;
            void setX(int32 v) { this->x = v; }
        };
        
        class Derived : public Base {
            public:
            int32 y;
            void setY(int32 v) { this->y = v; }
        };
        
        int32 main() {
            Derived d;
            d.setX(10);
            d.setY(20);
            return d.x + d.y;
        }
    )", "test_inherit.cdd"), 30);
}

TEST_F(ClassE2ETest, ClassAsFunctionParam) {
    EXPECT_EQ(runSource(R"(
        class Point {
            public:
            int32 x;
            int32 y;
        };
        
        int32 getSum(Point p) {
            return p.x + p.y;
        }
        
        int32 main() {
            Point p;
            p.x = 5;
            p.y = 10;
            return getSum(p);
        }
    )", "test_class_param.cdd"), 15);
}

// PAR-04/SEM-04 + DEC-01: access sections must not truncate the class body,
// and private members are reachable from methods via `this->`.
TEST_F(ClassE2ETest, AccessSectionsAndPrivateViaThis) {
    EXPECT_EQ(runSource(R"(
        class C {
        private:
            int32 secret;
        public:
            int32 get() { return this->secret; }
            void set(int32 v) { this->secret = v; }
        };
        int32 main() {
            C c;
            c.set(5);
            return c.get() == 5 ? 0 : 1;
        }
    )", "test_class_access.cdd"), 0);
}

// DEC-01: struct members default to public.
TEST_F(ClassE2ETest, StructFieldsDefaultPublic) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 v; };
        int32 main() {
            S s;
            s.v = 4;
            return s.v == 4 ? 0 : 1;
        }
    )", "test_struct_public.cdd"), 0);
}
