#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

namespace fs = std::filesystem;

class EndToEndTest : public ::testing::Test {
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
    if (!ast) return -1;

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return -1;

    CodegenContext ctx;
    ctx.setSourceFile(filename);
    ast->codegen(ctx);
    ctx.finalizeDebugInfo();

    std::string ve;
    llvm::raw_string_ostream vs(ve);
    bool bad = llvm::verifyModule(ctx.getModule(), &vs);
    if (bad) return -1;

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) return -1;
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb))
        .create();
    if (!jit) return -1;

    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto e = (*jit)->addIRModule(std::move(ts))) return -1;

    auto sym = (*jit)->lookup("main");
    if (!sym) return -1;

    auto fn = (int (*)())(intptr_t)sym->getValue();
    return fn();
}

TEST_F(EndToEndTest, SimpleReturn42) {
    EXPECT_EQ(runSource("int32 main() { return 42; }", "simple.cmm"), 42);
}

TEST_F(EndToEndTest, LocalVar) {
    EXPECT_EQ(runSource("int32 main() { int32 a = 10; return a; }", "local.cmm"), 10);
}

TEST_F(EndToEndTest, TwoVars) {
    EXPECT_EQ(runSource(
        "int32 main() { int32 a = 10; int32 b = 20; return a + b; }", "two.cmm"), 30);
}

TEST_F(EndToEndTest, ExpressionArithmetic) {
    EXPECT_EQ(runSource(
        "int32 main() { int32 a = 10; int32 b = 20; int32 c = a + b * 3; return c; }", "expr.cmm"), 70);
}

TEST_F(EndToEndTest, ForLoop) {
    EXPECT_EQ(runSource(
        "int32 main() { int32 sum = 0; for (int32 i = 0; i <= 10; i = i + 1) { sum = sum + i; } return sum; }", "for.cmm"), 55);
}

TEST_F(EndToEndTest, SimpleFromFile) {
    std::ifstream ifs(fs::path(TEST_INPUT_DIR) / "simple.cmm");
    ASSERT_TRUE(ifs.is_open());
    std::stringstream ss; ss << ifs.rdbuf();
    EXPECT_EQ(runSource(ss.str(), "simple.cmm"), 42);
}

TEST_F(EndToEndTest, ExpressionFromFile) {
    std::ifstream ifs(fs::path(TEST_INPUT_DIR) / "expressions.cmm");
    ASSERT_TRUE(ifs.is_open());
    std::stringstream ss; ss << ifs.rdbuf();
    EXPECT_EQ(runSource(ss.str(), "expressions.cmm"), 70);
}

TEST_F(EndToEndTest, ControlFlowFromFile) {
    std::ifstream ifs(fs::path(TEST_INPUT_DIR) / "control_flow.cmm");
    ASSERT_TRUE(ifs.is_open());
    std::stringstream ss; ss << ifs.rdbuf();
    EXPECT_EQ(runSource(ss.str(), "control_flow.cmm"), 55);
}

TEST_F(EndToEndTest, DeferRunsAtScopeExitInReverseOrder) {
    EXPECT_EQ(runSource(R"(
        int32 g = 0;
        int32 add(int32 v) { g = g * 10 + v; return 0; }
        int32 main() {
            { defer add(1); defer add(2); defer add(3); }
            return g;
        }
    )", "defer_order.cmm"), 321);
}

TEST_F(EndToEndTest, DeferRunsOnReturn) {
    EXPECT_EQ(runSource(R"(
        int32 counter = 0;
        int32 tick() { counter = counter + 1; return 0; }
        int32 helper() { defer tick(); return 0; }
        int32 main() { helper(); return counter; }
    )", "defer_return.cmm"), 1);
}

TEST_F(EndToEndTest, DeferRunsOnBreak) {
    EXPECT_EQ(runSource(R"(
        int32 counter = 0;
        int32 tick() { counter = counter + 1; return 0; }
        int32 main() {
            int32 i = 0;
            while (i < 3) {
                defer tick();
                i = i + 1;
                if (i == 2) break;
            }
            return counter;
        }
    )", "defer_break.cmm"), 2);
}

TEST_F(EndToEndTest, DeferRunsOnContinue) {
    EXPECT_EQ(runSource(R"(
        int32 counter = 0;
        int32 tick() { counter = counter + 1; return 0; }
        int32 main() {
            for (int32 i = 0; i < 3; i = i + 1) {
                defer tick();
                continue;
            }
            return counter;
        }
    )", "defer_continue.cmm"), 3);
}

TEST_F(EndToEndTest, MultipleDeclarators) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 5, b = 3;
            int32 c = 1, d = 2, e = 4;
            return a + b + c + d + e;
        }
    )", "multi_decl.cmm"), 15);
}

TEST_F(EndToEndTest, GlobalMultipleDeclarators) {
    EXPECT_EQ(runSource(R"(
        int32 g1 = 1, g2 = 2;
        int32 main() { return g1 + g2; }
    )", "multi_global.cmm"), 3);
}

TEST_F(EndToEndTest, PointerDeclaratorOnlyAppliesToFirst) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 x = 7;
            int32* a = &x, b = 3;
            return *a + b;
        }
    )", "multi_ptr.cmm"), 10);
}

