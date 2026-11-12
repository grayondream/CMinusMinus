// 演示文件 examples/all_features.cdd 建设期间发现并修复的编译器 bug 回归测试：
//  - Bug A: CompileTimeEvaluator::toLLVMType 对 Class 用 static_cast<StructType*>
//    读 isPacked（跨类 UB，读垃圾值）→ 布局损坏，size_of(派生类) 非确定性
//    "Invalid size request on a scalable vector" 崩溃。
//  - Bug B: 无后缀整型字面量恒 int32 → int64 v = 4000000000 以有符号 int32 构造
//    APInt（Debug 断言崩溃 / Release 静默截断为错误值）。
//  - Bug C: ForStmtAST 循环变量不建独立作用域 → 兄弟 for 循环复用同名变量时
//    第二个循环绑定首个循环的 i（结束值 11），条件立即为假、循环体不执行。
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

class DemoRegressionE2E : public ::testing::Test {
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
    return ((int (*)())(intptr_t)sym->getValue())();
}

// Bug B：无后缀大整数必须按值域提升（4000000000 > INT32_MAX → int64）。
TEST_F(DemoRegressionE2E, LargeUnSuffixedLiteralPromotes) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int64 big = 4000000000;
            if (big != 4000000000) { return 1; }
            int64 sum = big + 1;
            if (sum != 4000000001) { return 2; }
            return 0;
        }
    )", "reg_lit.cdd"), 0);
}

// Bug C：兄弟 for 循环复用同名循环变量，第二个循环必须重新绑定（C 语义）。
TEST_F(DemoRegressionE2E, SiblingForLoopReusesVarName) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 sum = 0;
            for (int32 i = 0; i <= 10; i = i + 1) { sum = sum + i; }
            int32 cont = 0;
            for (int32 i = 0; i < 5; i = i + 1) {
                if (i == 2) { continue; }
                if (i == 4) { break; }
                cont = cont + 1;
            }
            if (sum != 55 || cont != 3) { return 1; }
            return 0;
        }
    )", "reg_for.cdd"), 0);
}

// Bug A：类位域 + 派生类 size_of 组合不再损坏布局（编译 + 执行均通过）。
TEST_F(DemoRegressionE2E, ClassBitFieldWithSizeOfDerived) {
    EXPECT_EQ(runSource(R"(
        class Counter {
        public:
            int32 count;
            int32 bf : 3;
            void inc() { this->count = this->count + 1; }
        };
        class Animal {
        public:
            int32 age;
            void setAge(int32 v) { this->age = v; }
        };
        class Dog : public Animal {
        public:
            int32 legs;
            int32 bark() { return this->age * 10 + this->legs; }
        };
        int32 main() {
            Counter ctr;
            ctr.count = 0;
            ctr.inc();
            ctr.bf = 3;
            if (ctr.count != 1 || ctr.bf != 3) { return 1; }
            Dog d;
            d.setAge(4);
            d.legs = 4;
            if (d.bark() != 44) { return 2; }
            if (compile_time.size_of(Dog) != 8) { return 3; }
            return 0;
        }
    )", "reg_cls.cdd"), 0);
}

// Bug D：指针后缀 ++/-- 须按元素步进（GEP），不能字节 add——int32* ++ 只前进
// 1 字节时解引用读到错位值。
TEST_F(DemoRegressionE2E, PointerPostfixIncDecStepsByElement) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[3] = {10, 20, 30};
            int32* p = &arr[0];
            p++;
            if (*p != 20) { return 1; }
            p--;
            if (*p != 10) { return 2; }
            p++;
            int32* q = &arr[0];
            ++q;
            if (*q != 20) { return 3; }
            int32* r = &arr[2];
            r--;
            if (*r != 20) { return 4; }
            return 0;
        }
    )", "reg_postinc.cdd"), 0);
}

// Bug E：浮点复合赋值走整数指令（CreateMul 等）→ 位模式乘法垃圾值；须 F 系。
TEST_F(DemoRegressionE2E, FloatCompoundAssignmentUsesFloatOps) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            float64 fd = 1.5;
            fd *= 2.0;
            if (fd != 3.0) { return 1; }
            fd += 0.5;
            if (fd != 3.5) { return 2; }
            fd -= 1.0;
            if (fd != 2.5) { return 3; }
            fd /= 2.5;
            if (fd != 1.0) { return 4; }
            float32 h = 10.0f;
            h /= 4.0f;
            if (h != 2.5f) { return 5; }
            return 0;
        }
    )", "reg_fcmpd.cdd"), 0);
}

