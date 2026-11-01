// P0-02 / MEM-10: C interop through function pointers. Function-pointer
// variables, function-pointer parameters (callbacks), nested callbacks, and a
// full libc `qsort`/`bsearch` round trip through the std.c binding layer.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "ast/Decl.h"
#include "codegen/CodegenContext.h"
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

class FunctionPointerE2E : public ::testing::Test {
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

// Compile `source` together with the built-in std.c binding layer and run main
// in-process. Returns main's value, or -1 on any failure.
static int runWithPrelude(const std::string& source, const std::string& filename) {
    auto ast = smc::parseStdCPrelude(source, filename);
    if (!ast) {
        ADD_FAILURE() << "user parse failed";
        return -1;
    }
    auto prelude = smc::parseStdCPrelude(smc::loadStdCPrelude({STD_DIR}), "<std.c>");
    if (!prelude) {
        ADD_FAILURE() << "prelude parse failed";
        return -1;
    }
    smc::prependDeclarations(*ast, *prelude);

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) {
        for (auto& err : analyzer.getErrors()) {
            ADD_FAILURE() << "semantic error: " << err.format();
        }
        return -1;
    }

    CodegenContext ctx;
    ctx.setSourceFile(filename);
    ast->codegen(ctx);
    ctx.finalizeDebugInfo();

    std::string ve;
    llvm::raw_string_ostream vs(ve);
    if (llvm::verifyModule(ctx.getModule(), &vs)) {
        vs.flush();
        ADD_FAILURE() << "module verification failed: " << ve;
        return -1;
    }

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        ADD_FAILURE() << "detectHost failed";
        return -1;
    }
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb))
        .create();
    if (!jit) {
        ADD_FAILURE() << "LLJIT create failed";
        return -1;
    }

    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto e = (*jit)->addIRModule(std::move(ts))) {
        ADD_FAILURE() << "addIRModule failed";
        return -1;
    }

    auto sym = (*jit)->lookup("main");
    if (!sym) {
        ADD_FAILURE() << "lookup main failed";
        return -1;
    }
    return ((int (*)())(intptr_t)sym->getValue())();
}

