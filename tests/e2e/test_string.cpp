// P1-06 (FMT-01/02/03): str / string 端到端执行测试。
// fixture 仿 test_optional_result.cpp 的 runSource（完整 JIT 执行，返回 main 的返回值）。

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

class StringE2E : public ::testing::Test {
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

// ---- T2: 字面量 = str，基础互操作 ----

TEST_F(StringE2E, LiteralToCharPointer) {
    // Review 2.5: str -> const char* 隐式（只读字节视图）。
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view s = "hi";
            const char* p = s;
            if (p[0] != 'h') { return 1; }
            if (p[1] != 'i') { return 2; }
            return 0;
        }
    )", "str1.cdd"), 0);
}

TEST_F(StringE2E, PrintStrArg) {
    // print/println 的 str 实参（%.*s 路径）可编译执行。
    EXPECT_EQ(runSource(R"(
        int32 main() {
            println("{}", "lit");
            print("{}x", "ab");
            return 0;
        }
    )", "str2.cdd"), 0);
}

TEST_F(StringE2E, StrCompareRuntime) {
    // == / != 走运行时 memcmp 路径（非常量、经函数返回的 str）。
    // 注意：bool 字面量直接作实参是既有解析局限（与 str 无关），经变量传递。
    EXPECT_EQ(runSource(R"(
        string_view pick(bool which) {
            if (which) { return "abc"; }
            return "abd";
        }
        int32 main() {
            bool t = true;
            bool f = false;
            string_view a = pick(t);
            string_view b = pick(f);
            string_view c = pick(t);
            if (a == b) { return 1; }
            if (a != c) { return 2; }
            if (a == c) { return 0; }
            return 3;
        }
    )", "str3.cdd"), 0);
}

// ---- T3: str 内建方法与 str_from_c ----

TEST_F(StringE2E, StrLen) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view s = "hello";
            if (s.len() != 5) { return 1; }
            return 0;
        }
    )", "str4.cdd"), 0);
}

TEST_F(StringE2E, StrCharCount) {
    // "héllo" = 6 bytes / 5 code points (é is 2 bytes).
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view s = "h\xc3\xa9llo";
            if (s.len() != 6) { return 1; }
            if (s.char_count() != 5) { return 2; }
            return 0;
        }
    )", "str5.cdd"), 0);
}

TEST_F(StringE2E, StrCharAt) {
    // Byte indexing: byte 1 of "héllo" is the lead byte 0xC3.
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view s = "h\xc3\xa9llo";
            if (s.char_at(1) != '\xC3') { return 1; }
            return 0;
        }
    )", "str6.cdd"), 0);
}

TEST_F(StringE2E, StrCharLenAt) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view s = "h\xc3\xa9llo";
            if (s.char_len_at(0) != 1) { return 1; }
            if (s.char_len_at(1) != 2) { return 2; }
            return 0;
        }
    )", "str7.cdd"), 0);
}

TEST_F(StringE2E, StrFromCExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            const char* p = "abc";
            string_view s = str_from_c(p);
            if (s.len() != 3) { return 1; }
            return 0;
        }
    )", "str8.cdd"), 0);
}

// ---- T4: string 内存与方法 ----

TEST_F(StringE2E, NewLenExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("hello");
            if (s.len() != 5) { return 1; }
            return 0;
        }
    )", "str9.cdd"), 0);
}

TEST_F(StringE2E, AppendExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("ab");
            s.append("cd");
            if (s.len() != 4) { return 1; }
            if (s[2] != 'c') { return 2; }
            return 0;
        }
    )", "str10.cdd"), 0);
}

TEST_F(StringE2E, AppendGrowthExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("");
            int32 i = 0;
            while (i < 100) {
                s.append("ab");
                i = i + 1;
            }
            if (s.len() != 200) { return 1; }
            if (s[198] != 'a' || s[199] != 'b') { return 2; }
            return 0;
        }
    )", "str11.cdd"), 0);
}

TEST_F(StringE2E, PushExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("");
            s.push('x');
            s.push('y');
            s.push('z');
            if (s.len() != 3) { return 1; }
            if (s[2] != 'z') { return 2; }
            return 0;
        }
    )", "str12.cdd"), 0);
}

