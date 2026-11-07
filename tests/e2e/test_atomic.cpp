// CON-01: atomic type qualifier — integer scalars, atomicrmw subset.
// end-to-end execution tests. fixture 仿 test_string.cpp 的 runSource。

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

#include <sstream>

class AtomicE2E : public ::testing::Test {
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

static std::string compileToIR(const std::string& source, const std::string& filename) {
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) {
        ADD_FAILURE() << "Parse failed";
        return {};
    }
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) {
        for (auto& err : analyzer.getErrors()) {
            ADD_FAILURE() << "Semantic error: " << err.format();
        }
        return {};
    }
    CodegenContext ctx;
    ctx.setSourceFile(filename);
    ast->codegen(ctx);
    std::string out;
    llvm::raw_string_ostream os(out);
    ctx.getModule().print(os, nullptr);
    return os.str();
}

static std::vector<std::string> semanticErrors(const std::string& source,
                                               const std::string& filename) {
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    std::vector<std::string> errors;
    if (!ast) {
        errors.push_back("parse failed");
        return errors;
    }
    for (auto& e : parser.getErrors()) errors.push_back(e.format());
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    for (auto& e : analyzer.getErrors()) errors.push_back(e.format());
    return errors;
}

static int runSource(const std::string& source, const std::string& filename) {
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) {
        ADD_FAILURE() << "Parse failed";
        return -1;
    }

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) {
        for (auto& err : analyzer.getErrors()) {
            ADD_FAILURE() << "Semantic error: " << err.format();
        }
        return -1;
    }

    CodegenContext ctx;
    ctx.setSourceFile(filename);
    ast->codegen(ctx);
    ctx.finalizeDebugInfo();

    std::string ve;
    llvm::raw_string_ostream vs(ve);
    bool bad = llvm::verifyModule(ctx.getModule(), &vs);
    if (bad) {
        vs.flush();
        ADD_FAILURE() << "Module verification failed: " << ve;
        return -1;
    }

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        ADD_FAILURE() << "JITTargetMachineBuilder::detectHost failed";
        return -1;
    }
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb))
        .create();
    if (!jit) {
        ADD_FAILURE() << "LLJITBuilder::create failed";
        return -1;
    }

    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto err = (*jit)->addIRModule(std::move(ts))) {
        ADD_FAILURE() << "addIRModule failed";
        return -1;
    }

    auto sym = (*jit)->lookup("main");
    if (!sym) {
        ADD_FAILURE() << "main not found";
        return -1;
    }
    return ((int (*)())(intptr_t)sym->getValue())();
}

