#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

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

class CompileOnlyE2E : public ::testing::Test {
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

static std::unique_ptr<CodegenContext> codegenSource(const std::string& source,
                                                     const std::string& filename) {
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) return nullptr;

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return nullptr;

    auto ctx = std::make_unique<CodegenContext>();
    ctx->setSourceFile(filename);
    ast->codegen(*ctx);
    ctx->finalizeDebugInfo();

    std::string ve;
    llvm::raw_string_ostream vs(ve);
    if (llvm::verifyModule(ctx->getModule(), &vs)) return nullptr;
    return ctx;
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

static int runSource(const std::string& source, const std::string& filename) {
    auto ctx = codegenSource(source, filename);
    if (!ctx) return -1;

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) return -1;
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb))
        .create();
    if (!jit) return -1;

    auto ts = llvm::orc::ThreadSafeModule(ctx->takeModule(), ctx->takeContext());
    if (auto e = (*jit)->addIRModule(std::move(ts))) return -1;

    auto sym = (*jit)->lookup("main");
    if (!sym) return -1;
    auto fn = (int (*)())(intptr_t)sym->getValue();
    return fn();
}

TEST_F(CompileOnlyE2E, IfCompileTimeSelectsBothPaths) {
    EXPECT_EQ(runSource(R"(
compile_time int32 f(int32 x) {
    if compile_time {
        return x + 1;
    } else {
        return x - 1;
    }
}
compile_time int32 CT = f(5);
int32 main() {
    return f(5) * 10 + CT - 46;
}
)", "if_compile_time.cmm"), 0);
}

TEST_F(CompileOnlyE2E, IfCompileTimeRejectedInRuntimeFunction) {
    auto errs = compileErrors(R"(
int32 main() {
    if compile_time { return 1; } else { return 0; }
}
)", "if_compile_time_bad.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "if compile_time"));
}

TEST_F(CompileOnlyE2E, ScalarBuildsCompileTimeVariable) {
    EXPECT_EQ(runSource(R"(
compile_only int32 twice(int32 x) { return x * 2; }
compile_time int32 X = twice(21);
int32 main() { return X - 42; }
)", "compile_only_scalar.cmm"), 0);
}

TEST_F(CompileOnlyE2E, NoRuntimeSymbolOrDeclaration) {
    auto ctx = codegenSource(R"(
compile_only int32 helper(int32 x) { return x + 1; }
int32 main() { return 0; }
)", "compile_only_no_symbol.cmm");
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(ctx->getModule().getFunction("helper"), nullptr);
    EXPECT_EQ(ctx->getModule().getFunction("helper_int32"), nullptr);
}
