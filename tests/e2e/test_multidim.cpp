// TYP-11: multi-dimensional array bodies — declaration, nested init,
// chained subscripts, row decay to slice, sizeof.
// Spec: docs/superpowers/specs/2026-10-03-multidim-arrays-design.md
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

class MultiDimE2E : public ::testing::Test {
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



// Review Focus 1: layout-sensitive sum — catches [3][2] inversion.
TEST_F(MultiDimE2E, MultiDimInitAndSum) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a[2][3] = {{1, 2, 3}, {4, 5, 6}};
            int32 s = 0;
            for (int32 i = 0; i < 2; i = i + 1)
                for (int32 j = 0; j < 3; j = j + 1)
                    s = s + a[i][j];
            return s == 21 ? 0 : 1;
        }
    )", "test_md_sum.cmm"), 0);
}

TEST_F(MultiDimE2E, MultiDimSubscriptWriteRead) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a[2][3] = {{1, 2, 3}, {4, 5, 6}};
            a[1][2] = 60;
            a[0][0] = a[0][0] + 10;
            return (a[1][2] == 60 && a[0][0] == 11 && a[0][1] == 2) ? 0 : 1;
        }
    )", "test_md_write.cmm"), 0);
}

// Review Focus 3: a[1] is a 1-D array lvalue — decays to slice, writes go
// through to the original row.
TEST_F(MultiDimE2E, MultiDimRowDecayToSlice) {
    EXPECT_EQ(runSource(R"(
        int32 MDBump(int32[] s) { s[0] = s[0] + 100; return s[0]; }
        int32 main() {
            int32 a[2][3] = {{1, 2, 3}, {4, 5, 6}};
            return (MDBump(a[1]) == 104 && a[1][0] == 104 && a[0][0] == 1) ? 0 : 1;
        }
    )", "test_md_rowdecay.cmm"), 0);
}

// Review Focus 2: global constant init + DataLayout size (2*3*4 = 24).
TEST_F(MultiDimE2E, MultiDimGlobalConstAndSizeof) {
    EXPECT_EQ(runSource(R"(
        int32 g[2][3] = {{1, 2, 3}, {4, 5, 6}};
        int32 main() {
            return (g[1][2] == 6 && sizeof(g) == 24) ? 0 : 1;
        }
    )", "test_md_global.cmm"), 0);
}

// Review Focus 5: member-field multi-dim arrays through field load + nested
// GEP — write and read via method and direct subscript.
TEST_F(MultiDimE2E, MultiDimMemberField) {
    EXPECT_EQ(runSource(R"(
        class MDCell {
            public:
            int32 g[2][3];
            void set(int32 v) { this->g[1][2] = v; }
            int32 get() { return this->g[1][2]; }
        };
        int32 main() {
            MDCell c;
            c.set(77);
            return (c.get() == 77 && c.g[1][2] == 77) ? 0 : 1;
        }
    )", "test_md_member.cmm"), 0);
}

// Review Focus 1: aggregate-element lvalue re-subscript + zero-copy
// write-through to the caller's storage. Sum: 21 - 4 + 100 = 117.
TEST_F(MultiDimE2E, MultiDimParamRowSumWriteThrough) {
    EXPECT_EQ(runSource(R"(
        int32 MDSum(int32 m[][3]) {
            m[1][0] = 100;
            int32 s = 0;
            for (int32 i = 0; i < m.len; i = i + 1)
                for (int32 j = 0; j < 3; j = j + 1)
                    s = s + m[i][j];
            return s;
        }
        int32 main() {
            int32 a[2][3] = {{1, 2, 3}, {4, 5, 6}};
            int32 r = MDSum(a);
            return (r == 117 && a[1][0] == 100 && a[0][0] == 1) ? 0 : 1;
        }
    )", "test_mdp_writethrough.cmm"), 0);
}

// Review Focus 5: MethodCall with a 2-D array argument to a row-slice param.
TEST_F(MultiDimE2E, MultiDimParamMethodCall) {
    EXPECT_EQ(runSource(R"(
        class MDPMat {
            public:
            int32 rowSum(int32 m[][3]) { return m[1][2]; }
        };
        int32 main() {
            MDPMat obj;
            int32 a[2][3] = {{1, 2, 3}, {4, 5, 6}};
            return obj.rowSum(a) == 6 ? 0 : 1;
        }
    )", "test_mdp_method.cmm"), 0);
}

// D1': slice length on a row-slice param = row count.
TEST_F(MultiDimE2E, MultiDimParamLen) {
    EXPECT_EQ(runSource(R"(
        int32 MDRows(int32 m[][3]) { return m.len; }
        int32 main() {
            int32 a[2][3] = {{1, 2, 3}, {4, 5, 6}};
            return MDRows(a) == 2 ? 0 : 1;
        }
    )", "test_mdp_len.cmm"), 0);
}
