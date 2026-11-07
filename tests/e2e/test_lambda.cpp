// PAR-11/CG-11: lambda codegen e2e —— 无捕获=函数指针；有捕获=立即调用闭包。
#include <gtest/gtest.h>

#include "codegen/CodegenContext.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "driver/StdPrelude.h"
#include "sema/SemanticAnalyzer.h"
#include "support/Log.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"

class LambdaE2E : public ::testing::Test {
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
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit) return -1;
    if (auto e = (*jit)->addIRModule(
            llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext()))) {
        return -1;
    }
    auto sym = (*jit)->lookup("main");
    if (!sym) return -1;
    auto mainFn = sym->toPtr<int32_t(*)()>();
    return mainFn();
}

// 含 std prelude（qsort 等 libc 绑定经 std.c）。CLI 自动注入，JIT harness 需
// 手动前置（test_function_pointer 先例）。
static int runWithPrelude(const std::string& source, const std::string& filename) {
    auto ast = smc::parseStdCPrelude(source, filename);
    if (!ast) return -1;
    auto prelude = smc::parseStdCPrelude(smc::loadStdCPrelude({STD_DIR}), "<std.c>");
    if (!prelude) return -1;
    smc::prependDeclarations(*ast, *prelude);

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
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit) return -1;
    if (auto e = (*jit)->addIRModule(
            llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext()))) {
        return -1;
    }
    auto sym = (*jit)->lookup("main");
    if (!sym) return -1;
    auto mainFn = sym->toPtr<int32_t(*)()>();
    return mainFn();
}

TEST_F(LambdaE2E, NoCaptureAsFunctionPointer) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 (*fp)(int32) = [](int32 x) { return x * 2; };
            if (fp(21) != 42) return 1;
            return 0;
        }
    )", "lam_nocap.cmm"), 0);
}

TEST_F(LambdaE2E, NoCaptureImmediateInvocation) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 r = [](int32 x, int32 y) { return x + y; }(3, 4);
            if (r != 7) return 1;
            return 0;
        }
    )", "lam_immediate.cmm"), 0);
}

TEST_F(LambdaE2E, ByValueCaptureImmediate) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 3;
            int32 r = [a](int32 x) { return a + x; }(4);
            if (r != 7) return 1;
            return 0;
        }
    )", "lam_byval.cmm"), 0);
}

TEST_F(LambdaE2E, ByReferenceCaptureWrites) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 3;
            [&a]() { a = 9; }();
            if (a != 9) return 1;
            return 0;
        }
    )", "lam_byref.cmm"), 0);
}

TEST_F(LambdaE2E, CaptureAll) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 3; int32 b = 4;
            int32 r = [=]() { return a + b; }();
            if (r != 7) return 1;
            return 0;
        }
    )", "lam_captureall.cmm"), 0);
}

TEST_F(LambdaE2E, QsortComparator) {
    EXPECT_EQ(runWithPrelude(R"(
        int32 main() {
            int32 arr[5] = { 5, 2, 4, 1, 3 };
            // 无捕获 lambda 作 qsort 比较器（经函数指针实参传递）。
            int32 (*fp)(const void*, const void*) = [](const void* a, const void* b) {
                int32 x = *(const int32*)a;
                int32 y = *(const int32*)b;
                if (x < y) return -1;
                if (x > y) return 1;
                return 0;
            };
            qsort(arr, 5, 4, fp);
            if (arr[0] != 1 || arr[4] != 5) return 1;
            return 0;
        }
    )", "lam_qsort.cmm"), 0);
}

TEST_F(LambdaE2E, ReturnTypeInference) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 r = []() { return 1 + 2; }();
            if (r != 3) return 1;
            return 0;
        }
    )", "lam_infer.cmm"), 0);
}

TEST_F(LambdaE2E, CaptureAllByReferenceWrites) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 1; int32 b = 2;
            auto f = [&]() { a = 10; b = 20; };
            f();
            return (a == 10 && b == 20) ? 0 : 1;
        }
    )", "lam_refall.cmm"), 0);
}

TEST_F(LambdaE2E, MixedCaptureDefaultValueAndRefOverride) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 1; int32 b = 2;
            auto f = [=, &b]() -> int32 { b = 20; return a + b; };
            int32 r = f();
            return (r == 21 && a == 1 && b == 20) ? 0 : 1;
        }
    )", "lam_mixed1.cmm"), 0);
}

TEST_F(LambdaE2E, MixedCaptureDefaultRefAndValueOverride) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 1; int32 b = 2;
            auto f = [&, a]() -> int32 { b = 20; return a + b; };
            int32 r = f();
            return (r == 21 && a == 1 && b == 20) ? 0 : 1;
        }
    )", "lam_mixed2.cmm"), 0);
}

TEST_F(LambdaE2E, CaptureInitializerEvaluatedOnce) {
    EXPECT_EQ(runSource(R"(
        int32 calls = 0;
        int32 next() { calls = calls + 1; return calls; }
        int32 main() {
            int32 r = [x = next()]() -> int32 { return x; }();
            return (r == 1 && calls == 1) ? 0 : 1;
        }
    )", "lam_init_once.cmm"), 0);
}

TEST_F(LambdaE2E, CaptureInitializerByReference) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 n = 1;
            auto f = [&x = n]() -> int32 { x = x + 1; return x; };
            int32 r = f();
            return (r == 2 && n == 2) ? 0 : 1;
        }
    )", "lam_init_ref.cmm"), 0);
}

