// PAR-22 / MOD-12: namespace declarations, qualified names, and name isolation.
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

class NamespaceE2E : public ::testing::Test {
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

TEST_F(NamespaceE2E, ParsesNamespaceDeclaration) {
    Lexer lexer("ns.cmm", "namespace geometry { int32 f() { return 1; } }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    ASSERT_TRUE(parser.getErrors().empty());
    ASSERT_EQ(ast->declarations.size(), 1u);
    auto* ns = dynamic_cast<NamespaceDeclAST*>(ast->declarations[0].get());
    ASSERT_NE(ns, nullptr);
    EXPECT_EQ(ns->name, "geometry");
    ASSERT_EQ(ns->declarations.size(), 1u);
}

TEST_F(NamespaceE2E, QualifiedCallResolves) {
    EXPECT_EQ(runSource(R"(
namespace A { int32 f() { return 7; } }
int32 main() { return A::f() - 7; }
)", "ns_qualified.cmm"), 0);
}

TEST_F(NamespaceE2E, UnqualifiedSiblingCallInsideNamespace) {
    EXPECT_EQ(runSource(R"(
namespace A {
    int32 g() { return 1; }
    int32 f() { return g() + 1; }
}
int32 main() { return A::f() - 2; }
)", "ns_sibling.cmm"), 0);
}

TEST_F(NamespaceE2E, NamespacesDoNotCollideWithGlobals) {
    EXPECT_EQ(runSource(R"(
int32 f() { return 1; }
namespace A { int32 f() { return 2; } }
int32 main() { return f() + A::f() - 3; }
)", "ns_collision.cmm"), 0);
}

TEST_F(NamespaceE2E, NestedNamespaces) {
    EXPECT_EQ(runSource(R"(
namespace A::B { int32 f() { return 9; } }
int32 main() { return A::B::f() - 9; }
)", "ns_nested.cmm"), 0);
}

TEST_F(NamespaceE2E, QualifiedStructType) {
    EXPECT_EQ(runSource(R"(
namespace A {
    struct Point { int32 x; int32 y; };
    int32 sum(Point* p) { return p->x + p->y; }
}
int32 main() {
    A::Point p;
    p.x = 3;
    p.y = 4;
    return A::sum(&p) - 7;
}
)", "ns_type_struct.cmm"), 0);
}

TEST_F(NamespaceE2E, QualifiedUsingType) {
    EXPECT_EQ(runSource(R"(
namespace A { using Handle = int32; }
int32 main() {
    A::Handle h = 5;
    return h - 5;
}
)", "ns_type_using.cmm"), 0);
}

TEST_F(NamespaceE2E, QualifiedEnumConstant) {
    EXPECT_EQ(runSource(R"(
namespace A { enum Color { Red, Green }; }
int32 main() { return A::Green - 1; }
)", "ns_type_enum.cmm"), 0);
}

TEST_F(NamespaceE2E, NestedQualifiedStructType) {
    EXPECT_EQ(runSource(R"(
namespace A::B { struct P { int32 v; }; }
int32 main() {
    A::B::P p;
    p.v = 9;
    return p.v - 9;
}
)", "ns_type_nested.cmm"), 0);
}