TEST_F(StringE2E, CapacityExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("hi");
            if (s.capacity() < 2) { return 1; }
            s.append("abc");
            if (s.len() != 5) { return 2; }
            if (s.capacity() < 5) { return 3; }
            return 0;
        }
    )", "str13.cdd"), 0);
}

TEST_F(StringE2E, DestroyFrees) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("data");
            s.destroy();
            if (s.len() != 0) { return 1; }
            if (s.capacity() != 0) { return 2; }
            return 0;
        }
    )", "str14.cdd"), 0);
}

TEST_F(StringE2E, StringToStrView) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("ab");
            string_view v = s;
            if (v.len() != 2) { return 1; }
            return 0;
        }
    )", "str15.cdd"), 0);
}

TEST_F(StringE2E, AppendAcceptsString) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string a = string.new("x");
            string b = string.new("y");
            a.append(b);
            if (a.len() != 2) { return 1; }
            if (a[1] != 'y') { return 2; }
            return 0;
        }
    )", "str16.cdd"), 0);
}

TEST_F(StringE2E, SubscriptExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("abc");
            if (s[0] != 'a') { return 1; }
            if (s[1] != 'b') { return 2; }
            if (s[2] != 'c') { return 3; }
            return 0;
        }
    )", "str17.cdd"), 0);
}

// P1-06 / T5: str_from_c over a runtime buffer validates UTF-8 and panics on
// invalid input (E2015's runtime companion). Panic terminates, so we pin the
// IR shape instead of executing it.
static std::string compileStringIR(const std::string& source, const std::string& filename) {
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) return "";
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return "";
    CodegenContext ctx;
    ctx.setSourceFile(filename);
    ast->codegen(ctx);
    std::string ir;
    llvm::raw_string_ostream os(ir);
    ctx.getModule().print(os, nullptr);
    return ir;
}

TEST_F(StringE2E, StrFromCRuntimeValidates) {
    std::string ir = compileStringIR(R"(
        int32 fill(char* buf) {
            buf[0] = 'a';
            buf[1] = static_cast<char>(0xFF);
            return 2;
        }
        int32 main() {
            char buf[4];
            buf[2] = 0;
            fill(buf);
            string_view s = str_from_c(buf);
            return 0;
        }
    )", "str18.cdd");
    ASSERT_FALSE(ir.empty());
    EXPECT_NE(ir.find("cdd.utf8.validate"), std::string::npos);
}

TEST_F(StringE2E, StrFromCRuntimeValidExec) {
    // Valid runtime buffer: strlen path, no panic.
    EXPECT_EQ(runSource(R"(
        int32 main() {
            char buf[4];
            buf[0] = 'a';
            buf[1] = 'b';
            buf[2] = 'c';
            buf[3] = 0;
            string_view s = str_from_c(buf);
            if (s.len() != 3) { return 1; }
            return 0;
        }
    )", "str19.cdd"), 0);
}

// ---- T6: 硬化与边界（Review Focus pin）----

TEST_F(StringE2E, DoubleDestroySafe) {
    // destroy() 清零字段 → 第二次 destroy 走 free(NULL)，不崩。
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("data");
            s.destroy();
            s.destroy();
            if (s.len() != 0) { return 1; }
            if (s.capacity() != 0) { return 2; }
            return 0;
        }
    )", "str21.cdd"), 0);
}

TEST_F(StringE2E, DataIsNulTerminatedForLibcInterop) {
    // string.data is handed to C APIs (getenv/fopen/strlen). Every mutation
    // path must keep the implicit terminator even though len/cap track bytes.
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("hi");
            if (s.data[2] != 0) { return 1; }
            s.append("abc");
            if (s.data[5] != 0) { return 2; }
            s.push('!');
            if (s.data[6] != 0) { return 3; }
            if (s.data[0] != 'h' || s.data[4] != 'c' || s.data[5] != '!') { return 4; }
            s.destroy();
            return 0;
        }
    )", "str23.cdd"), 0);
}