TEST_F(LambdaE2E, ThisImplicitMember) {
    EXPECT_EQ(runSource(R"(
        class LambdaThisC {
        public:
            int32 v;
            int32 f() {
                auto g = [this]() -> int32 { return v; };
                return g();
            }
            int32 h() {
                auto g = [=]() -> int32 { return v + 1; };
                return g();
            }
        };
        int32 main() { LambdaThisC c; c.v = 7; return (c.f() == 7 && c.h() == 8) ? 0 : 1; }
    )", "lam_this.cmm"), 0);
}

TEST_F(LambdaE2E, NestedLambdaCapturesOuter) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 x = 1;
            auto outer = [&x]() -> int32 {
                auto inner = [=]() -> int32 { return x + 1; };
                return inner();
            };
            return outer() == 2 ? 0 : 1;
        }
    )", "lam_nested.cmm"), 0);
}

TEST_F(LambdaE2E, ReturnInferenceSeesBodyLocal) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            auto f = []() {
                int32 y = 1;
                return y;
            };
            return f() == 1 ? 0 : 1;
        }
    )", "lam_retlocal.cmm"), 0);
}

TEST_F(LambdaE2E, CaptureInitializerArrayByValue) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[3] = { 1, 2, 3 };
            auto f = [t = arr]() -> int32 { return t[0] + t[2]; };
            arr[0] = 99;
            return f() == 4 ? 0 : 1;
        }
    )", "lam_init_array.cmm"), 0);
}

TEST_F(LambdaE2E, CaptureInitializerArrayImmediate) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[3] = { 1, 2, 3 };
            int32 r = [t = arr]() -> int32 { return t[0] + t[2]; }();
            return r == 4 ? 0 : 1;
        }
    )", "lam_init_array_imm.cmm"), 0);
}

TEST_F(LambdaE2E, CaptureInitializerMemberArrayByValue) {
    EXPECT_EQ(runSource(R"(
        class LambdaArrayHolder {
        public:
            int32 a[3];
        };
        int32 main() {
            LambdaArrayHolder p;
            p.a[0] = 1; p.a[1] = 2; p.a[2] = 3;
            auto f = [t = p.a]() -> int32 { return t[0] + t[2]; };
            p.a[0] = 99;
            return f() == 4 ? 0 : 1;
        }
    )", "lam_init_member_array.cmm"), 0);
}

// PAR-11 final review regressions.

TEST_F(LambdaE2E, InferredArrayReturnDecaysToPointer) {
    EXPECT_EQ(runSource(R"(
        int32 g_lambda_arr[3] = { 10, 20, 30 };
        int32 main() {
            auto f = [](int32 c) {
                if (c) { return g_lambda_arr; }
                return g_lambda_arr;
            };
            return f(1)[0] - 10;
        }
    )", "lam_ret_array.cmm"), 0);
}

TEST_F(LambdaE2E, CaptureAllBodyLocalShadowsByRef) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 x = 1;
            auto f = [&]() { int32 x = 5; x = 9; };
            f();
            return x - 1;
        }
    )", "lam_shadow_ref.cmm"), 0);
}

TEST_F(LambdaE2E, CaptureAllBodyLocalShadowsByValue) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 x = 1;
            auto f = [=]() -> int32 { int32 x = 5; return x; };
            return f() - 5;
        }
    )", "lam_shadow_val.cmm"), 0);
}

TEST_F(LambdaE2E, NestedInitCaptureDoesNotCaptureOuterName) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 z = 3;
            int32 (*fp)(int32) = [=](int32 x) -> int32 {
                auto inner = [z = 5]() -> int32 { return z; };
                return inner() + x;
            };
            return fp(1) + z - 9;
        }
    )", "lam_nested_init.cmm"), 0);
}

TEST_F(LambdaE2E, ImplicitThisInsideSwitch) {
    EXPECT_EQ(runSource(R"(
        class LambdaSwitchC {
        public:
            int32 v;
            int32 f() {
                auto g = [=]() -> int32 {
                    switch (v) {
                        case 7: return 70;
                        default: return v;
                    }
                };
                return g();
            }
        };
        int32 main() {
            LambdaSwitchC c;
            c.v = 7;
            return c.f() == 70 ? 0 : 1;
        }
    )", "lam_this_switch.cmm"), 0);
}

TEST_F(LambdaE2E, StaticLocalBeatsSameNamedField) {
    EXPECT_EQ(runSource(R"(
        class LambdaStaticShadowC {
        public:
            int32 x;
            int32 f() {
                static int32 x = 5;
                auto g = [=]() -> int32 { return x; };
                return g();
            }
        };
        int32 main() {
            LambdaStaticShadowC c;
            c.x = 99;
            return c.f() - 5;
        }
    )", "lam_static_shadow.cmm"), 0);
}

TEST_F(LambdaE2E, EnumeratorLosesToSameNamedField) {
    EXPECT_EQ(runSource(R"(
        enum LambdaColor { Red = 1, Green = 2 };
        class LambdaEnumShadowC {
        public:
            int32 Red;
            int32 f() {
                auto g = [this]() -> int32 { return Red; };
                return g();
            }
        };
        int32 main() {
            LambdaEnumShadowC c;
            c.Red = 7;
            return c.f() - 7;
        }
    )", "lam_enum_shadow.cmm"), 0);
}
