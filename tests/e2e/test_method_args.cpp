// TYP-12 legacy: method calls accept array arguments via array->slice decay
// (zero-copy). Spec: docs/superpowers/specs/2026-10-02-methodcall-decay-design.md
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
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"

class MethodArgsE2E : public ::testing::Test {
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

    auto entry = (*jit)->lookup("main");
    if (!entry) return -1;
    using MainFn = int (*)();
    auto fn = reinterpret_cast<MainFn>(entry->getValue());
    if (!fn) return -1;
    return fn();
}


// TYP-12 legacy: array argument decays into the method's slice parameter;
// writes go through to the caller's array, this-> still works.
TEST_F(MethodArgsE2E, MethodArrayParamWriteThrough) {
    EXPECT_EQ(runSource(R"(
        class Box {
            public:
            int32 x;
            void bump(int32[] s) { s[0] = s[0] + 100; this->x = 1; }
        };
        int32 main() {
            Box b;
            int32 arr[2] = {1, 2};
            b.bump(arr);
            return (arr[0] == 101 && arr[1] == 2 && b.x == 1) ? 0 : 1;
        }
    )", "test_mc_writethrough.cmm"), 0);
}

TEST_F(MethodArgsE2E, MethodArrayParamReturnValue) {
    EXPECT_EQ(runSource(R"(
        class Sum {
            public:
            int32 total(int32[] s) { return s[0] + s[1]; }
        };
        int32 main() {
            Sum m;
            int32 arr[2] = {3, 4};
            return m.total(arr) == 7 ? 0 : 1;
        }
    )", "test_mc_retval.cmm"), 0);
}

// Review Focus 3: resolution through the inheritance chain must store the
// base class's declared parameter types (incl. base this) so codegen mangles
// against the base class definition.
TEST_F(MethodArgsE2E, MethodInheritedArrayParam) {
    EXPECT_EQ(runSource(R"(
        class BaseBox {
            public:
            void bump(int32[] s) { s[0] = s[0] + 42; }
        };
        class DerivedBox : public BaseBox {
            public:
            int32 y;
        };
        int32 main() {
            DerivedBox d;
            int32 arr[2] = {0, 9};
            d.bump(arr);
            return (arr[0] == 42 && arr[1] == 9) ? 0 : 1;
        }
    )", "test_mc_inherited.cmm"), 0);
}
