// STD-06..08: std container modules end-to-end (JIT), plus namespace-template
// export from a real stdlib module.
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

class StdCollectionsTest : public ::testing::Test {
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
    if (llvm::verifyModule(ctx.getModule(), &vs)) { std::cerr << "verify: " << ve << "\n"; return -1; }

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) return -1;
    auto jit = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!jit) return -1;

    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto e = (*jit)->addIRModule(std::move(ts))) { std::cerr << "jit add failed\n"; return -1; }

    auto sym = (*jit)->lookup("main");
    if (!sym) { std::cerr << "main not found\n"; return -1; }
    return ((int (*)())(intptr_t)sym->getValue())();
}

} // namespace

TEST_F(StdCollectionsTest, VecGrowPushInsertRemove) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.hash;
        int32 main() {
            std::Vec<int32> v;
            v.init();
            if (!v.is_empty()) return 1;
            int32 i = 0;
            while (i < 100) {
                if (!v.push(i)) return 2;
                i = i + 1;
            }
            if (v.len != 100) return 3;
            if (v.cap < 100) return 4;
            if (v.get(42) != 42) return 5;
            if (!v.insert(10, 999)) return 6;
            if (v.get(10) != 999 || v.get(11) != 10) return 7;
            if (!v.remove(10)) return 8;
            if (v.get(10) != 10 || v.len != 100) return 9;
            int32 out = -1;
            if (!v.pop(&out) || out != 99) return 10;
            if (v.len != 99) return 11;
            if (!v.contains(55, std::equal_i32)) return 12;
            v.destroy();
            return 0;
        }
    )", "coll_vec.cmm"), 0);
}

TEST_F(StdCollectionsTest, DequeRingWrap) {
    EXPECT_EQ(runWithImports(R"(
        import std.deque;
        int32 main() {
            std::Deque<int64> d;
            d.init();
            int64 i = 0;
            while (i < 32) {
                if (!d.push_back(i)) return 1;
                i = i + 1;
            }
            if (!d.push_front(-1)) return 2;
            if (d.len != 33) return 3;
            if (d.get(0) != -1 || d.get(1) != 0 || d.get(32) != 31) return 4;
            int64 x = 0;
            if (!d.pop_front(&x) || x != -1) return 5;
            if (!d.pop_back(&x) || x != 31) return 6;
            if (d.len != 31) return 7;
            d.destroy();
            return 0;
        }
    )", "coll_deque.cmm"), 0);
}

TEST_F(StdCollectionsTest, HashMapPutGetRemoveTombstone) {
    EXPECT_EQ(runWithImports(R"(
        import std.hashmap;
        import std.hash;
        int32 main() {
            std::HashMap<uint32,int32> m;
            m.init(std::hash_key_u32, std::equal_u32);
            int32 i = 0;
            while (i < 200) {
                if (!m.put((uint32)i, i * 3)) return 1;
                i = i + 1;
            }
            if (m.size() != 200) return 2;
            if (!m.put(7, 777)) return 3;
            if (m.size() != 200) return 4;
            int32 out = 0;
            if (!m.get(7, &out) || out != 777) return 5;
            if (!m.remove(7)) return 6;
            if (m.contains(7)) return 7;
            if (m.size() != 199) return 8;
            if (!m.put(7, 7)) return 9;
            if (!m.get(7, &out) || out != 7) return 10;
            m.destroy();
            return 0;
        }
    )", "coll_map.cmm"), 0);
}

TEST_F(StdCollectionsTest, HashSetInsertRemove) {
    EXPECT_EQ(runWithImports(R"(
        import std.hashset;
        import std.hash;
        int32 main() {
            std::HashSet<uint64> s;
            s.init(std::hash_key_u64, std::equal_u64);
            uint64 i = 0;
            while (i < 64) {
                if (!s.insert(i * 2)) return 1;
                i = i + 1;
            }
            if (s.size() != 64) return 2;
            if (!s.contains(42)) return 3;
            if (s.contains(43)) return 4;
            if (!s.remove(42)) return 5;
            if (s.contains(42)) return 6;
            if (!s.insert(42)) return 7;
            if (!s.contains(42)) return 8;
            s.destroy();
            return 0;
        }
    )", "coll_set.cmm"), 0);
}

// Each container lives in its own module and must be usable on its own.
TEST_F(StdCollectionsTest, DirectContainerModuleImports) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        import std.hash;
        int32 main() {
            std::Vec<int32> v;
            v.init();
            if (!v.push(1) || v.get(0) != 1) return 1;
            v.destroy();

            std::Deque<int32> d;
            d.init();
            if (!d.push_back(2) || d.get(0) != 2) return 2;
            d.destroy();

            std::HashMap<uint32,int32> m;
            m.init(std::hash_key_u32, std::equal_u32);
            if (!m.put(3, 33)) return 3;
            int32 out = 0;
            if (!m.get(3, &out) || out != 33) return 4;
            m.destroy();

            std::HashSet<uint32> s;
            s.init(std::hash_key_u32, std::equal_u32);
            if (!s.insert(4) || !s.contains(4)) return 5;
            s.destroy();
            return 0;
        }
    )", "coll_direct.cmm"), 0);
}