TEST_F(EndToEndTest, ArrayDecaysToPointer) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[3];
            arr[0] = 1; arr[1] = 2; arr[2] = 3;
            int32* p = arr;
            return p[2] + arr[0];
        }
    )", "array_decay.cmm"), 4);
}

TEST_F(EndToEndTest, ArrayArgumentDecaysToPointer) {
    EXPECT_EQ(runSource(R"(
        int32 sum(int32* p, int32 n) {
            int32 s = 0;
            for (int32 i = 0; i < n; i = i + 1) { s = s + p[i]; }
            return s;
        }
        int32 main() {
            int32 arr[3];
            arr[0] = 1; arr[1] = 2; arr[2] = 3;
            return sum(arr, 3);
        }
    )", "array_arg.cmm"), 6);
}

TEST_F(EndToEndTest, FunctionPointerCall) {
    EXPECT_EQ(runSource(R"(
        int32 mul(int32 a, int32 b) { return a * b; }
        int32 main() {
            int32 (*fp)(int32, int32) = mul;
            return fp(2, 3);
        }
    )", "func_ptr.cmm"), 6);
}

TEST_F(EndToEndTest, FunctionPointerReassignment) {
    EXPECT_EQ(runSource(R"(
        int32 add(int32 a, int32 b) { return a + b; }
        int32 mul(int32 a, int32 b) { return a * b; }
        int32 main() {
            int32 (*fp)(int32, int32) = add;
            int32 x = fp(2, 3);
            fp = mul;
            return x + fp(2, 3);
        }
    )", "func_ptr_reassign.cmm"), 11);
}

TEST_F(EndToEndTest, DereferenceIsAssignable) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 y = 5;
            int32* p = &y;
            *p = 9;
            return y;
        }
    )", "deref_assign.cmm"), 9);
}

TEST_F(EndToEndTest, DereferenceInExpression) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 y = 5;
            int32* p = &y;
            int32 a = *p;
            return a + *p;
        }
    )", "deref_expr.cmm"), 10);
}

TEST_F(EndToEndTest, PointerToPointerDereference) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 y = 5;
            int32* p = &y;
            int32** pp = &p;
            **pp = 11;
            return y;
        }
    )", "deref_double.cmm"), 11);
}

TEST_F(EndToEndTest, AssignAddressOfToPointer) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 y = 5;
            int32* q;
            q = &y;
            return *q;
        }
    )", "addr_assign.cmm"), 5);
}

TEST_F(EndToEndTest, StructInitializerList) {
    EXPECT_EQ(runSource(R"(
        struct Point { int32 x; int32 y; };
        int32 main() {
            struct Point p = {10, 20};
            return p.x + p.y;
        }
    )", "struct_init.cmm"), 30);
}

TEST_F(EndToEndTest, ArrayInitializerList) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[5] = {1, 2, 3, 4, 5};
            int32 s = 0;
            for (int32 i = 0; i < 5; i = i + 1) { s = s + arr[i]; }
            return s;
        }
    )", "array_init.cmm"), 15);
}

TEST_F(EndToEndTest, UnsizedArrayInitializerList) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[] = {1, 2, 3};
            return arr[0] + arr[1] + arr[2];
        }
    )", "unsized_array_init.cmm"), 6);
}

