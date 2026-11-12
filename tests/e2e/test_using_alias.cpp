// PAR-24: `using` is the sole type-alias form; its RHS is a full type-id.
// Coverage: fixed-size arrays (1D/multi-dim/sizeof/slice/designated init),
// function pointers, inline enums, annotations and removed legacy aliases.

// (`B<int32> g;`) and `using` aliases placed after a record definition.

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

class UsingAliasE2E : public ::testing::Test {
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
    if (!ast) { ADD_FAILURE() << "Parse failed"; return -1; }
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) {
        for (auto& err : analyzer.getErrors()) ADD_FAILURE() << "Semantic error: " << err.format();
        return -1;
    }
    CodegenContext ctx;
    ctx.setSourceFile(filename);
    ast->codegen(ctx);
    ctx.finalizeDebugInfo();
    std::string ve;
    llvm::raw_string_ostream vs(ve);
    if (llvm::verifyModule(ctx.getModule(), &vs)) {
        vs.flush(); ADD_FAILURE() << "Module verification failed: " << ve; return -1;
    }
    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) { ADD_FAILURE() << "detectHost failed"; return -1; }
    auto jit = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit) { ADD_FAILURE() << "LLJIT create failed"; return -1; }
    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto e = (*jit)->addIRModule(std::move(ts))) { ADD_FAILURE() << "addIRModule failed"; return -1; }
    auto sym = (*jit)->lookup("main");
    if (!sym) { ADD_FAILURE() << "lookup main failed"; return -1; }
    auto fn = (int (*)())(intptr_t)sym->getValue();
    return fn();
}

