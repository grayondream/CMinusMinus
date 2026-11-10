#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

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

class CompileOnlyE2E : public ::testing::Test {
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

static std::unique_ptr<CodegenContext> codegenSource(const std::string& source,
                                                     const std::string& filename) {
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) return nullptr;

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return nullptr;

    auto ctx = std::make_unique<CodegenContext>();
    ctx->setSourceFile(filename);
    ast->codegen(*ctx);
    ctx->finalizeDebugInfo();

    std::string ve;
    llvm::raw_string_ostream vs(ve);
    if (llvm::verifyModule(ctx->getModule(), &vs)) return nullptr;
    return ctx;
}

static std::vector<std::string> compileErrors(const std::string& source,
                                              const std::string& filename) {
    Lexer lexer(filename, source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    std::vector<std::string> errors;
    for (auto& d : parser.getErrors()) errors.push_back(d.format());
    if (!ast) return errors;
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    for (auto& d : analyzer.getErrors()) errors.push_back(d.format());
    return errors;
}

static bool hasErrorContaining(const std::vector<std::string>& errors,
                               const std::string& needle) {
    for (auto& e : errors) {
        if (e.find(needle) != std::string::npos) return true;
    }
    return false;
}

static int runSource(const std::string& source, const std::string& filename) {
    auto ctx = codegenSource(source, filename);
    if (!ctx) return -1;

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) return -1;
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb))
        .create();
    if (!jit) return -1;

    auto ts = llvm::orc::ThreadSafeModule(ctx->takeModule(), ctx->takeContext());
    if (auto e = (*jit)->addIRModule(std::move(ts))) return -1;

    auto sym = (*jit)->lookup("main");
    if (!sym) return -1;
    auto fn = (int (*)())(intptr_t)sym->getValue();
    return fn();
}

TEST_F(CompileOnlyE2E, CompileTimeFunctionReturnsAggregate) {
    EXPECT_EQ(runSource(R"(
struct Pair { int32 a; int32 b; };
compile_time Pair make(int32 x) {
    Pair p;
    p.a = x;
    p.b = x + 1;
    return p;
}
compile_time Pair P = make(20);
int32 main() { return P.a + P.b - 41; }
)", "ct_compile_time_pair.cmm"), 0);
}

TEST_F(CompileOnlyE2E, CompileOnlyPointerReturnRejected) {
    auto errs = compileErrors(R"(
compile_only int32* bad() { return nullptr; }
int32 main() { return 0; }
)", "ct_compile_only_ptr.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "return a fixed-size"));
}

TEST_F(CompileOnlyE2E, AggregateReturnAndMemberWrites) {
    EXPECT_EQ(runSource(R"(
struct Pair { int32 a; int32 b; };
compile_only Pair makePair(int32 x) {
    Pair p;
    p.a = x;
    p.b = x + 1;
    return p;
}
compile_time Pair P = makePair(20);
int32 main() { return P.a + P.b - 41; }
)", "ct_pair_return.cmm"), 0);
}

TEST_F(CompileOnlyE2E, AggregateParameterIsDeepCopied) {
    EXPECT_EQ(runSource(R"(
struct Pair { int32 a; int32 b; };
compile_only Pair makePair(int32 x) {
    Pair p;
    p.a = x;
    p.b = x + 1;
    return p;
}
compile_only int32 mutate(Pair p) {
    p.a = 99;
    return p.a;
}
compile_time Pair P = makePair(20);
compile_time int32 X = mutate(P);
int32 main() { return P.a + X - 119; }
)", "ct_pair_copy.cmm"), 0);
}

TEST_F(CompileOnlyE2E, DfaTableBuildsAtCompileTimeWithoutSymbol) {
    auto ctx = codegenSource(R"(
struct DfaTable {
    int32 next[4][8];
    int32 initial;
};
compile_only DfaTable build_dfa(string_view pattern) {
    DfaTable t;
    for (usize s = 0; s < 4; s = s + 1) {
        for (usize c = 0; c < 8; c = c + 1) {
            t.next[s][c] = 0;
        }
    }
    t.initial = 0;
    for (usize i = 0; i < pattern.len(); i = i + 1) {
        t.next[0][i] = pattern.char_at(i);
    }
    return t;
}
compile_time DfaTable EMAIL_DFA = build_dfa("abc");
int32 main() { return EMAIL_DFA.next[0][1] - 'b'; }
)", "ct_dfa.cmm");
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(ctx->getModule().getFunction("build_dfa"), nullptr);
    EXPECT_NE(ctx->getModule().getNamedGlobal("EMAIL_DFA"), nullptr);
}

TEST_F(CompileOnlyE2E, CompileOnlyStepLimitDiagnosed) {
    auto errs = compileErrors(R"(
compile_only int32 spin(int32 x) {
    while (x > 0) { x = x; }
    return x;
}
compile_time int32 N = spin(1);
int32 main() { return 0; }
)", "ct_step_limit.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "limit"));
}

