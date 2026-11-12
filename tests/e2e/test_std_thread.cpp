// std.thread end-to-end tests: pthread lifecycle, ids, detach, TLS and
// concurrent std.atomic stress.

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

class StdThreadTest : public ::testing::Test {
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

    cdd::ModuleSearchPaths paths{{STD_DIR}};
    std::set<std::string> loaded;
    std::vector<std::unique_ptr<DeclAST>> importedDecls;
    std::vector<std::string> errors;
    cdd::preloadImports(cdd::collectImportSpecifiers(tokens), ".", paths, loaded,
                        importedDecls, errors);
    if (!errors.empty()) return -1;

    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast || !parser.getErrors().empty()) return -1;
    if (!importedDecls.empty()) {
        ast->declarations.insert(ast->declarations.begin(),
                                 std::make_move_iterator(importedDecls.begin()),
                                 std::make_move_iterator(importedDecls.end()));
    }
    cdd::processImports(*ast, ".", paths, loaded, errors);
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

TEST_F(StdThreadTest, JoinableAndIds) {
    EXPECT_EQ(runWithImports(R"(
        import std.thread;
        void* noop(void* arg) { return null; }
        int32 main() {
            std::Thread t;
            t.init();
            if (t.joinable()) return 1;
            if (t.get_id() != 0) return 2;
            if (!t.start(noop, null)) return 3;
            if (!t.joinable()) return 4;
            if (t.get_id() == 0) return 5;
            usize id = t.get_id();
            if (id != t.get_id()) return 6;
            if (!std::thread_equal(id, id)) return 7;
            if (!t.join()) return 8;
            if (t.joinable()) return 9;
            if (std::thread_equal(std::this_thread_get_id(), 0)) return 10;
            if (!std::thread_equal(std::this_thread_get_id(), std::this_thread_get_id())) return 11;
            return 0;
        }
    )", "std_thread_ids.cdd"), 0);
}

TEST_F(StdThreadTest, RepeatedLifecycleReturnsFalse) {
    EXPECT_EQ(runWithImports(R"(
        import std.thread;
        void* noop(void* arg) { return null; }
        int32 main() {
            std::Thread t;
            t.init();
            if (t.join()) return 10;
            if (t.detach()) return 11;
            if (!t.start(noop, null)) return 1;
            if (t.start(noop, null)) return 2;
            if (!t.join()) return 3;
            if (t.join()) return 4;
            if (t.detach()) return 5;

            std::Thread d;
            d.init();
            if (!d.start(noop, null)) return 6;
            if (!d.detach()) return 7;
            if (d.join()) return 8;
            if (d.detach()) return 9;
            std::this_thread_sleep_ms(10);
            return 0;
        }
    )", "std_thread_lifecycle.cdd"), 0);
}

TEST_F(StdThreadTest, DetachRunsToCompletion) {
    EXPECT_EQ(runWithImports(R"(
        import std.atomic;
        import std.thread;
        std::Atomic<bool> done = {false};
        void* worker(void* arg) {
            done.store(true);
            return null;
        }
        int32 main() {
            std::Thread t;
            t.init();
            if (!t.start(worker, null)) return 1;
            if (!t.detach()) return 2;
            for (int32 i = 0; i < 1000; i = i + 1) {
                if (done.load()) return 0;
                std::this_thread_sleep_ms(1);
            }
            return 3;
        }
    )", "std_thread_detach.cdd"), 0);
}

TEST_F(StdThreadTest, HardwareConcurrencyPositive) {
    EXPECT_EQ(runWithImports(R"(
        import std.thread;
        int32 main() {
            return std::hardware_concurrency() > 0 ? 0 : 1;
        }
    )", "std_thread_hw.cdd"), 0);
}

TEST_F(StdThreadTest, ThisThreadYieldAndSleep) {
    EXPECT_EQ(runWithImports(R"(
        import std.thread;
        int32 main() {
            std::this_thread_yield();
            std::this_thread_sleep_ms(1);
            return 0;
        }
    )", "std_thread_yield.cdd"), 0);
}

TEST_F(StdThreadTest, ThreadLocalIsPerThread) {
    EXPECT_EQ(runWithImports(R"(
        import std.thread;
        thread_local int32 tls = 0;
        int32 results[4];
        void* worker(void* arg) {
            int32 id = (int32)(usize)arg;
            tls = tls + 1;
            tls = tls + 1;
            results[id] = tls;
            return null;
        }
        int32 main() {
            std::Thread t0; std::Thread t1; std::Thread t2; std::Thread t3;
            t0.init(); t1.init(); t2.init(); t3.init();
            if (!t0.start(worker, (void*)(usize)0)) return 1;
            if (!t1.start(worker, (void*)(usize)1)) return 2;
            if (!t2.start(worker, (void*)(usize)2)) return 3;
            if (!t3.start(worker, (void*)(usize)3)) return 4;
            if (!t0.join()) return 5;
            if (!t1.join()) return 6;
            if (!t2.join()) return 7;
            if (!t3.join()) return 8;
            for (int32 i = 0; i < 4; i = i + 1) {
                if (results[i] != 2) return 9;
            }
            return 0;
        }
    )", "std_thread_tls.cdd"), 0);
}

TEST_F(StdThreadTest, AtomicCounterStress) {
    EXPECT_EQ(runWithImports(R"(
        import std.atomic;
        import std.thread;
        std::Atomic<int64> counter = {0};
        void* worker(void* arg) {
            int64 n = (int64)arg;
            for (int64 i = 0; i < n; i = i + 1) {
                counter.fetch_add(1);
            }
            return null;
        }
        int32 main() {
            std::Thread t0; std::Thread t1; std::Thread t2; std::Thread t3;
            t0.init(); t1.init(); t2.init(); t3.init();
            if (!t0.start(worker, (void*)10000)) return 1;
            if (!t1.start(worker, (void*)10000)) return 2;
            if (!t2.start(worker, (void*)10000)) return 3;
            if (!t3.start(worker, (void*)10000)) return 4;
            if (!t0.join()) return 5;
            if (!t1.join()) return 6;
            if (!t2.join()) return 7;
            if (!t3.join()) return 8;
            return counter.load() == 40000 ? 0 : 9;
        }
    )", "std_thread_stress.cdd"), 0);
}

TEST_F(StdThreadTest, StartJoinPassesArgument) {
    EXPECT_EQ(runWithImports(R"(
        import std.thread;
        int64 seen = 0;
        void* worker(void* arg) {
            seen = (int64)arg;
            return null;
        }
        int32 main() {
            std::Thread t;
            t.init();
            if (!t.start(worker, (void*)42)) return 1;
            if (!t.join()) return 2;
            return seen == 42 ? 0 : 3;
        }
    )", "std_thread_start.cdd"), 0);
}

} // namespace
