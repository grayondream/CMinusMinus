// P0-04 / MOD-04: source-file import resolution and declaration splicing, and
// the file-backed std.c binding layer (MOD-09 / STD-23).
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

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

class ModuleLoaderTest : public ::testing::Test {
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

int jitRun(TranslationUnitAST& ast) {
    CodegenContext ctx;
    ctx.setSourceFile("module_test.cdd");
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

} // namespace

TEST_F(ModuleLoaderTest, ResolvesStdCFromLibsDir) {
    cdd::ModuleSearchPaths paths{{STD_DIR}};
    std::string resolved = cdd::resolveImport("std.c", paths);
    ASSERT_FALSE(resolved.empty());
    EXPECT_NE(resolved.find("c.cdd"), std::string::npos);
}

TEST_F(ModuleLoaderTest, StdPreludeLoadsFromFile) {
    const std::string text = cdd::loadStdCPrelude({STD_DIR});
    EXPECT_NE(text.find("memcpy"), std::string::npos);
    EXPECT_NE(text.find("malloc"), std::string::npos);
    EXPECT_NE(text.find("abs"), std::string::npos);
}

TEST_F(ModuleLoaderTest, UnresolvedImportReturnsEmpty) {
    cdd::ModuleSearchPaths paths{{STD_DIR}};
    EXPECT_TRUE(cdd::resolveImport("no_such_module_xyz", paths).empty());
}

TEST_F(ModuleLoaderTest, DottedSpecifierMapsToPath) {
    cdd::ModuleSearchPaths paths{{STD_DIR}};
    // `std.c` resolves through the dotted-name rule to `<dir>/std/c.cdd`.
    EXPECT_FALSE(cdd::resolveImport("std.c", paths).empty());
    // A specifier with a slash is treated as a relative path.
    EXPECT_FALSE(cdd::resolveImport("std/c.cdd", paths).empty());
}

TEST_F(ModuleLoaderTest, ProcessImportsSplicesDeclarations) {
    const fs::path dir = fs::temp_directory_path() / "cdd_module_loader_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "util.cdd");
        out << "int32 helper() { return 5; }\n";
    }

    auto ast = cdd::parseStdCPrelude(
        "import util;\nint32 main() { return helper() - 5; }\n",
        (dir / "main.cdd").string());
    ASSERT_NE(ast, nullptr);

    std::set<std::string> loaded;
    std::vector<std::string> errors;
    cdd::processImports(*ast, dir.string(), {}, loaded, errors);
    for (const auto& e : errors) ADD_FAILURE() << e;
    ASSERT_TRUE(errors.empty());
    ASSERT_FALSE(ast->declarations.empty());

    // The imported declaration must be the first declaration.
    auto* first = dynamic_cast<FunctionDeclAST*>(ast->declarations.front().get());
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->name, "helper");

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());

    EXPECT_EQ(jitRun(*ast), 0);
    fs::remove_all(dir);
}

TEST_F(ModuleLoaderTest, CyclicImportsReported) {
    const fs::path dir = fs::temp_directory_path() / "cdd_module_cycle_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "a.cdd");
        out << "import b;\nint32 fa() { return 1; }\n";
    }
    {
        std::ofstream out(dir / "b.cdd");
        out << "import a;\nint32 fb() { return 2; }\n";
    }

    auto ast = cdd::parseStdCPrelude("import a;\nint32 main() { return fa() + fb() - 3; }\n",
                                     (dir / "main.cdd").string());
    ASSERT_NE(ast, nullptr);

    std::set<std::string> loaded;
    std::vector<std::string> errors;
    cdd::processImports(*ast, dir.string(), {}, loaded, errors);

    // MOD-07: an import cycle is reported (and loading terminates).
    bool reported = false;
    for (const auto& e : errors) {
        if (e.find("circular import detected") != std::string::npos) {
            reported = true;
            break;
        }
    }
    EXPECT_TRUE(reported) << "expected a circular-import diagnostic";
    fs::remove_all(dir);
}

TEST_F(ModuleLoaderTest, ExportedTypesVisibleBeforeParsingImporter) {
    const fs::path dir = fs::temp_directory_path() / "cdd_module_types_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "types.cdd");
        out << "module types;\n"
               "export struct Point { int32 x; int32 y; };\n"
               "export using Handle = int32;\n"
               "export class Box { public: int32 v; };\n"
               "export template<typename T> struct Wrap { T v; };\n"
               "export int32 add(int32 a, int32 b) { return a + b; }\n";
    }

    const std::string mainSource = R"(
        module main;
        import types;
        int32 main() {
            Point p; p.x = 1; p.y = 2;
            Handle h = 3;
            Box b; b.v = 4;
            Wrap<int32> w; w.v = 5;
            return add(p.x + p.y, h + b.v + w.v) - 15;
        }
    )";
    const std::string mainPath = (dir / "main.cdd").string();
    Lexer lexer(mainPath, mainSource);
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(lexer.getDiagnostics().empty());

    std::set<std::string> loaded;
    std::vector<std::unique_ptr<DeclAST>> imported;
    std::vector<std::string> errors;
    const auto specs = cdd::collectImportSpecifiers(tokens);
    ASSERT_EQ(specs.size(), 1u);
    EXPECT_EQ(specs[0], "types");
    cdd::preloadImports(specs, dir.string(), cdd::ModuleSearchPaths{{dir.string()}},
                        loaded, imported, errors);
    for (const auto& e : errors) ADD_FAILURE() << e;
    ASSERT_TRUE(errors.empty());
    ASSERT_FALSE(imported.empty());

    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    for (const auto& d : parser.getErrors()) ADD_FAILURE() << d.formatWithSeverity();
    ASSERT_TRUE(parser.getErrors().empty());

    ast->declarations.insert(ast->declarations.begin(),
                             std::make_move_iterator(imported.begin()),
                             std::make_move_iterator(imported.end()));
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    for (const auto& e : analyzer.getErrors()) ADD_FAILURE() << e.formatWithSeverity();
    ASSERT_TRUE(analyzer.getErrors().empty());
    EXPECT_EQ(jitRun(*ast), 0);
    fs::remove_all(dir);
}