// Bug F：无符号复合赋值走有符号指令——`>>=` 算术移、`/=`/`%=` 高位错值。
TEST_F(DemoRegressionE2E, UnsignedCompoundAssignmentUsesUnsignedOps) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            uint32 w = 0x80000000;
            w >>= 4;
            if (w != 0x08000000) { return 1; }
            uint32 d = 0x80000000;
            d /= 2;
            if (d != 0x40000000) { return 2; }
            uint32 m = 0x80000000;
            m %= 3;                  // 2147483648 % 3 = 2
            if (m != 2) { return 3; }
            return 0;
        }
    )", "reg_ucomp.cdd"), 0);
}

// Bug G：运算符重载解析后表达式类型被设为左操作数类型而非 operator 函数
// 返回类型 → `int32 s = p + q` 类型不匹配、`p + q == 10` 比较 V2 与 int32。
TEST_F(DemoRegressionE2E, OperatorOverloadExpressionTypeIsReturnType) {
    EXPECT_EQ(runSource(R"(
        struct V2 { int32 x; int32 y; };
        int32 operator+(V2 a, V2 b) { return a.x + b.x + a.y + b.y; }
        bool operator==(V2 a, V2 b) { return a.x == b.x && a.y == b.y; }
        int32 main() {
            V2 p = {1, 2};
            V2 q = {3, 4};
            int32 s = p + q;                 // 直接赋值
            if (s != 10) { return 1; }
            if (!(p + q == 10)) { return 2; } // 复合比较
            int32 c = p + q + 1;              // 与 int 链接
            if (c != 11) { return 3; }
            V2 r = {1, 2};
            if (!(p == r)) { return 4; }
            return 0;
        }
    )", "reg_opov.cdd"), 0);
}

// Bug H：void 函数裸 `return;`（早退）此前解析器硬性要求表达式被拒；
// sema/codegen 早已支持（void 检查 + CreateRetVoid），仅解析放行。
TEST_F(DemoRegressionE2E, BareReturnInVoidFunction) {
    EXPECT_EQ(runSource(R"(
        int32 trace = 0;
        void early(int32 x) {
            if (x < 0) return;
            if (x == 0) return;
            trace = trace + x;
        }
        int32 main() {
            early(5); early(-1); early(0); early(7);
            if (trace != 12) { return 1; }
            return 0;
        }
    )", "reg_ret.cdd"), 0);
}

// Gap 1：`&fn` 取函数地址（函数设计符 &fn == fn，C 语义）。
TEST_F(DemoRegressionE2E, AddressOfFunctionDesignator) {
    EXPECT_EQ(runSource(R"(
        int32 add(int32 a, int32 b) { return a + b; }
        int32 main() {
            int32 (*fp)(int32, int32) = &add;
            if (fp(2, 3) != 5) { return 1; }
            int32 (*fp2)(int32, int32) = add;
            if (fp2(4, 5) != 9) { return 2; }
            return 0;
        }
    )", "reg_andfn.cdd"), 0);
}

// Gap 2：string_view 下标 s[i] 取字节（等价 char_at）。
TEST_F(DemoRegressionE2E, StringViewSubscript) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view s = "hello";
            if (s[0] != 'h' || s[4] != 'o') { return 1; }
            if (s.char_at(1) != 'e') { return 2; }
            int32 sum2 = s[0] + s[1] + s[2];
            if (sum2 != 'h' + 'e' + 'l') { return 3; }
            return 0;
        }
    )", "reg_stridx.cdd"), 0);
}

// Gap 3：指针相减 p - q → 元素个数（ptrdiff）。
TEST_F(DemoRegressionE2E, PointerDifference) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[5] = {10, 20, 30, 40, 50};
            int32* a = &arr[0];
            int32* b = &arr[4];
            if (b - a != 4) { return 1; }
            if (a - b != -4) { return 2; }
            int32* m = &arr[2];
            if (b - m != 2) { return 3; }
            return 0;
        }
    )", "reg_ptrdiff.cdd"), 0);
}

