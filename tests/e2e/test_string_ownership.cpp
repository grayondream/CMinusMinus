// I4 收口（spec: docs/superpowers/specs/2026-10-11-string-ownership-design.md）：
// string 拥有型值语义端到端执行测试。拷贝模型 B（D2）——活值深拷贝、新鲜值
// 移动；`b = a` 赋值经 getStringAssignFn（D7，自动释放旧缓冲）。
// fixture 仿 test_string.cpp 的 StringE2E runSource（完整 JIT 执行，返回 main 的返回值）。

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

class StringOwnershipE2E : public ::testing::Test {
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

// I4 (D7): `b = a`（string 赋值）必须深拷贝——`a`/`b` 各自独立缓冲，
// 双双 destroy 不得双 free abort（当前浅拷贝 → 共享缓冲 → 双 free）。
TEST_F(StringOwnershipE2E, AssignCopyIndependent) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string a = string.new("y");
            string b = string.new("x");
            b = a;
            a.destroy();
            b.destroy();
            return 0;
        }
    )", "own_assign.cdd"), 0);
}

// I4 (D7, 自赋值守卫): `b = b;` 不得 free 共享缓冲（双 free abort）——
// getStringAssignFn 在 dst.ptr == src.ptr 时什么都不做。
TEST_F(StringOwnershipE2E, SelfAssignSafe) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string b = string.new("x");
            b = b;
            string_view v = b;
            if (v != "x") { return 1; }
            b.destroy();
            return 0;
        }
    )", "own_self.cdd"), 0);
}

// I4 (D7): `b = a;` 后 b 持有 a 的深拷贝（值正确、len 正确），a 仍有效
// 可 destroy——旧缓冲被赋值 helper 自动释放。
TEST_F(StringOwnershipE2E, AssignFreesOldBuffer) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string a = string.new("abc");
            string b = string.new("zzzzzz");
            b = a;
            a.destroy();
            string_view v = b;
            if (v != "abc") { return 1; }
            if (b.len() != 3) { return 2; }
            b.destroy();
            return 0;
        }
    )", "own_frees_old.cdd"), 0);
}

// I4 (D7, fresh 赋值): `b = string.new("y");` 覆盖已初始化 b——移动（free
// 旧缓冲 + 裸 store），值正确且无泄漏/双 free。
TEST_F(StringOwnershipE2E, AssignFreshValueFreesOld) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string b = string.new("x");
            b = string.new("y");
            string_view v = b;
            if (v != "y") { return 1; }
            b.destroy();
            return 0;
        }
    )", "own_fresh.cdd"), 0);
}

// I4 (D2): `string t = s;`（初始化）必须深拷贝——`s`/`t` 各自独立缓冲，
// 双双 destroy 不得双 free abort（I4 路径 1 的初始化形态；当前浅拷贝）。
TEST_F(StringOwnershipE2E, InitCopyIndependent) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("x");
            string t = s;
            s.destroy();
            t.destroy();
            return 0;
        }
    )", "own_init.cdd"), 0);
}

// I4 (D2, fresh 初始化): `string s = string.new("x");` 直接 store（移动），
// 行为不变且不崩。
TEST_F(StringOwnershipE2E, InitMoveFromNew) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("x");
            s.destroy();
            return 0;
        }
    )", "own_init_fresh.cdd"), 0);
}

// I4 (D2, fresh 初始化/函数调用): `string s = fresh();`——函数调用 rvalue
// 返回 string → 移动（零拷贝），值正确。
TEST_F(StringOwnershipE2E, InitFromCall) {
    EXPECT_EQ(runSource(R"(
        string fresh() { return string.new("abc"); }
        int32 main() {
            string s = fresh();
            string_view v = s;
            if (v != "abc") { return 1; }
            s.destroy();
            return 0;
        }
    )", "own_init_call.cdd"), 0);
}

// I4 (D2, 默认安全): `string s = flag ? a : b;`——三元非调用 → 深拷贝，
// a/b 保持有效可 destroy（review focus: 三元含活变量 → 拷贝）。
TEST_F(StringOwnershipE2E, InitTernaryCopiesOperands) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string a = string.new("aa");
            string b = string.new("bb");
            bool flag = true;
            string s = flag ? a : b;
            a.destroy();
            b.destroy();
            string_view v = s;
            if (v != "aa") { return 1; }
            s.destroy();
            return 0;
        }
    )", "own_init_ternary.cdd"), 0);
}

