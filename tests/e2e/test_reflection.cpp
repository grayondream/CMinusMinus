// CT-07/13: compile_time reflection — name_of / fields_of / attributes_of.
// end-to-end execution tests. fixture 仿 test_string.cpp 的 runSource。

#include <gtest/gtest.h>

#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "codegen/CodegenContext.h"
#include "support/Log.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"

#include <spdlog/spdlog.h>

class ReflectionE2E : public ::testing::Test {
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

static int runSource(const std::string& source, const std::string& filename) {
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) {
        ADD_FAILURE() << "Parse failed";
        return -1;
    }

    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    if (!analyzer.getErrors().empty()) {
        for (auto& err : analyzer.getErrors()) {
            ADD_FAILURE() << "Semantic error: " << err.format();
        }
        return -1;
    }

    CodegenContext ctx;
    ctx.setSourceFile(filename);
    ast->codegen(ctx);
    ctx.finalizeDebugInfo();

    std::string ve;
    llvm::raw_string_ostream vs(ve);
    bool bad = llvm::verifyModule(ctx.getModule(), &vs);
    if (bad) {
        vs.flush();
        ADD_FAILURE() << "Module verification failed: " << ve;
        return -1;
    }

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        ADD_FAILURE() << "JITTargetMachineBuilder::detectHost failed";
        return -1;
    }
    auto jit = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*jtmb))
        .create();
    if (!jit) {
        ADD_FAILURE() << "LLJITBuilder::create failed";
        return -1;
    }

    auto ts = llvm::orc::ThreadSafeModule(ctx.takeModule(), ctx.takeContext());
    if (auto err = (*jit)->addIRModule(std::move(ts))) {
        ADD_FAILURE() << "addIRModule failed";
        return -1;
    }

    auto sym = (*jit)->lookup("main");
    if (!sym) {
        ADD_FAILURE() << "main not found";
        return -1;
    }
    return ((int (*)())(intptr_t)sym->getValue())();
}

TEST_F(ReflectionE2E, NameOfReturnsTypeName) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 a; };
        int32 main() {
            string_view n = compile_time.name_of(S);
            // typeToString(S) == "struct S" (8 chars).
            if (n.len() != 8) return 1;
            if (n[0] != 's') return 2;
            if (n[4] != 'c') return 3;
            if (n[7] != 'S') return 4;
            return 0;
        }
    )", "refl_name.cdd"), 0);
}

TEST_F(ReflectionE2E, FieldsOfReturnsFieldTable) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 a; float64 b; };
        int32 main() {
            FieldInfo[] fs = compile_time.fields_of(S);
            if (fs.len != 2) return 1;
            if (fs[0].name.len() != 1 || fs[0].name[0] != 'a') return 2;
            if (fs[0].offset != 0) return 3;
            if (fs[1].name.len() != 1 || fs[1].name[0] != 'b') return 4;
            if (fs[1].offset != 8) return 5;
            return 0;
        }
    )", "refl_fields.cdd"), 0);
}

TEST_F(ReflectionE2E, AttributesOfReturnsAnnotations) {
    EXPECT_EQ(runSource(R"(
        [[deprecated("old")]] struct S { int32 a; };
        int32 main() {
            Attribute[] attrs = compile_time.attributes_of(S);
            if (attrs.len != 1) return 1;
            string_view n = attrs[0].name;
            if (n.len() != 10) return 2;
            for (int32 i = 0; i < 10; ++i) {
                if (n[i] != "deprecated"[i]) return 3;
            }
            return 0;
        }
    )", "refl_attrs.cdd"), 0);
}

TEST_F(ReflectionE2E, FieldsOfNonAggregateRejected) {
    auto ast = [&]() {
        Lexer lexer("refl_na.cdd", R"(
            int32 main() {
                FieldInfo[] fs = compile_time.fields_of(int32);
                return 0;
            }
        )");
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        return parser.parse();
    }();
    if (!ast) {
        ADD_FAILURE() << "Parse failed";
        return;
    }
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
}
