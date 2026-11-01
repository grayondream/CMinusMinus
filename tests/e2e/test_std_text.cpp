// STD-15..18: std.sort/std.parse/std.ascii/std.path/std.hash end-to-end.
#include <gtest/gtest.h>

#include <iterator>
#include <iostream>
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

class StdTextTest : public ::testing::Test {
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
    if (llvm::verifyModule(ctx.getModule(), &vs)) { std::cerr << "verify: " << ve << "\n"; return -1; }

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) return -1;
    auto jit = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit) return -1;

    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto e = (*jit)->addIRModule(std::move(ts))) { std::cerr << "jit add failed\n"; return -1; }

    auto sym = (*jit)->lookup("main");
    if (!sym) return -1;
    return ((int (*)())(intptr_t)sym->getValue())();
}

} // namespace

TEST_F(StdTextTest, SortAndBinarySearch) {
    EXPECT_EQ(runWithImports(R"(
        import std.sort;
        int32 cmp(int32 a, int32 b) {
            if (a < b) return -1;
            if (a > b) return 1;
            return 0;
        }
        int32 main() {
            int32 a[7];
            a[0] = 5; a[1] = 1; a[2] = 4; a[3] = 2; a[4] = 7; a[5] = 3; a[6] = 6;
            std::sort<int32>(a, 7, cmp);
            if (!std::is_sorted<int32>(a, 7, cmp)) return 1;
            if (a[0] != 1 || a[6] != 7) return 2;
            isize at = std::binary_search<int32>(a, 7, 4, cmp);
            if (at != 3) return 3;
            if (std::binary_search<int32>(a, 7, 9, cmp) != -1) return 4;
            return 0;
        }
    )", "text_sort.c"), 0);
}

TEST_F(StdTextTest, ParseAndRenderIntegers) {
    EXPECT_EQ(runWithImports(R"(
        import std.parse;
        int32 main() {
            int32 i = 0;
            if (!std::parse_i32("-123", &i) || i != -123) return 1;
            int64 v = 0;
            if (!std::parse_i64("0x2a", 16, &v) || v != 42) return 2;
            uint64 u = 0;
            if (!std::parse_u64("11111111", 2, &u) || u != 255) return 3;
            if (std::parse_i32("12x", &i)) return 4;
            string s = std::to_string_i64(-255, 16);
            string_view sv = s;
            if (sv != "-ff") return 5;
            s.destroy();
            string t = std::to_string_i64(42, 10);
            string_view tv = t;
            if (tv != "42") return 6;
            t.destroy();
            float64 f = 0.0;
            if (!std::parse_float64("1.5e2", &f) || f != 150.0) return 7;
            return 0;
        }
    )", "text_parse.c"), 0);
}

TEST_F(StdTextTest, AsciiAndPath) {
    EXPECT_EQ(runWithImports(R"(
        import std.ascii;
        import std.path;
        int32 main() {
            if (!std::is_alpha('a') || std::is_alpha('1')) return 1;
            if (!std::is_digit('7') || std::is_digit('x')) return 2;
            if (std::to_upper('q') != 'Q' || std::to_lower('Q') != 'q') return 3;
            if (!std::is_hex_digit('F') || std::to_digit('F') != 15) return 4;

            if (std::basename("/tmp/a.txt") != "a.txt") return 5;
            if (std::dirname("/tmp/a.txt") != "/tmp") return 6;
            if (std::extension("a.txt") != ".txt") return 7;
            if (std::stem("a.txt") != "a") return 8;
            string j = std::join("/tmp", "data/file.bin");
            string_view jv = j;
            if (jv != "/tmp/data/file.bin") return 9;
            j.destroy();
            return 0;
        }
    )", "text_path.c"), 0);
}

TEST_F(StdTextTest, HashStability) {
    EXPECT_EQ(runWithImports(R"(
        import std.hash;
        int32 main() {
            uint64 a = std::hash_string("hello");
            uint64 b = std::hash_string("hello");
            if (a != b || a == 0) return 1;
            if (std::hash_string("world") == a) return 2;
            if (std::hash_u64(0) == std::hash_u64(1)) return 3;
            uint64 c = std::hash_combine(1, 2);
            uint64 d = std::hash_combine(1, 3);
            if (c == d) return 4;
            return 0;
        }
    )", "text_hash.c"), 0);
}
