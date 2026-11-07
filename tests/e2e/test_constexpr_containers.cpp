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

static std::vector<std::string> analyzeWithImports(const std::string& source,
                                                   const std::string& filename) {
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    std::vector<std::string> errors;
    for (auto& d : lexer.getDiagnostics()) errors.push_back(d.formatWithSeverity());

    smc::ModuleSearchPaths paths{{STD_DIR}};
    std::set<std::string> loaded;
    std::vector<std::unique_ptr<DeclAST>> importedDecls;
    smc::preloadImports(smc::collectImportSpecifiers(tokens), ".", paths, loaded,
                        importedDecls, errors);
    if (!errors.empty()) return errors;

    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) {
        errors.push_back("parse failed");
        return errors;
    }
    for (auto& d : parser.getErrors()) errors.push_back(d.formatWithSeverity());
    if (!errors.empty()) return errors;
    if (!importedDecls.empty()) {
        ast->declarations.insert(ast->declarations.begin(),
                                 std::make_move_iterator(importedDecls.begin()),
                                 std::make_move_iterator(importedDecls.end()));
    }
    smc::processImports(*ast, ".", paths, loaded, errors);
    if (!errors.empty()) return errors;

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    for (auto& d : analyzer.getErrors()) errors.push_back(d.format());
    return errors;
}

static bool hasErrorContaining(const std::vector<std::string>& errors,
                               const std::string& needle) {
    for (const auto& e : errors) {
        if (e.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST_F(ConstexprContainersTest, VecSumInConstexpr) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
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
    )", "ct_vec_sum.cmm"), 0);
}

TEST_F(ConstexprContainersTest, HashMapInConstexpr) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        import std.hash;
        constexpr int32 mapWork() {
            std::HashMap<uint32,int32> m;
            m.init(std::hash_key_u32, std::equal_u32);
            uint32 i = 0;
            while (i < 200) {
                if (!m.put(i, (int32)(i * 3))) return -1;
                i = i + 1;
            }
            if (m.size() != 200) return -2;
            if (!m.put(7, 777)) return -3;
            if (m.size() != 200) return -4;
            int32 out = 0;
            if (!m.get(7, &out) || out != 777) return -5;
            if (!m.remove(7)) return -6;
            if (m.contains(7)) return -7;
            if (m.size() != 199) return -8;
            if (!m.put(7, 7)) return -9;
            if (!m.get(7, &out) || out != 7) return -10;
            m.destroy();
            return 0;
        }
        int32 main() { constexpr int32 x = mapWork(); return x; }
    )", "ct_map.cmm"), 0);
}

TEST_F(ConstexprContainersTest, HashSetInConstexpr) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        import std.hash;
        constexpr int32 setWork() {
            std::HashSet<uint64> s;
            s.init(std::hash_key_u64, std::equal_u64);
            uint64 i = 0;
            while (i < 64) {
                if (!s.insert(i * 2)) return -1;
                i = i + 1;
            }
            if (s.size() != 64) return -2;
            if (!s.contains(42)) return -3;
            if (s.contains(43)) return -4;
            if (!s.remove(42)) return -5;
            if (s.contains(42)) return -6;
            if (!s.insert(42)) return -7;
            if (!s.contains(42)) return -8;
            s.destroy();
            return 0;
        }
        int32 main() { constexpr int32 x = setWork(); return x; }
    )", "ct_set.cmm"), 0);
}

TEST_F(ConstexprContainersTest, StringViewElementsInConstexpr) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        import std.hash;
        constexpr int32 strWork() {
            std::Vec<string_view> v;
            v.init();
            v.push("alpha");
            v.push("beta");
            v.push("gamma");
            if (v.len != 3) return -1;
            if (v.get(1) != "beta") return -2;
            if (!v.contains("gamma", std::equal_string)) return -3;
            if (v.contains("delta", std::equal_string)) return -4;
            v.destroy();
            return 0;
        }
        int32 main() { constexpr int32 x = strWork(); return x; }
    )", "ct_vec_string.cmm"), 0);
}

TEST_F(ConstexprContainersTest, OutParametersInConstexpr) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr int32 outWork() {
            std::Vec<int32> v;
            v.init();
            v.push(10);
            v.push(20);
            int32 out = 0;
            if (!v.pop(&out) || out != 20) return -1;
            if (v.len != 1) return -2;
            return out;
        }
        int32 main() { constexpr int32 x = outWork(); return x - 20; }
    )", "ct_out.cmm"), 0);
}

TEST_F(ConstexprContainersTest, ConstexprLoopStepLimit) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr int32 spin() {
            while (true) { }
            return 0;
        }
        int32 main() { constexpr int32 x = spin(); return x; }
    )", "ct_step_limit.cmm"), -1);
}

TEST_F(ConstexprContainersTest, DequeRingInConstexpr) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
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
    )", "ct_deque_ring.cmm"), 0);
}

