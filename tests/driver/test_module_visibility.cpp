// P0-04 / MOD-05/06/07/13: module declaration/file binding, export-based
// visibility, and import cycle diagnostics.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

#include "ast/Decl.h"
#include "codegen/CodegenContext.h"
#include "driver/ModuleLoader.h"
#include "driver/StdPrelude.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "support/Log.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace {

class ModuleVisibilityTest : public ::testing::Test {
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
        // CTest runs each gtest case in its own process; a fixed directory
        // makes concurrent ModuleVisibility tests overwrite each other's
        // util.cmm. Isolate by pid so -j8 is deterministic.
        dir = fs::temp_directory_path() /
              ("cmm_module_visibility_test_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }

    void TearDown() override { fs::remove_all(dir); }

    void write(const fs::path& rel, const std::string& text) {
        fs::create_directories(rel.parent_path());
        std::ofstream out(rel);
        out << text;
    }

    // Parses `mainSource` as the entry unit, resolves its imports, and returns
    // the spliced translation unit plus any loader errors.
    std::unique_ptr<TranslationUnitAST> loadMain(const std::string& mainSource,
                                                 std::vector<std::string>& errors) {
        auto ast = cmm::parseStdCPrelude(mainSource, (dir / "main.cmm").string());
        std::set<std::string> loaded;
        cmm::processImports(*ast, dir.string(), {}, loaded, errors);
        return ast;
    }

    // Variant for tests that use imported *types*: preload imported modules
    // before parsing the entry unit so the parser's type context is populated,
    // matching CompilerDriver/runWithImports order.
    std::unique_ptr<TranslationUnitAST> loadMainWithPreloadedImports(
        const std::string& mainSource, std::vector<std::string>& errors) {
        Lexer lexer((dir / "main.cmm").string(), mainSource);
        auto tokens = lexer.tokenize();
        std::set<std::string> loaded;
        std::vector<std::unique_ptr<DeclAST>> importedDecls;
        cmm::preloadImports(cmm::collectImportSpecifiers(tokens), dir.string(), {},
                            loaded, importedDecls, errors);
        Parser parser(tokens);
        auto ast = parser.parse();
        if (!ast) return nullptr;
        if (!importedDecls.empty()) {
            ast->declarations.insert(ast->declarations.begin(),
                                     std::make_move_iterator(importedDecls.begin()),
                                     std::make_move_iterator(importedDecls.end()));
        }
        cmm::processImports(*ast, dir.string(), {}, loaded, errors);
        return ast;
    }

    // Runs sema + codegen + JIT for a unit whose `main` returns an int.
    int jitRun(TranslationUnitAST& ast) {
        CodegenContext ctx;
        ctx.setSourceFile("module_visibility_test.cmm");
        ast.codegen(ctx);
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

    fs::path dir;
};

bool hasErrorContaining(const std::vector<Diagnostic>& errors, const std::string& needle) {
    for (const auto& d : errors) {
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool hasStringContaining(const std::vector<std::string>& errors, const std::string& needle) {
    for (const auto& e : errors) {
        if (e.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST_F(ModuleVisibilityTest, ExportedFunctionIsVisible) {
    write(dir / "util.cmm", "module util;\nexport int32 helper() { return 5; }\n");

    std::vector<std::string> errors;
    auto ast = loadMain("import util;\nint32 main() { return helper() - 5; }\n", errors);
    ASSERT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
    EXPECT_EQ(jitRun(*ast), 0);
}

TEST_F(ModuleVisibilityTest, NonExportedFunctionIsHidden) {
    write(dir / "util.cmm", "module util;\nint32 helper() { return 5; }\n");

    std::vector<std::string> errors;
    auto ast = loadMain("import util;\nint32 main() { return helper(); }\n", errors);
    ASSERT_TRUE(errors.empty());

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    // The importer must not see util's module-private helper (MOD-05/06).
    EXPECT_TRUE(hasErrorContaining(analyzer.getErrors(), "helper"));
    EXPECT_TRUE(hasErrorContaining(analyzer.getErrors(), "undeclared"));
}

TEST_F(ModuleVisibilityTest, PrivateHelperUsableInsideModule) {
    write(dir / "util.cmm",
          "module util;\n"
          "int32 helper() { return 7; }\n"
          "export int32 compute() { return helper() + 1; }\n");

    std::vector<std::string> errors;
    auto ast = loadMain("import util;\nint32 main() { return compute() - 8; }\n", errors);
    ASSERT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
    EXPECT_EQ(jitRun(*ast), 0);
}

TEST_F(ModuleVisibilityTest, LegacyUnitWithoutModuleDeclIsFullyVisible) {
    write(dir / "util.cmm", "int32 helper() { return 3; }\n");

    std::vector<std::string> errors;
    auto ast = loadMain("import util;\nint32 main() { return helper() - 3; }\n", errors);
    ASSERT_TRUE(errors.empty());

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
    EXPECT_EQ(jitRun(*ast), 0);
}

TEST_F(ModuleVisibilityTest, ExportedNamespaceMembersAreVisible) {
    write(dir / "geom.cmm",
          "module geom;\n"
          "export namespace g { int32 add(int32 a, int32 b) { return a + b; } }\n");

    std::vector<std::string> errors;
    auto ast = loadMain("import geom;\nint32 main() { return g::add(2, 3) - 5; }\n", errors);
    ASSERT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
    EXPECT_EQ(jitRun(*ast), 0);
}

TEST_F(ModuleVisibilityTest, DottedModuleNameMustMatchPath) {
    write(dir / "lib" / "util.cmm",
          "module lib.util;\nexport int32 f() { return 9; }\n");

    std::vector<std::string> errors;
    auto ast = loadMain("import lib.util;\nint32 main() { return f() - 9; }\n", errors);
    ASSERT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
    EXPECT_EQ(jitRun(*ast), 0);
}

TEST_F(ModuleVisibilityTest, ModuleNameMismatchIsReported) {
    write(dir / "util.cmm", "module other;\nexport int32 helper() { return 1; }\n");

    std::vector<std::string> errors;
    auto ast = loadMain("import util;\nint32 main() { return helper(); }\n", errors);
    EXPECT_TRUE(hasStringContaining(errors, "declares name 'other'"));
}

TEST_F(ModuleVisibilityTest, RootExportIsUnqualified) {
    write(dir / "util.cmm", "module util;\nexport int32 helper() { return 5; }\n");

    std::vector<std::string> errors;
    auto ast = loadMain("import util;\nint32 main() { return helper() - 5; }\n", errors);
    ASSERT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
    EXPECT_EQ(jitRun(*ast), 0);
}

TEST_F(ModuleVisibilityTest, NamespaceMemberRequiresQualification) {
    write(dir / "geom.cmm",
          "module geom;\n"
          "export namespace g { int32 add(int32 a, int32 b) { return a + b; } }\n");

    std::vector<std::string> errors;
    auto ast = loadMain("import geom;\nint32 main() { return add(2, 3); }\n", errors);
    ASSERT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(hasErrorContaining(analyzer.getErrors(), "undeclared function 'add'"));
}

TEST_F(ModuleVisibilityTest, ModuleNameDoesNotCreateQualifier) {
    write(dir / "std" / "vec.cmm",
          "module std.vec;\n"
          "export namespace std { struct Vec { int32 x; }; }\n");

    std::vector<std::string> errors;
    auto ast = loadMainWithPreloadedImports(
        "import std.vec;\nint32 main() { std::Vec v = {}; return v.x; }\n", errors);
    ASSERT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);
    ASSERT_TRUE(ast != nullptr);

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty())
        << (analyzer.getErrors().empty() ? "" : analyzer.getErrors()[0].message);
}