TEST_F(AtomicE2E, AtomicLoadStoreWorks) {
    EXPECT_EQ(runSource(R"(
        atomic int32 a = 0;
        int32 main() {
            a = a + 1;
            a = a + 1;
            return a == 2 ? 0 : 1;
        }
    )", "atomic_ls.cmm"), 0);
}

TEST_F(AtomicE2E, AtomicIncrement) {
    EXPECT_EQ(runSource(R"(
        atomic int32 c = 0;
        int32 main() {
            c++;
            ++c;
            return c == 2 ? 0 : 1;
        }
    )", "atomic_inc.cmm"), 0);
}

TEST_F(AtomicE2E, AtomicCompoundAssign) {
    EXPECT_EQ(runSource(R"(
        atomic int32 v = 7;
        int32 main() {
            v += 3;
            v -= 2;
            v &= 15;
            v |= 32;
            v ^= 1;
            return v == (7+3-2)&15|32^1 ? 0 : 1;
        }
    )", "atomic_ca.cmm"), 0);
}

TEST_F(AtomicE2E, AtomicPointerRejected) {
    auto ast = [&]() {
        Lexer lexer("at_ptr.cmm", R"(
            atomic int32* p;
            int32 main() { return 0; }
        )");
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        return parser.parse();
    }();
    if (!ast) {
        ADD_FAILURE() << "Parse failed";
        return;
    }
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    EXPECT_NE(analyzer.getErrors()[0].message.find("atomic"),
              std::string::npos);
}

TEST_F(AtomicE2E, AtomicMultiplyRejected) {
    auto ast = [&]() {
        Lexer lexer("at_mul.cmm", R"(
            atomic int32 a = 2;
            int32 main() { a *= 3; return 0; }
        )");
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        return parser.parse();
    }();
    if (!ast) {
        ADD_FAILURE() << "Parse failed";
        return;
    }
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    EXPECT_NE(analyzer.getErrors()[0].message.find("atomic"),
              std::string::npos);
}

// C1 (review): `atomic` on an alias base strips to the underlying scalar and
// works (must not be silently dropped nor crash).
TEST_F(AtomicE2E, AtomicAliasBaseWorks) {
    EXPECT_EQ(runSource(R"(
        using myint = int32;
        atomic myint x = 5;
        int32 main() {
            x = x + 1;
            return x == 6 ? 0 : 1;
        }
    )", "at_using.cmm"), 0);
}

// I2 (review): `atomic` works in a function-local declaration too (was
// silently dropped because isTypeStart didn't recognize the qualifier).
TEST_F(AtomicE2E, AtomicLocalDeclarationWorks) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            atomic int32 a = 1;
            a = a + 1;
            return a == 2 ? 0 : 1;
        }
    )", "at_local.cmm"), 0);
}

// C2 (review): `atomic` on an aggregate is rejected cleanly (parse-time), not
// a crash.
TEST_F(AtomicE2E, AtomicAggregateRejectedCleanly) {
    auto tokens = [&]() {
        Lexer lexer("at_agg.cmm", R"(
            struct S { int32 a; };
            atomic struct S f;
            int32 main() { return 0; }
        )");
        return lexer.tokenize();
    }();
    Parser parser(tokens);
    parser.parse();
    bool parserError = !parser.getErrors().empty();
    EXPECT_TRUE(parserError);
    if (parserError) {
        EXPECT_NE(parser.getErrors()[0].message.find("atomic"),
                  std::string::npos);
    }
}

TEST_F(AtomicE2E, AtomicTemplateFieldWorks) {
    EXPECT_EQ(runSource(R"(
        template<typename T> struct Box { atomic T v; };
        int32 main() {
            Box<int32> b;
            b.v = 5;
            b.v += 2;
            return b.v == 7 ? 0 : 1;
        }
    )", "at_tmpl.cmm"), 0);
}

TEST_F(AtomicE2E, AtomicBoolUsesByteAtomic) {
    std::string ir = compileToIR(R"(
        atomic bool flag = false;
        int32 main() { flag = true; return flag ? 0 : 1; }
    )", "at_bool_ir.cmm");
    EXPECT_NE(ir.find("atomic i8"), std::string::npos);
    EXPECT_EQ(ir.find("atomic i1"), std::string::npos);
}

TEST_F(AtomicE2E, AtomicInt64FieldUsesNaturalAlignment) {
    std::string ir = compileToIR(R"(
        struct Cell { atomic int64 v; };
        int64 main() { Cell c; c.v = 11; return c.v - 11; }
    )", "at_align_ir.cmm");
    std::istringstream lines(ir);
    std::string line;
    int atomicLines = 0;
    while (std::getline(lines, line)) {
        if (line.find("atomic i64") == std::string::npos) continue;
        ++atomicLines;
        EXPECT_NE(line.find("align 8"), std::string::npos) << line;
    }
    EXPECT_GT(atomicLines, 0);
    EXPECT_EQ(ir.find("__atomic_load"), std::string::npos);
    EXPECT_EQ(ir.find("__atomic_store"), std::string::npos);
}

TEST_F(AtomicE2E, AtomicBuiltinLoadStore) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 x = 0;
            __smc_atomic_store(&x, 7, 5);
            return __smc_atomic_load(&x, 5) == 7 ? 0 : 1;
        }
    )", "at_builtin_ls.cmm"), 0);
}

TEST_F(AtomicE2E, AtomicBuiltinCASAndFetch) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 x = 3;
            int32 expected = 3;
            if (!__smc_atomic_compare_exchange_strong(&x, &expected, 9, 5, 5)) return 1;
            if (__smc_atomic_fetch_add(&x, 2, 5) != 9) return 2;
            return __smc_atomic_load(&x, 5) == 11 ? 0 : 3;
        }
    )", "at_builtin_cas.cmm"), 0);
}

TEST_F(AtomicE2E, AtomicBuiltinPointerAndFloat) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 x = 1;
            int32 y = 2;
            int32* p = &x;
            int32* expected = &x;
            if (!__smc_atomic_compare_exchange_strong(&p, &expected, &y, 5, 5)) return 1;
            float32 f = 1.5f;
            float32 old = __smc_atomic_exchange(&f, 2.5f, 5);
            if (old != 1.5f || f != 2.5f) return 2;
            return p == &y ? 0 : 3;
        }
    )", "at_builtin_ptr_float.cmm"), 0);
}

