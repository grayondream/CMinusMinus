// P1-03 / GEN-03: 类模板端到端。fixture 仿 test_optional_result.cpp。

#include <gtest/gtest.h>

#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "sema/TemplateRegistry.h"
#include "codegen/CodegenContext.h"
#include "support/Log.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"

#include <spdlog/spdlog.h>

class GenericE2E : public ::testing::Test {
protected:
    void SetUp() override {
        spdlog::set_level(spdlog::level::off);
        TemplateRegistry::instance().resetForTesting();
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
    if (auto e = (*jit)->addIRModule(std::move(ts))) {
        ADD_FAILURE() << "addIRModule failed";
        return -1;
    }

    auto sym = (*jit)->lookup("main");
    if (!sym) {
        ADD_FAILURE() << "lookup main failed";
        return -1;
    }

    auto fn = (int (*)())(intptr_t)sym->getValue();
    return fn();
}

TEST_F(GenericE2E, BoxRoundTrip) {
    EXPECT_EQ(runSource(R"(
        template<typename T> struct Box { T value; };
        int32 main() {
            Box<int32> b;
            b.value = 42;
            return b.value;
        }
    )", "gen1.c"), 42);
}

TEST_F(GenericE2E, BoxNested) {
    EXPECT_EQ(runSource(R"(
        template<typename T> struct Box { T value; };
        int32 main() {
            Box<Box<int32>> b;
            b.value.value = 7;
            return b.value.value;
        }
    )", "gen2.c"), 7);
}

TEST_F(GenericE2E, GenericFuncRoundTrip) {
    EXPECT_EQ(runSource(R"(
        template<typename T> T tmax(T a, T b) { return a; }
        int32 main() {
            return tmax(3, 4);
        }
    )", "gen3.c"), 3);
}

TEST_F(GenericE2E, GenericFuncExplicit) {
    EXPECT_EQ(runSource(R"(
        template<typename T> T tmax(T a, T b) { return a; }
        int32 main() {
            return tmax<float64>(2.5, 1.5) == 2.5 ? 7 : 0;
        }
    )", "gen4.c"), 7);
}

TEST_F(GenericE2E, NonTemplateExactMatchWins) {
    EXPECT_EQ(runSource(R"(
        template<typename T> T tmax(T a, T b) { return a; }
        int32 tmax(int32 a, int32 b) { return 99; }
        int32 main() {
            // 普通函数精确匹配优先（SEM-16 最小规则）
            if (tmax(3, 4) == 99) return 1;
            return 0;
        }
    )", "gen5.c"), 1);
}

TEST_F(GenericE2E, FixedArrayTemplate) {
    EXPECT_EQ(runSource(R"(
        template<typename T, usize N> struct Arr { T data[N]; };
        int32 main() {
            Arr<int32, 4> a;
            a.data[0] = 1; a.data[1] = 2; a.data[2] = 3; a.data[3] = 4;
            return a.data[0] + a.data[1] + a.data[2] + a.data[3];
        }
    )", "gen6.c"), 10);
}

TEST_F(GenericE2E, AliasTemplateE2E) {
    EXPECT_EQ(runSource(R"(
        template<typename T, usize N> struct Arr { T data[N]; };
        template<typename T> using Vec = Arr<T, 2>;
        int32 main() {
            Vec<int32> v;
            v.data[0] = 5; v.data[1] = 6;
            return v.data[0] * 10 + v.data[1];
        }
    )", "gen7.c"), 56);
}

TEST_F(GenericE2E, ThisFieldAccess) {
    EXPECT_EQ(runSource(R"(
        class FieldA {
        public:
            int32 v;
            int32 get() { return this.v; }
        };
        int32 main() {
            FieldA a;
            a.v = 9;
            return a.get();
        }
    )", "gen8.c"), 9);
}

TEST_F(GenericE2E, ThisPassthrough) {
    EXPECT_EQ(runSource(R"(
        class SelfA {
        public:
            int32 v;
            SelfA* self() { return this; }
        };
        int32 main() {
            SelfA a;
            a.v = 1;
            a.self()->v = 6;
            return a.v;
        }
    )", "gen9.c"), 6);
}

// ========== P1-03 / INH-05 / GEN-09: CRTP 全链路 ==========

TEST_F(GenericE2E, CrtpStaticDispatch) {
    EXPECT_EQ(runSource(R"(
        template<typename D>
        class Shape {
        public:
            float64 twice_area() { return static_cast<D*>(this)->area() * 2.0; }
        };
        class Circle : Shape<Circle> {
        public:
            float64 r;
            float64 area() { return 3.14159 * this->r * this->r; }
        };
        int32 main() {
            Circle c;
            c.r = 1.0;
            float64 t = c.twice_area();
            return t > 6.0 && t < 6.3 ? 1 : 0;
        }
    )", "gen10.c"), 1);
}

TEST_F(GenericE2E, CrtpSameShape) {
    EXPECT_EQ(runSource(R"(
        template<typename D>
        class Shape {
        public:
            float64 twice_area() { return static_cast<D*>(this)->area() * 2.0; }
            bool same_shape(D* other) {
                return static_cast<D*>(this)->area() == other->area();
            }
        };
        class Circle2 : Shape<Circle2> {
        public:
            float64 r;
            float64 area() { return 3.14159 * this->r * this->r; }
        };
        int32 main() {
            Circle2 c1;
            Circle2 c2;
            c1.r = 1.0;
            c2.r = 1.0;
            return c1.same_shape(&c2) ? 1 : 0;
        }
    )", "gen11.c"), 1);
}

// ========== P1-03 / T8 硬化 ==========

TEST_F(GenericE2E, UnusedTemplateNoSymbol) {
    // 惰性实例化：只声明不使用 → 零符号。
    Lexer lexer("gen12.c",
        "template<typename T> T unused_fn(T x) { return x; }\n"
        "int32 main() { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_TRUE(analyzer.getErrors().empty());
    CodegenContext ctx;
    ctx.setSourceFile("gen12.c");
    ast->codegen(ctx);
    EXPECT_EQ(ctx.getModule().getFunction("unused_fn"), nullptr);
    EXPECT_EQ(ctx.getModule().getFunction("unused_fn_int32"), nullptr);
}

TEST_F(GenericE2E, DeepNestingBox) {
    EXPECT_EQ(runSource(R"(
        template<typename T> struct Box { T value; };
        int32 main() {
            Box<Box<Box<int32>>> b;
            b.value.value.value = 5;
            return b.value.value.value * 2;
        }
    )", "gen13.c"), 10);
}

TEST_F(GenericE2E, CallerScopeTypeParamName) {
    // 调用方作用域里与模板参数同名的变量——克隆体绑定具体类型，
    // 调用方变量的名字不参与推导/替换。
    EXPECT_EQ(runSource(R"(
        template<typename T> T id2(T a) { return a; }
        int32 main() {
            int32 T = 5;
            int32 r = id2(7);
            return r == 7 && T == 5 ? 1 : 0;
        }
    )", "gen14.c"), 1);
}

// ========== P1-03 评审修复轮（fix-first） ==========

TEST_F(GenericE2E, InstanceSharedAcrossFunctions) {
    // C1：同一实例在两个函数中调用——实例符号不得落在首次调用者的局部
    // 作用域。
    EXPECT_EQ(runSource(R"(
        template<typename T> T tmax(T a, T b) { return a; }
        int32 f() { return tmax(3, 4); }
        int32 main() { return f() + tmax(1, 2); }
    )", "gen15.c"), 4);
}

TEST_F(GenericE2E, SpeculativeRollbackRestoresRshift) {
    // C2：投机解析失败回滚须恢复被拆分的 >> token（静默错译回归）。
    EXPECT_EQ(runSource(R"(
        using b = int32;
        int32 main() {
            int32 b2 = 8; int32 x = 0;
            int32 b = b2;
            int32 y = x < b >> 2;
            return y ? 1 : 2;
        }
    )", "gen16.c"), 1);
}

TEST_F(GenericE2E, TemplateClassInheritsPlainBase) {
    // C3：模板类继承非模板基类——实例布局须含基类子对象槽。
    EXPECT_EQ(runSource(R"(
        class Base {
        public:
            int32 b;
        };
        template<typename T> class W : Base {
        public:
            T v;
        };
        int32 main() {
            W<int32> w;
            w.b = 3;
            w.v = 4;
            return w.b * 10 + w.v;
        }
    )", "gen17.c"), 34);
}

TEST_F(GenericE2E, NestedEnumInTemplateClass) {
    // I4：克隆器覆盖嵌套 union/enum/type 声明——不得静默丢弃。
    EXPECT_EQ(runSource(R"(
        template<typename T> class EE {
        public:
            enum Color { Red = 7 };
            T tag;
            int32 first() { return (int32)EE::Red; }
        };
        int32 main() {
            EE<int32> e;
            e.tag = 1;
            return e.first();
        }
    )", "gen18.c"), 7);
}

TEST_F(GenericE2E, NamespaceQualifiedTemplateLookup) {
    // 回归：模板注册表使用 Parser 的扁平键（`NsTpl_Box`），而用户写
    // `NsTpl::Box<int32>`。限定名与命名空间内非限定名必须命中同一模板。
    EXPECT_EQ(runSource(R"(
        namespace NsTpl {
            template<typename T> struct Box { T value; };
            template<typename T> T id(T x) { return x; }
        }
        int32 main() {
            NsTpl::Box<int32> b;
            b.value = 9;
            if (NsTpl::id<int32>(b.value) != 9) return 1;
            return 0;
        }
    )", "gen21.c"), 0);
}

TEST_F(GenericE2E, BoolLiteralInTemplateMethodBody) {
    // 回归：模板实例化必须保留 true/false 表达式。此前克隆器未覆盖
    // BoolLiteralExprAST，return true/false 被当成裸 return，非 void 方法
    // 全部被误诊为 "must return a value"。
    EXPECT_EQ(runSource(R"(
        template<typename T> struct FlagBox {
            T value;
            bool always_true() { return true; }
            bool always_false() { return false; }
            bool positive() {
                if (this->value > 0) { return true; }
                return false;
            }
        };
        int32 main() {
            FlagBox<int32> b;
            b.value = 3;
            if (!b.always_true()) return 1;
            if (b.always_false()) return 2;
            if (!b.positive()) return 3;
            b.value = -1;
            if (b.positive()) return 4;
            return 0;
        }
    )", "gen20.c"), 0);
}

TEST_F(GenericE2E, SelfValueFieldDiagnosed) {
    // I3：非模板类值语义自引用（A x）须诊断而非编译器崩溃。
    Lexer lexer("gen19.c", "class BadA { BadA x; };\nint32 main() { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool found = false;
    for (auto& e : analyzer.getErrors()) {
        if (e.message.find("cannot have its own type by value") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found);
}
