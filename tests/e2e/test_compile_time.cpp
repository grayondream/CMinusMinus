// P1-04 / CT-04/05/12: 目标/构建查询端到端 + LLVM 常量集成（零运行时指令）。

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

class CompileTimeE2E : public ::testing::Test {
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

// 完整管线：parse → sema（buildDebug 注入）→ codegen → JIT 执行 main。
static int runSourceImpl(const std::string& source, const std::string& filename,
                         bool buildDebug) {
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) {
        ADD_FAILURE() << "Parse failed";
        return -1;
    }

    SemanticAnalyzer analyzer;
    analyzer.setBuildConfig(buildDebug, "O0");
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

static int runSource(const std::string& source, const std::string& filename) {
    return runSourceImpl(source, filename, false);
}

TEST_F(CompileTimeE2E, TargetOsIsLinux) {
    // host 上 target.os 必为 "linux"（本测试环境）；== 折叠为编译期常量。
    int r = runSource(R"(
constexpr int32 isLinux = compile_time.target.os == "linux";
int32 main() { return isLinux ? 1 : 0; }
)", "ct_target_os.cmm");
    EXPECT_EQ(r, 1);
}

TEST_F(CompileTimeE2E, BuildDebugSetter) {
    const char* src = R"(
int32 main() { bool d = compile_time.build.debug; return d ? 1 : 0; }
)";
    EXPECT_EQ(runSourceImpl(src, "ct_debug_true.cmm", true), 1);
    EXPECT_EQ(runSourceImpl(src, "ct_debug_false.cmm", false), 0);
}

// ---- P1-04 / CT-06: 布局查询（size_of/align_of/offset_of）----

TEST_F(CompileTimeE2E, SizeOfScalars) {
    int r = runSource(R"(
constexpr usize s32 = compile_time.size_of(int32);
constexpr usize s64 = compile_time.size_of(float64);
constexpr usize s8 = compile_time.size_of(int8);
constexpr usize sb = compile_time.size_of(bool);
constexpr usize sp = compile_time.size_of(int32*);
int32 main() {
    return (s32 == 4 && s64 == 8 && s8 == 1 && sb == 1 && sp == 8) ? 1 : 0;
}
)", "ct_sizeof_scalars.cmm");
    EXPECT_EQ(r, 1);
}

TEST_F(CompileTimeE2E, SizeOfClassWithBase) {
    // Review Focus 4: 基类子对象占槽 0，布局须与 codegen DataLayout 一致。
    int r = runSource(R"(
class B { public: int32 b; }
class D : B { public: float64 v; }
int32 main() {
    return (compile_time.size_of(D) == 16 && compile_time.offset_of(D, "v") == 8) ? 1 : 0;
}
)", "ct_sizeof_base.cmm");
    EXPECT_EQ(r, 1);
}

TEST_F(CompileTimeE2E, SizeOfNestedStructUnion) {
    int r = runSource(R"(
struct Inner { int32 a; float64 b; }
union U { int32 i; float64 f; }
struct Outer { Inner in; U u; int16 c; }
int32 main() {
    return (compile_time.size_of(Outer) == 32
         && compile_time.offset_of(Outer, "u") == 16
         && compile_time.offset_of(Outer, "c") == 24
         && compile_time.size_of(U) == 8
         && compile_time.align_of(Outer) == 8) ? 1 : 0;
}
)", "ct_sizeof_nested.cmm");
    EXPECT_EQ(r, 1);
}

TEST_F(CompileTimeE2E, ConstVarFromCompileTime) {
    int r = runSource(R"(
constexpr usize N = compile_time.size_of(float64);
int32 main() { return N == 8 ? 1 : 0; }
)", "ct_constvar.cmm");
    EXPECT_EQ(r, 1);
}

TEST_F(CompileTimeE2E, ConstExprFnInCompileTime) {
    // Review Focus 3: constexpr 函数在 compile_time 表达式内（委托路径）。
    int r = runSource(R"(
constexpr int32 twice(int32 x) { return x * 2; }
constexpr int32 r = compile_time.size_of(int32) * 0 + twice(21);
int32 main() { return r == 42 ? 1 : 0; }
)", "ct_constexpr_fn.cmm");
    EXPECT_EQ(r, 1);
}

TEST_F(CompileTimeE2E, StaticAssertE2E) {
    int r = runSource(R"(
compile_time.static_assert(compile_time.target.os == "linux", "this test runs on linux");
compile_time.static_assert(compile_time.size_of(int32) == 4, "int32 is 4 bytes");
int32 main() { return 0; }
)", "ct_static_assert.cmm");
    EXPECT_EQ(r, 0);
}

// ---- P1-04 / CT-03 / SEM-07: compile_time.if 条件编译 ----

TEST_F(CompileTimeE2E, CompileTimeIfSelectsThen) {
    // 假分支引用未定义符号——若被出码，链接/验证必失败（死代码消除证据）。
    int r = runSource(R"(
compile_time.if (compile_time.target.os == "linux") {
    int32 f() { return 42; }
} else {
    int32 f() { return NoSuchSymbolAnywhere(); }
}
int32 main() { return f(); }
)", "ct_if_then.cmm");
    EXPECT_EQ(r, 42);
}

TEST_F(CompileTimeE2E, CompileTimeIfSelectsElse) {
    int r = runSource(R"(
compile_time.if (1 == 2) {
    int32 g() { return NoSuchSymbolAnywhere(); }
} else {
    int32 g() { return 7; }
}
int32 main() { return g(); }
)", "ct_if_else.cmm");
    EXPECT_EQ(r, 7);
}