TEST_F(FunctionPointerE2E, FunctionPointerVariable) {
    EXPECT_EQ(runWithPrelude(R"(
        int32 add(int32 a, int32 b) { return a + b; }
        int32 main() {
            int32 (*fp)(int32, int32) = add;
            return fp(3, 4) - 7;
        }
    )", "fp_var.c"), 0);
}

TEST_F(FunctionPointerE2E, FunctionPointerParameter) {
    EXPECT_EQ(runWithPrelude(R"(
        int32 apply(int32 (*cb)(int32, int32), int32 a, int32 b) { return cb(a, b); }
        int32 mul(int32 a, int32 b) { return a * b; }
        int32 main() { return apply(mul, 6, 7) - 42; }
    )", "fp_param.c"), 0);
}

TEST_F(FunctionPointerE2E, UnnamedFunctionPointerParameter) {
    // `int (*)(int)` has no name; it may still be declared and the body may
    // ignore it.
    EXPECT_EQ(runWithPrelude(R"(
        int32 apply(int32 (*)(int32), int32 x) { return x + 1; }
        int32 main() { return apply(0, 41) - 42; }
    )", "fp_unnamed.c"), 0);
}

TEST_F(FunctionPointerE2E, NestedCallback) {
    // A callback that itself takes a callback.
    EXPECT_EQ(runWithPrelude(R"(
        int32 twice(int32 (*f)(int32), int32 x) { return f(f(x)); }
        int32 inc(int32 x) { return x + 1; }
        int32 main() { return twice(inc, 5) - 7; }
    )", "fp_nested.c"), 0);
}

TEST_F(FunctionPointerE2E, LibcQsortCallback) {
    EXPECT_EQ(runWithPrelude(R"(
        int32 cmp_int(void* a, void* b) {
            int32* x = (int32*)a;
            int32* y = (int32*)b;
            if (*x < *y) return -1;
            if (*x > *y) return 1;
            return 0;
        }
        int32 main() {
            int32 arr[5];
            arr[0] = 5; arr[1] = 3; arr[2] = 4; arr[3] = 1; arr[4] = 2;
            qsort(arr, 5, 4, cmp_int);
            int32 i = 0;
            while (i < 5) {
                if (arr[i] != i + 1) return 1;
                i = i + 1;
            }
            return 0;
        }
    )", "fp_qsort.c"), 0);
}

TEST_F(FunctionPointerE2E, LibcBsearchCallback) {
    EXPECT_EQ(runWithPrelude(R"(
        int32 cmp_int(void* a, void* b) {
            int32* x = (int32*)a;
            int32* y = (int32*)b;
            if (*x < *y) return -1;
            if (*x > *y) return 1;
            return 0;
        }
        int32 main() {
            int32 arr[5];
            arr[0] = 1; arr[1] = 2; arr[2] = 3; arr[3] = 4; arr[4] = 5;
            int32 key = 4;
            int32* found = (int32*)bsearch(&key, arr, 5, 4, cmp_int);
            if (found == null) return 1;
            return *found - 4;
        }
    )", "fp_bsearch.c"), 0);
}

TEST_F(FunctionPointerE2E, CastRvaluePointerMemberAccess) {
    // `((struct S*)p)->field`: the cast is an rvalue pointer, so member access
    // must not dereference it as if it were an alloca (regression).
    EXPECT_EQ(runWithPrelude(R"(
        struct Point { int32 x; int32 y; };
        int32 main() {
            struct Point pt;
            pt.x = 9; pt.y = 2;
            void* p = &pt;
            struct Point* q = (struct Point*)p;
            int32 viaVar = q->x;
            int32 viaCast = ((struct Point*)p)->y;
            return viaVar + viaCast - 11;
        }
    )", "fp_cast_member.c"), 0);
}

TEST_F(FunctionPointerE2E, CastPointerRoundTrip) {
    EXPECT_EQ(runWithPrelude(R"(
        int32 main() {
            int32 v = 7;
            void* p = (void*)&v;
            int32* q = (int32*)p;
            return *q - 7;
        }
    )", "fp_cast_roundtrip.c"), 0);
}

TEST_F(FunctionPointerE2E, FunctionReturningFunctionPointer) {
    EXPECT_EQ(runWithPrelude(R"(
        int32 add1(int32 x) { return x + 1; }
        int32 (*get(int32 k))(int32) { return add1; }
        int32 main() {
            if (get(0)(1) != 2) return 1;
            int32 (*p)(int32) = get(1);
            return p(3) - 4;
        }
    )", "fp_ret_fnptr.c"), 0);
}

TEST_F(FunctionPointerE2E, FunctionTypeAlias) {
    EXPECT_EQ(runWithPrelude(R"(
        using F = int32(int32);
        int32 inc(int32 x) { return x + 1; }
        F* get() { return inc; }
        int32 main() {
            F* f = get();
            return f(1) == 2 ? 0 : 1;
        }
    )", "fp_fn_type_alias.c"), 0);
}

TEST_F(FunctionPointerE2E, PointerToArrayDeclarator) {
    EXPECT_EQ(runWithPrelude(R"(
        int32 main() {
            int32 a[3] = {1, 2, 3};
            int32 (*p)[3] = &a;
            (*p)[1] = 7;
            return a[1] - 7;
        }
    )", "fp_ptr_to_array.c"), 0);
}

TEST_F(FunctionPointerE2E, ArrayOfFunctionPointers) {
    EXPECT_EQ(runWithPrelude(R"(
        int32 inc(int32 x) { return x + 1; }
        int32 dbl(int32 x) { return x * 2; }
        int32 (*arr[2])(int32) = { inc, dbl };
        int32 main() {
            if (arr[0](3) != 4) return 1;
            if (arr[1](3) != 6) return 2;
            return 0;
        }
    )", "fp_array_fnptr.c"), 0);
}
