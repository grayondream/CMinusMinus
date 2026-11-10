#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>

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
#include "driver/CompilerDriver.h"

namespace fs = std::filesystem;

class ConstConstexprE2E : public ::testing::Test {
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

static std::vector<std::string> compileErrors(const std::string& source,
                                              const std::string& filename) {
    Lexer lexer(filename, source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    std::vector<std::string> errors;
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

TEST_F(ConstConstexprE2E, CompileTimeVariableMutationRejected) {
    auto errs = compileErrors(R"(
compile_time int32 N = 1;
int32 main() { N = 2; return N; }
)", "ct_mutate.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "read-only"));
}

TEST_F(ConstConstexprE2E, ConstVariable) {
    EXPECT_EQ(runSource(
        "const int32 x = 42; int32 main() { return x; }", "const_var.cmm"), 42);
}

TEST_F(ConstConstexprE2E, ConstexprVariable) {
    EXPECT_EQ(runSource(
        "compile_time int32 x = 42; int32 main() { return x; }", "constexpr_var.cmm"), 42);
}

TEST_F(ConstConstexprE2E, ConstexprTwoGlobals) {
    EXPECT_EQ(runSource(
        "compile_time int32 a = 10; compile_time int32 b = 23; int32 main() { return b; }",
        "constexpr_two.cmm"), 23);
}

TEST_F(ConstConstexprE2E, ConstexprExpression) {
    EXPECT_EQ(runSource(
        "compile_time int32 a = 10; compile_time int32 b = a * 2 + 3; int32 main() { return b; }",
        "constexpr_expr.cmm"), 23);
}

TEST_F(ConstConstexprE2E, LocalConstVariable) {
    EXPECT_EQ(runSource(
        "int32 main() { const int32 x = 42; return x; }", "local_const.cmm"), 42);
}

TEST_F(ConstConstexprE2E, LocalConstexprVariable) {
    EXPECT_EQ(runSource(
        "int32 main() { compile_time int32 x = 42; return x; }", "local_constexpr.cmm"), 42);
}

TEST_F(ConstConstexprE2E, MixedConstConstexpr) {
    EXPECT_EQ(runSource(
        "compile_time int32 a = 10; const int32 b = 20; int32 main() { return a + b; }",
        "mixed.cmm"), 30);
}

TEST_F(ConstConstexprE2E, ConstexprInFunction) {
    EXPECT_EQ(runSource(
        "int32 main() { compile_time int32 x = 5; compile_time int32 y = x * 3; return y; }",
        "constexpr_func.cmm"), 15);
}

TEST_F(ConstConstexprE2E, ConstFromResourceFile) {
    std::ifstream ifs(fs::path(E2E_RESOURCE_DIR) / "test_const.cmm");
    ASSERT_TRUE(ifs.is_open());
    std::stringstream ss;
    ss << ifs.rdbuf();
    EXPECT_EQ(runSource(ss.str(), "test_const.cmm"), 0);
}

TEST_F(ConstConstexprE2E, DriverBasicUsage) {
    CompilerDriver driver;
    std::string testFile = std::string(E2E_RESOURCE_DIR) + "/test_const.cmm";
    const char* argv[] = {"cmm", "-o", "test_const_e2e", testFile.c_str()};
    ASSERT_TRUE(driver.parseArguments(4, argv));
    EXPECT_EQ(driver.run(), 0);
    EXPECT_EQ(system("./test_const_e2e"), 0);
    std::remove("test_const_e2e");
}