TEST_F(EndToEndTest, NestedAggregateInitializer) {
    EXPECT_EQ(runSource(R"(
        struct P { int32 x; int32 y; };
        struct R { struct P p; int32 z; };
        int32 main() {
            struct R r = {{1, 2}, 3};
            return r.p.x + r.p.y + r.z;
        }
    )", "nested_init.cmm"), 6);
}

TEST_F(EndToEndTest, GlobalAggregateInitializer) {
    EXPECT_EQ(runSource(R"(
        struct P { int32 x; int32 y; };
        struct P g = {4, 5};
        int32 main() { return g.x + g.y; }
    )", "global_init.cmm"), 9);
}

TEST_F(EndToEndTest, PartialAggregateInitializerZeroFills) {
    EXPECT_EQ(runSource(R"(
        struct P { int32 x; int32 y; };
        int32 main() {
            struct P p = {10};
            return p.x + p.y;
        }
    )", "partial_init.cmm"), 10);
}

TEST_F(EndToEndTest, PointerArithmeticInLoop) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[5] = {1, 2, 3, 4, 5};
            int32* p = arr;
            int32 s = 0;
            for (int32 i = 0; i < 5; i = i + 1) { s = s + *(p + i); }
            return s;
        }
    )", "ptr_arith.cmm"), 15);
}

TEST_F(EndToEndTest, PointerSubtraction) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[5] = {1, 2, 3, 4, 5};
            int32* p = arr;
            return *(p + 3) - *(p + 1);
        }
    )", "ptr_sub.cmm"), 2);
}

TEST_F(EndToEndTest, NegativeFloatLiteral) {
    EXPECT_EQ(runSource(
        "int32 main() { float64 d = -1.5; return (int32)(d * 10.0); }", "negfloat.cmm"), -15);
}

TEST_F(EndToEndTest, NegativeFloatLiteralFloat) {
    EXPECT_EQ(runSource(
        "int32 main() { float32 f = -2.0f; return (int32)f; }", "negfloatf.cmm"), -2);
}

TEST_F(EndToEndTest, PrefixIncrement) {
    EXPECT_EQ(runSource(
        "int32 main() { int32 a = 5; int32 k = ++a; return k * 10 + a; }", "preinc.cmm"), 66);
}

TEST_F(EndToEndTest, PrefixDecrement) {
    EXPECT_EQ(runSource(
        "int32 main() { int32 a = 5; int32 k = --a; return k * 10 + a; }", "predec.cmm"), 44);
}

TEST_F(EndToEndTest, PrefixIncrementThroughDeref) {
    EXPECT_EQ(runSource(
        "int32 main() { int32 y = 1; int32* p = &y; int32 k = ++*p; return k + y; }",
        "preinc_deref.cmm"), 4);
}

TEST_F(EndToEndTest, SwitchDispatch) {
    EXPECT_EQ(runSource(R"(
        int32 pick(int32 n) {
            switch (n) {
                case 1: return 10;
                case 2:
                case 3: return 20;
                default: return 30;
            }
        }
        int32 main() { return pick(2) + pick(9); }
    )", "switch.cmm"), 50);
}

TEST_F(EndToEndTest, SwitchBreakStopsFallthrough) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 r = 0;
            switch (1) {
                case 1: r = r + 1; break;
                case 2: r = r + 100; break;
            }
            return r;
        }
    )", "switch_break.cmm"), 1);
}

TEST_F(EndToEndTest, PrintFormatsIntegers) {
    testing::internal::CaptureStdout();
    EXPECT_EQ(runSource(
        "int32 main() { print(\"{} + {} = {}\", 1, 2, 3); return 0; }", "print_ints.cmm"), 0);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "1 + 2 = 3");
}

TEST_F(EndToEndTest, PrintlnAppendsNewline) {
    testing::internal::CaptureStdout();
    EXPECT_EQ(runSource("int32 main() { println(\"hi\"); return 0; }", "println.cmm"), 0);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "hi\n");
}

TEST_F(EndToEndTest, PrintFormatsMixedTypes) {
    testing::internal::CaptureStdout();
    EXPECT_EQ(runSource(
        "int32 main() { print(\"{} {} {} {} {}\", 42, 1.5, 'A', \"str\", 7); return 0; }",
        "print_mixed.cmm"), 0);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "42 1.5 A str 7");
}

// TYP-04: float16/float128 accept the full print specifier set; values are
// promoted to double (float16 losslessly, float128 truncated to double).
TEST_F(EndToEndTest, PrintFormatsHalfAndQuad) {
    testing::internal::CaptureStdout();
    EXPECT_EQ(runSource(
        R"(int32 main() { print("{} {} {:f} {:.2f} {:e}", 1.5f16, 2.5f128, 1.5f16, 1.5f128, 1.5f16); return 0; })",
        "print_f16_f128.cmm"), 0);
    EXPECT_EQ(testing::internal::GetCapturedStdout(),
              "1.5 2.5 1.500000 1.50 1.500000e+00");
}