TEST_F(StringE2E, EmptyStringInvariants) {
    // 空串：len 0、cap >= 1（ptr 保证非空，str→char* 视图安全）。
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("");
            if (s.len() != 0) { return 1; }
            if (s.capacity() < 1) { return 2; }
            const char* p = s;
            // malloc 不清零：p[0] 内容未定义。不变量 = p 非空（cap>=1 保证）。
            if (p == null) { return 3; }
            return 0;
        }
    )", "str22.cdd"), 0);
}

TEST_F(StringE2E, SubscriptCompilesWithoutBoundsCheck) {
    // DEC-06 未决：不加运行时边界检查。只编译执行合法下标；
    // 越界行为本测试不执行（结果未定义，DEC-06 落地时另行裁决）。
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("abc");
            if (s[2] != 'c') { return 1; }
            return 0;
        }
    )", "str23.cdd"), 0);
}

// 终审 I3（修复轮守卫）：自别名 append —— 源缓冲区可能与接收者相同。
TEST_F(StringE2E, AppendSelfAlias) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = string.new("ab");
            s.append(s);
            if (s.len() != 4) { return 1; }
            if (s[2] != 'a' || s[3] != 'b') { return 2; }
            return 0;
        }
    )", "str24.cdd"), 0);
}

// ===== P1-09 (FMT-07/08/12): builtin format() 值断言 =====
// 断言模式：format 结果经 `str v = s;` 投影后与字面量 == 比较，main 返回码即断言。

TEST_F(StringE2E, FormatBasicExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = format("x={} y={}", 7, -3);
            string_view v = s;
            if (v == "x=7 y=-3") { return 0; }
            return 1;
        }
    )", "fmt01.cdd"), 0);
}

TEST_F(StringE2E, FormatEmptyExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = format("");
            if (s.len() != 0) { return 1; }
            return 0;
        }
    )", "fmt02.cdd"), 0);
}

TEST_F(StringE2E, FormatSpecsExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = format("{:x} {:X} {:o} {:b}", 255, 255, 8, 5);
            string_view v = s;
            if (v == "ff FF 10 101") { return 0; }
            return 1;
        }
    )", "fmt03.cdd"), 0);
}

TEST_F(StringE2E, FormatZeroPadExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = format("[{:02}:{:02}]", 5, 45);
            string_view v = s;
            if (v == "[05:45]") { return 0; }
            return 1;
        }
    )", "fmt04.cdd"), 0);
}

TEST_F(StringE2E, FormatFloatPrecExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = format("{:.2f} {:e}", 3.14159, 31415.926);
            string_view v = s;
            if (v == "3.14 3.141593e+04") { return 0; }
            return 1;
        }
    )", "fmt05.cdd"), 0);
}

TEST_F(StringE2E, FormatWidthAlignExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = format("{:>5}|{:<5}|", 42, 42);
            string_view v = s;
            if (v == "   42|42   |") { return 0; }
            return 1;
        }
    )", "fmt06.cdd"), 0);
}

TEST_F(StringE2E, FormatCenteredExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = format("{:^5}|", 42);
            string_view v = s;
            if (v == " 42  |") { return 0; }
            return 1;
        }
    )", "fmt07.cdd"), 0);
}

TEST_F(StringE2E, FormatCustomFillExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = format("{:*<5}", 42);
            string_view v = s;
            if (v == "42***") { return 0; }
            return 1;
        }
    )", "fmt08.cdd"), 0);
}

TEST_F(StringE2E, FormatStrExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = format("s={} t={}", "ab", "cd");
            string_view v = s;
            if (v == "s=ab t=cd") { return 0; }
            return 1;
        }
    )", "fmt09.cdd"), 0);
}

TEST_F(StringE2E, FormatStrNulPreserved) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string b = string.new("a");
            b.push((char)0);
            b.push('b');
            string s = format("{}", b);
            if (s.len() != 3) { return 1; }
            if (s[0] != 'a') { return 2; }
            if (s[2] != 'b') { return 3; }
            return 0;
        }
    )", "fmt10.cdd"), 0);
}

TEST_F(StringE2E, FormatStringArgExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = format("{}", string.new("vv"));
            string_view v = s;
            if (v == "vv") { return 0; }
            return 1;
        }
    )", "fmt11.cdd"), 0);
}