TEST_F(ConstexprContainersTest, AcceptsContainerConstexprFunction) {
    auto errors = analyzeWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr std::Vec<int32> make() {
            std::Vec<int32> v;
            v.init();
            v.push(1);
            return v;
        }
        constexpr std::Vec<int32> T = make();
        constexpr std::Vec<int32> B = T;
        int32 main() { return T.get(0) + B.get(0) - 2; }
    )", "ct_accept_vec.cmm");
    for (auto& e : errors) std::cerr << "sema: " << e << "\n";
    EXPECT_TRUE(errors.empty());
}

TEST_F(ConstexprContainersTest, RejectsMutationOfConstexprContainer) {
    auto errors = analyzeWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr std::Vec<int32> make() {
            std::Vec<int32> v;
            v.init();
            return v;
        }
        constexpr std::Vec<int32> T = make();
        int32 main() { T.push(1); return 0; }
    )", "ct_mutate_vec.cmm");
    EXPECT_TRUE(hasErrorContaining(errors, "constexpr container"));
}

TEST_F(ConstexprContainersTest, RejectsFieldWriteOnConstexprContainer) {
    auto errors = analyzeWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr std::Vec<int32> make() {
            std::Vec<int32> v;
            v.init();
            return v;
        }
        constexpr std::Vec<int32> T = make();
        int32 main() { T.len = 0; return 0; }
    )", "ct_mutate_field.cmm");
    EXPECT_TRUE(hasErrorContaining(errors, "constexpr container"));
}

TEST_F(ConstexprContainersTest, RejectsMutableCopyOfConstexprContainer) {
    auto errors = analyzeWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr std::Vec<int32> make() {
            std::Vec<int32> v;
            v.init();
            return v;
        }
        constexpr std::Vec<int32> T = make();
        int32 main() { std::Vec<int32> M = T; return 0; }
    )", "ct_copy_vec.cmm");
    EXPECT_TRUE(hasErrorContaining(errors, "read-only"));
}

TEST_F(ConstexprContainersTest, StillRejectsStringConstexprFunction) {
    auto errors = analyzeWithImports(R"(
        constexpr string make() { return string.new("x"); }
        int32 main() { return 0; }
    )", "ct_string_constexpr.cmm");
    EXPECT_TRUE(hasErrorContaining(errors, "literal return type"));
}

TEST_F(ConstexprContainersTest, VecTableIsReadableAtRuntime) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr std::Vec<int32> make() {
            std::Vec<int32> v;
            v.init();
            for (usize i = 0; i < 8; i = i + 1) v.push((int32)(i * i));
            return v;
        }
        constexpr std::Vec<int32> T = make();
        int32 main() {
            if (T.len != 8) return 1;
            if (T.get(5) != 25) return 2;
            if (T.data[6] != 36) return 3;
            return 0;
        }
    )", "ct_table_vec.cmm"), 0);
}

TEST_F(ConstexprContainersTest, DequeTableIsReadableAtRuntime) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr std::Deque<int32> make() {
            std::Deque<int32> d;
            d.init();
            d.push_back(1);
            d.push_front(0);
            d.push_back(2);
            return d;
        }
        constexpr std::Deque<int32> T = make();
        int32 main() {
            if (T.len != 3) return 1;
            if (T.get(0) != 0 || T.get(1) != 1 || T.get(2) != 2) return 2;
            return 0;
        }
    )", "ct_table_deque.cmm"), 0);
}

TEST_F(ConstexprContainersTest, EmptyVecTableIsReadableAtRuntime) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr std::Vec<int32> make() {
            std::Vec<int32> v;
            v.init();
            return v;
        }
        constexpr std::Vec<int32> T = make();
        int32 main() {
            if (T.len != 0) return 1;
            if (!T.is_empty()) return 2;
            return 0;
        }
    )", "ct_table_empty.cmm"), 0);
}

TEST_F(ConstexprContainersTest, StringViewTableIsReadableAtRuntime) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr std::Vec<string_view> make() {
            std::Vec<string_view> v;
            v.init();
            v.push("alpha");
            v.push("beta");
            v.push("gamma");
            return v;
        }
        constexpr std::Vec<string_view> T = make();
        int32 main() {
            if (T.len != 3) return 1;
            if (T.get(1) != "beta") return 2;
            if (T.get(2).len() != 5) return 3;
            return 0;
        }
    )", "ct_table_str.cmm"), 0);
}

TEST_F(ConstexprContainersTest, HashMapTableIsReadableAtRuntime) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        import std.hash;
        constexpr std::HashMap<uint32,int32> make() {
            std::HashMap<uint32,int32> m;
            m.init(std::hash_key_u32, std::equal_u32);
            for (uint32 i = 0; i < 64; i = i + 1) m.put(i, (int32)(i * 2));
            return m;
        }
        constexpr std::HashMap<uint32,int32> T = make();
        int32 main() {
            int32 out = 0;
            if (!T.get(17, &out) || out != 34) return 1;
            if (T.size() != 64) return 2;
            if (!T.contains(63)) return 3;
            return 0;
        }
    )", "ct_table_map.cmm"), 0);
}