TEST_F(CompileOnlyE2E, CompileTimeAggregateInitializerMaterializes) {
    EXPECT_EQ(runSource(R"(
struct Pair { int32 a; int32 b; };
compile_time Pair P = { 20, 21 };
int32 main() { return P.a + P.b - 41; }
)", "ct_pair.cmm"), 0);
}

TEST_F(CompileOnlyE2E, CompileTimeAggregateDoesNotPoisonType) {
    EXPECT_EQ(runSource(R"(
struct S { int32 x; };
compile_time S A = { 1 };
int32 main() {
    S b;
    b.x = 2;
    return A.x + b.x - 3;
}
)", "ct_no_type_poison.cmm"), 0);
}

TEST_F(CompileOnlyE2E, CompileOnlyInsideIfCompileTimeAllowed) {
    EXPECT_EQ(runSource(R"(
compile_only int32 build(int32 x) { return x * 2; }
compile_time int32 f(int32 x) {
    if compile_time {
        return build(x);
    } else {
        return 0;
    }
}
compile_time int32 CT = f(2);
int32 main() {
    return f(2) + CT - 4;
}
)", "compile_only_guard.cmm"), 0);
}

TEST_F(CompileOnlyE2E, DirectCallInCompileTimeFunctionRejected) {
    auto errs = compileErrors(R"(
compile_only int32 build(int32 x) { return x * 2; }
compile_time int32 make(int32 x) { return build(x); }
int32 main() { return make(1); }
)", "compile_only_direct.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "outside 'if compile_time'"));
}

TEST_F(CompileOnlyE2E, RuntimeCallChainReportsPath) {
    auto errs = compileErrors(R"(
compile_only int32 build(int32 x) { return x * 2; }
int32 make(int32 x) { return build(x); }
int32 main() { return make(1); }
)", "compile_only_runtime.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "main"));
    EXPECT_TRUE(hasErrorContaining(errs, "make"));
    EXPECT_TRUE(hasErrorContaining(errs, "build"));
}

TEST_F(CompileOnlyE2E, CompileOnlyBodyCannotCallRuntimeFunction) {
    auto errs = compileErrors(R"(
int32 runtime_helper(int32 x) { return x; }
compile_only int32 build(int32 x) { return runtime_helper(x); }
int32 main() { return 0; }
)", "compile_only_runtime_body.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "cannot call runtime function"));
}

TEST_F(CompileOnlyE2E, CompileOnlyValueUseRejected) {
    auto errs = compileErrors(R"(
compile_only int32 build(int32 x) { return x; }
int32 main() { int32 (*p)(int32) = build; return p(1); }
)", "compile_only_value.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "value"));
}

TEST_F(CompileOnlyE2E, CompileOnlyBodyCannotPrint) {
    auto errs = compileErrors(R"(
compile_only int32 build() { print("x"); return 0; }
int32 main() { return 0; }
)", "compile_only_print.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "runtime"));
}

TEST_F(CompileOnlyE2E, CompileOnlyBodyCannotUseVolatile) {
    auto errs = compileErrors(R"(
compile_only int32 build() { volatile int32 x = 1; return x; }
int32 main() { return 0; }
)", "compile_only_volatile.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "volatile"));
}

TEST_F(CompileOnlyE2E, CompileOnlyBodyCannotReadMutableGlobal) {
    auto errs = compileErrors(R"(
int32 g = 1;
compile_only int32 build() { return g; }
int32 main() { return 0; }
)", "compile_only_global.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "mutable global"));
}

TEST_F(CompileOnlyE2E, AddressOfCompileOnlyRejected) {
    auto errs = compileErrors(R"(
compile_only int32 build(int32 x) { return x; }
int32 main() { int32 (*p)(int32) = &build; return p(1); }
)", "compile_only_addr.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "address"));
}

TEST_F(CompileOnlyE2E, IfCompileTimeSelectsBothPaths) {
    EXPECT_EQ(runSource(R"(
compile_time int32 f(int32 x) {
    if compile_time {
        return x + 1;
    } else {
        return x - 1;
    }
}
compile_time int32 CT = f(5);
int32 main() {
    return f(5) * 10 + CT - 46;
}
)", "if_compile_time.cmm"), 0);
}

TEST_F(CompileOnlyE2E, IfCompileTimeRejectedInRuntimeFunction) {
    auto errs = compileErrors(R"(
int32 main() {
    if compile_time { return 1; } else { return 0; }
}
)", "if_compile_time_bad.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "if compile_time"));
}

TEST_F(CompileOnlyE2E, ScalarBuildsCompileTimeVariable) {
    EXPECT_EQ(runSource(R"(
compile_only int32 twice(int32 x) { return x * 2; }
compile_time int32 X = twice(21);
int32 main() { return X - 42; }
)", "compile_only_scalar.cmm"), 0);
}

TEST_F(CompileOnlyE2E, NoRuntimeSymbolOrDeclaration) {
    auto ctx = codegenSource(R"(
compile_only int32 helper(int32 x) { return x + 1; }
int32 main() { return 0; }
)", "compile_only_no_symbol.cmm");
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(ctx->getModule().getFunction("helper"), nullptr);
    EXPECT_EQ(ctx->getModule().getFunction("helper_int32"), nullptr);
}
