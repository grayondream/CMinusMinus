// std.atomic end-to-end tests: module import, Atomic<T> convenience surface,
// per-order free functions, and explicit compare-exchange.

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

class StdAtomicTest : public ::testing::Test {
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

static int runWithImports(const std::string& source, const std::string& filename,
                          std::vector<std::string>* semaOut = nullptr) {
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
    if (!ast || !parser.getErrors().empty()) return -1;
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
        if (semaOut) {
            for (auto& e : analyzer.getErrors()) {
                semaOut->push_back(e.format());
            }
        }
        return -1;
    }

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

TEST_F(StdAtomicTest, PerOrderAndRuntimeOrder) {
    EXPECT_EQ(runWithImports(R"(
        import std.atomic;
        std::Atomic<int32> a = {0};
        int32 main() {
            a.store_order(3, std::MemoryOrderRelease);
            if (a.load_order(std::MemoryOrderAcquire) != 3) return 1;
            if (std::atomic_load_acquire(&a.value) != 3) return 2;
            std::atomic_store_release(&a.value, 4);
            if (a.load() != 4) return 3;
            return 0;
        }
    )", "std_atomic_order.c"), 0);
}

TEST_F(StdAtomicTest, RuntimeOrderRmwAndFences) {
    EXPECT_EQ(runWithImports(R"(
        import std.atomic;
        std::Atomic<int32> a = {10};
        int32 main() {
            if (a.exchange_order(20, std::MemoryOrderAcqRel) != 10) return 1;
            if (a.fetch_add_order(5, std::MemoryOrderRelaxed) != 20) return 2;
            if (a.fetch_sub_order(3, std::MemoryOrderAcquire) != 25) return 3;
            if (a.fetch_and_order(6, std::MemoryOrderRelease) != 22) return 4;
            if (a.fetch_or_order(8, std::MemoryOrderSeqCst) != 6) return 5;
            if (a.fetch_xor_order(1, std::MemoryOrderRelaxed) != 14) return 6;
            if (a.load_order(std::MemoryOrderSeqCst) != 15) return 7;
            if (a.exchange_order(5, std::MemoryOrderConsume) != 15) return 8;
            if (a.fetch_add_order(1, std::MemoryOrderConsume) != 5) return 9;
            if (a.load() != 6) return 10;
            std::atomic_thread_fence<0>();
            std::atomic_signal_fence<0>();
            std::atomic_thread_fence<5>();
            std::atomic_signal_fence<5>();
            std::atomic_thread_fence_seq_cst();
            std::atomic_signal_fence_seq_cst();
            return 0;
        }
    )", "std_atomic_rmw_order.c"), 0);
}

TEST_F(StdAtomicTest, ExplicitCompareExchange) {
    EXPECT_EQ(runWithImports(R"(
        import std.atomic;
        std::Atomic<int32> a = {1};
        int32 main() {
            int32 expected = 1;
            // Numeric orders are compile-time template values; enum constants
            // from an imported module are not visible to the parse-time
            // constant folder (use the named per-order wrappers otherwise).
            if (!std::atomic_compare_exchange_strong_explicit<int32, 4, 2>(
                    &a.value, &expected, 2)) return 1;
            expected = 2;
            if (!std::atomic_compare_exchange_weak_explicit<int32, 0, 0>(
                    &a.value, &expected, 3)) return 2;
            return a.load() == 3 ? 0 : 3;
        }
    )", "std_atomic_cas_explicit.c"), 0);
}

TEST_F(StdAtomicTest, FloatBoolEnumAndPointer) {
    EXPECT_EQ(runWithImports(R"(
        import std.atomic;
        enum Color : int32 { Red = 1, Blue = 2 };
        int32 main() {
            std::Atomic<float32> f = {1.5f};
            float32 fe = 1.5f;
            if (!f.compare_exchange_strong(&fe, 2.5f)) return 1;
            float32 old = f.exchange(3.5f);
            if (old != 2.5f || f.load() != 3.5f) return 2;

            std::Atomic<bool> b = {false};
            bool be = false;
            if (!b.compare_exchange_strong(&be, true)) return 3;
            if (!b.load()) return 4;

            std::Atomic<Color> c = {Red};
            Color ce = Red;
            if (!c.compare_exchange_strong(&ce, Blue)) return 5;
            if (c.load() != Blue) return 6;

            int32 arr[3];
            std::Atomic<int32*> p = {&arr[0]};
            int32* oldp = p.fetch_add(1);
            if (oldp != &arr[0]) return 7;
            if (p.load() != &arr[1]) return 8;

            int16 v16 = 1;
            std::Atomic<int16> a16 = {v16};
            int16 two16 = 2;
            a16.store(two16);
            int16 expected16 = 2;
            if (!a16.compare_exchange_strong(&expected16, v16)) return 9;
            if (a16.load() != 1) return 10;

            uint8 v8 = 3;
            std::Atomic<uint8> a8 = {v8};
            uint8 expected8 = 3;
            uint8 four8 = 4;
            if (!a8.compare_exchange_strong(&expected8, four8)) return 11;
            if (a8.load() != 4) return 12;
            return 0;
        }
    )", "std_atomic_types.c"), 0);
}

TEST_F(StdAtomicTest, LockFreeQueries) {
    EXPECT_EQ(runWithImports(R"(
        import std.atomic;
        int32 main() {
            std::Atomic<int32> a = {0};
            if (!a.is_lock_free()) return 1;
            if (!a.is_always_lock_free()) return 2;
            return 0;
        }
    )", "std_atomic_lockfree.c"), 0);
}

TEST_F(StdAtomicTest, UnsupportedSizeRejected) {
    std::vector<std::string> errors;
    int rc = runWithImports(R"(
        import std.atomic;
        int32 main() {
            std::Atomic<int128> a = {0};
            return (int32)a.value;
        }
    )", "std_atomic_bad_size.c", &errors);
    EXPECT_EQ(rc, -1);
    ASSERT_FALSE(errors.empty());
    bool mentionsLimit = false;
    for (const auto& e : errors) {
        if (e.find("1/2/4/8") != std::string::npos) mentionsLimit = true;
    }
    EXPECT_TRUE(mentionsLimit);

    std::vector<std::string> aggregateErrors;
    int aggregateRc = runWithImports(R"(
        import std.atomic;
        struct Pair { int32 a; int32 b; };
        int32 main() {
            std::Atomic<Pair> a;
            return a.is_lock_free() ? 0 : 1;
        }
    )", "std_atomic_bad_struct.c", &aggregateErrors);
    EXPECT_EQ(aggregateRc, -1);
    EXPECT_FALSE(aggregateErrors.empty());
}

TEST_F(StdAtomicTest, SeqCstConvenience) {
    EXPECT_EQ(runWithImports(R"(
        import std.atomic;
        std::Atomic<int32> a = {0};
        int32 main() {
            a.store(3);
            a.store(a.load() + 1);
            int32 expected = 4;
            if (!a.compare_exchange_strong(&expected, 9)) return 1;
            if (a.fetch_add(2) != 9) return 2;
            int32 wrong = 999;
            if (a.compare_exchange_strong(&wrong, 99)) return 3;
            if (wrong != a.load()) return 4;
            return a.load() == 11 ? 0 : 5;
        }
    )", "std_atomic_seq.c"), 0);
}

} // namespace
