// P1-07 / PAR-10: 位域（Bit-fields）——语法解析、语义校验、System V 布局、RMW 读写。
// fixture 仿 test_annotations.cpp（runSource 正向专用；错误路径走显式管线）。

#include <gtest/gtest.h>

#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "codegen/CodegenContext.h"
#include "support/Log.h"
#include "ast/Decl.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"

#include <spdlog/spdlog.h>

class BitFieldE2E : public ::testing::Test {
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

    // 完整管线：main 返回值即断言。parse/sema/codegen/verify 失败 → -1（正向用例专用）。
    int runSource(const std::string& source, const std::string& filename) {
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
                ADD_FAILURE() << "Semantic error: " << err.message;
            }
            return -1;
        }
        CodegenContext ctx;
        ctx.setSourceFile(filename);
        ast->codegen(ctx);
        std::string ve;
        llvm::raw_string_ostream vs(ve);
        if (llvm::verifyModule(ctx.getModule(), &vs)) {
            vs.flush();
            ADD_FAILURE() << "Module verification failed: " << ve;
            return -1;
        }
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
        auto fn = (int (*)())(intptr_t)sym->getValue();
        return fn();
    }
};

// ---- Task 1: 语法解析 + 数据载体 ----

// 正向测试走显式 parser 管线并精确断言 FieldInfo.bitWidth（runSource 只查
// ast!=nullptr，无法捕获"位宽被静默丢弃"——当前解析器把 `a : 3` 当普通字段 `a`）。
TEST_F(BitFieldE2E, ParseBasicDeclaration) {
    Lexer lexer("bf_parse.cmm",
        "struct S { int32 a : 3; uint32 b : 5; int32 : 0; int32 : 3; }; "
        "int32 main() { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    ASSERT_TRUE(parser.getErrors().empty());
    ASSERT_GE(ast->declarations.size(), 1u);
    auto* sd = dynamic_cast<StructDeclAST*>(ast->declarations[0].get());
    ASSERT_NE(sd, nullptr);
    ASSERT_EQ(sd->fields.size(), 4u);
    EXPECT_EQ(sd->fields[0].name, "a");
    EXPECT_EQ(sd->fields[0].bitWidth, 3);
    EXPECT_EQ(sd->fields[1].name, "b");
    EXPECT_EQ(sd->fields[1].bitWidth, 5);
    EXPECT_EQ(sd->fields[2].name, "");   // 无名零宽位域
    EXPECT_EQ(sd->fields[2].bitWidth, 0);
    EXPECT_EQ(sd->fields[3].name, "");   // 无名填充位域
    EXPECT_EQ(sd->fields[3].bitWidth, 3);
}

// 位域 + 数组后缀互斥：`int32 a : 3[2]` 报错（数组后缀落在位域宽度之后）。
TEST_F(BitFieldE2E, ParseArraySuffixRejected) {
    Lexer lexer("bf_arr.cmm",
        "struct S { int32 a : 3[2]; }; int32 main() { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    EXPECT_FALSE(parser.getErrors().empty());
}

// 位域宽度须为常量表达式：`int32 a : x;`（x 为运行时变量）报错。
TEST_F(BitFieldE2E, ParseNonConstWidthRejected) {
    Lexer lexer("bf_nc.cmm",
        "struct S { int32 a : x; }; int32 main() { int32 x = 3; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    EXPECT_FALSE(parser.getErrors().empty());
}

// ---- Task 2: 位域语义校验（sema）----

// 负例辅助：显式管线取 sema 诊断（runSource 对任何错误 ADD_FAILURE，仅正向可用）。
static std::vector<Diagnostic> semaErrorsOf(const std::string& source, const std::string& filename) {
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) return {};
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    return analyzer.getErrors();
}

TEST_F(BitFieldE2E, FloatBaseRejected) {
    // 浮点基础类型 → E2025。
    auto errs = semaErrorsOf(
        "struct S { float64 a : 3; }; int32 main(){return 0;}", "bf_f.cmm");
    ASSERT_FALSE(errs.empty());
    EXPECT_EQ(errs.front().code, DiagnosticCode::SemBitFieldInvalidType);
}

TEST_F(BitFieldE2E, EnumBaseRejected) {
    // 枚举基础类型 → E2025。
    auto errs = semaErrorsOf(
        "enum E : uint8 { A }; struct S { enum E e : 2; }; int32 main(){return 0;}", "bf_e.cmm");
    ASSERT_FALSE(errs.empty());
    EXPECT_EQ(errs.front().code, DiagnosticCode::SemBitFieldInvalidType);
}

TEST_F(BitFieldE2E, WidthTooWideRejected) {
    // 宽度超类型位宽 → E2026。
    auto errs = semaErrorsOf(
        "struct S { int32 a : 40; }; int32 main(){return 0;}", "bf_w.cmm");
    ASSERT_FALSE(errs.empty());
    EXPECT_EQ(errs.front().code, DiagnosticCode::SemBitFieldWidthOutOfRange);
}

TEST_F(BitFieldE2E, NamedZeroWidthRejected) {
    // 命名零宽位域 → parse 期拒绝（FieldInfo 编码不可表示该状态，Ruling 记录）。
    Lexer lexer("bf_z.cmm",
        "struct S { int32 a : 0; }; int32 main(){return 0;}");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    EXPECT_FALSE(parser.getErrors().empty());
}

TEST_F(BitFieldE2E, AddrOfRejected) {
    // 对位域取址 → E2027。
    auto errs = semaErrorsOf(
        "struct S { int32 a : 3; }; int32 main(){ struct S s; int32* p = &s.a; return 0; }", "bf_addr.cmm");
    ASSERT_FALSE(errs.empty());
    EXPECT_EQ(errs.front().code, DiagnosticCode::SemBitFieldNotAddressable);
}

// ---- Task 3: 布局物化（System V 存储单元分配）----

// 布局 pin：compile_time.size_of/align_of 与 IR 布局同源（LayoutBuilder 单源）。
TEST_F(BitFieldE2E, LayoutPack29) {
    // a(3)+b(29) 共单元0，c(3) 放不下 → 单元1（offset 4），size 8。
    EXPECT_EQ(runSource(R"(
        struct S { int32 a : 3; int32 b : 29; int32 c : 3; };
        int32 main() {
            return (compile_time.size_of(struct S) == 8 &&
                    compile_time.align_of(struct S) == 4) ? 0 : 1;
        }
    )", "bf_l1.cmm"), 0);
}

TEST_F(BitFieldE2E, LayoutSplit28) {
    // a 单元0，b 放不下 → 单元1（offset 4），size 8。
    EXPECT_EQ(runSource(R"(
        struct S { int32 a : 28; int32 b : 28; };
        int32 main() { return (compile_time.size_of(struct S) == 8) ? 0 : 1; }
    )", "bf_l2.cmm"), 0);
}

TEST_F(BitFieldE2E, LayoutUint8Share) {
    // uint8 a:3 + uint8 b:3 共享 1 字节单元。
    EXPECT_EQ(runSource(R"(
        struct S { uint8 a : 3; uint8 b : 3; };
        int32 main() { return (compile_time.size_of(struct S) == 1) ? 0 : 1; }
    )", "bf_l3.cmm"), 0);
}

TEST_F(BitFieldE2E, LayoutZeroWidthAlign) {
    // 零宽对齐：a 单元0，b 单元1（offset 4），size 8。
    EXPECT_EQ(runSource(R"(
        struct S { int32 a : 3; int32 : 0; int32 b : 3; };
        int32 main() { return (compile_time.size_of(struct S) == 8) ? 0 : 1; }
    )", "bf_l4.cmm"), 0);
}

TEST_F(BitFieldE2E, LayoutUnnamedPad) {
    // 无名填充：a + 填充 + b 共 1 单元，size 4。
    EXPECT_EQ(runSource(R"(
        struct S { int32 a : 3; int32 : 5; int32 b : 3; };
        int32 main() { return (compile_time.size_of(struct S) == 4) ? 0 : 1; }
    )", "bf_l5.cmm"), 0);
}

TEST_F(BitFieldE2E, LayoutBoolUnit) {
    // bool 位域 → 1 字节存储单元。
    EXPECT_EQ(runSource(R"(
        struct S { bool b : 1; };
        int32 main() { return (compile_time.size_of(struct S) == 1) ? 0 : 1; }
    )", "bf_l6.cmm"), 0);
}

TEST_F(BitFieldE2E, LayoutPackedTight) {
    // [[packed]]：单元对齐 1 无填充。int32 a:24 + int32 b:8 → 一个 4B 单元，size 4。
    EXPECT_EQ(runSource(R"(
        [[packed]] struct S { int32 a : 24; int32 b : 8; };
        int32 main() { return (compile_time.size_of(struct S) == 4) ? 0 : 1; }
    )", "bf_l7.cmm"), 0);
}

TEST_F(BitFieldE2E, OffsetOfBitFieldRejected) {
    // offset_of 对位域报错（C 语义）。runSource 对任何 sema 错误 ADD_FAILURE
    //（正向专用），负例走显式 sema 管线。
    auto errs = semaErrorsOf(R"(
        struct S { int32 a : 3; };
        int32 main() { return (compile_time.offset_of(struct S, "a") == 0) ? 0 : 1; }
    )", "bf_off.cmm");
    ASSERT_FALSE(errs.empty());
}

// ---- Task 4: codegen 位域读写（read-modify-write）----

TEST_F(BitFieldE2E, ReadWriteBasic) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 a : 3; };
        int32 main() {
            struct S s = { 3 };
            if (s.a != 3) { return 1; }
            s.a = 2;
            if (s.a != 2) { return 2; }
            return 0;
        }
    )", "bf_rw.cmm"), 0);
}

TEST_F(BitFieldE2E, SignedExtendNegative) {
    // int32 a:3 存负值读回负值（sext），可参与负数比较。
    EXPECT_EQ(runSource(R"(
        struct S { int32 a : 3; };
        int32 main() {
            struct S s = { -3 };
            if (s.a != -3) { return 1; }
            if (!(s.a < 0)) { return 2; }
            return 0;
        }
    )", "bf_sgn.cmm"), 0);
}

TEST_F(BitFieldE2E, UnsignedTruncate) {
    // uint32 a:3 存 9（1001）→ 截断回 1。
    EXPECT_EQ(runSource(R"(
        struct S { uint32 a : 3; };
        int32 main() {
            struct S s = { 9 };
            if (s.a != 1) { return 1; }
            return 0;
        }
    )", "bf_uns.cmm"), 0);
}

TEST_F(BitFieldE2E, CompoundAssign) {
    // 复合赋值回绕：7 += 2 → 9 → 1。
    EXPECT_EQ(runSource(R"(
        struct S { uint32 a : 3; };
        int32 main() {
            struct S s = { 7 };
            s.a += 2;
            if (s.a != 1) { return 1; }
            return 0;
        }
    )", "bf_cmp.cmm"), 0);
}

TEST_F(BitFieldE2E, IncDec) {
    // ++/-- 前缀后缀，宽度边界回绕。
    EXPECT_EQ(runSource(R"(
        struct S { uint32 a : 3; };
        int32 main() {
            struct S s = { 7 };
            s.a++;
            if (s.a != 0) { return 1; }
            ++s.a;
            if (s.a != 1) { return 2; }
            s.a--;
            if (s.a != 0) { return 3; }
            return 0;
        }
    )", "bf_inc.cmm"), 0);
}

TEST_F(BitFieldE2E, BoolBitField) {
    // bool b:1 位域读写（i1 语义值）。
    EXPECT_EQ(runSource(R"(
        struct S { bool b : 1; };
        int32 main() {
            struct S s = { 1 };
            if (!s.b) { return 1; }
            s.b = 0;
            if (s.b) { return 2; }
            return 0;
        }
    )", "bf_bool.cmm"), 0);
}

TEST_F(BitFieldE2E, NestedChain) {
    // 链式位域访问 arr[1].i.bf / o.i.bf——unit GEP 在最内层做。
    // 注意：int32 bf:3 有符号，值须在 [-4, 3]（写 5 会读回 -3，符合 C 语义）。
    EXPECT_EQ(runSource(R"(
        struct I { int32 bf : 3; };
        struct O { struct I i; };
        int32 main() {
            struct O o = { { 1 } };
            struct O arr[2];
            arr[1].i.bf = 3;
            o.i.bf = 2;
            if (o.i.bf != 2) { return 1; }
            if (arr[1].i.bf != 3) { return 2; }
            return 0;
        }
    )", "bf_chain.cmm"), 0);
}

TEST_F(BitFieldE2E, AsArgAndReturn) {
    // 位域值传参（按基础类型值）；经指针位域读作返回值。
    // （struct 整体作函数返回是 MEM-14 既有限制，Ruling 记录。）
    EXPECT_EQ(runSource(R"(
        struct S { int32 a : 3; };
        int32 take(int32 v) { return v; }
        int32 readA(struct S* p) { return p->a; }
        int32 main() {
            struct S s = { 1 };
            if (take(s.a) != 1) { return 1; }
            struct S t = { -2 };
            if (readA(&t) != -2) { return 2; }
            return 0;
        }
    )", "bf_arg.cmm"), 0);
}

TEST_F(BitFieldE2E, StructCopy) {
    // 含位域 struct 整体拷贝（存储单元 = 整数，字节拷贝等价位拷贝）。
    EXPECT_EQ(runSource(R"(
        struct S { int32 a : 3; uint32 b : 5; };
        int32 main() {
            struct S x = { -3, 7 };
            struct S y;
            y = x;
            if (y.a != -3 || y.b != 7) { return 1; }
            return 0;
        }
    )", "bf_copy.cmm"), 0);
}

// ---- Task 5: 全局/static 位域初始化（常量构造位域感知）----

TEST_F(BitFieldE2E, GlobalInitSharedUnit) {
    // a 与 b 共享 4B 单元——全局常量须把两值 OR 进同一单元。
    // （uint32 避免有符号 3 位域装不下 5 的 sext 语义。）
    EXPECT_EQ(runSource(R"(
        struct S { uint32 a : 3; uint32 b : 5; };
        struct S g = { 5, 7 };
        int32 main() {
            if (g.a != 5 || g.b != 7) { return 1; }
            return 0;
        }
    )", "bf_ginit.cmm"), 0);
}

TEST_F(BitFieldE2E, LocalBraceInitTruncation) {
    // 局部花括号初始化位域感知：9 截断回 1（emitAggregateInitializer 语义字段路径）。
    // 注：原 StaticLocalInit 用 `static` 局部——本语言不支持 static 局部（Parser
    // 仅在类成员处理 TOKEN_STATIC），旧测试靠 parse 错误恢复静默丢弃 `static` 才
    // "碰巧"通过；改为诚实的局部初始化断言（Ruling 2026-10-14-1）。
    EXPECT_EQ(runSource(R"(
        struct S { uint32 a : 3; };
        int32 main() {
            struct S s = { 9 };
            if (s.a != 1) { return 1; }
            return 0;
        }
    )", "bf_linit.cmm"), 0);
}

TEST_F(BitFieldE2E, ZeroWidthInitSkipsUnnamed) {
    // 无名零宽位域不消费初始值（C 语义）——a/b 分别拿到 5/6，全局与局部都验证。
    EXPECT_EQ(runSource(R"(
        struct S { uint32 a : 3; uint32 : 0; uint32 b : 3; };
        struct S g = { 5, 6 };
        int32 main() {
            if (g.a != 5 || g.b != 6) { return 1; }
            struct S t = { 3, 4 };
            if (t.a != 3 || t.b != 4) { return 1; }
            return 0;
        }
    )", "bf_zwinit.cmm"), 0);
}

// ---- Task 6: union / [[packed]] / class 位域 ----

TEST_F(BitFieldE2E, UnionSizeAlign) {
    // union 位域成员共享 4B 存储块（max size/align 按 bitFieldStorageType）。
    EXPECT_EQ(runSource(R"(
        union U { int32 a : 3; uint32 b : 5; };
        int32 main() {
            return (compile_time.size_of(union U) == 4) ? 0 : 1;
        }
    )", "bf_usz.cmm"), 0);
}

TEST_F(BitFieldE2E, UnionOverlap) {
    // C 语义：a 的 3 位与 b 的 3 位重叠于 offset 0。
    // （uint32 避免有符号 3 位域存 5 读回 -3 的 sext 语义。）
    EXPECT_EQ(runSource(R"(
        union U { uint32 a : 3; uint32 b : 3; };
        int32 main() {
            union U u;
            u.a = 5;
            if (u.b != 5) { return 1; }
            return 0;
        }
    )", "bf_uov.cmm"), 0);
}

TEST_F(BitFieldE2E, PackedZeroWidth) {
    // packed 下零宽位域把位流游标按声明类型自然边界取整（int→4B）：
    // a:3 占字节 0，int:0 对齐到字节 4，b:3 占字节 4 → sizeof 5（GCC 实测）。
    EXPECT_EQ(runSource(R"(
        [[packed]] struct S { int32 a : 3; int32 : 0; int32 b : 3; };
        int32 main() { return (compile_time.size_of(struct S) == 5) ? 0 : 1; }
    )", "bf_pz.cmm"), 0);
}

TEST_F(BitFieldE2E, PackedTightFit) {
    // packed 位域连续位流紧排：int a:3; int b:3 → 6 位 → 1 字节（GCC 实测）。
    EXPECT_EQ(runSource(R"(
        [[packed]] struct S { int32 a : 3; int32 b : 3; };
        int32 main() { return (compile_time.size_of(struct S) == 1) ? 0 : 1; }
    )", "bf_pt.cmm"), 0);
}

TEST_F(BitFieldE2E, ClassBitField) {
    EXPECT_EQ(runSource(R"(
        class C { public: int32 a : 3; };
        int32 main() {
            class C c;
            c.a = -2;
            if (c.a != -2) { return 1; }
            return 0;
        }
    )", "bf_cls.cmm"), 0);
}

TEST_F(BitFieldE2E, ClassInheritedBitField) {
    EXPECT_EQ(runSource(R"(
        class B { public: int32 base; };
        class D : public B { public: int32 bf : 3; };
        int32 main() {
            class D d;
            d.base = 11;
            d.bf = 2;
            if (d.base != 11 || d.bf != 2) { return 1; }
            return 0;
        }
    )", "bf_cls2.cmm"), 0);
}

// ---- 评审修复回归（union 位域全通路、派生混合字段、位宽溢出）----

TEST_F(BitFieldE2E, UnionBitFieldTruncates) {
    // C1：union 位域必须是 RMW（截断），而非整字写。
    EXPECT_EQ(runSource(R"(
        union UB { uint32 a : 3; };
        int32 main() {
            union UB u;
            u.a = 9;
            if (u.a != 1) { return 1; }
            return 0;
        }
    )", "bf_utrunc.cmm"), 0);
}

TEST_F(BitFieldE2E, UnionBitFieldAddrOfRejected) {
    // C1：union 位域取址同样拒绝（E2027）。
    auto errs = semaErrorsOf(
        "union UB { uint32 a : 3; }; int32 main(){ union UB u; uint32* p = &u.a; return 0; }", "bf_uaddr.cmm");
    ASSERT_FALSE(errs.empty());
    EXPECT_EQ(errs.front().code, DiagnosticCode::SemBitFieldNotAddressable);
}

TEST_F(BitFieldE2E, UnionBitFieldGlobalInit) {
    // C2：union 位域全局常量须截断（9 → 1）。
    EXPECT_EQ(runSource(R"(
        union UB { uint32 a : 3; };
        union UB g = { 9 };
        int32 main() { return (g.a == 1) ? 0 : 1; }
    )", "bf_uginit.cmm"), 0);
}

TEST_F(BitFieldE2E, DerivedNormalAndBitField) {
    // I1：派生聚合含基类 + 普通字段 + 位域——普通字段 GEP 不得因物化多 +1。
    EXPECT_EQ(runSource(R"(
        class BM { public: int32 base; };
        class DM : public BM { public: int32 x; int32 bf : 3; };
        int32 main() {
            class DM d;
            d.base = 11;
            d.x = 111;
            d.bf = 2;
            if (d.base != 11 || d.x != 111 || d.bf != 2) { return 1; }
            return 0;
        }
    )", "bf_dmix.cmm"), 0);
}

TEST_F(BitFieldE2E, WidthOverflowRejected) {
    // M1：超 int32 范围位宽在 parse 期拒绝（防 static_cast<int> 回绕绕过 E2026）。
    Lexer lexer("bf_wovf.cmm",
        "struct S { int32 a : 4294967297; }; int32 main() { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    EXPECT_FALSE(parser.getErrors().empty());
}