TEST_F(ConstexprContainersTest, HashSetTableIsReadableAtRuntime) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        import std.hash;
        constexpr std::HashSet<uint64> make() {
            std::HashSet<uint64> s;
            s.init(std::hash_key_u64, std::equal_u64);
            for (uint64 i = 0; i < 32; i = i + 1) s.insert(i * 3);
            return s;
        }
        constexpr std::HashSet<uint64> T = make();
        int32 main() {
            if (T.size() != 32) return 1;
            if (!T.contains(30)) return 2;
            if (T.contains(31)) return 3;
            return 0;
        }
    )", "ct_table_set.cmm"), 0);
}

TEST_F(ConstexprContainersTest, StringViewMapTableIsReadableAtRuntime) {
    EXPECT_EQ(runWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        import std.hash;
        constexpr std::HashMap<string_view,int32> make() {
            std::HashMap<string_view,int32> m;
            m.init(std::hash_key_string, std::equal_string);
            m.put("one", 1);
            m.put("two", 2);
            m.put("three", 3);
            return m;
        }
        constexpr std::HashMap<string_view,int32> T = make();
        int32 main() {
            int32 out = 0;
            if (!T.get("two", &out) || out != 2) return 1;
            if (!T.contains("three")) return 2;
            if (T.contains("four")) return 3;
            return 0;
        }
    )", "ct_table_strmap.cmm"), 0);
}

TEST_F(ConstexprContainersTest, RejectsMutableAssignmentFromConstexprContainer) {
    auto errors = analyzeWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr std::Vec<int32> make() {
            std::Vec<int32> v;
            v.init();
            return v;
        }
        constexpr std::Vec<int32> T = make();
        int32 main() {
            std::Vec<int32> M;
            M.init();
            M = T;
            return 0;
        }
    )", "ct_assign_copy.cmm");
    EXPECT_TRUE(hasErrorContaining(errors, "read-only"));
}

TEST_F(ConstexprContainersTest, RejectsGetPtrOnConstexprContainer) {
    auto errors = analyzeWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr std::Vec<int32> make() { std::Vec<int32> v; v.init(); v.push(1); return v; }
        constexpr std::Vec<int32> T = make();
        int32 main() { int32* p = T.get_ptr(0); return *p; }
    )", "ct_get_ptr.cmm");
    EXPECT_TRUE(hasErrorContaining(errors, "constexpr container"));
}

TEST_F(ConstexprContainersTest, RejectsMutablePointerIntoConstexprData) {
    auto errors = analyzeWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr std::Vec<int32> make() { std::Vec<int32> v; v.init(); v.push(1); return v; }
        constexpr std::Vec<int32> T = make();
        int32 main() {
            int32* p = T.data;
            int32* q = T.data + 1;
            *p = 9;
            *q = 10;
            return 0;
        }
    )", "ct_data_ptr.cmm");
    EXPECT_TRUE(hasErrorContaining(errors, "constexpr container"));
}

TEST_F(ConstexprContainersTest, RejectsPassingConstexprTableByValue) {
    auto errors = analyzeWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr std::Vec<int32> make() { std::Vec<int32> v; v.init(); v.push(1); return v; }
        constexpr std::Vec<int32> T = make();
        int32 mutate(std::Vec<int32> w) { w.push(2); return w.len; }
        int32 main() { return mutate(T); }
    )", "ct_pass_value.cmm");
    EXPECT_TRUE(hasErrorContaining(errors, "read-only"));
}

TEST_F(ConstexprContainersTest, AllowsScalarReadFromConstexprTable) {
    auto errors = analyzeWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr std::Vec<int32> make() { std::Vec<int32> v; v.init(); v.push(1); return v; }
        constexpr std::Vec<int32> T = make();
        int32 main() {
            int32 n = T.len;
            int32 x = T.get(0);
            int32 y = T.data[0];
            return n + x + y - 3;
        }
    )", "ct_scalar_read.cmm");
    for (auto& e : errors) std::cerr << "sema: " << e << "\n";
    EXPECT_TRUE(errors.empty());
}

TEST_F(ConstexprContainersTest, AllowsShadowingConstexprContainerName) {
    auto errors = analyzeWithImports(R"(
        import std.vec;
        import std.deque;
        import std.hashmap;
        import std.hashset;
        constexpr std::Vec<int32> make() { std::Vec<int32> v; v.init(); return v; }
        constexpr std::Vec<int32> T = make();
        int32 main() {
            std::Vec<int32> T;
            T.init();
            T.push(9);
            return T.len == 1 ? 0 : 1;
        }
    )", "ct_shadow.cmm");
    for (auto& e : errors) std::cerr << "sema: " << e << "\n";
    EXPECT_TRUE(errors.empty());
}
