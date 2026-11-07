// STD-19..22: std.fs/std.time/std.os/std.testing end-to-end.
#include <gtest/gtest.h>

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

class StdSystemTest : public ::testing::Test {
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
    if (!errors.empty()) return -1;

    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) return -1;
    if (!importedDecls.empty()) {
        ast->declarations.insert(ast->declarations.begin(),
                                 std::make_move_iterator(importedDecls.begin()),
                                 std::make_move_iterator(importedDecls.end()));
    }
    smc::processImports(*ast, ".", paths, loaded, errors);
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

} // namespace

TEST_F(StdSystemTest, FileReadWriteAppendRemove) {
    EXPECT_EQ(runWithImports(R"(
        import std.fs;
        int32 main() {
            string_view path = "/tmp/smc_std_fs_e2e.txt";
            std::remove_file(path);
            if (!std::write_file(path, "hello")) return 1;
            if (!std::exists(path) || !std::is_file(path)) return 2;
            if (std::file_size(path) != 5) return 3;
            string s = std::read_file(path);
            string_view sv = s;
            if (sv != "hello") return 4;
            s.destroy();
            if (!std::append_file(path, " world")) return 5;
            s = std::read_file(path);
            string_view sv2 = s;
            if (sv2 != "hello world") return 6;
            s.destroy();
            string_view other = "/tmp/smc_std_fs_e2e_renamed.txt";
            std::remove_file(other);
            if (!std::rename_path(path, other)) return 7;
            if (std::exists(path) || !std::exists(other)) return 8;
            if (!std::remove_file(other)) return 9;
            if (std::exists(other)) return 10;
            return 0;
        }
    )", "sys_fs.cmm"), 0);
}

TEST_F(StdSystemTest, ReadLineZeroCapacityDoesNotWrite) {
    EXPECT_EQ(runWithImports(R"(
        import std.io;
        int32 main() {
            char buf[1];
            buf[0] = 'X';
            int32 n = std::read_line(buf, 0);
            if (n != 0) return 1;
            if (buf[0] != 'X') return 2;
            return 0;
        }
    )", "sys_readline.cmm"), 0);
}

TEST_F(StdSystemTest, TimeOsAndTesting) {
    EXPECT_EQ(runWithImports(R"(
        import std.time;
        import std.os;
        import std.testing;
        import std.fs;
        int32 main() {
            if (std::unix_seconds() <= 0) return 1;
            if (std::unix_millis() <= 0) return 2;
            if (std::pid() <= 0) return 3;
            if (!std::env_set("SMC_STD_E2E_VAR", "abc123")) return 5;
            string v = std::env_get("SMC_STD_E2E_VAR");
            string_view vv = v;
            if (vv != "abc123") { v.destroy(); return 6; }
            v.destroy();
            if (!std::env_unset("SMC_STD_E2E_VAR")) return 7;
            std::sleep_ms(1);
            std::expect(true, "expect true");
            std::expect_eq_i64(42, 42, "eq");
            std::expect_eq_str("abc", "abc", "str eq");
            string cwd = std::current_dir();
            if (cwd.len() == 0) { cwd.destroy(); return 4; }
            cwd.destroy();
            return 0;
        }
    )", "sys_time.cmm"), 0);
}