TEST_F(CompileTimeE2E, DeadBranchTypeUseDiagnosed) {
    // Review Focus 1: 未选分支 parse 期注册的类型名被使用 → 诊断不崩溃。
    Lexer lexer("ct_if_dead.cmm", R"(
compile_time.if (1 == 2) {
    struct Ghost { int32 x; }
} else {
    int32 h() { return 1; }
}
int32 main() { Ghost g; return g.x; }
)");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool found = false;
    for (const auto& d : analyzer.getErrors()) {
        // 评审 M3: 消息须点名类型（非泛化类型错误）。
        if (d.message.find("Ghost") != std::string::npos
            && d.message.find("non-selected compile_time.if branch") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

// ---- CTX-01: developer ergonomics, batch 1 --------------------------------

TEST_F(CompileTimeE2E, ErgonomicsScalarQueries) {
    int r = runSource(R"(
constexpr bool same = compile_time.is_same(int32, int32);
int32 main() {
    compile_time.assert_size(int32, 4);
    compile_time.assert_align(int32, 4);
    compile_time.static_assert(compile_time.line > 0, "line");
    if (!same) return 1;
    if (compile_time.int_max(int32) != 2147483647) return 2;
    if (compile_time.bit_width(int64) != 64) return 3;
    if (compile_time.target.pointer_width != 64) return 4;
    if (compile_time.version.major != 1) return 5;
    if (!compile_time.version.at_least("1.0.0")) return 6;
    return 42;
}
)", "ct_ergonomics_scalar.cmm");
    EXPECT_EQ(r, 42);
}

TEST_F(CompileTimeE2E, ErgonomicsReflectionQueries) {
    int r = runSource(R"(
[[align(16)]] struct E2ES { int32 a; int8 b; };
int32 main() {
    if (!compile_time.has_field(E2ES, "a")) return 1;
    if (compile_time.field_count_of(E2ES) != 2) return 2;
    if (compile_time.field_offset_of(E2ES, "b") != 4) return 3;
    if (!compile_time.has_attribute(E2ES, "align")) return 4;
    if (compile_time.attribute_arg_of(E2ES, "align", 0) != 16) return 5;
    return 42;
}
)", "ct_ergonomics_reflection.cmm");
    EXPECT_EQ(r, 42);
}

TEST_F(CompileTimeE2E, ErgonomicsBatch2Queries) {
    int r = runSource(R"(
enum E2EColor { E2ERed, E2EGreen = 5, E2EBlue };
struct E2EInner { int32 x; int8 y; };
struct E2EOuter { int32 a; E2EInner in; };
class E2EBase { public: int32 f(int32 x); };
class E2EDerived : E2EBase { public: int32 g(); };
int32 main() {
    if (compile_time.enum_count_of(E2EColor) != 3) return 1;
    if (compile_time.enum_value_of(E2EColor, "E2EBlue") != 6) return 2;
    if (compile_time.offset_of(E2EOuter, "in.y") != 8) return 3;
    if (compile_time.padding_of(E2EInner) != 3) return 4;
    if (!compile_time.has_method(E2EDerived, "f")) return 5;
    if (!compile_time.is_base_of(E2EBase, E2EDerived)) return 6;
    if (compile_time.format("%d/%s/%x", 42, "ok", 255) != "42/ok/ff") return 7;
    if (compile_time.to_string(true) != "true") return 8;
    if (compile_time.type_kind_of(1) != "integer") return 9;
    return 42;
}
)", "ct_ergonomics_batch2.cmm");
    EXPECT_EQ(r, 42);
}

TEST_F(CompileTimeE2E, RepeatUnrollsAtCompileTime) {
    int r = runSource(R"(
int32 main() {
    int32 total = 0;
    compile_time.repeat(3) { total += 1; }
    return total;
}
)", "ct_repeat.cmm");
    EXPECT_EQ(r, 3);
}

TEST_F(CompileTimeE2E, RepeatBreakContinueAndTemplate) {
    EXPECT_EQ(runSource(R"(
int32 main() {
    int32 t = 0;
    compile_time.repeat(3) { t += 1; break; }
    return t;
}
)", "ct_repeat_break.cmm"), 1);

    EXPECT_EQ(runSource(R"(
int32 main() {
    int32 t = 0;
    compile_time.repeat(3) { continue; t += 1; }
    return t;
}
)", "ct_repeat_continue.cmm"), 0);

    EXPECT_EQ(runSource(R"(
template<typename T> int32 repeat_in_template() {
    int32 t = 0;
    compile_time.repeat(3) { t += 1; }
    return t;
}
int32 main() { return repeat_in_template<int32>(); }
)", "ct_repeat_template.cmm"), 3);
}

TEST_F(CompileTimeE2E, RepeatUsesSourceLocation) {
    EXPECT_EQ(runSource(R"(
int32 main() {
    int32 t = 0;
    compile_time.repeat(compile_time.line) { t += 1; }
    return t;
}
)", "ct_repeat_loc.cmm"), 4);
}

TEST_F(CompileTimeE2E, CastAndParenthesizedQueries) {
    EXPECT_EQ(runSource(R"(
int32 main() {
    int64 m = (int64)compile_time.int_max(int32);
    if (m != 2147483647) return 1;
    if ((int32)compile_time.line <= 0) return 2;
    return 42;
}
)", "ct_cast_query.cmm"), 42);
}

TEST_F(CompileTimeE2E, CTDepthLimitE2E) {
    // 深度超限：100 层嵌套加法 → 诊断，编译失败（e2e 复 pin）。
    std::string expr = "compile_time.size_of(int32) + ";
    for (int i = 0; i < 100; ++i) expr += "(";
    expr += "1";
    for (int i = 0; i < 100; ++i) expr += "+1)";
    std::string src = "int32 main() { int32 x = " + expr + "; return 0; }\n";
    Lexer lexer("ct_depth_e2e.cmm", src);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_FALSE(analyzer.getErrors().empty());
}