TEST_F(StringE2E, FormatToStringExec) {
    EXPECT_EQ(runSource(R"(
        class P { public: int32 x; public: const char* to_string() { return "P!"; } };
        int32 main() {
            P p;
            string s = format("{}", p);
            string_view v = s;
            if (v == "P!") { return 0; }
            return 1;
        }
    )", "fmt12.cdd"), 0);
}

TEST_F(StringE2E, FormatEscapesExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = format("{{a}}");
            string_view v = s;
            if (v == "{a}") { return 0; }
            return 1;
        }
    )", "fmt13.cdd"), 0);
}

TEST_F(StringE2E, FormatDynExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view f = "{}-{}";
            string s = format(f, 1, 2);
            string_view v = s;
            if (v == "1-2") { return 0; }
            return 1;
        }
    )", "fmt14.cdd"), 0);
}

TEST_F(StringE2E, FormatDynEscapeExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view f = "a{{b";
            string s = format(f);
            string_view v = s;
            if (v == "a{b") { return 0; }
            return 1;
        }
    )", "fmt15.cdd"), 0);
}

TEST_F(StringE2E, FormatDynExtraExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view f = "{}";
            string s = format(f, 1, 2);
            string_view v = s;
            if (v == "1") { return 0; }
            return 1;
        }
    )", "fmt16.cdd"), 0);
}

// Review Focus 2: dynamic format strings ignore spec text — args render with
// their default conversion (documented in stdlib.md).
TEST_F(StringE2E, FormatDynSpecIgnored) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view f = "{:x}";
            string s = format(f, 10);
            string_view v = s;
            if (v == "10") { return 0; }
            return 1;
        }
    )", "fmt17.cdd"), 0);
}

TEST_F(StringE2E, FormatDynShortExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view f = "{} {}";
            string s = format(f, 1);
            string_view v = s;
            if (v == "1 ") { return 0; }
            return 1;
        }
    )", "fmt18.cdd"), 0);
}

TEST_F(StringE2E, FormatLargeExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string s = format("{} {} {} {} {} {} {} {} {} {}",
                              1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
            string_view v = s;
            if (v == "1 2 3 4 5 6 7 8 9 10") { return 0; }
            return 1;
        }
    )", "fmt19.cdd"), 0);
}

// print/println 的渲染槽位（^ / 自定义 fill）冒烟：编译执行不崩即通过。
TEST_F(StringE2E, PrintRenderSlotSmoke) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            println("{:^7}", "ab");
            println("{:*<6}", 5);
            println("{:b}", 5);
            return 0;
        }
    )", "fmt20.cdd"), 0);
}

// ===== P1-09 (STD-10): find/rfind/sub 内建原语 =====

TEST_F(StringE2E, FindRFindExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view s = "hello";
            if (s.find("ll") != 2) { return 1; }
            if (s.find("z") != -1) { return 2; }
            if (s.find("") != 0) { return 3; }
            string_view t = "a/b/c";
            if (t.rfind("/") != 3) { return 4; }
            if (t.rfind("z") != -1) { return 5; }
            if (t.rfind("") != 5) { return 6; }
            return 0;
        }
    )", "str19.cdd"), 0);
}

TEST_F(StringE2E, SubExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view s = "hello";
            string_view a = s.sub(1, 3);
            if (a == "el") { return 0; }
            string_view b = s.sub(0, 5);
            if (b == "hello") { return 0; }
            string_view c = s.sub(2, 2);
            if (c.len() == 0) { return 0; }
            return 9;
        }
    )", "str20.cdd"), 0);
}

TEST_F(StringE2E, FindOnStringView) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string src = string.new("xabx");
            string_view v = src;
            if (v.find("b") != 2) { return 1; }
            return 0;
        }
    )", "str22.cdd"), 0);
}

TEST_F(StringE2E, SubBoundsPanicPath) {
    // 越界 panic 不可执行（abort）；钉编译路径存在。
    std::string ir = compileStringIR(R"(
        int32 main() {
            string_view s = "hello";
            string_view a = s.sub(3, 2);
            return 0;
        }
    )", "str21.cdd");
    ASSERT_FALSE(ir.empty());
}