TEST_F(ModuleLoaderTest, ExportedNamespaceTypesWithMethodsTemplatesAndInheritance) {
    const fs::path dir = fs::temp_directory_path() / "cdd_module_namespace_types_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "api_types.cdd");
        out << "module api_types;\n"
               "export namespace api {\n"
               "    struct Point {\n"
               "        int32 x; int32 y;\n"
               "        int32 sum() { return this->x + this->y; }\n"
               "    };\n"
               "    class Box {\n"
               "    public:\n"
               "        int32 v;\n"
               "        int32 get() { return this->v; }\n"
               "        static int32 twice(int32 x) { return x * 2; }\n"
               "    };\n"
               "    template<typename T> struct Wrap {\n"
               "        T v;\n"
               "        T get() { return this->v; }\n"
               "    };\n"
               "    class Base { public: int32 b; int32 basev() { return this->b; } };\n"
               "    class Derived : public Base {\n"
               "    public:\n"
               "        int32 d;\n"
               "        int32 total() { return this->b + this->d; }\n"
               "    };\n"
               "}\n";
    }

    const std::string mainSource = R"(
        module main;
        import api_types;
        int32 main() {
            api::Point p; p.x = 1; p.y = 2;
            api::Box b; b.v = 4;
            api::Wrap<int32> w; w.v = 5;
            api::Derived d; d.b = 6; d.d = 7;
            int32 r = p.sum() + b.get() + api::Box::twice(3) + w.get() + d.total() + d.basev();
            return r - 37;
        }
    )";
    const std::string mainPath = (dir / "main.cdd").string();
    Lexer lexer(mainPath, mainSource);
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(lexer.getDiagnostics().empty());

    std::set<std::string> loaded;
    std::vector<std::unique_ptr<DeclAST>> imported;
    std::vector<std::string> errors;
    const auto specs = cdd::collectImportSpecifiers(tokens);
    ASSERT_EQ(specs.size(), 1u);
    cdd::preloadImports(specs, dir.string(), cdd::ModuleSearchPaths{{dir.string()}},
                        loaded, imported, errors);
    for (const auto& e : errors) ADD_FAILURE() << e;
    ASSERT_TRUE(errors.empty());

    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    for (const auto& d : parser.getErrors()) ADD_FAILURE() << d.formatWithSeverity();
    ASSERT_TRUE(parser.getErrors().empty());

    ast->declarations.insert(ast->declarations.begin(),
                             std::make_move_iterator(imported.begin()),
                             std::make_move_iterator(imported.end()));
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    for (const auto& e : analyzer.getErrors()) ADD_FAILURE() << e.formatWithSeverity();
    ASSERT_TRUE(analyzer.getErrors().empty());
    EXPECT_EQ(jitRun(*ast), 0);
    fs::remove_all(dir);
}

TEST_F(ModuleLoaderTest, RejectsNestedLegacySmcImportWithContext) {
    const fs::path dir = fs::temp_directory_path() / "cdd_legacy_nested_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "a.cdd");
        out << "module a;\nimport x.smc;\n"
               "export int32 fa() { return 1; }\n";
    }
    auto ast = cdd::parseStdCPrelude(
        "import a;\nint32 main() { return 0; }\n",
        (dir / "main.cdd").string());
    ASSERT_NE(ast, nullptr);
    std::set<std::string> loaded;
    std::vector<std::string> errors;
    cdd::processImports(*ast, dir.string(), {}, loaded, errors);
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors[0].find("legacy .smc source is not supported"),
              std::string::npos);
    EXPECT_NE(errors[0].find("x.smc"), std::string::npos);
    EXPECT_NE(errors[0].find("a.cdd"), std::string::npos);
    fs::remove_all(dir);
}

TEST_F(ModuleLoaderTest, RejectsLegacySmcImport) {
    const fs::path dir = fs::temp_directory_path() / "cdd_legacy_import_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto ast = cdd::parseStdCPrelude(
        "import \"old.smc\";\nint32 main() { return 0; }\n",
        (dir / "main.cdd").string());
    ASSERT_NE(ast, nullptr);
    std::set<std::string> loaded;
    std::vector<std::string> errors;
    cdd::processImports(*ast, dir.string(), {}, loaded, errors);
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors[0].find("legacy .smc source is not supported"), std::string::npos);
    fs::remove_all(dir);
}

TEST_F(ModuleLoaderTest, CollectImportSpecifiersSkipsColonColon) {
    Lexer lexer("main.cdd", "import a::b;\nint32 main() { return 0; }\n");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(lexer.getDiagnostics().empty());
    EXPECT_TRUE(cdd::collectImportSpecifiers(tokens).empty());
}