TEST_F(UsingAliasE2E, FixedArrayAlias) {
    EXPECT_EQ(runSource(R"(
        using IntArr = int32[4];
        int32 main() { IntArr a = {1, 2, 3, 4}; return a[3] - 4; }
    )", "using_arr.cdd"), 0);
}

TEST_F(UsingAliasE2E, MultidimArrayAlias) {
    EXPECT_EQ(runSource(R"(
        using Mat = int32[2][3];
        int32 main() { Mat m; m[1][2] = 6; return m[1][2] - 6; }
    )", "using_mat.cdd"), 0);
}

TEST_F(UsingAliasE2E, ArrayAliasSizeofAndSlice) {
    EXPECT_EQ(runSource(R"(
        using IntArr = int32[10];
        int32 main() {
            IntArr a; a[9] = 9;
            int32[] s = a;
            return (sizeof(a) == 40 && s.len == 10) ? 0 : 1;
        }
    )", "using_arr_meta.cdd"), 0);
}

TEST_F(UsingAliasE2E, SliceAliasStillWorks) {
    EXPECT_EQ(runSource(R"(
        using Sl = int32[];
        int32 main() { int32 a[3]; a[0] = 1; Sl s = a; return s[0] - 1; }
    )", "using_slice.cdd"), 0);
}

TEST_F(UsingAliasE2E, ArrayAliasDesignatedZeroFill) {
    EXPECT_EQ(runSource(R"(
        using Arr = int32[4];
        int32 main() { Arr a = {[1] = 5}; return a[0]; }
    )", "using_arr_di.cdd"), 0);
}

TEST_F(UsingAliasE2E, FnPtrAliasCall) {
    EXPECT_EQ(runSource(R"(
        using Fn = int32 (*)(int32, int32);
        int32 add(int32 a, int32 b) { return a + b; }
        int32 main() { Fn f = add; return f(2, 3) - 5; }
    )", "using_fnptr.cdd"), 0);
}

TEST_F(UsingAliasE2E, FnPtrAliasParam) {
    EXPECT_EQ(runSource(R"(
        using Fn = int32 (*)(int32, int32);
        int32 add(int32 a, int32 b) { return a + b; }
        int32 apply(Fn f, int32 x) { return f(x, 1); }
        int32 main() { return apply(add, 4) - 5; }
    )", "using_fnptr_param.cdd"), 0);
}

TEST_F(UsingAliasE2E, FnPtrAliasReturn) {
    EXPECT_EQ(runSource(R"(
        using Fn = int32 (*)(int32, int32);
        int32 add(int32 a, int32 b) { return a + b; }
        Fn pick() { return add; }
        int32 main() { Fn f = pick(); return f(3, 4) - 7; }
    )", "using_fnptr_ret.cdd"), 0);
}

TEST_F(UsingAliasE2E, InlineEnumAliasExposesEnumerators) {
    EXPECT_EQ(runSource(R"(
        using E = enum : uint16 { Lo = 1, Hi = 2 };
        int32 main() { E e = Hi; return (int32)e + Lo - 3; }
    )", "using_enum.cdd"), 0);
}

TEST_F(UsingAliasE2E, DerefPointerAliasReadWrite) {
    EXPECT_EQ(runSource(R"(
        using P = int32*;
        int32 main() { int32 x = 7; P p = &x; *p = 9; return *p - 9; }
    )", "deref_alias.cdd"), 0);
}

TEST_F(UsingAliasE2E, DerefStructPointerAliasMember) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 x; };
        using PS = struct S*;
        int32 main() { S s; s.x = 1; PS p = &s; (*p).x = 5; return p->x - 5; }
    )", "deref_struct_alias.cdd"), 0);
}

TEST_F(UsingAliasE2E, DerefAliasChain) {
    EXPECT_EQ(runSource(R"(
        using P = int32*;
        using PP = P*;
        int32 main() { int32 x = 3; P p = &x; PP pp = &p; return **pp - 3; }
    )", "deref_alias_chain.cdd"), 0);
}

TEST_F(UsingAliasE2E, BlockScopedAliasRuns) {
    EXPECT_EQ(runSource(R"(
        int32 main() { using T = int32; T x = 3; return x - 3; }
    )", "block_alias.cdd"), 0);
}

TEST_F(UsingAliasE2E, TemplateAliasAcrossFunctions) {
    EXPECT_EQ(runSource(R"(
        template<typename T, usize N> struct Arr { T data[N]; };
        template<typename T> using Vec = Arr<T, 2>;
        int32 first() { Vec<int32> v; v.data[0] = 1; return v.data[0]; }
        int32 second() { Vec<int32> v; v.data[0] = 2; return v.data[0]; }
        int32 main() { return first() + second() - 3; }
    )", "tpl_alias_fns.cdd"), 0);
}

TEST_F(UsingAliasE2E, StructMemberAlias) {
    EXPECT_EQ(runSource(R"(
        struct S { using T = int32; T x; };
        int32 main() { S s; s.x = 3; return s.x - 3; }
    )", "member_alias_struct.cdd"), 0);
}

TEST_F(UsingAliasE2E, ClassPublicMemberAliasQualified) {
    EXPECT_EQ(runSource(R"(
        class C { public: using T = int32; };
        int32 main() { C::T v = 4; return v - 4; }
    )", "member_alias_class.cdd"), 0);
}

TEST_F(UsingAliasE2E, UnionMemberAlias) {
    EXPECT_EQ(runSource(R"(
        union U { using T = int32; T i; };
        int32 main() { U u; u.i = 6; return u.i - 6; }
    )", "member_alias_union.cdd"), 0);
}

TEST_F(UsingAliasE2E, PrivateMemberAliasInsideMethod) {
    EXPECT_EQ(runSource(R"(
        class C { using T = int32; public: T get() { T v = 5; return v; } };
        int32 main() { C c; return c.get() - 5; }
    )", "member_alias_private_method.cdd"), 0);
}

TEST_F(UsingAliasE2E, QualifiedMemberAliasBypassesBlockAlias) {
    EXPECT_EQ(runSource(R"(
        class C { public: using T = int32; };
        int32 main() { using T = float64; C::T v = 0; return sizeof(v) == 4 ? 0 : 1; }
    )", "member_alias_qualified.cdd"), 0);
}
