// TYP-20 / TYP-09 / AGG-15: enum types are strongly typed. Implicit conversion
// between an enum and an integer (or a distinct enum) is rejected; explicit
// casts are required. Arithmetic/comparison still promote enum operands to int.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "codegen/CodegenContext.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/Diagnostic.h"
#include "sema/SemanticAnalyzer.h"
#include "support/Log.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"

class EnumStrongE2E : public ::testing::Test {
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

struct AnalyzeResult {
    std::vector<Diagnostic> errors;
    bool hasCode(DiagnosticCode code) const {
        for (const auto& d : errors) {
            if (d.code == code) return true;
        }
        return false;
    }
    bool hasMessage(const std::string& needle) const {
        for (const auto& d : errors) {
            if (d.message.find(needle) != std::string::npos) return true;
        }
        return false;
    }
};

static AnalyzeResult analyzeSource(const std::string& source,
                                   const std::string& file = "enum_strong.cmm") {
    Lexer lexer(file, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    AnalyzeResult r;
    if (!ast) {
        r.errors.emplace_back(Diagnostic::Level::Error, "parse failed", file, 0, 0);
        return r;
    }
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    r.errors = analyzer.getErrors();
    return r;
}

static int runSource(const std::string& source, const std::string& file = "enum_strong.cmm") {
    Lexer lexer(file, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast || !parser.getErrors().empty()) return -1;

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return -1;

    CodegenContext ctx;
    ctx.setSourceFile(file);
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

// ---------------------------------------------------------------------------
// Rejected implicit conversions
// ---------------------------------------------------------------------------

TEST_F(EnumStrongE2E, EnumToIntInitializationRequiresCast) {
    auto r = analyzeSource(
        "enum E { A };\n"
        "int32 main() { enum E e = A; int32 x = e; return x; }\n");
    EXPECT_FALSE(r.errors.empty());
}

TEST_F(EnumStrongE2E, EnumToIntAssignmentRequiresCast) {
    auto r = analyzeSource(
        "enum E { A };\n"
        "int32 main() { enum E e = A; int32 x = 0; x = e; return x; }\n");
    EXPECT_TRUE(r.hasCode(DiagnosticCode::SemIncompatibleAssignment));
    EXPECT_TRUE(r.hasMessage("TYP-20"));
}

TEST_F(EnumStrongE2E, IntToEnumInitializationRequiresCast) {
    auto r = analyzeSource(
        "enum E { A };\n"
        "int32 main() { enum E e = 5; return 0; }\n");
    EXPECT_FALSE(r.errors.empty());
}

TEST_F(EnumStrongE2E, IntToEnumAssignmentRequiresCast) {
    auto r = analyzeSource(
        "enum E { A };\n"
        "int32 main() { enum E e = A; e = 1; return 0; }\n");
    EXPECT_TRUE(r.hasCode(DiagnosticCode::SemIncompatibleAssignment));
}

TEST_F(EnumStrongE2E, DistinctEnumsAreNotImplicitlyCompatible) {
    auto r = analyzeSource(
        "enum E { A };\n"
        "enum F { B };\n"
        "int32 main() { enum E e = A; enum F f = e; return 0; }\n");
    EXPECT_FALSE(r.errors.empty());
}

TEST_F(EnumStrongE2E, EnumToFloatRequiresCast) {
    auto r = analyzeSource(
        "enum E { A };\n"
        "int32 main() { float32 x = A; return 0; }\n");
    EXPECT_FALSE(r.errors.empty());
}

TEST_F(EnumStrongE2E, EnumFunctionArgumentRequiresCast) {
    auto r = analyzeSource(
        "int32 takes_int(int32 v);\n"
        "enum E { A };\n"
        "int32 main() { return takes_int(A); }\n");
    EXPECT_FALSE(r.errors.empty());
}

TEST_F(EnumStrongE2E, EnumReturnFromIntFunctionRequiresCast) {
    auto r = analyzeSource(
        "enum E { A };\n"
        "int32 f() { return A; }\n"
        "int32 main() { return f(); }\n");
    EXPECT_FALSE(r.errors.empty());
}

// ---------------------------------------------------------------------------
// Allowed (same enum / explicit cast / arithmetic / comparison)
// ---------------------------------------------------------------------------

TEST_F(EnumStrongE2E, SameEnumAssignmentAllowed) {
    auto r = analyzeSource(
        "enum E { A, B };\n"
        "int32 main() { enum E e = A; enum E f = e; f = B; return 0; }\n");
    EXPECT_TRUE(r.errors.empty());
}

TEST_F(EnumStrongE2E, ExplicitCastEnumToIntAllowed) {
    EXPECT_EQ(runSource(
        "enum E { A = 3 };\n"
        "int32 main() { enum E e = A; return (int32)e - 3; }\n"), 0);
}

TEST_F(EnumStrongE2E, ExplicitCastIntToEnumAllowed) {
    EXPECT_EQ(runSource(
        "enum E { A, B, C };\n"
        "int32 main() { enum E e = (enum E)2; return e == C ? 0 : 1; }\n"), 0);
}

TEST_F(EnumStrongE2E, StaticCastEnumToIntAllowed) {
    EXPECT_EQ(runSource(
        "enum E { A = 7 };\n"
        "int32 main() { return static_cast<int32>(A) - 7; }\n"), 0);
}

TEST_F(EnumStrongE2E, EnumArithmeticPromotesToInt) {
    EXPECT_EQ(runSource(
        "enum E { A = 2, B = 5 };\n"
        "int32 main() { return (A + B) - 7; }\n"), 0);
}

TEST_F(EnumStrongE2E, EnumComparisonWithIntAllowed) {
    EXPECT_EQ(runSource(
        "enum E { A = 4 };\n"
        "int32 main() { enum E e = A; return (e == 4 && e >= 4 && e <= 4) ? 0 : 1; }\n"), 0);
}

TEST_F(EnumStrongE2E, EnumSwitchStillWorks) {
    EXPECT_EQ(runSource(R"(
enum E { A, B, C };
int32 main() {
    enum E e = B;
    switch (e) {
        case A: return 1;
        case B: return 0;
        default: return 2;
    }
}
)"), 0);
}
