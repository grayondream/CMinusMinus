// PAR-11/CG-11: lambda codegen e2e —— 无捕获=函数指针；有捕获=立即调用闭包。
#include <gtest/gtest.h>

#include <string>
#include <vector>

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

static std::vector<std::string> compileErrors(const std::string& source,
                                              const std::string& filename) {
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    std::vector<std::string> errors;
    for (auto& d : lexer.getDiagnostics()) errors.push_back(d.format());
    Parser parser(tokens);
    auto ast = parser.parse();
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

// 含 std prelude（qsort 等 libc 绑定经 std.c）。CLI 自动注入，JIT harness 需
// 手动前置（test_function_pointer 先例）。
static int runWithPrelude(const std::string& source, const std::string& filename) {
    auto ast = cdd::parseStdCPrelude(source, filename);
    if (!ast) return -1;
    auto prelude = cdd::parseStdCPrelude(cdd::loadStdCPrelude({STD_DIR}), "<std.c>");
    if (!prelude) return -1;
    cdd::prependDeclarations(*ast, *prelude);

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
    )", "lam_nocap.cdd"), 0);
}

TEST_F(LambdaE2E, NoCaptureImmediateInvocation) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 r = [](int32 x, int32 y) { return x + y; }(3, 4);
            if (r != 7) return 1;
            return 0;
        }
    )", "lam_immediate.cdd"), 0);
}

TEST_F(LambdaE2E, ByValueCaptureImmediate) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 3;
            int32 r = [a](int32 x) { return a + x; }(4);
            if (r != 7) return 1;
            return 0;
        }
    )", "lam_byval.cdd"), 0);
}

TEST_F(LambdaE2E, ByReferenceCaptureWrites) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 3;
            [&a]() { a = 9; }();
            if (a != 9) return 1;
            return 0;
        }
    )", "lam_byref.cdd"), 0);
}

TEST_F(LambdaE2E, CaptureAll) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 3; int32 b = 4;
            int32 r = [=]() { return a + b; }();
            if (r != 7) return 1;
            return 0;
        }
    )", "lam_captureall.cdd"), 0);
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
    )", "lam_qsort.cdd"), 0);
}

TEST_F(LambdaE2E, ReturnTypeInference) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 r = []() { return 1 + 2; }();
            if (r != 3) return 1;
            return 0;
        }
    )", "lam_infer.cdd"), 0);
}

TEST_F(LambdaE2E, CaptureAllByReferenceWrites) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 1; int32 b = 2;
            auto f = [&]() { a = 10; b = 20; };
            f();
            return (a == 10 && b == 20) ? 0 : 1;
        }
    )", "lam_refall.cdd"), 0);
}

TEST_F(LambdaE2E, MixedCaptureDefaultValueAndRefOverride) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 1; int32 b = 2;
            auto f = [=, &b]() -> int32 { b = 20; return a + b; };
            int32 r = f();
            return (r == 21 && a == 1 && b == 20) ? 0 : 1;
        }
    )", "lam_mixed1.cdd"), 0);
}

TEST_F(LambdaE2E, MixedCaptureDefaultRefAndValueOverride) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 1; int32 b = 2;
            auto f = [&, a]() -> int32 { b = 20; return a + b; };
            int32 r = f();
            return (r == 21 && a == 1 && b == 20) ? 0 : 1;
        }
    )", "lam_mixed2.cdd"), 0);
}

TEST_F(LambdaE2E, CaptureInitializerEvaluatedOnce) {
    EXPECT_EQ(runSource(R"(
        int32 calls = 0;
        int32 next() { calls = calls + 1; return calls; }
        int32 main() {
            int32 r = [x = next()]() -> int32 { return x; }();
            return (r == 1 && calls == 1) ? 0 : 1;
        }
    )", "lam_init_once.cdd"), 0);
}

TEST_F(LambdaE2E, CaptureInitializerByReference) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 n = 1;
            auto f = [&x = n]() -> int32 { x = x + 1; return x; };
            int32 r = f();
            return (r == 2 && n == 2) ? 0 : 1;
        }
    )", "lam_init_ref.cdd"), 0);
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
    )", "lam_this.cdd"), 0);
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
    )", "lam_nested.cdd"), 0);
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
    )", "lam_retlocal.cdd"), 0);
}

