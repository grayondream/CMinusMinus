// Compile-time containers: the std container API executed by the constexpr
// evaluator, plus materialized constant tables.
#include <gtest/gtest.h>

#include <iostream>
#include <iterator>
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

class ConstexprContainersTest : public ::testing::Test {
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
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    if (!lexer.getDiagnostics().empty()) return -1;

    smc::ModuleSearchPaths paths{{STD_DIR}};
    std::set<std::string> loaded;
    std::vector<std::unique_ptr<DeclAST>> importedDecls;
    std::vector<std::string> errors;
    smc::preloadImports(smc::collectImportSpecifiers(tokens), ".", paths, loaded,
                        importedDecls, errors);
    if (!errors.empty()) {
        for (auto& e : errors) std::cerr << "import error: " << e << "\n";
        return -1;
    }

    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) return -1;
    if (!importedDecls.empty()) {
        ast->declarations.insert(ast->declarations.begin(),
                                 std::make_move_iterator(importedDecls.begin()),
                                 std::make_move_iterator(importedDecls.end()));
    }
    smc::processImports(*ast, ".", paths, loaded, errors);
    if (!errors.empty()) {
        for (auto& e : errors) std::cerr << "import error: " << e << "\n";
        return -1;
    }

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) {
        for (auto& e : analyzer.getErrors()) std::cerr << "sema: " << e.format() << "\n";
        return -1;
    }

    CodegenContext ctx;
    ctx.setSourceFile(filename);
    ast->codegen(ctx);
    ctx.finalizeDebugInfo();

    std::string ve;
    llvm::raw_string_ostream vs(ve);
    if (llvm::verifyModule(ctx.getModule(), &vs)) {
        std::cerr << "verify: " << ve << "\n";
        return -1;
    }

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) return -1;
    auto jit = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit) return -1;

    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto e = (*jit)->addIRModule(std::move(ts))) {
        std::cerr << "jit add failed\n";
        return -1;
    }

    auto sym = (*jit)->lookup("main");
    if (!sym) {
        std::cerr << "main not found\n";
        return -1;
    }
    return ((int (*)()) (intptr_t) sym->getValue())();
}

} // namespace

TEST_F(ConstexprContainersTest, VecSumInConstexpr) {
    EXPECT_EQ(runWithImports(R"(
        import std.collections;
        constexpr int32 sum8() {
            std::Vec<int32> v;
            v.init();
            for (usize i = 0; i < 8; i = i + 1) v.push((int32)(i * i));
            int32 total = 0;
            for (usize i = 0; i < v.len; i = i + 1) total = total + v.get(i);
            v.destroy();
            return total;
        }
        int32 main() { constexpr int32 x = sum8(); return x - 140; }
    )", "ct_vec_sum.c"), 0);
}

TEST_F(ConstexprContainersTest, DequeRingInConstexpr) {
    EXPECT_EQ(runWithImports(R"(
        import std.collections;
        constexpr int64 dequeSum() {
            std::Deque<int64> d;
            d.init();
            int64 i = 0;
            while (i < 8) {
                d.push_back(i);
                i = i + 1;
            }
            d.push_front(100);
            if (d.len != 9) return -1;
            if (d.get(0) != 100) return -2;
            if (d.get(1) != 0) return -3;
            if (d.get(8) != 7) return -4;
            d.destroy();
            return 0;
        }
        int32 main() { constexpr int64 x = dequeSum(); return (int32)x; }
    )", "ct_deque_ring.c"), 0);
}