// I4 (D2/D6): `kill(a)`（string 按值传参）必须深拷贝——callee 内 destroy
// 释放的是形参副本缓冲，调用方 a 仍有效可 destroy（I4 路径 2；当前浅拷贝
// → callee destroy 释放共享缓冲 → 调用方 a 双 free abort）。
TEST_F(StringOwnershipE2E, PassByValueIndependent) {
    EXPECT_EQ(runSource(R"(
        void kill(string s) { s.destroy(); }
        int32 main() {
            string a = string.new("data");
            kill(a);
            a.destroy();
            return 0;
        }
    )", "own_pass.cdd"), 0);
}

// I4 (D6): `return s;`（局部 string）→ 移动——返回值独占缓冲、源存储置零，
// 双 destroy 安全（moved-from s 的 free(null) 无害）。
TEST_F(StringOwnershipE2E, ReturnMovesLocal) {
    EXPECT_EQ(runSource(R"(
        string f() {
            string s = string.new("x");
            return s;
        }
        int32 main() {
            string r = f();
            string_view v = r;
            if (v != "x") { return 1; }
            r.destroy();
            return 0;
        }
    )", "own_ret_local.cdd"), 0);
}

// I4 (D6, review focus): `return g.s;`（struct 字段 string 返回）→ 拷贝，
// 不得移动字段损坏 g——g.s 仍可 destroy，返回值独立。
TEST_F(StringOwnershipE2E, ReturnFieldCopies) {
    EXPECT_EQ(runSource(R"(
        struct S { string s; };
        struct S g;
        string get() { return g.s; }
        int32 main() {
            g.s = string.new("data");
            string r = get();
            g.s.destroy();
            string_view v = r;
            if (v != "data") { return 1; }
            r.destroy();
            return 0;
        }
    )", "own_ret_field.cdd"), 0);
}

// I4 (D6, review focus): `return flag ? ga : gb;`（三元含活变量）→ 拷贝，
// ga/gb 保持有效可 destroy（若移动则与返回值共享缓冲 → 双 free）。
TEST_F(StringOwnershipE2E, ConditionalReturnCopiesOperands) {
    EXPECT_EQ(runSource(R"(
        string ga;
        string gb;
        string pick(bool flag) { return flag ? ga : gb; }
        int32 main() {
            ga = string.new("aa");
            gb = string.new("bb");
            string r = pick(true);
            ga.destroy();
            gb.destroy();
            string_view v = r;
            if (v != "aa") { return 1; }
            r.destroy();
            return 0;
        }
    )", "own_ret_ternary.cdd"), 0);
}

// I4 (D6): `check(string.new("tmp"))`——新鲜实参 → 移动（零拷贝），
// 形参独占缓冲，值正确、destroy 一次即安全。
TEST_F(StringOwnershipE2E, TempArgMove) {
    EXPECT_EQ(runSource(R"(
        int32 check(string s) {
            string_view v = s;
            if (v != "tmp") { return 1; }
            s.destroy();
            return 0;
        }
        int32 main() {
            return check(string.new("tmp"));
        }
    )", "own_temp_arg.cdd"), 0);
}

// I4 (D7, spec §3.1/§5): `struct S y = x;`（聚合初始化含 string 字段）必须
// 字段感知深拷贝——x.s/y.s 各自独立缓冲，双双 destroy 不得双 free abort
// （I4 路径 3；当前整段浅拷贝 → 字段共享缓冲 → 双 free）。
TEST_F(StringOwnershipE2E, StructFieldCopyIndependent) {
    EXPECT_EQ(runSource(R"(
        struct S { string s; };
        int32 main() {
            struct S x = { string.new("x") };
            struct S y = x;
            x.s.destroy();
            y.s.destroy();
            return 0;
        }
    )", "own_struct_init.cdd"), 0);
}

// I4 (D7): 聚合赋值 `b = a;` 覆盖旧 string 字段——free 旧缓冲 + 深拷贝，
// 值正确、无泄漏/双 free。
TEST_F(StringOwnershipE2E, StructWithStringAssignFreesOld) {
    EXPECT_EQ(runSource(R"(
        struct S { string s; };
        int32 main() {
            struct S a = { string.new("aa") };
            struct S b = { string.new("bb") };
            b = a;
            a.s.destroy();
            string_view v = b.s;
            if (v != "aa") { return 1; }
            b.s.destroy();
            return 0;
        }
    )", "own_struct_assign.cdd"), 0);
}

// I4 (D7): brace-init string 字段活值深拷贝——`struct S y = { src };` 后
// y.s 独立于 src（src destroy 不影响 y.s）。
TEST_F(StringOwnershipE2E, BraceInitStringFieldCopies) {
    EXPECT_EQ(runSource(R"(
        struct S { string s; };
        int32 main() {
            string src = string.new("data");
            struct S y = { src };
            src.destroy();
            string_view v = y.s;
            if (v != "data") { return 1; }
            y.s.destroy();
            return 0;
        }
    )", "own_struct_brace.cdd"), 0);
}

// Found issue（I4 review）: 未初始化 `string s;` 零初始化 {null,0,0}——
// fresh 赋值 free 守卫安全（不 free 野指针）、值正确、destroy 一次即安全。
TEST_F(StringOwnershipE2E, UninitializedStringLocalAssignSafe) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s;
            s = string.new("x");
            string_view v = s;
            if (v != "x") { return 1; }
            s.destroy();
            return 0;
        }
    )", "own_uninit_safe.cdd"), 0);
}

// Found issue（I4 review）: 聚合内 string 数组字段——字段感知逐元素拷贝。
// `struct S y = x;` 后 y.arr[i] 独立于 x.arr[i]，双双 destroy 不得双 free。
TEST_F(StringOwnershipE2E, AggregateArrayOfStringsCopyIndependent) {
    EXPECT_EQ(runSource(R"(
        struct S { string arr[2]; };
        int32 main() {
            struct S x;
            x.arr[0] = string.new("a");
            x.arr[1] = string.new("b");
            struct S y = x;
            x.arr[0].destroy();
            x.arr[1].destroy();
            string_view v0 = y.arr[0];
            string_view v1 = y.arr[1];
            if (v0 != "a" || v1 != "b") { return 1; }
            y.arr[0].destroy();
            y.arr[1].destroy();
            return 0;
        }
    )", "own_agg_array.cdd"), 0);
}

// Found issue（I4 review）: 聚合内 optional<string> 字段——valid 时字段感知
// 深拷贝。`struct T y = x;` 后 x.o.value/y.o.value 独立缓冲。
TEST_F(StringOwnershipE2E, AggregateOptionalStringCopyIndependent) {
    EXPECT_EQ(runSource(R"(
        struct T { optional<string> o; };
        int32 main() {
            struct T x;
            x.o.valid = true;
            x.o.value = string.new("data");
            struct T y = x;
            x.o.value.destroy();
            string_view v = y.o.value;
            if (!y.o.valid) { return 1; }
            if (v != "data") { return 2; }
            y.o.value.destroy();
            return 0;
        }
    )", "own_agg_opt.cdd"), 0);
}

// Found issue（I4 review）: 聚合内 result<string, int32> 字段——ok 时字段
// 感知深拷贝。`struct U y = x;` 后 x.r.value/y.r.value 独立缓冲。
TEST_F(StringOwnershipE2E, AggregateResultStringCopyIndependent) {
    EXPECT_EQ(runSource(R"(
        struct U { result<string, int32> r; };
        int32 main() {
            struct U x;
            x.r.ok = true;
            x.r.value = string.new("ok");
            x.r.error = 0;
            struct U y = x;
            x.r.value.destroy();
            string_view v = y.r.value;
            if (!y.r.ok) { return 1; }
            if (v != "ok") { return 2; }
            y.r.value.destroy();
            return 0;
        }
    )", "own_agg_res.cdd"), 0);
}

// Found issue（I4 review）: 聚合赋值 `y = x;`（optional<string> 字段，
// assign=true 路径）——free y 旧缓冲 + 深拷贝，值正确、无泄漏/双 free。
TEST_F(StringOwnershipE2E, AggregateOptionalAssignFreesOld) {
    EXPECT_EQ(runSource(R"(
        struct T { optional<string> o; };
        int32 main() {
            struct T x;
            x.o.valid = true;
            x.o.value = string.new("data");
            struct T y;
            y.o.valid = true;
            y.o.value = string.new("old");
            y = x;
            x.o.value.destroy();
            string_view v = y.o.value;
            if (!y.o.valid) { return 1; }
            if (v != "data") { return 2; }
            y.o.value.destroy();
            return 0;
        }
    )", "own_agg_opt_assign.cdd"), 0);
}

// Found issue（I4 review 顺带发现）: struct 字段 optional<string> 的嵌套
// brace-init `{ { true, string.new("data") } }`——sema 的
// InitializerListExprAST 访问此前只处理首元素，`string.new` 静态调用未被
// 解析 → codegen 报 `unknown variable: string`。修复后应直接可用。
TEST_F(StringOwnershipE2E, NestedBraceInitOptionalStringField) {
    EXPECT_EQ(runSource(R"(
        struct T { optional<string> o; };
        int32 main() {
            struct T x = { { true, string.new("data") } };
            if (!x.o.valid) { return 1; }
            string_view v = x.o.value;
            if (v != "data") { return 2; }
            x.o.value.destroy();
            return 0;
        }
    )", "own_brinit_opt.cdd"), 0);
}

// Found issue（I4 review 顺带发现）: struct 字段 result<string, int32> 的嵌套
// brace-init `{ { true, string.new("ok"), 0 } }`——同上，修复后应直接可用。
TEST_F(StringOwnershipE2E, NestedBraceInitResultStringField) {
    EXPECT_EQ(runSource(R"(
        struct U { result<string, int32> r; };
        int32 main() {
            struct U x = { { true, string.new("ok"), 0 } };
            if (!x.r.ok) { return 1; }
            string_view v = x.r.value;
            if (v != "ok") { return 2; }
            x.r.value.destroy();
            return 0;
        }
    )", "own_brinit_res.cdd"), 0);
}

// 遗留缺陷（新遗留项）: optional/result 聚合赋值 valid→invalid（ok→err、
// err→ok）时，目标中"变为失效"的字段槽从不释放——旧 string 缓冲泄漏，且槽
// 位残留旧缓冲（破坏"valid⟹value 含缓冲"不变量）。可观测点：失效后读伪字段
// 槽位，泄漏代码残留旧内容（len>0），修复后应已 free+置零（len==0）。
TEST_F(StringOwnershipE2E, OptionalAssignValidToInvalidReleasesOld) {
    EXPECT_EQ(runSource(R"(
        struct T { optional<string> o; };
        int32 main() {
            struct T x = { { true, string.new("hello") } };
            struct T y = { { false, string.new("w") } };
            x = y;
            if (x.o.valid) { return 1; }
            string_view v = x.o.value;
            if (v.len() != 0) { return 2; }
            return 0;
        }
    )", "own_opt_v2i.cdd"), 0);
}

// 同上——optional 元素为含 string 字段的 struct：递归释放失效槽内聚合的缓冲。
TEST_F(StringOwnershipE2E, OptionalAssignValidToInvalidReleasesStructField) {
    EXPECT_EQ(runSource(R"(
        struct S { string s; };
        struct T { optional<struct S> o; };
        int32 main() {
            struct T x = { { true, { string.new("hello") } } };
            struct T y = { { false, { string.new("w") } } };
            x = y;
            if (x.o.valid) { return 1; }
            string_view v = x.o.value.s;
            if (v.len() != 0) { return 2; }
            return 0;
        }
    )", "own_opt_v2i_struct.cdd"), 0);
}

// 同上——optional 元素为含 string 数组字段的 struct：递归逐元素释放。
TEST_F(StringOwnershipE2E, OptionalAssignValidToInvalidReleasesArrayField) {
    EXPECT_EQ(runSource(R"(
        struct S { string arr[2]; };
        struct T { optional<struct S> o; };
        int32 main() {
            struct T x = { { true, { { string.new("a"), string.new("b") } } } };
            struct T y = { { false, { { string.new("c"), string.new("d") } } } };
            x = y;
            if (x.o.valid) { return 1; }
            string_view v0 = x.o.value.arr[0];
            string_view v1 = x.o.value.arr[1];
            if (v0.len() != 0 || v1.len() != 0) { return 2; }
            return 0;
        }
    )", "own_opt_v2i_arr.cdd"), 0);
}

// 同上——result ok→err：失效的 value 槽释放。
TEST_F(StringOwnershipE2E, ResultAssignOkToErrReleasesOldValue) {
    EXPECT_EQ(runSource(R"(
        struct U { result<string, int32> r; };
        int32 main() {
            struct U x = { { true, string.new("ok"), 0 } };
            struct U y = { { false, string.new("x"), 7 } };
            x = y;
            if (x.r.ok) { return 1; }
            string_view v = x.r.value;
            if (v.len() != 0) { return 2; }
            return 0;
        }
    )", "own_res_o2e.cdd"), 0);
}

// 同上——result err→ok：失效的 error 槽释放（error 为 string）。
TEST_F(StringOwnershipE2E, ResultAssignErrToOkReleasesOldError) {
    EXPECT_EQ(runSource(R"(
        struct U { result<string, string> r; };
        int32 main() {
            struct U x = { { false, string.new("v"), string.new("err") } };
            struct U y = { { true, string.new("ok"), string.new("e") } };
            x = y;
            if (!x.r.ok) { return 1; }
            string_view e = x.r.error;
            if (e.len() != 0) { return 2; }
            return 0;
        }
    )", "own_res_e2o.cdd"), 0);
}

// 同上——valid→invalid 释放并置零后，再 invalid→valid 不得双 free（value 槽
// 已零化，assign helper 的 free(null) 无害），值正确。
TEST_F(StringOwnershipE2E, OptionalInvalidateThenRevalidateSafe) {
    EXPECT_EQ(runSource(R"(
        struct T { optional<string> o; };
        int32 main() {
            struct T x = { { true, string.new("hello") } };
            struct T y = { { false, string.new("w") } };
            x = y;
            struct T z = { { true, string.new("back") } };
            x = z;
            if (!x.o.valid) { return 1; }
            string_view v = x.o.value;
            if (v != "back") { return 2; }
            x.o.value.destroy();
            return 0;
        }
    )", "own_opt_retrans.cdd"), 0);
}
