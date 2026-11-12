// Review 2.2–2.6: language ergonomics fixes.
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

class ErgonomicsE2E : public ::testing::Test {
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
    Lexer lexer("ergonomics.cdd", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast || !parser.getErrors().empty()) return -1;

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return -1;

    CodegenContext ctx;
    ctx.setSourceFile("ergonomics.cdd");
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

static bool compiles(const std::string& source) {
    Lexer lexer("ergonomics.cdd", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast || !parser.getErrors().empty()) return false;
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    return analyzer.getErrors().empty();
}

static std::string compileIR(const std::string& source) {
    Lexer lexer("ergonomics.cdd", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast || !parser.getErrors().empty()) return "<parse-error>";
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return "<sema-error>";
    CodegenContext ctx;
    ctx.setSourceFile("ergonomics.cdd");
    ast->codegen(ctx);
    std::string ir;
    llvm::raw_string_ostream os(ir);
    ctx.getModule().print(os, nullptr);
    return ir;
}

TEST_F(ErgonomicsE2E, NullptrIsNotAKeyword) {
    EXPECT_FALSE(compiles("int32 main() { int32* p = nullptr; return p == null ? 0 : 1; }"));
}

TEST_F(ErgonomicsE2E, NullPointerLiteralStillWorks) {
    EXPECT_EQ(runSource(
        "int32 main() { int32* p = null; return p == null ? 0 : 1; }"), 0);
}

TEST_F(ErgonomicsE2E, OptionalConditionAndMethods) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32? good = {true, 42};
            if (!good) return 1;
            if (good.value != 42) return 2;
            if (!good.is_some()) return 3;
            if (good.is_none()) return 4;
            if (good.unwrap() != 42) return 5;
            int32? bad = {false, 0};
            if (bad) return 6;
            if (!bad.is_none()) return 7;
            if (bad.value_or(7) != 7) return 8;
            if (!(good && !bad)) return 9;
            return good ? 0 : 10;
        }
    )"), 0);
}

TEST_F(ErgonomicsE2E, ResultConditionAndMethods) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            result<int32, int32> ok = {true, 42, 0};
            if (!ok) return 1;
            if (!ok.is_ok()) return 2;
            if (ok.is_err()) return 3;
            if (ok.unwrap() != 42) return 4;
            result<int32, int32> err = {false, 0, 9};
            if (err) return 5;
            if (!err.is_err()) return 6;
            if (err.value_or(7) != 7) return 7;
            return 0;
        }
    )"), 0);
}

TEST_F(ErgonomicsE2E, StringViewDoesNotImplicitlyBecomeMutableCharPtr) {
    EXPECT_FALSE(compiles(
        "int32 main() { string_view v = \"abc\"; char* p = v; return p[0]; }"));
}

TEST_F(ErgonomicsE2E, StringLiteralDoesNotImplicitlyBecomeMutableCharPtr) {
    EXPECT_FALSE(compiles(
        "int32 main() { char* p = \"abc\"; return p[0]; }"));
}

TEST_F(ErgonomicsE2E, StringViewExplicitDataAndCastWork) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view v = "abc";
            const char* ro = v;
            if (ro[0] != 'a') return 1;
            const char* d = v.data;
            if (d[1] != 'b') return 2;
            char* p = (char*)v;
            if (p[2] != 'c') return 3;
            const char* lit = "hello";
            if (lit[0] != 'h') return 4;
            string s = string.new("hi");
            char* q = s.data;
            if (q[0] != 'h') return 5;
            const char* r = s.data;
            if (r[1] != 'i') return 6;
            return 0;
        }
    )"), 0);
}

TEST_F(ErgonomicsE2E, LocalStringIsReleasedAtScopeExit) {
    EXPECT_NE(compileIR(
        "int32 main() { string s = string.new(\"x\"); return 0; }")
        .find("call void @free"), std::string::npos);
}

TEST_F(ErgonomicsE2E, AutoStoresCapturedClosure) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 base = 30;
            auto add = [base](int32 x) -> int32 { return base + x; };
            if (add(12) != 42) return 1;
            base = 0;
            if (add(12) != 42) return 2;
            return 0;
        }
    )"), 0);
}

TEST_F(ErgonomicsE2E, AutoStoresReferenceClosure) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 n = 0;
            auto inc = [&n]() { n = n + 1; };
            inc();
            inc();
            if (n != 2) return 1;
            return 0;
        }
    )"), 0);
}

TEST_F(ErgonomicsE2E, AutoStoresNoCaptureLambda) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            auto twice = [](int32 x) { return x * 2; };
            if (twice(21) != 42) return 1;
            return 0;
        }
    )"), 0);
}

TEST_F(ErgonomicsE2E, AutoWithoutInitializerFails) {
    EXPECT_FALSE(compiles("int32 main() { auto x; return 0; }"));
}

TEST_F(ErgonomicsE2E, SequencePropertiesAndRange) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a[4] = {10, 20, 30, 40};
            if (a.len != 4 || a.len() != 4) return 1;
            string_view v = "abcd";
            if (v.len != 4 || v.len() != 4) return 2;
            if (v.char_count != 4 || v.char_count() != 4) return 3;
            string s = string.new("abcd");
            if (s.len != 4 || s.len() != 4) return 4;
            if (s.capacity < 4 || s.capacity() < 4) return 5;
            if (s.data == null || s.data[0] != 'a') return 6;
            if (v.data == null || v.data[0] != 'a') return 7;
            string_view mid = v[1:3];
            if (mid.len != 2 || mid[0] != 'b' || mid[1] != 'c') return 8;
            int32[] sl = a[1:3];
            if (sl.len != 2 || sl[0] != 20 || sl[1] != 30) return 9;
            string_view sm = s[1:3];
            if (sm.len != 2 || sm[0] != 'b' || sm[1] != 'c') return 10;
            return 0;
        }
    )"), 0);
}