// TYP-04: float16 varargs follow C23 default argument promotions (-> double).
TEST_F(EndToEndTest, Float16VarargPromotesToDouble) {
    testing::internal::CaptureStdout();
    EXPECT_EQ(runSource(
        R"(extern int32 printf(const char* format, ...);
int32 main() { printf("%f\n", 1.5f16); return 0; })",
        "vararg_f16.cmm"), 0);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "1.500000\n");
}

TEST_F(EndToEndTest, PrintFormatsBool) {
    testing::internal::CaptureStdout();
    EXPECT_EQ(runSource(
        "int32 main() { bool t = 1; bool f = 0; print(\"{} {}\", t, f); return 0; }",
        "print_bool.cmm"), 0);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "true false");
}

TEST_F(EndToEndTest, PrintEscapesLiteralPercent) {
    testing::internal::CaptureStdout();
    EXPECT_EQ(runSource("int32 main() { print(\"{}%\", 50); return 0; }", "print_pct.cmm"), 0);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "50%");
}

TEST_F(EndToEndTest, PrintUsesFreeFunctionToString) {
    testing::internal::CaptureStdout();
    EXPECT_EQ(runSource(
        "struct P { int32 x; int32 y; };"
        "const char* to_string(struct P p) { return \"Point\"; }"
        "int32 main() { struct P p; print(\"<{}>\", p); return 0; }",
        "print_to_string.cmm"), 0);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "<Point>");
}

TEST_F(EndToEndTest, PrintUsesMethodToString) {
    testing::internal::CaptureStdout();
    EXPECT_EQ(runSource(
        "class C { public: int32 x; public: const char* to_string() { return \"C!\"; } };"
        "int32 main() { C c; print(\"{}\", c); return 0; }",
        "print_method_to_string.cmm"), 0);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "C!");
}

TEST_F(EndToEndTest, VariableCopyInitializationLoadsValue) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 5;
            int32 b = a;
            int32 c = b;
            return a + b + c;
        }
    )", "copy_init.cmm"), 15);
}

TEST_F(EndToEndTest, UnionMemberAccess) {
    EXPECT_EQ(runSource(R"(
        union Data { int32 i; char str[20]; };
        int32 main() {
            union Data d;
            d.i = 65;
            int32 a = d.i;
            char c = d.str[0];
            int32 s = sizeof(union Data);
            return a + c + s;
        }
    )", "union_access.cmm"), 150);
}

TEST_F(EndToEndTest, UnionSizeIsLargestMember) {
    EXPECT_EQ(runSource(R"(
        union Data { int32 i; float32 f; char str[20]; };
        int32 main() { return sizeof(union Data); }
    )", "union_size.cmm"), 20);
}

TEST_F(EndToEndTest, UnionPointerMemberAccess) {
    EXPECT_EQ(runSource(R"(
        union Data { int32 i; float32 f; };
        int32 main() {
            union Data d;
            d.i = 3;
            union Data* p = &d;
            p->i = 7;
            return p->i + d.i;
        }
    )", "union_ptr.cmm"), 14);
}

TEST_F(EndToEndTest, StructArrayMember) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 n; char name[3]; };
        int32 main() {
            struct S s;
            s.n = 5;
            s.name[0] = 1;
            s.name[1] = 2;
            s.name[2] = 3;
            return s.n + s.name[0] + s.name[1] + s.name[2];
        }
    )", "struct_array_member.cmm"), 11);
}

TEST_F(EndToEndTest, PrototypeThenDefinition) {
    EXPECT_EQ(runSource(R"(
        int32 add(int32 a, int32 b);
        int32 add(int32 a, int32 b) { return a + b; }
        int32 main() { return add(2, 3); }
    )", "prototype.cmm"), 5);
}

TEST_F(EndToEndTest, SizeofTypeAndExpr) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 x = 10;
            int32 a = sizeof(int32);
            int32 b = sizeof(x);
            int32 c = sizeof(x + 1);
            return a + b + c;
        }
    )", "sizeof.cmm"), 12);
}

TEST_F(EndToEndTest, BitwiseNot) {
    EXPECT_EQ(runSource(
        "int32 main() { int32 a = 5; return ~a; }", "bitnot.cmm"), -6);
}

TEST_F(EndToEndTest, ShiftOperators) {
    EXPECT_EQ(runSource(
        "int32 main() { int32 a = 1 << 4; int32 b = 32 >> 2; return a + b; }", "shift.cmm"), 24);
}