TEST_F(LambdaE2E, CaptureInitializerArrayByValue) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[3] = { 1, 2, 3 };
            auto f = [t = arr]() -> int32 { return t[0] + t[2]; };
            arr[0] = 99;
            return f() == 4 ? 0 : 1;
        }
    )", "lam_init_array.cdd"), 0);
}

TEST_F(LambdaE2E, CaptureInitializerArrayImmediate) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[3] = { 1, 2, 3 };
            int32 r = [t = arr]() -> int32 { return t[0] + t[2]; }();
            return r == 4 ? 0 : 1;
        }
    )", "lam_init_array_imm.cdd"), 0);
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
    )", "lam_init_member_array.cdd"), 0);
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
    )", "lam_ret_array.cdd"), 0);
}

TEST_F(LambdaE2E, CaptureAllBodyLocalShadowsByRef) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 x = 1;
            auto f = [&]() { int32 x = 5; x = 9; };
            f();
            return x - 1;
        }
    )", "lam_shadow_ref.cdd"), 0);
}

TEST_F(LambdaE2E, CaptureAllBodyLocalShadowsByValue) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 x = 1;
            auto f = [=]() -> int32 { int32 x = 5; return x; };
            return f() - 5;
        }
    )", "lam_shadow_val.cdd"), 0);
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
    )", "lam_nested_init.cdd"), 0);
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
    )", "lam_this_switch.cdd"), 0);
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
    )", "lam_static_shadow.cdd"), 0);
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
    )", "lam_enum_shadow.cdd"), 0);
}

TEST_F(LambdaE2E, AutoReturnScalar) {
    EXPECT_EQ(runSource(R"(
        auto twice(int32 x) { return x * 2; }
        int32 main() { auto y = twice(21); return y - 42; }
    )", "auto_ret_scalar.cdd"), 0);
}

TEST_F(LambdaE2E, AutoReturnVoid) {
    EXPECT_EQ(runSource(R"(
        auto noop() { return; }
        int32 main() { noop(); return 0; }
    )", "auto_ret_void.cdd"), 0);
}

TEST_F(LambdaE2E, AutoReturnInconsistentTypesRejected) {
    auto errs = compileErrors(R"(
        auto bad(int32 x) {
            if (x > 0) return x;
            return "no";
        }
        int32 main() { return 0; }
    )", "auto_ret_mismatch.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "inconsistent return types"));
}

TEST_F(LambdaE2E, AutoReturnBareAfterValueRejected) {
    auto errs = compileErrors(R"(
        auto bad(int32 x) {
            if (x > 0) return x;
            return;
        }
        int32 main() { return 0; }
    )", "auto_ret_bare.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "must return a value"));
}

TEST_F(LambdaE2E, AutoReturnPrototypeRejected) {
    auto errs = compileErrors(R"(
        auto f(int32 x);
        int32 main() { return 0; }
    )", "auto_ret_proto.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "must have a body"));
}

TEST_F(LambdaE2E, AutoReturnMethodRejected) {
    auto errs = compileErrors(R"(
        class C { public: auto f() { return 1; } };
        int32 main() { return 0; }
    )", "auto_ret_method.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "not supported on methods"));
}

TEST_F(LambdaE2E, AutoReturnCompileTimeRejected) {
    auto errs = compileErrors(R"(
        compile_time auto f() { return 1; }
        int32 main() { return 0; }
    )", "auto_ret_ct.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "compile_time"));
}

TEST_F(LambdaE2E, ByReferenceClosureReturnRejected) {
    auto errs = compileErrors(R"(
        auto bad() {
            int32 x = 7;
            return [&x]() -> int32 { return x; };
        }
        int32 main() { return 0; }
    )", "auto_ret_ref.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "by-reference captures cannot escape by return"));
}

TEST_F(LambdaE2E, IndirectByReferenceClosureReturnRejected) {
    auto errs = compileErrors(R"(
        auto bad() {
            int32 x = 7;
            auto f = [&x]() -> int32 { return x; };
            return f;
        }
        int32 main() { return 0; }
    )", "auto_ret_ref_indirect.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "by-reference captures cannot escape by return"));
}

TEST_F(LambdaE2E, ByReferenceClosureLocalCallStillAllowed) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 x = 7;
            auto f = [&x]() -> int32 { return x; };
            return f() - 7;
        }
    )", "lambda_ref_local_ok.cdd"), 0);
}

