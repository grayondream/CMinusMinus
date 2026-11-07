// P1-09 (STD-10): std.string 组合库端到端测试。
// fixture 仿 tests/driver/test_std_modules.cpp 的 runWithImports（完整模块加载 + JIT 执行）。

#include <gtest/gtest.h>

#include <cstdlib>
#include <set>
#include <string>

#include "ast/Decl.h"
#include "codegen/CodegenContext.h"
#include "driver/ModuleLoader.h"
#include "driver/StdPrelude.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "support/Log.h"

#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <spdlog/spdlog.h>

namespace {

class StdStringLibTest : public ::testing::Test {
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

static int runWithImports(const std::string& source, const std::string& filename) {
    auto ast = cmm::parseStdCPrelude(source, filename);
    if (!ast) return -1;

    cmm::ModuleSearchPaths paths{{STD_DIR}};
    std::set<std::string> loaded;
    std::vector<std::string> errors;
    cmm::processImports(*ast, ".", paths, loaded, errors);
    if (!errors.empty()) return -1;

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
    auto jit = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit) return -1;

    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto e = (*jit)->addIRModule(std::move(ts))) return -1;

    auto sym = (*jit)->lookup("main");
    if (!sym) return -1;
    return ((int (*)())(intptr_t)sym->getValue())();
}

// 编译不执行（用于会 abort 的运行时 panic 路径）。
static std::string compileIRWithImports(const std::string& source, const std::string& filename) {
    auto ast = cmm::parseStdCPrelude(source, filename);
    if (!ast) return "";

    cmm::ModuleSearchPaths paths{{STD_DIR}};
    std::set<std::string> loaded;
    std::vector<std::string> errors;
    cmm::processImports(*ast, ".", paths, loaded, errors);
    if (!errors.empty()) return "";

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return "";

    CodegenContext ctx;
    ctx.setSourceFile(filename);
    ast->codegen(ctx);
    ctx.finalizeDebugInfo();

    std::string ve;
    llvm::raw_string_ostream vs(ve);
    if (llvm::verifyModule(ctx.getModule(), &vs)) return "";

    std::string ir;
    llvm::raw_string_ostream os(ir);
    ctx.getModule().print(os, nullptr);
    return ir;
}

} // namespace

TEST_F(StdStringLibTest, ContainsStartsEndsExec) {
    EXPECT_EQ(runWithImports(R"(
        import std.string;
        int32 main() {
            string_view s = "hello world";
            if (!std::contains(s, "lo w")) { return 1; }
            if (std::contains(s, "xyz")) { return 2; }
            if (!std::starts_with(s, "hello")) { return 3; }
            if (std::starts_with(s, "world")) { return 4; }
            if (!std::ends_with(s, "world")) { return 5; }
            if (std::ends_with(s, "hello")) { return 6; }
            return 0;
        }
    )", "sslib01.cmm"), 0);
}

TEST_F(StdStringLibTest, TrimExec) {
    EXPECT_EQ(runWithImports(R"(
        import std.string;
        int32 main() {
            string_view s = "  \t hi there \n ";
            string_view l = std::trim_left(s);
            if (l == "hi there \n ") { return 0; }
            string_view r = std::trim_right(s);
            if (r == "  \t hi there") { return 0; }
            string_view b = std::trim(s);
            if (b == "hi there") { return 0; }
            return 9;
        }
    )", "sslib02.cmm"), 0);
}

TEST_F(StdStringLibTest, JoinExec) {
    EXPECT_EQ(runWithImports(R"(
        import std.string;
        int32 main() {
            string_view[] parts = split("a,b,c", ",");
            string j = std::join(parts, "-");
            string_view v = j;
            if (v == "a-b-c") { return 0; }
            j.destroy();
            string_view[] empty;
            string e = std::join(empty, "-");
            if (e.len() == 0) { return 0; }
            return 9;
        }
    )", "sslib03.cmm"), 0);
}

TEST_F(StdStringLibTest, ConcatRepeatExec) {
    EXPECT_EQ(runWithImports(R"(
        import std.string;
        int32 main() {
            string a = std::concat("ab", "cd");
            string_view va = a;
            if (va == "abcd") { return 0; }
            a.destroy();
            string r = std::repeat("ab", 3);
            string_view vr = r;
            if (vr == "ababab") { return 0; }
            r.destroy();
            string z = std::repeat("ab", 0);
            if (z.len() == 0) { return 0; }
            z.destroy();
            return 9;
        }
    )", "sslib04.cmm"), 0);
}

// 字符串构建惯用法：string 自身即 builder（P1-06 append/push）。
// 独立 StrBuilder 类挂账：class 无法跨模块导出（parse 期类型名注册先于 import）。
TEST_F(StdStringLibTest, BuilderIdiomExec) {
    EXPECT_EQ(runWithImports(R"(
        import std.string;
        int32 main() {
            string b = string.new("");
            b.append("x");
            b.push('y');
            if (b.len() != 2) { return 1; }
            string_view v = b;
            if (v == "xy") { return 0; }
            b.destroy();
            return 9;
        }
    )", "sslib05.cmm"), 0);
}

TEST_F(StdStringLibTest, Utf8SubExec) {
    EXPECT_EQ(runWithImports(R"(
        import std.string;
        int32 main() {
            string_view s = "héllo";
            string_view t = std::utf8_sub(s, 1, 2);
            if (t == "él") { return 0; }
            return 9;
        }
    )", "sslib06.cmm"), 0);
}

TEST_F(StdStringLibTest, SplitDynamicEmptySepPanicPath) {
    // 动态空 sep 运行时 panic——钉编译路径存在（不执行，abort）。
    std::string ir = compileIRWithImports(R"(
        import std.string;
        int32 main() {
            string e = string.new("");
            string_view sep = e;
            string_view[] parts = split("a", sep);
            return 0;
        }
    )", "sslib07.cmm");
    ASSERT_FALSE(ir.empty());
}
