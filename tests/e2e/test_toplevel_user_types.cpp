// Top-level declarations of user-defined types.
//
// Regression: the record parser consumes the declaration's terminating ';',
// but the caller then still treated the *next* token as an attached
// declarator.  `struct S { ... }; S g;` read the `S` of the next declaration
// as the variable name of the struct declaration.  The same greedy lookahead
// broke functions returning a bare tag type (`V f()`), template instances
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

class TopLevelUserTypeE2E : public ::testing::Test {
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

TEST_F(TopLevelUserTypeE2E, StructVariableAfterDefinition) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 x; };
        S g;
        int32 main() { g.x = 5; return g.x - 5; }
    )", "tl_struct_var.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, StructPointerAfterDefinition) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 x; };
        S* p;
        int32 main() { S s; s.x = 3; p = &s; return p->x - 3; }
    )", "tl_struct_ptr.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, BareTagReturningFunction) {
    EXPECT_EQ(runSource(R"(
        struct V { int32 x; };
        V make(int32 v) { V r; r.x = v; return r; }
        int32 main() { V a = make(7); return a.x - 7; }
    )", "tl_struct_ret.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, ElaboratedTagReturningFunction) {
    EXPECT_EQ(runSource(R"(
        struct V { int32 x; };
        struct V make(int32 v) { struct V r; r.x = v; return r; }
        int32 main() { V a = make(8); return a.x - 8; }
    )", "tl_struct_ret_elab.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, BareClassReturningFunction) {
    EXPECT_EQ(runSource(R"(
        class C { public: int32 x; };
        C make(int32 v) { C c; c.x = v; return c; }
        int32 main() { C a = make(4); return a.x - 4; }
    )", "tl_class_ret.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, UnionVariableAfterDefinition) {
    EXPECT_EQ(runSource(R"(
        union U { int32 i; float32 f; };
        U u;
        int32 main() { u.i = 9; return u.i - 9; }
    )", "tl_union_var.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, TemplateInstanceVariableAndReturn) {
    EXPECT_EQ(runSource(R"(
        template<typename T> struct B { T x; };
        B<int32> g;
        B<int32> make() { B<int32> b; b.x = 1; return b; }
        int32 main() { g.x = 2; B<int32> m = make(); return g.x + m.x - 3; }
    )", "tl_template_inst.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, UsingAliasAfterStructDefinition) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 x; };
        using W = S;
        W g;
        int32 main() { g.x = 6; return g.x - 6; }
    )", "tl_using_alias.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, AttachedVariableStillWorks) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 x; } g;
        int32 main() { g.x = 1; return g.x - 1; }
    )", "tl_attached_var.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, BareTagOperatorReturn) {
    EXPECT_EQ(runSource(R"(
        struct V { int32 x; };
        V operator+(V a, V b) { V r; r.x = a.x + b.x; return r; }
        int32 main() { V a; V b; a.x = 1; b.x = 2; V c = a + b; return c.x - 3; }
    )", "tl_operator_bare.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, PointerAliasAfterDefinition) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 x; };
        using P = S*;
        P p;
        int32 main() { S s; s.x = 2; p = &s; return p->x - 2; }
    )", "tl_using_ptr.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, UsingAliasBeforeStructDefinition) {
    EXPECT_EQ(runSource(R"(
        using N = struct Node;
        struct Node { int32 x; };
        int32 main() { N n; n.x = 5; return n.x - 5; }
    )", "fwd_alias_struct.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, ForwardDeclThenAliasBeforeDefinition) {
    EXPECT_EQ(runSource(R"(
        struct Node;
        using N = Node;
        struct Node { int32 x; };
        int32 main() { N n; n.x = 6; return n.x - 6; }
    )", "fwd_decl_alias.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, UsingAliasBeforeUnionDefinition) {
    EXPECT_EQ(runSource(R"(
        using U = union Value;
        union Value { int32 i; float32 f; };
        int32 main() { U u; u.i = 7; return u.i - 7; }
    )", "fwd_alias_union.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, ForwardPointerMemberAfterDefinition) {
    EXPECT_EQ(runSource(R"(
        struct S; S* p;
        struct S { int32 x; };
        int32 main() { S s; s.x = 4; p = &s; return p->x - 4; }
    )", "fwd_ptr_member.cdd"), 0);
}

TEST_F(TopLevelUserTypeE2E, PointerAliasToForwardStruct) {
    EXPECT_EQ(runSource(R"(
        using P = struct S*;
        struct S { int32 x; };
        int32 main() { S s; s.x = 8; P p = &s; return p->x - 8; }
    )", "fwd_ptr_alias.cdd"), 0);
}