TEST_F(LambdaE2E, ByValueClosureReturn) {
    EXPECT_EQ(runSource(R"(
        auto make_adder(int32 x) {
            return [x](int32 y) -> int32 { return x + y; };
        }
        int32 main() {
            auto f = make_adder(40);
            return f(2) - 42;
        }
    )", "auto_ret_closure.cdd"), 0);
}

TEST_F(LambdaE2E, ReturnedClosureIsIndependentCopy) {
    EXPECT_EQ(runSource(R"(
        auto make_adder(int32 x) {
            return [x](int32 y) -> int32 { return x + y; };
        }
        int32 main() {
            auto add40 = make_adder(40);
            auto add2 = make_adder(2);
            return add40(2) + add2(2) - 46;
        }
    )", "auto_ret_closure_copy.cdd"), 0);
}

TEST_F(LambdaE2E, ReturnedClosureCalledRepeatedly) {
    EXPECT_EQ(runSource(R"(
        auto make_counter(int32 start) {
            return [start]() -> int32 { return start + 1; };
        }
        int32 main() {
            auto f = make_counter(10);
            if (f() != 11) return 1;
            if (f() != 11) return 2;
            return 0;
        }
    )", "auto_ret_closure_repeat.cdd"), 0);
}

TEST_F(LambdaE2E, ReturnedClosureFromBranch) {
    EXPECT_EQ(runSource(R"(
        auto make(int32 x) {
            auto f = [x](int32 y) -> int32 { return x + y; };
            if (x > 0) return f;
            return f;
        }
        int32 main() {
            auto plus = make(10);
            auto minus = make(-10);
            return plus(3) + minus(3) - 6;
        }
    )", "auto_ret_closure_branch.cdd"), 0);
}

TEST_F(LambdaE2E, AutoReturnCompoundLiteralRejected) {
    auto errs = compileErrors(R"(
        auto make() { return (int32[2]){1, 2}; }
        int32 main() { return 0; }
    )", "auto_ret_compound_literal.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "cannot return a compound literal"));
}

TEST_F(LambdaE2E, AutoReturnSliceLiteralRejected) {
    auto errs = compileErrors(R"(
        auto make() {
            int32[] s = {1, 2};
            return s;
        }
        int32 main() { return 0; }
    )", "auto_ret_slice_literal.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "cannot return a slice backed by a stack literal"));
}

TEST_F(LambdaE2E, NestedByReferenceClosureReturnRejected) {
    auto errs = compileErrors(R"(
        auto bad() {
            int32 y = 7;
            auto inner = [&y]() -> int32 { return y; };
            auto outer = [inner]() -> int32 { return inner(); };
            return outer;
        }
        int32 main() { return 0; }
    )", "auto_ret_nested_ref.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "by-reference captures cannot escape by return"));
}

TEST_F(LambdaE2E, AutoReturnOutOfClassStaticRejected) {
    auto errs = compileErrors(R"(
        class C { public: static int32 f(); };
        auto C::f() { return 1; }
        int32 main() { return 0; }
    )", "auto_ret_ooc_static.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "not supported on methods"));
}

TEST_F(LambdaE2E, AutoReturnCallBeforeDefinitionRejected) {
    auto errs = compileErrors(R"(
        auto early(int32 x) { return later(x); }
        auto later(int32 x) { return x; }
        int32 main() { return 0; }
    )", "auto_ret_order.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "use of undeclared function 'later'"));
}

TEST_F(LambdaE2E, AutoReturnRecursiveRejected) {
    auto errs = compileErrors(R"(
        auto fac(int32 n) {
            if (n <= 1) return 1;
            return n * fac(n - 1);
        }
        int32 main() { return 0; }
    )", "auto_ret_recursive.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "use of undeclared function 'fac'"));
    EXPECT_FALSE(hasErrorContaining(errs, "<unknown>"));
}

TEST_F(LambdaE2E, AutoReturnBareBeforeValueRejected) {
    auto errs = compileErrors(R"(
        auto bad(int32 x) {
            if (x > 0) return;
            return x;
        }
        int32 main() { return 0; }
    )", "auto_ret_bare_before_value.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "must return a value"));
}

TEST_F(LambdaE2E, AutoReturnMethodPrototypeDiagnostic) {
    auto errs = compileErrors(R"(
        class C { public: auto f(); };
        int32 main() { return 0; }
    )", "auto_ret_method_proto.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "not supported on methods"));
}
