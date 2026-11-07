// 三元根因缺陷轮：TernaryExprAST 分支须取值（rvalue）再 phi。
// 旧行为：lvalue 分支把地址喂进 phi——标量截断、指针存变量地址、
// struct/Optional 垃圾。fixture 仿 test_optional_result.cpp。

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

class TernaryE2E : public ::testing::Test {
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
    if (auto e = (*jit)->addIRModule(std::move(ts))) {
        ADD_FAILURE() << "addIRModule failed";
        return -1;
    }

    auto sym = (*jit)->lookup("main");
    if (!sym) {
        ADD_FAILURE() << "lookup main failed";
        return -1;
    }

    auto fn = (int (*)())(intptr_t)sym->getValue();
    return fn();
}

TEST_F(TernaryE2E, ScalarLvalueBranchTrue) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 3; int32 b = 7; int32 t = 1;
            int32 x = t ? a : b;
            return x;
        }
    )", "ternary1.cmm"), 3);
}

TEST_F(TernaryE2E, ScalarLvalueBranchFalse) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 3; int32 b = 7; int32 t = 0;
            int32 x = t ? a : b;
            return x;
        }
    )", "ternary2.cmm"), 7);
}

TEST_F(TernaryE2E, PointerBranchSelectsTarget) {
    // 旧行为：r 拿到的是变量 p/q 的地址（*r = 5 写坏 p/q 而非 v1）。
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 v1 = 11; int32 v2 = 22;
            int32* p = &v1; int32* q = &v2;
            int32 t = 1;
            int32* r = p ? p : q;
            *r = 5;
            return v1;
        }
    )", "ternary3.cmm"), 5);
}

TEST_F(TernaryE2E, SameTypeStructBranch) {
    EXPECT_EQ(runSource(R"(
        struct P { int32 x; int32 y; };
        int32 main() {
            P a = {1, 2}; P b = {3, 4}; int32 t = 1;
            P c = t ? a : b;
            return c.x * 10 + c.y;
        }
    )", "ternary4.cmm"), 12);
}

TEST_F(TernaryE2E, OptionalBranch) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32? a = {true, 5}; int32? b = {false, 0}; int32 t = 1;
            int32? c = t ? a : b;
            if (c.valid) { return c.value; }
            return -1;
        }
    )", "ternary5.cmm"), 5);
}

TEST_F(TernaryE2E, ResultBranch) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            result<int32, int32> ok = {true, 9, 0};
            result<int32, int32> err = {false, 0, -3};
            int32 t = 0;
            result<int32, int32> c = t ? ok : err;
            if (c.ok) { return c.value; }
            return -c.error;
        }
    )", "ternary6.cmm"), 3);
}

TEST_F(TernaryE2E, MixedWidthPromotion) {
    // int32 分支 + float64 分支 → 常规算术转换统一到 float64。
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 3; float64 b = 0.5; int32 t = 1;
            float64 x = t ? a : b;
            return x == 3.0 ? 1 : 0;
        }
    )", "ternary7.cmm"), 1);
}

TEST_F(TernaryE2E, NestedTernary) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 1; int32 b = 2; int32 c = 3;
            int32 t = 1; int32 u = 0;
            return t ? (u ? a : b) : c;
        }
    )", "ternary8.cmm"), 2);
}

TEST_F(TernaryE2E, UnaryMinusOnStructMember) {
    // 解析优先级缺陷轮：后缀须紧于前缀一元——`-a.v` 曾解析成 `(-a).v`。
    EXPECT_EQ(runSource(R"(
        struct A { int32 v; int32 w; };
        int32 main() {
            A a = {5, 6};
            return -a.v + a.w;
        }
    )", "ternary9.cmm"), 1);
}

TEST_F(TernaryE2E, UnaryMinusOnSubscript) {
    // `-arr[i]` 曾解析成 `(-arr)[i]` 双重报错。
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[2] = {7, 9};
            int32 i = 1;
            return -arr[i];
        }
    )", "ternary10.cmm"), -9);
}

TEST_F(TernaryE2E, PrefixIncOnMember) {
    // `++a.v` 曾解析成 `(++a).v`。
    EXPECT_EQ(runSource(R"(
        struct A { int32 v; };
        int32 main() {
            A a = {5};
            return ++a.v;
        }
    )", "ternary11.cmm"), 6);
}