TEST_F(AtomicE2E, AtomicBuiltinAllOpsAndFences) {
    EXPECT_EQ(runSource(R"(
        enum Color : int32 { Red = 1, Blue = 2 };
        int32 main() {
            int32 x = 10;
            if (__smc_atomic_fetch_sub(&x, 3, 5) != 10) return 1;
            if (__smc_atomic_fetch_and(&x, 6, 5) != 7) return 2;
            if (__smc_atomic_fetch_or(&x, 8, 5) != 6) return 3;
            if (__smc_atomic_fetch_xor(&x, 1, 5) != 14) return 4;
            if (__smc_atomic_exchange(&x, 1, 5) != 15) return 5;

            bool b = false;
            bool be = false;
            if (!__smc_atomic_compare_exchange_strong(&b, &be, true, 5, 5)) return 6;
            if (!b) return 7;

            Color c = Red;
            Color ce = Red;
            if (!__smc_atomic_compare_exchange_strong(&c, &ce, Blue, 5, 5)) return 8;
            if (c != Blue) return 9;

            __smc_atomic_thread_fence(5);
            __smc_atomic_signal_fence(5);
            return 0;
        }
    )", "at_builtin_all.cmm"), 0);
}

TEST_F(AtomicE2E, AtomicBuiltinRejectsBadTypesAndOrders) {
    auto expectSemaError = [](const std::string& src) {
        Lexer lexer("at_bad.cmm", src);
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        auto ast = parser.parse();
        ASSERT_NE(ast, nullptr);
        SemanticAnalyzer analyzer;
        analyzer.analyze(*ast);
        ASSERT_FALSE(analyzer.getErrors().empty());
        bool meaningful = false;
        for (const auto& err : analyzer.getErrors()) {
            if (err.message.find("undeclared function") == std::string::npos) {
                meaningful = true;
            }
        }
        EXPECT_TRUE(meaningful);
    };
    expectSemaError(R"(
        float32 f = 1.0f;
        int32 main() { return (int32)__smc_atomic_fetch_and(&f, 1.0f, 5); }
    )");
    expectSemaError(R"(
        int32 x = 0;
        int32 main() { return __smc_atomic_load(&x, 3); }
    )");
    expectSemaError(R"(
        int32 x = 0;
        int32 main() { __smc_atomic_store(&x, 1, 2); return 0; }
    )");
    expectSemaError(R"(
        int32 x = 0;
        int32 expected = 0;
        int32 main() {
            return __smc_atomic_compare_exchange_strong(&x, &expected, 1, 5, 3);
        }
    )");
}

TEST_F(AtomicE2E, AtomicInt128Rejected) {
    auto errors = semanticErrors(R"(
        atomic int128 x;
        int32 main() { return 0; }
    )", "at_int128.cmm");
    ASSERT_FALSE(errors.empty());
    bool mentionsLimit = false;
    for (const auto& e : errors) {
        if (e.find("1/2/4/8") != std::string::npos) mentionsLimit = true;
    }
    EXPECT_TRUE(mentionsLimit);
}

TEST_F(AtomicE2E, AtomicTemplateUnsupportedRejected) {
    auto expectError = [](const std::string& src) {
        auto errors = semanticErrors(src, "at_tmpl_bad.cmm");
        ASSERT_FALSE(errors.empty()) << src;
        bool mentionsLimit = false;
        for (const auto& e : errors) {
            if (e.find("1/2/4/8") != std::string::npos) mentionsLimit = true;
        }
        EXPECT_TRUE(mentionsLimit) << src;
    };
    expectError(R"(
        template<typename T> struct S { atomic T v; };
        int32 main() { S<int32*> s; return 0; }
    )");
    expectError(R"(
        template<typename T> struct S { atomic T v; };
        struct X { int32 a; };
        int32 main() { S<X> s; return 0; }
    )");
    expectError(R"(
        template<typename T> struct S { atomic T v; };
        enum E : int32 { A = 1 };
        int32 main() { S<E> s; return 0; }
    )");
    expectError(R"(
        template<typename T> struct S { atomic T v; };
        int32 main() { S<float32> s; return 0; }
    )");
    expectError(R"(
        template<typename T> struct S { atomic T v; };
        int32 main() { S<int128> s; return 0; }
    )");
}

TEST_F(AtomicE2E, AtomicUnionAndTypedefFieldsRejected) {
    auto expectError = [](const std::string& src) {
        auto errors = semanticErrors(src, "at_union_alias.cmm");
        ASSERT_FALSE(errors.empty()) << src;
        bool mentionsLimit = false;
        for (const auto& e : errors) {
            if (e.find("1/2/4/8") != std::string::npos) mentionsLimit = true;
        }
        EXPECT_TRUE(mentionsLimit) << src;
    };
    expectError(R"(
        union U { atomic int128 x; };
        int32 main() { return 0; }
    )");
    expectError(R"(
        union U { atomic float32 x; };
        int32 main() { return 0; }
    )");
    expectError(R"(
        using A = atomic int128;
        struct S { A x; };
        int32 main() { return 0; }
    )");
    expectError(R"(
        using A = atomic float32;
        struct S { A x; };
        int32 main() { return 0; }
    )");
}