// ===== P1-09: split/split_destroy 内建（零拷贝 []str 视图） =====

TEST_F(StringE2E, SplitExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view[] parts = split("a,b,c", ",");
            if (parts.len != 3) { return 1; }
            if (parts[1] == "b") { return 0; }
            return 2;
        }
    )", "str23.cdd"), 0);
}

TEST_F(StringE2E, SplitNoSepExec) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view[] parts = split("abc", ",");
            if (parts.len != 1) { return 1; }
            if (parts[0] == "abc") { return 0; }
            return 2;
        }
    )", "str24.cdd"), 0);
}

TEST_F(StringE2E, SplitTailEmpty) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string_view[] parts = split("a,", ",");
            if (parts.len != 2) { return 1; }
            if (parts[0] == "a") { return 0; }
            if (parts[1].len() == 0) { return 0; }
            return 2;
        }
    )", "str25.cdd"), 0);
}

TEST_F(StringE2E, SplitViewAndDestroy) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            string src = string.new("x,y");
            string_view[] parts = split(src, ",");
            if (parts.len != 2) { return 1; }
            if (parts[0] == "x") { return 0; }
            split_destroy(parts);
            return 0;
        }
    )", "str26.cdd"), 0);
}

// P1-09: && / || 为 C 语义逻辑运算符——判零真值 + 短路求值。
// （修复前：按位 And/Or，8192 之类高位真值被截断为假、RHS 永远求值。）
TEST_F(StringE2E, LogicalOpsTruthinessAndShortCircuit) {
    EXPECT_EQ(runSource(R"(
        int32 calls = 0;
        int32 side() { calls = calls + 1; return 1; }
        int32 main() {
            int32 bad = 0;
            int32 a = 8192;
            int32 b = 4;
            if (!(a && b)) { bad = bad + 1; }
            if (a && 0) { bad = bad + 2; }
            int32 z = 0;
            if (z && side()) { bad = bad + 4; }
            if (calls != 0) { bad = bad + 8; }
            if (!(a && side())) { bad = bad + 16; }
            if (calls != 1) { bad = bad + 32; }
            if (!(z || side())) { bad = bad + 64; }
            if (calls != 2) { bad = bad + 128; }
            if (a || side()) { }
            if (calls != 2) { bad = bad + 256; }
            return bad;
        }
    )", "logic1.cdd"), 0);
}

// ===== 终审修复回归 =====

// C1: RHS 含控制流（str == 的 memcmp 块 / 嵌套 &&）时，PHI incoming 块
// 必须取 RHS 求值结束的块——修复前硬编码 RHS 起始块，JIT 管线段错误。
TEST_F(StringE2E, LogicalOpsBlockProducingRhs) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 bad = 0;
            int32 a = 8192;
            string_view s = "x";
            if (!(a && s == "x")) { bad = bad + 1; }
            if (a && s != "x") { bad = bad + 2; }
            int32 b = 1;
            int32 c = 0;
            if (!(a && (b || c))) { bad = bad + 4; }
            if (c && (b || c)) { bad = bad + 8; }
            if (!(a && s == "x" && b == 1)) { bad = bad + 16; }
            return bad;
        }
    )", "logic2.cdd"), 0);
}

// C2: snprintf 截断时返回「本应写入」的长度——chunk 长度必须 clamp 到
// 缓冲容量，否则越界读栈。钉长 to_string（>64 字节动态缓冲）与长浮点字面量。
TEST_F(StringE2E, FormatClampsTruncatedChunks) {
    EXPECT_EQ(runSource(R"(
        class Big { public: int32 v; public: const char* to_string() { return "0123456789012345678901234567890123456789012345678901234567890123456789"; } };
        int32 main() {
            Big big;
            string_view f = "[{}]";
            string s = format(f, big);
            if (s.len() != 72) { return static_cast<int32>(s.len()); }
            if (s[0] != '[') { return 2; }
            if (s[71] != ']') { return 3; }
            if (s[70] != '9') { return 4; }
            return 0;
        }
    )", "clamp1.cdd"), 0);
}
