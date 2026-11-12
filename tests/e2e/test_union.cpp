// AGG-17 / AGG-03: union layout/behaviour baseline and anonymous member promotion.
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

class UnionE2E : public ::testing::Test {
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

static int runSource(const std::string& source) {
    Lexer lexer("union.cdd", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast || !parser.getErrors().empty()) return -1;

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return -1;

    CodegenContext ctx;
    ctx.setSourceFile("union.cdd");
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

// AGG-17: declaration, `{42}`-style init, copy init, assignment.
TEST_F(UnionE2E, DeclarationInitCopyAccess) {
    EXPECT_EQ(runSource(R"(
        union U { int32 i; float32 f; char c; };
        int32 main() {
            union U u = {42};
            if (u.i != 42) return 1;
            union U v = u;
            union U w;
            w = u;
            return (v.i == 42 && w.i == 42) ? 0 : 1;
        }
    )"), 0);
}

// AGG-17: a union is as large as its largest member (aligned).
TEST_F(UnionE2E, SizeofIsLargestMember) {
    EXPECT_EQ(runSource(R"(
        union U { int8 c; int32 i; int64 l; };
        int32 main() {
            return sizeof(union U) == 8 ? 0 : 1;
        }
    )"), 0);
}

// AGG-17: nested unions and array members inside a union.
TEST_F(UnionE2E, NestedUnionAndArrayMember) {
    EXPECT_EQ(runSource(R"(
        union Inner { int32 i; float32 f; };
        union Outer { union Inner in; int32 arr[3]; int64 raw; };
        int32 main() {
            union Outer o;
            o.arr[0] = 2;
            o.arr[1] = 3;
            o.arr[2] = 4;
            if (o.arr[1] != 3) return 1;
            o.in.i = 7;
            return o.in.i == 7 ? 0 : 1;
        }
    )"), 0);
}

// AGG-17: union pointer member access (`u->i`) in both directions.
TEST_F(UnionE2E, PointerMemberAccess) {
    EXPECT_EQ(runSource(R"(
        union U { int32 i; float32 f; };
        int32 peek(union U* u) { return u->i; }
        int32 main() {
            union U u;
            u.i = 11;
            union U* p = &u;
            return (peek(p) == 11 && p->i == 11) ? 0 : 1;
        }
    )"), 0);
}

// AGG-17: tagged-union idiom (explicit tag member next to a named inner union).
TEST_F(UnionE2E, TaggedUnionIdiom) {
    EXPECT_EQ(runSource(R"(
        struct Value {
            int32 tag;
            union Data { int32 i; float32 f; } data;
        };
        int32 main() {
            struct Value v;
            v.tag = 1;
            v.data.i = 5;
            if (v.data.i != 5) return 1;
            v.tag = 2;
            v.data.f = 2.5f32;
            return v.tag == 2 ? 0 : 1;
        }
    )"), 0);
}

// AGG-03: members of an anonymous union are promoted into the enclosing struct.
TEST_F(UnionE2E, AnonymousUnionMembersArePromoted) {
    EXPECT_EQ(runSource(R"(
        struct S {
            int32 tag;
            union { int32 i; float32 f; };
        };
        int32 main() {
            struct S s;
            s.tag = 1;
            s.i = 9;
            if (s.i != 9) return 1;
            s.f = 1.25f32;
            return s.tag == 1 ? 0 : 1;
        }
    )"), 0);
}

// AGG-03: members of an anonymous struct are promoted into the enclosing struct.
TEST_F(UnionE2E, AnonymousStructMembersArePromoted) {
    EXPECT_EQ(runSource(R"(
        struct S {
            int32 tag;
            struct { int32 x; int32 y; };
        };
        int32 main() {
            struct S s;
            s.x = 3;
            s.y = 4;
            return (s.x + s.y == 7) ? 0 : 1;
        }
    )"), 0);
}

// Aliases to aggregates must behave like the underlying type for member access.
TEST_F(UnionE2E, UsingAggregateMemberAccess) {
    EXPECT_EQ(runSource(R"(
        using S2 = struct S2 { int32 x; };
        using U2 = union U2 { int32 i; float32 f; };
        int32 main() {
            S2 s;
            s.x = 3;
            U2 u;
            u.i = 4;
            return (s.x == 3 && u.i == 4) ? 0 : 1;
        }
    )"), 0);
}