// Gap 4：数组别名（下标 / slice / sizeof / 多维）。
TEST_F(DemoRegressionE2E, ArrayAliasWorks) {
    EXPECT_EQ(runSource(R"(
        using IntArr = int32[4];
        using Mat = int32[2][3];
        int32 sum(int32[] s) {
            int32 t = 0;
            for (int32 i = 0; i < s.len; i = i + 1) { t = t + s[i]; }
            return t;
        }
        int32 main() {
            IntArr arr = {1, 2, 3, 4};
            if (arr[0] != 1 || arr[3] != 4) { return 1; }
            if (sizeof(arr) != 16) { return 2; }
            int32[] s = arr;
            if (s.len != 4 || sum(arr) != 10) { return 3; }
            Mat m = {{1, 2, 3}, {4, 5, 6}};
            if (m[1][2] != 6 || sizeof(m) != 24) { return 4; }
            return 0;
        }
    )", "reg_arrtyp.cdd"), 0);
}

// Gap 5：函数指针别名 + 函数返回函数指针（经 using 惯用法）。
TEST_F(DemoRegressionE2E, FunctionPointerAliasAndReturn) {
    EXPECT_EQ(runSource(R"(
        using BinOp = int32 (*)(int32, int32);
        int32 add(int32 a, int32 b) { return a + b; }
        int32 mul(int32 a, int32 b) { return a * b; }
        BinOp pick(bool useAdd) { return useAdd ? add : mul; }
        int32 main() {
            BinOp f = add;
            if (f(2, 3) != 5) { return 1; }
            BinOp g = pick(true);
            if (g(3, 4) != 7) { return 2; }
            g = pick(false);
            if (g(3, 4) != 12) { return 3; }
            return 0;
        }
    )", "reg_fnptrtyp.cdd"), 0);
}

// Gap 6：int128/uint128 字面量（>64 位）。
TEST_F(DemoRegressionE2E, Int128Literal) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int128 big = 170141183460469231731687303715884105727;
            if (big != 170141183460469231731687303715884105727) { return 1; }
            if (!(big > 0)) { return 2; }
            int128 shifted = 1;
            shifted = shifted << 100;
            if (shifted != 1267650600228229401496703205376) { return 3; }
            uint128 ub = 340282366920938463463374607431768211455;
            if (!(ub > 170141183460469231731687303715884105727)) { return 4; }
            return 0;
        }
    )", "reg_i128.cdd"), 0);
}

