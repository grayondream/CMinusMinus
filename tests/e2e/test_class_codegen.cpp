#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>

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

class ClassCodegenE2E : public ::testing::Test {
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

TEST_F(ClassCodegenE2E, SimpleClass) {
    EXPECT_EQ(runSource(R"(
        class Foo {
            public:
            int32 x;
            void setX(int32 v) { this->x = v; }
            int32 getX() { return this->x; }
        };
        
        int32 main() {
            Foo f;
            f.setX(42);
            return f.getX() - 42;
        }
    )", "test_class_simple.cdd"), 0);
}

TEST_F(ClassCodegenE2E, ClassWithMultipleMethods) {
    EXPECT_EQ(runSource(R"(
        class Counter {
            public:
            int32 count;
            void init() { this->count = 0; }
            void increment() { this->count = this->count + 1; }
            int32 getCount() { return this->count; }
        };
        
        int32 main() {
            Counter c;
            c.init();
            c.increment();
            c.increment();
            c.increment();
            return c.getCount() - 3;
        }
    )", "test_class_multi_method.cdd"), 0);
}

TEST_F(ClassCodegenE2E, ClassWithInheritance) {
    EXPECT_EQ(runSource(R"(
        class Base {
            public:
            int32 x;
            void setX(int32 v) { this->x = v; }
            int32 getX() { return this->x; }
        };
        
        class Derived : public Base {
            public:
            int32 y;
            void setY(int32 v) { this->y = v; }
            int32 getY() { return this->y; }
        };
        
        int32 main() {
            Derived d;
            d.setX(10);
            d.setY(20);
            return d.getX() + d.getY() - 30;
        }
    )", "test_class_inherit.cdd"), 0);
}

TEST_F(ClassCodegenE2E, ClassMethodCallFromMain) {
    EXPECT_EQ(runSource(R"(
        class Math {
            public:
            int32 value;
            void setValue(int32 v) { this->value = v; }
            int32 doubleIt() { return this->value + this->value; }
        };
        
        int32 main() {
            Math m;
            m.setValue(21);
            return m.doubleIt() - 42;
        }
    )", "test_class_method_call.cdd"), 0);
}

// AGG-10: static members — global storage, class-qualified access only.
TEST_F(ClassCodegenE2E, SMCounterPersist) {
    EXPECT_EQ(runSource(R"(
        class SMCtr2 {
            private: static int32 count = 0;
            public: static int32 next() { SMCtr2::count = SMCtr2::count + 1; return SMCtr2::count; }
        };
        int32 main() {
            SMCtr2::next(); SMCtr2::next();
            return SMCtr2::next() == 3 ? 0 : 1;
        }
    )", "test_sm_counter.cdd"), 0);
}

TEST_F(ClassCodegenE2E, SMStaticFactory) {
    EXPECT_EQ(runSource(R"(
        class SMPoint { public: int32 x; int32 y; };
        class SMRegistry {
            public: static SMPoint origin() { SMPoint p; p.x = 0; p.y = 0; return p; }
        };
        int32 main() {
            SMPoint p = SMRegistry::origin();
            return (p.x == 0 && p.y == 0) ? 0 : 1;
        }
    )", "test_sm_factory.cdd"), 0);
}

TEST_F(ClassCodegenE2E, SMPrivateInternalUse) {
    // Review Focus 3: currentClass context covers static methods — no E2009.
    EXPECT_EQ(runSource(R"(
        class SMAcc {
            private: static int32 secret = 11;
            public: int32 reveal() { return SMAcc::secret; }
        };
        int32 main() {
            SMAcc obj;
            return obj.reveal() == 11 ? 0 : 1;
        }
    )", "test_sm_internal.cdd"), 0);
}

TEST_F(ClassCodegenE2E, SMStaticAfterForwardDecl) {
    // Review Focus 2: the type is registered by the forward declaration;
    // the later definition's staticMembers must still emit the global.
    EXPECT_EQ(runSource(R"(
        class SMFwd;
        class SMFwd { public: static int32 v = 9; };
        int32 main() { return SMFwd::v == 9 ? 0 : 1; }
    )", "test_sm_fwd.cdd"), 0);
}

// Review C2: struct with only static members — global storage, no fallback.
TEST_F(ClassCodegenE2E, SMStructStaticE2E) {
    EXPECT_EQ(runSource(R"(
        struct SMS3 { static int32 v = 4; };
        int32 main() { return SMS3::v == 4 ? 0 : 1; }
    )", "test_sm_struct.cdd"), 0);
}

// ========== AGG-11: nested types e2e ==========

TEST_F(ClassCodegenE2E, NTStructFieldE2E) {
    EXPECT_EQ(runSource(R"(
        class NTE1 { public: struct Inner { int32 x; }; };
        int32 main() {
            NTE1::Inner obj; obj.x = 3;
            return obj.x == 3 ? 0 : 1;
        }
    )", "test_nt_struct.cdd"), 0);
}

TEST_F(ClassCodegenE2E, NTEnumConstantE2E) {
    EXPECT_EQ(runSource(R"(
        class NTE2 { public: enum Color { Red, Green }; };
        int32 main() { NTE2::Color c = NTE2::Green; return c == 1 ? 0 : 1; }
    )", "test_nt_enum.cdd"), 0);
}

TEST_F(ClassCodegenE2E, NTStaticE2E) {
    // 注：嵌套带方法的类型用 class（struct 无方法通路，Task 2 Ruling）。
    EXPECT_EQ(runSource(R"(
        class NTE3 { public: class S {
            private: static int32 count = 0;
            public: static int32 next() { NTE3::S::count = NTE3::S::count + 1; return NTE3::S::count; }
        }; };
        int32 main() { NTE3::S::next(); return NTE3::S::next() == 2 ? 0 : 1; }
    )", "test_nt_static.cdd"), 0);
}

TEST_F(ClassCodegenE2E, NTMethodE2E) {
    EXPECT_EQ(runSource(R"(
        class NTE4 { public: class S { public: int32 f(int32 x) { return x; } }; };
        int32 main() { NTE4::S obj; return obj.f(7) == 7 ? 0 : 1; }
    )", "test_nt_method.cdd"), 0);
}

TEST_F(ClassCodegenE2E, NTDeepE2E) {
    EXPECT_EQ(runSource(R"(
        class NTE5 { public: struct A { struct B { int32 v; }; }; };
        int32 main() { NTE5::A::B obj; obj.v = 2; return obj.v == 2 ? 0 : 1; }
    )", "test_nt_deep.cdd"), 0);
}

TEST_F(ClassCodegenE2E, NTFwdWithNestedE2E) {
    // Review Focus 2：前向声明+定义时 nestedTypes 仍生成。嵌套类带方法，
    // 确保既有类型提前 return 分支也执行 nestedTypes 遍历。
    EXPECT_EQ(runSource(R"(
        class NTE6;
        class NTE6 { public: class S { public: int32 f(int32 x) { return x; } }; };
        int32 main() { NTE6::S obj; return obj.f(6) == 6 ? 0 : 1; }
    )", "test_nt_fwd.cdd"), 0);
}

TEST_F(ClassCodegenE2E, NTSelfRefPtrE2E) {
    // 前向声明语义钉住（DS6）：自引用指针。
    EXPECT_EQ(runSource(R"(
        struct NFNode { int32 v; struct NFNode* next; };
        int32 main() {
            struct NFNode n; n.v = 1; n.next = 0;
            return n.v == 1 ? 0 : 1;
        }
    )", "test_nt_selfref.cdd"), 0);
}

// ========== INH: inheritance e2e ==========

TEST_F(ClassCodegenE2E, INHStructE2E) {
    EXPECT_EQ(runSource(R"(
        struct B20 { int32 x; };
        struct D20 : public B20 { int32 y; };
        int32 main() { D20 d; d.x = 3; d.y = 4; return (d.x == 3 && d.y == 4) ? 0 : 1; }
    )", "test_inh_struct.cdd"), 0);
}

TEST_F(ClassCodegenE2E, INHStructLayoutOffset0E2E) {
    // INH-03：基类子对象偏移 0——派生指针隐式转基指针后字段同址。
    EXPECT_EQ(runSource(R"(
        struct B21 { int32 x; };
        struct D21 : public B21 { int32 y; };
        int32 main() { D21 d; d.x = 8; B21* b = &d; return b->x == 8 ? 0 : 1; }
    )", "test_inh_layout.cdd"), 0);
}

TEST_F(ClassCodegenE2E, INHDeepChainE2E) {
    // Review Focus 2：A→B→C 只用 C。
    EXPECT_EQ(runSource(R"(
        class A22 { public: int32 a; };
        class B22 : public A22 { public: int32 b; };
        class C22 : public B22 { public: int32 c; };
        int32 main() { C22 o; o.a = 1; o.b = 2; o.c = 3;
            return (o.a == 1 && o.b == 2 && o.c == 3) ? 0 : 1; }
    )", "test_inh_deep.cdd"), 0);
}

TEST_F(ClassCodegenE2E, INHShadowE2E) {
    // 字段遮蔽 pin（p5 行为）。遮蔽 = 两个独立字段：d.v 写派生槽；
    // 基槽未写（实测裁决：a->v 读到未初始化基槽，断言移除，台账记录）。
    EXPECT_EQ(runSource(R"(
        class A23 { public: int32 v; };
        class D23 : public A23 { public: int32 v; };
        int32 main() { D23 d; d.v = 4;
            return d.v == 4 ? 0 : 1; }
    )", "test_inh_shadow.cdd"), 0);
}

TEST_F(ClassCodegenE2E, INHSliceE2E) {
    // 派生→基 值赋值（切片语义）pin（p6 行为）。
    EXPECT_EQ(runSource(R"(
        class A24 { public: int32 x; int32 w; };
        class D24 : public A24 { public: int32 t; };
        int32 main() { D24 d; d.x = 3; d.w = 5; d.t = 9; A24 a = d;
            a.x = 6; return (a.x == 6 && a.w == 5 && d.x == 3) ? 0 : 1; }
    )", "test_inh_slice.cdd"), 0);
}

TEST_F(ClassCodegenE2E, INHDowncastE2E) {
    // static_cast 向下转换 pin（p7 行为）。经派生指针写、基指针读同址
    // （实测裁决：原 a->x==0 读未初始化基槽不稳，改为先写后读，台账记录）。
    EXPECT_EQ(runSource(R"(
        class A25 { public: int32 x; };
        class D25 : public A25 { public: int32 t; };
        int32 main() { D25 d; A25* a = &d; D25* d2 = static_cast<D25*>(a);
            d2->x = 5; d2->t = 2; return (d.t == 2 && a->x == 5) ? 0 : 1; }
    )", "test_inh_downcast.cdd"), 0);
}

TEST_F(ClassCodegenE2E, INHMultiInheritDiagE2E) {
    // INH-02 端到端：多继承 → parser 诊断（fixture runSource 不查 parser
    // 错误，实测裁决：改用显式管线断言，台账记录）。
    Lexer lexer("test_inh_multi.cdd",
        "class A26 { public: int32 x; }; class B26 { public: int32 y; }; "
        "class C26 : public A26, public B26 { public: int32 z; }; "
        "int32 main() { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    EXPECT_FALSE(parser.getErrors().empty());
}

TEST_F(ClassCodegenE2E, INHStructChainGEPE2E) {
    // 经中间 struct 的两级链 + 基指针写透。
    EXPECT_EQ(runSource(R"(
        struct A27 { int32 a; };
        struct B27 : public A27 { int32 b; };
        struct C27 : public B27 { int32 c; };
        int32 main() { C27 o; B27* b = &o; A27* a = &o;
            o.c = 9; b->b = 8; a->a = 7;
            return (o.a == 7 && o.b == 8 && o.c == 9) ? 0 : 1; }
    )", "test_inh_gep.cdd"), 0);
}

// ========== Redef: 类型重复定义诊断 e2e ==========

TEST_F(ClassCodegenE2E, RedefStructE2E) {
    // 重定义 → 诊断。fixture 的 runSource 对任何 sema 错误 ADD_FAILURE
    //（正向用例专用），错误路径按 INHMultiInheritDiagE2E 先例走显式管线。
    Lexer lexer("test_redef.cdd",
        "struct B48 { int32 x; };\nstruct B48 { int32 y; };\nint32 main() { return 0; }\n");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    // 内存 Diagnostic::format() 不含码位（E2004 由驱动打印时附加），断言消息文本。
    EXPECT_NE(analyzer.getErrors().front().format().find("redefinition of type"), std::string::npos);
}
