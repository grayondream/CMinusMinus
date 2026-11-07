// P1-08 / CG-10: inline asm 从源码到宿主 x86-64 LLJIT 可执行。
#include <gtest/gtest.h>

#include "codegen/CodegenContext.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "support/Log.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"

class InlineAsmE2E : public ::testing::Test {
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

static int runSource(const std::string& source, const std::string& file = "inline_asm.cmm") {
    Lexer lexer(file, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast || !parser.getErrors().empty()) return -1;

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) return -1;

    CodegenContext ctx;
    ctx.setSourceFile(file);
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

TEST_F(InlineAsmE2E, NoOpVolatile) {
    EXPECT_EQ(runSource("int32 main() { asm volatile(\"nop\"); return 0; }"), 0);
}

TEST_F(InlineAsmE2E, OutputRegister) {
    EXPECT_EQ(runSource("int32 main() { int32 out; asm volatile(\"mov $7, %0\" : \"=r\"(out)); return out; }"), 7);
}

TEST_F(InlineAsmE2E, ReadWriteOperand) {
    EXPECT_EQ(runSource("int32 main() { int32 x = 5; int32 y = 3; asm(\"add %1, %0\" : \"+r\"(x) : \"r\"(y)); return x; }"), 8);
}

TEST_F(InlineAsmE2E, MemoryClobber) {
    EXPECT_EQ(runSource("int32 main() { asm volatile(\"\" : : : \"memory\"); return 0; }"), 0);
}

TEST_F(InlineAsmE2E, PercentEscape) {
    EXPECT_EQ(runSource("int32 main() { int32 out = 0; asm(\"mov %%eax, %0\" : \"=r\"(out)); return out == out; }"), 1);
}

TEST_F(InlineAsmE2E, MultipleOutputs) {
    EXPECT_EQ(runSource("int32 main() { int32 a; int32 b; asm(\"mov $1, %0; mov $2, %1\" : \"=r\"(a), \"=r\"(b)); return a * 10 + b; }"), 12);
}

// 最终评审 I1：把 `=r`/`+r` 混用、多 `+r`、多输入、early-clobber、`m` 输入、
// 多位 `%N` 等偏差路径钉成 e2e（评审前只有基础 6 用例，未覆盖 tied 约束编号机制）。
TEST_F(InlineAsmE2E, MixedOutputKinds) {
    // GCC 编号: %0=a(=r 输出0), %1=b(+r 输出1), %2=c(输入0)。
    EXPECT_EQ(runSource("int32 main() { int32 a; int32 b = 7; int32 c = 3; "
                        "asm(\"mov %2, %0; add %2, %1\" : \"=r\"(a), \"+r\"(b) : \"r\"(c)); "
                        "return a * 10 + b; }"), 40);
}

TEST_F(InlineAsmE2E, MultipleReadWrite) {
    // %0=x, %1=y, %2=z；x+=z, y+=x。
    EXPECT_EQ(runSource("int32 main() { int32 x = 5; int32 y = 7; int32 z = 2; "
                        "asm(\"add %2, %0; add %0, %1\" : \"+r\"(x), \"+r\"(y) : \"r\"(z)); "
                        "return x * 10 + y; }"), 84);
}

TEST_F(InlineAsmE2E, MultipleInputs) {
    // %0=a, %1=b, %2=c；a=c 后 a+=b。用 & 早破坏防输出与输入共享寄存器
    // （模板先写输出再读 %1，无 & 时是 GCC/LLVM 都允许的别名 UB）。
    EXPECT_EQ(runSource("int32 main() { int32 a; int32 b = 3; int32 c = 4; "
                        "asm(\"mov %2, %0; add %1, %0\" : \"=&r\"(a) : \"r\"(b), \"r\"(c)); "
                        "return a; }"), 7);
}

TEST_F(InlineAsmE2E, EarlyClobberOutput) {
    EXPECT_EQ(runSource("int32 main() { int32 a; int32 b = 5; "
                        "asm(\"mov %1, %0\" : \"=&r\"(a) : \"r\"(b)); return a; }"), 5);
}

TEST_F(InlineAsmE2E, MemoryInput) {
    // `m` 输入按值 spill（LLVM 限制，见 spec §5）：只读模板读到的值一致。
    EXPECT_EQ(runSource("int32 main() { int32 a; int32 b = 5; "
                        "asm(\"mov %1, %0\" : \"=r\"(a) : \"m\"(b)); return a; }"), 5);
}

TEST_F(InlineAsmE2E, MultiDigitOperand) {
    // %10 指第 11 个操作数（10 个输出 + 1 个输入），钉多位编号解析。
    EXPECT_EQ(runSource("int32 main() { int32 r0; int32 r1; int32 r2; int32 r3; int32 r4; "
                        "int32 r5; int32 r6; int32 r7; int32 r8; int32 r9; int32 v = 6; "
                        "asm(\"mov %10, %0; mov %10, %1\" : \"=r\"(r0), \"=r\"(r1), \"=r\"(r2), "
                        "\"=r\"(r3), \"=r\"(r4), \"=r\"(r5), \"=r\"(r6), \"=r\"(r7), \"=r\"(r8), "
                        "\"=r\"(r9) : \"r\"(v)); return r0 * 10 + r1; }"), 66);
}

// 最终评审 I3：GCC 语义——无输出操作数的 asm 无论有无 volatile 关键字都视为
// volatile（HasSideEffects=true），否则优化下可被 DCE 掉。此测试在 IR 层钉住
// InlineAsm 的 sideeffect 标志。
TEST_F(InlineAsmE2E, ZeroOutputAsmIsImplicitlyVolatile) {
    const std::string source = "int32 main() { asm(\"nop\"); return 0; }";
    Lexer lexer("inline_asm.cmm", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty());

    CodegenContext ctx;
    ctx.setSourceFile("inline_asm.cmm");
    ast->codegen(ctx);
    ctx.finalizeDebugInfo();

    bool foundAsm = false;
    for (auto& fn : ctx.getModule()) {
        for (auto& bb : fn) {
            for (auto& inst : bb) {
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    if (auto* ia = llvm::dyn_cast<llvm::InlineAsm>(call->getCalledOperand())) {
                        foundAsm = true;
                        EXPECT_TRUE(ia->hasSideEffects())
                            << "zero-output asm must be implicitly volatile (GCC semantics)";
                    }
                }
            }
        }
    }
    EXPECT_TRUE(foundAsm);
}