// CG-05：rvalue 聚合的成员访问必须按值读取（`make().x`），
// 不能把 GEP 直接作用在聚合值上；rvalue 字段也不可赋值。
TEST_F(DemoRegressionE2E, RvalueAggregateMemberReadsValue) {
    EXPECT_EQ(runSource(R"(
        struct P { int32 x; int32 y; };
        struct P make() { struct P p; p.x = 7; p.y = 9; return p; }
        int32 main() {
            if (make().x != 7) { return 1; }
            if (make().y != 9) { return 2; }
            if (make().x + make().y != 16) { return 3; }
            return 0;
        }
    )", "reg_rvalue_member.cdd"), 0);
}

TEST_F(DemoRegressionE2E, RvalueAggregateMemberNotAssignable) {
    EXPECT_EQ(runSource(R"(
        struct P { int32 x; };
        struct P make() { struct P p; p.x = 7; return p; }
        int32 main() { make().x = 5; return 0; }
    )", "reg_rvalue_member_assign.cdd"), -1);
}

// MOD-03：前向类型按值使用必须报 incomplete；指针/引用形态仍允许。
TEST_F(DemoRegressionE2E, IncompleteTypeByValueRejected) {
    EXPECT_EQ(runSource(R"(
        struct S;
        struct S s;
        struct S { int32 x; };
        int32 main() { return 0; }
    )", "reg_incomplete_value.cdd"), -1);
    EXPECT_EQ(runSource(R"(
        struct S;
        struct S arr[2];
        struct S { int32 x; };
        int32 main() { return 0; }
    )", "reg_incomplete_array.cdd"), -1);
}

TEST_F(DemoRegressionE2E, IncompleteTypePointerAllowed) {
    EXPECT_EQ(runSource(R"(
        struct S;
        struct S* p;
        struct S { int32 x; };
        int32 main() { struct S s; s.x = 1; p = &s; return p->x - 1; }
    )", "reg_incomplete_ptr.cdd"), 0);
}

TEST_F(DemoRegressionE2E, StaticLocalPersistsAcrossCalls) {
    EXPECT_EQ(runSource(R"(
        int32 next() { static int32 x = 0; x = x + 1; return x; }
        int32 main() {
            if (next() != 1) return 1;
            if (next() != 2) return 2;
            if (next() != 3) return 3;
            return 0;
        }
    )", "reg_static_local.cdd"), 0);
}

TEST_F(DemoRegressionE2E, StaticLocalWithoutInitializerIsZero) {
    EXPECT_EQ(runSource(R"(
        int32 next() { static int32 x; x = x + 1; return x; }
        int32 main() { return next() == 1 ? 0 : 1; }
    )", "reg_static_local_zero.cdd"), 0);
}

TEST_F(DemoRegressionE2E, StaticLocalNonConstantInitRejected) {
    EXPECT_EQ(runSource(R"(
        int32 seed() { return 5; }
        int32 f() { static int32 x = seed(); return x; }
        int32 main() { return f(); }
    )", "reg_static_local_nonconst.cdd"), -1);
}

TEST_F(DemoRegressionE2E, ThreadLocalStaticLocalCompiles) {
    EXPECT_EQ(runSource(R"(
        int32 next() { static thread_local int32 x = 3; x = x + 1; return x; }
        int32 main() {
            if (next() != 4) return 1;
            if (next() != 5) return 2;
            return 0;
        }
    )", "reg_tls_local.cdd"), 0);
}

TEST_F(DemoRegressionE2E, LocalRecordDefinitions) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            struct S { int32 x; }; S s; s.x = 1;
            union U { int32 i; float32 f; }; U u; u.i = 2;
            enum E { A = 3 }; int32 e = (int32)A;
            return s.x + u.i + e - 6;
        }
    )", "reg_local_records.cdd"), 0);
}

// 自引用类（`Node* next`）曾因 getLLVMType(Pointer) 递归解析 pointee 爆栈。
TEST_F(DemoRegressionE2E, SelfReferentialClassPointerField) {
    EXPECT_EQ(runSource(R"(
        class Node { public: Node* next; int32 v; };
        int32 main() {
            Node a; Node b;
            a.next = &b; a.v = 1; b.next = null; b.v = 2;
            return a.next->v + a.v - 3;
        }
    )", "reg_class_self.cdd"), 0);
}

TEST_F(DemoRegressionE2E, ClassMemberBinaryAndUnaryOperators) {
    EXPECT_EQ(runSource(R"(
        class V {
        public:
            int32 x;
            int32 operator+(V o) { return this->x + o.x; }
            int32 operator-() { return -(this->x); }
        };
        int32 main() {
            V a; a.x = 3; V b; b.x = 4;
            if (a + b != 7) return 1;
            if (-a != -3) return 2;
            return 0;
        }
    )", "reg_class_ops.cdd"), 0);
}

TEST_F(DemoRegressionE2E, OutOfLineMemberOperatorDefinition) {
    EXPECT_EQ(runSource(R"(
        class V { public: int32 x; int32 operator+(V o); };
        int32 V::operator+(V o) { return this->x + o.x; }
        int32 main() { V a; a.x = 1; V b; b.x = 2; return a + b - 3; }
    )", "reg_class_op_ooc.cdd"), 0);
}

TEST_F(DemoRegressionE2E, CompileTimeIfInFunctionBody) {
    EXPECT_EQ(runSource(R"(
        int32 f() {
            compile_time.if(true) { return 1; } else { return 2; }
        }
        int32 g() {
            int32 x = 0;
            compile_time.if(false) { x = 5; } else { x = 7; }
            return x;
        }
        int32 main() { return f() + g() - 8; }
    )", "reg_ct_if_stmt.cdd"), 0);
}

TEST_F(DemoRegressionE2E, NestedOffsetof) {
    EXPECT_EQ(runSource(R"(
        struct Inner { int32 a; int32 b; };
        struct Outer { int32 z; struct Inner in; int32 arr[3]; };
        int32 main() {
            if (offsetof(Outer, in.b) != 8) return 1;
            if (offsetof(Outer, arr[1]) != 16) return 2;
            return 0;
        }
    )", "reg_offsetof_nested.cdd"), 0);
}
