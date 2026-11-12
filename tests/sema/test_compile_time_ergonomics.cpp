// CTX-01: compile_time developer-ergonomics queries — batch 1.
// These tests pin compile-time scalar/string results and diagnostics.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "sema/CompileTimeEvaluator.h"
#include "ast/Decl.h"
#include "ast/Expr.h"
#include "ast/Stmt.h"

#include <spdlog/spdlog.h>

using ConstValue = CompileTimeEvaluator::ConstValue;

class CompileTimeErgonomics : public ::testing::Test {
protected:
    void SetUp() override {
        spdlog::set_level(spdlog::level::off);
    }

    struct Extracted {
        ExprAST* expr = nullptr;
        size_t baseErrors = 0;
        std::unique_ptr<TranslationUnitAST> tu;
        std::unique_ptr<SemanticAnalyzer> analyzer;
    };

    void extract(Extracted& ex, const std::string& decls, const std::string& exprSrc) {
        Lexer lexer("ct_ergonomics_test.c",
                    decls + " int32 f() { return " + exprSrc + "; }");
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        ex.tu = parser.parse();
        ex.analyzer = std::make_unique<SemanticAnalyzer>();
        ex.analyzer->analyze(*ex.tu);
        ex.baseErrors = ex.analyzer->getErrors().size();
        ASSERT_FALSE(ex.tu->declarations.empty());
        FunctionDeclAST* fn = nullptr;
        for (auto it = ex.tu->declarations.rbegin(); it != ex.tu->declarations.rend(); ++it) {
            fn = dynamic_cast<FunctionDeclAST*>(it->get());
            if (fn) break;
        }
        ASSERT_NE(fn, nullptr);
        ASSERT_FALSE(fn->body->stmts.empty());
        auto* ret = dynamic_cast<ReturnStmtAST*>(fn->body->stmts[0].get());
        ASSERT_NE(ret, nullptr);
        ASSERT_NE(ret->value, nullptr);
        ex.expr = ret->value.get();
    }

    std::optional<ConstValue> evalExpr(Extracted& ex) {
        CompileTimeEvaluator ev(*ex.analyzer);
        return ev.eval(ex.expr, *ex.expr);
    }

    struct Analysis {
        std::unique_ptr<TranslationUnitAST> tu;
        std::unique_ptr<SemanticAnalyzer> analyzer;
    };

    Analysis analyze(const std::string& source) {
        Lexer lexer("ct_ergonomics_test.c", source);
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        Analysis a;
        a.tu = parser.parse();
        a.analyzer = std::make_unique<SemanticAnalyzer>();
        if (a.tu) a.analyzer->analyze(*a.tu);
        return a;
    }
};

// ---- A1: source location -------------------------------------------------

TEST_F(CompileTimeErgonomics, SourceLineAndFile) {
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, "", "compile_time.line"));
    ASSERT_EQ(ex.baseErrors, 0u);
    auto v = evalExpr(ex);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->type, ConstValue::INT);
    EXPECT_EQ(v->intVal, 1);

    Extracted fileEx;
    ASSERT_NO_FATAL_FAILURE(extract(fileEx, "",
        "compile_time.file == \"ct_ergonomics_test.c\""));
    ASSERT_EQ(fileEx.baseErrors, 0u);
    auto f = evalExpr(fileEx);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->type, ConstValue::INT);
    EXPECT_EQ(f->intVal, 1);
}

TEST_F(CompileTimeErgonomics, SourceFunctionAndColumn) {
    auto fn = analyze(
        "int32 f() { compile_time.static_assert(compile_time.function == \"f\", \"fn\"); return 0; }");
    ASSERT_TRUE(fn.tu != nullptr);
    EXPECT_TRUE(fn.analyzer->getErrors().empty());

    Extracted colEx;
    ASSERT_NO_FATAL_FAILURE(extract(colEx, "", "compile_time.column"));
    ASSERT_EQ(colEx.baseErrors, 0u);
    auto c = evalExpr(colEx);
    ASSERT_TRUE(c.has_value());
    EXPECT_GT(c->intVal, 0);
}

// ---- A2: fail / warn / debug --------------------------------------------

TEST_F(CompileTimeErgonomics, FailDiagnoses) {
    auto a = analyze("int32 f() { compile_time.fail(\"boom\"); return 0; }");
    ASSERT_TRUE(a.tu != nullptr);
    ASSERT_FALSE(a.analyzer->getErrors().empty());
    bool found = false;
    for (const auto& d : a.analyzer->getErrors())
        if (d.message.find("boom") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST_F(CompileTimeErgonomics, WarnEmitsWarning) {
    auto a = analyze("int32 f() { compile_time.warn(\"careful\"); return 0; }");
    ASSERT_TRUE(a.tu != nullptr);
    EXPECT_TRUE(a.analyzer->getErrors().empty());
    ASSERT_FALSE(a.analyzer->getWarnings().empty());
    EXPECT_NE(a.analyzer->getWarnings()[0].message.find("careful"), std::string::npos);
}

TEST_F(CompileTimeErgonomics, DebugIsSilentInRelease) {
    auto a = analyze("int32 f() { compile_time.debug(\"trace\"); return 0; }");
    ASSERT_TRUE(a.tu != nullptr);
    EXPECT_TRUE(a.analyzer->getErrors().empty());
    EXPECT_TRUE(a.analyzer->getWarnings().empty());
}

// ---- A3: assertion helpers ----------------------------------------------

TEST_F(CompileTimeErgonomics, AssertEqPassAndFail) {
    auto ok = analyze("int32 f() { compile_time.assert_eq(2 + 2, 4); return 0; }");
    ASSERT_TRUE(ok.tu != nullptr);
    EXPECT_TRUE(ok.analyzer->getErrors().empty());

    auto bad = analyze("int32 f() { compile_time.assert_eq(2 + 2, 5); return 0; }");
    ASSERT_TRUE(bad.tu != nullptr);
    ASSERT_FALSE(bad.analyzer->getErrors().empty());
    EXPECT_NE(bad.analyzer->getErrors()[0].message.find("4"), std::string::npos);
    EXPECT_NE(bad.analyzer->getErrors()[0].message.find("5"), std::string::npos);
}

TEST_F(CompileTimeErgonomics, AssertSizeAlignOffset) {
    const std::string decls = "struct S { int32 a; int8 b; };";
    auto ok = analyze(decls + R"(
int32 f() {
  compile_time.assert_size(S, 8);
  compile_time.assert_align(S, 4);
  compile_time.assert_offset(S, "b", 4);
  return 0;
}
)");
    ASSERT_TRUE(ok.tu != nullptr);
    EXPECT_TRUE(ok.analyzer->getErrors().empty());

    auto bad = analyze(decls + R"(
int32 f() {
  compile_time.assert_size(S, 9);
  return 0;
}
)");
    ASSERT_TRUE(bad.tu != nullptr);
    EXPECT_FALSE(bad.analyzer->getErrors().empty());
}

// ---- B1: type predicates ------------------------------------------------

TEST_F(CompileTimeErgonomics, TypePredicates) {
    const std::string decls = R"(
struct S { int32 x; };
union U { int32 x; };
class C { public: int32 x; };
enum E { A, B };
using Arr = int32[4];
)";
    auto check = [&](const std::string& expr, long long expected) {
        Extracted ex;
        ASSERT_NO_FATAL_FAILURE(extract(ex, decls, expr));
        auto v = evalExpr(ex);
        ASSERT_TRUE(v.has_value()) << expr;
        EXPECT_EQ(v->intVal, expected) << expr;
    };
    check("compile_time.is_same(int32, int32)", 1);
    check("compile_time.is_same(int32, int64)", 0);
    check("compile_time.is_integer(int32)", 1);
    check("compile_time.is_float(float64)", 1);
    check("compile_time.is_bool(bool)", 1);
    check("compile_time.is_pointer(int32*)", 1);
    check("compile_time.is_array(Arr)", 1);
    check("compile_time.is_aggregate(S)", 1);
    check("compile_time.is_struct(S)", 1);
    check("compile_time.is_class(C)", 1);
    check("compile_time.is_union(U)", 1);
    check("compile_time.is_enum(E)", 1);
    check("compile_time.is_void(void)", 1);
    check("compile_time.is_signed(int32)", 1);
    check("compile_time.is_signed(uint32)", 0);
}

// ---- B2: numeric limits -------------------------------------------------

TEST_F(CompileTimeErgonomics, IntegerLimitsAndWidths) {
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, "",
        "compile_time.int_max(int32)"));
    auto v = evalExpr(ex);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->intVal, 2147483647LL);

    Extracted mn;
    ASSERT_NO_FATAL_FAILURE(extract(mn, "", "compile_time.int_min(int8)"));
    auto m = evalExpr(mn);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->intVal, -128LL);

    Extracted um;
    ASSERT_NO_FATAL_FAILURE(extract(um, "", "compile_time.uint_max(uint8)"));
    auto u = evalExpr(um);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->intVal, 255LL);
    EXPECT_TRUE(u->isUnsigned);

    Extracted bw;
    ASSERT_NO_FATAL_FAILURE(extract(bw, "", "compile_time.bit_width(int128)"));
    auto b = evalExpr(bw);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->intVal, 128LL);
}

TEST_F(CompileTimeErgonomics, FloatLimits) {
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, "",
        "compile_time.float_epsilon(float32)"));
    auto v = evalExpr(ex);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->type, ConstValue::DOUBLE);
    EXPECT_NEAR(v->doubleVal, 1.1920928955078125e-7, 1e-20);

    Extracted digits;
    ASSERT_NO_FATAL_FAILURE(extract(digits, "",
        "compile_time.float_digits(float32)"));
    auto d = evalExpr(digits);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->intVal, 24LL);
}

// ---- C1: field scalar queries -------------------------------------------

TEST_F(CompileTimeErgonomics, FieldQueries) {
    const std::string decls = "struct S { int32 a; int8 b; };";
    auto check = [&](const std::string& expr, long long expected) {
        Extracted ex;
        ASSERT_NO_FATAL_FAILURE(extract(ex, decls, expr));
        auto v = evalExpr(ex);
        ASSERT_TRUE(v.has_value()) << expr;
        EXPECT_EQ(v->intVal, expected) << expr;
    };
    check("compile_time.has_field(S, \"a\")", 1);
    check("compile_time.has_field(S, \"nope\")", 0);
    check("compile_time.field_count_of(S)", 2);
    check("compile_time.field_size_of(S, \"a\")", 4);
    check("compile_time.field_offset_of(S, \"b\")", 4);
    check("compile_time.field_is_bitfield(S, \"a\")", 0);
}

TEST_F(CompileTimeErgonomics, FieldNameAndTypeName) {
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex,
        "struct S { int32 a; int8 b; };",
        "compile_time.field_name_at(S, 1)"));
    auto v = evalExpr(ex);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->type, ConstValue::STR);
    EXPECT_EQ(v->strVal, "b");

    Extracted ty;
    ASSERT_NO_FATAL_FAILURE(extract(ty,
        "struct S { int32 a; int8 b; };",
        "compile_time.field_type_name_of(S, \"a\")"));
    auto t = evalExpr(ty);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->type, ConstValue::STR);
    EXPECT_NE(t->strVal.find("int32"), std::string::npos);
}

TEST_F(CompileTimeErgonomics, BitfieldQueriesAndOffsetDiagnosis) {
    const std::string decls = "struct B { int32 a:3; int32 b:5; };";
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, decls,
        "compile_time.field_bit_width_of(B, \"a\")"));
    auto v = evalExpr(ex);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->intVal, 3LL);

    auto bad = analyze(decls + R"(
int32 f() { return compile_time.field_offset_of(B, "a"); }
)");
    ASSERT_TRUE(bad.tu != nullptr);
    EXPECT_FALSE(bad.analyzer->getErrors().empty());

    auto badLayout = analyze(decls + R"(
int32 f() { return compile_time.offset_of(B, "a"); }
)");
    ASSERT_TRUE(badLayout.tu != nullptr);
    EXPECT_FALSE(badLayout.analyzer->getErrors().empty());
}

// ---- C2: attribute scalar queries ---------------------------------------

TEST_F(CompileTimeErgonomics, TypeAttributeQueries) {
    const std::string decls = "[[align(16)]] struct S { int32 a; };";
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, decls,
        "compile_time.has_attribute(S, \"align\")"));
    auto v = evalExpr(ex);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->intVal, 1LL);

    Extracted cnt;
    ASSERT_NO_FATAL_FAILURE(extract(cnt, decls,
        "compile_time.attribute_count_of(S)"));
    auto c = evalExpr(cnt);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->intVal, 1LL);

    Extracted arg;
    ASSERT_NO_FATAL_FAILURE(extract(arg, decls,
        "compile_time.attribute_arg_of(S, \"align\", 0)"));
    auto a = evalExpr(arg);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->intVal, 16LL);
}

TEST_F(CompileTimeErgonomics, FieldAttributeQueries) {
    const std::string decls = "struct S { [[align(8)]] int32 a; };";
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, decls,
        "compile_time.field_annotation_count_of(S, \"a\")"));
    auto v = evalExpr(ex);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->intVal, 1LL);

    Extracted name;
    ASSERT_NO_FATAL_FAILURE(extract(name, decls,
        "compile_time.field_annotation_name_at(S, \"a\", 0)"));
    auto n = evalExpr(name);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->strVal, "align");

    Extracted arg;
    ASSERT_NO_FATAL_FAILURE(extract(arg, decls,
        "compile_time.field_annotation_arg_of(S, \"a\", \"align\", 0)"));
    auto a = evalExpr(arg);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->intVal, 8LL);
}

// ---- D1: target/build additions -----------------------------------------

TEST_F(CompileTimeErgonomics, TargetQueries) {
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, "",
        "compile_time.target.pointer_width"));
    auto v = evalExpr(ex);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->intVal, 64LL);

    Extracted endian;
    ASSERT_NO_FATAL_FAILURE(extract(endian, "",
        "compile_time.target.endian"));
    auto e = evalExpr(endian);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->type, ConstValue::STR);
    EXPECT_TRUE(e->strVal == "little" || e->strVal == "big");

    Extracted triple;
    ASSERT_NO_FATAL_FAILURE(extract(triple, "",
        "compile_time.target.triple"));
    auto t = evalExpr(triple);
    ASSERT_TRUE(t.has_value());
    EXPECT_FALSE(t->strVal.empty());
}

// ---- Batch 2: expression layout, type names, enums, classes, format ------

TEST_F(CompileTimeErgonomics, ExpressionLayoutQueries) {
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, "int32 g;",
        "compile_time.size_of(g)"));
    auto v = evalExpr(ex);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->intVal, 4);

    Extracted al;
    ASSERT_NO_FATAL_FAILURE(extract(al, "float64 d;",
        "compile_time.align_of(d)"));
    auto a = evalExpr(al);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->intVal, 8);

    Extracted lit;
    ASSERT_NO_FATAL_FAILURE(extract(lit, "", "compile_time.size_of(1 + 2)"));
    auto l = evalExpr(lit);
    ASSERT_TRUE(l.has_value());
    EXPECT_EQ(l->intVal, 4);
}

TEST_F(CompileTimeErgonomics, NestedOffsetOf) {
    const std::string decls = R"(
struct Inner { int32 x; int8 y; };
struct Outer { int32 a; Inner in; };
)";
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, decls,
        "compile_time.offset_of(Outer, \"in.y\")"));
    auto v = evalExpr(ex);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->intVal, 8);

    Extracted bare;
    ASSERT_NO_FATAL_FAILURE(extract(bare, decls,
        "compile_time.offset_of(Outer, in)"));
    auto b = evalExpr(bare);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->intVal, 4);
}

TEST_F(CompileTimeErgonomics, BitSizePaddingPacked) {
    Extracted bits;
    ASSERT_NO_FATAL_FAILURE(extract(bits, "",
        "compile_time.bit_size_of(int32)"));
    auto b = evalExpr(bits);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->intVal, 32);

    const std::string decls = "struct P { int32 a; int8 b; };";
    Extracted pad;
    ASSERT_NO_FATAL_FAILURE(extract(pad, decls,
        "compile_time.padding_of(P)"));
    auto p = evalExpr(pad);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->intVal, 3);

    Extracted packed;
    ASSERT_NO_FATAL_FAILURE(extract(packed,
        "[[packed]] struct Pk { int32 a; int8 b; };",
        "compile_time.is_packed(Pk)"));
    auto k = evalExpr(packed);
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->intVal, 1);
}

TEST_F(CompileTimeErgonomics, TypeNameAndKind) {
    Extracted name;
    ASSERT_NO_FATAL_FAILURE(extract(name, "int32 g;",
        "compile_time.type_name_of(g)"));
    auto n = evalExpr(name);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->type, ConstValue::STR);
    EXPECT_NE(n->strVal.find("int32"), std::string::npos);

    Extracted kind;
    ASSERT_NO_FATAL_FAILURE(extract(kind, "float64 d;",
        "compile_time.type_kind_of(d)"));
    auto k = evalExpr(kind);
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->strVal, "float");

    Extracted ptr;
    ASSERT_NO_FATAL_FAILURE(extract(ptr, "int32* p;",
        "compile_time.type_kind_of(p)"));
    auto pv = evalExpr(ptr);
    ASSERT_TRUE(pv.has_value());
    EXPECT_EQ(pv->strVal, "pointer");
}

TEST_F(CompileTimeErgonomics, EnumQueries) {
    const std::string decls = "enum Color { Red, Green = 5, Blue };";
    auto checkInt = [&](const std::string& expr, long long expected) {
        Extracted ex;
        ASSERT_NO_FATAL_FAILURE(extract(ex, decls, expr));
        auto v = evalExpr(ex);
        ASSERT_TRUE(v.has_value()) << expr;
        EXPECT_EQ(v->intVal, expected) << expr;
    };
    checkInt("compile_time.enum_count_of(Color)", 3);
    checkInt("compile_time.enum_value_at(Color, 1)", 5);
    checkInt("compile_time.enum_value_of(Color, \"Blue\")", 6);
    checkInt("compile_time.enum_name_of(Color, 5) == \"Green\"", 1);

    Extracted name;
    ASSERT_NO_FATAL_FAILURE(extract(name, decls,
        "compile_time.enum_name_at(Color, 2)"));
    auto n = evalExpr(name);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->strVal, "Blue");

    Extracted underlying;
    ASSERT_NO_FATAL_FAILURE(extract(underlying, decls,
        "compile_time.enum_underlying_name(Color)"));
    auto u = evalExpr(underlying);
    ASSERT_TRUE(u.has_value());
    EXPECT_NE(u->strVal.find("int32"), std::string::npos);
}

TEST_F(CompileTimeErgonomics, MethodAndBaseQueries) {
    const std::string decls = R"(
class Base { public: int32 f(int32 x); };
class Derived : Base { public: int32 g(); };
)";
    auto checkInt = [&](const std::string& expr, long long expected) {
        Extracted ex;
        ASSERT_NO_FATAL_FAILURE(extract(ex, decls, expr));
        auto v = evalExpr(ex);
        ASSERT_TRUE(v.has_value()) << expr;
        EXPECT_EQ(v->intVal, expected) << expr;
    };
    checkInt("compile_time.method_count_of(Derived)", 2);
    checkInt("compile_time.has_method(Derived, \"f\")", 1);
    checkInt("compile_time.has_method(Derived, \"g\")", 1);
    checkInt("compile_time.is_base_of(Base, Derived)", 1);
    checkInt("compile_time.is_base_of(Derived, Base)", 0);

    Extracted base;
    ASSERT_NO_FATAL_FAILURE(extract(base, decls,
        "compile_time.base_name_of(Derived)"));
    auto b = evalExpr(base);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->strVal, "Base");

    Extracted ret;
    ASSERT_NO_FATAL_FAILURE(extract(ret, decls,
        "compile_time.method_return_type_name(Derived, \"g\")"));
    auto r = evalExpr(ret);
    ASSERT_TRUE(r.has_value());
    EXPECT_NE(r->strVal.find("int32"), std::string::npos);

    Extracted params;
    ASSERT_NO_FATAL_FAILURE(extract(params, decls,
        "compile_time.method_param_count(Derived, \"f\")"));
    auto p = evalExpr(params);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->intVal, 1);
}

TEST_F(CompileTimeErgonomics, BuildQueries) {
    Extracted mode;
    ASSERT_NO_FATAL_FAILURE(extract(mode, "", "compile_time.build.mode"));
    auto m = evalExpr(mode);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->strVal, "release");

    Extracted compiler;
    ASSERT_NO_FATAL_FAILURE(extract(compiler, "", "compile_time.build.compiler"));
    auto c = evalExpr(compiler);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->strVal, "cdd");

    Extracted host;
    ASSERT_NO_FATAL_FAILURE(extract(host, "", "compile_time.build.host_os"));
    auto h = evalExpr(host);
    ASSERT_TRUE(h.has_value());
    EXPECT_FALSE(h->strVal.empty());
}

TEST_F(CompileTimeErgonomics, FormatAndToString) {
    Extracted fmt;
    ASSERT_NO_FATAL_FAILURE(extract(fmt, "",
        "compile_time.format(\"%d/%s/%x\", 42, \"ok\", 255) == \"42/ok/ff\""));
    auto f = evalExpr(fmt);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->intVal, 1);

    Extracted ts;
    ASSERT_NO_FATAL_FAILURE(extract(ts, "",
        "compile_time.to_string(42) == \"42\""));
    auto t = evalExpr(ts);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->intVal, 1);

    Extracted tb;
    ASSERT_NO_FATAL_FAILURE(extract(tb, "",
        "compile_time.to_string(true) == \"true\""));
    auto b = evalExpr(tb);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->intVal, 1);
}

// ---- Batch 3: type values, conversion and template introspection ----------

TEST_F(CompileTimeErgonomics, TypeValues) {
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, "",
        "compile_time.size_of(compile_time.array_of(int32, 4))"));
    auto v = evalExpr(ex);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->intVal, 16);

    Extracted ptr;
    ASSERT_NO_FATAL_FAILURE(extract(ptr, "",
        "compile_time.is_same(compile_time.pointer_to(int32), int32*)"));
    auto p = evalExpr(ptr);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->intVal, 1);

    Extracted same;
    ASSERT_NO_FATAL_FAILURE(extract(same, "",
        "compile_time.is_same(compile_time.type_of(int32), int32)"));
    auto s = evalExpr(same);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->intVal, 1);
}

TEST_F(CompileTimeErgonomics, ConversionQueries) {
    Extracted yes;
    ASSERT_NO_FATAL_FAILURE(extract(yes, "",
        "compile_time.is_convertible(int32, int64)"));
    auto y = evalExpr(yes);
    ASSERT_TRUE(y.has_value());
    EXPECT_EQ(y->intVal, 1);

    Extracted no;
    ASSERT_NO_FATAL_FAILURE(extract(no, "",
        "compile_time.is_convertible(int64, int32)"));
    auto n = evalExpr(no);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->intVal, 0);
}

TEST_F(CompileTimeErgonomics, TemplateIntrospection) {
    const std::string decls = R"(
template<typename T> struct Box { T value; };
)";
    Extracted name;
    ASSERT_NO_FATAL_FAILURE(extract(name, decls,
        "compile_time.template_name_of(Box<int32>)"));
    auto n = evalExpr(name);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->strVal, "Box");

    Extracted count;
    ASSERT_NO_FATAL_FAILURE(extract(count, decls,
        "compile_time.template_arg_count(Box<int32>)"));
    auto c = evalExpr(count);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->intVal, 1);

    Extracted arg;
    ASSERT_NO_FATAL_FAILURE(extract(arg, decls,
        "compile_time.template_arg_name(Box<int32>, 0)"));
    auto a = evalExpr(arg);
    ASSERT_TRUE(a.has_value());
    EXPECT_NE(a->strVal.find("int32"), std::string::npos);
}

TEST_F(CompileTimeErgonomics, RepeatConstantAndDiagnosis) {
    auto ok = analyze(R"(
int32 main() {
    int32 total = 0;
    compile_time.repeat(3) { total += 1; }
    return total;
}
)");
    ASSERT_TRUE(ok.tu != nullptr);
    EXPECT_TRUE(ok.analyzer->getErrors().empty());

    auto bad = analyze(R"(
int32 main() {
    int32 n = 3;
    compile_time.repeat(n) { }
    return 0;
}
)");
    ASSERT_TRUE(bad.tu != nullptr);
    EXPECT_FALSE(bad.analyzer->getErrors().empty());
}

// ---- Review regressions --------------------------------------------------

TEST_F(CompileTimeErgonomics, ConstOfAggregateIsSafe) {
    const std::string decls = R"(
struct C1S { int32 a; int8 b; };
union C1U { int32 a; int8 b; };
)";
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, decls,
        "compile_time.size_of(compile_time.const_of(C1S))"));
    auto v = evalExpr(ex);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->intVal, 8);

    Extracted u;
    ASSERT_NO_FATAL_FAILURE(extract(u, decls,
        "compile_time.size_of(compile_time.const_of(C1U))"));
    auto uv = evalExpr(u);
    ASSERT_TRUE(uv.has_value());
    EXPECT_EQ(uv->intVal, 4);
}

TEST_F(CompileTimeErgonomics, IsSameTypedefAndTemplate) {
    const std::string decls = R"(
using MyInt = int32;
template<typename T> struct Box { T value; };
)";
    Extracted td;
    ASSERT_NO_FATAL_FAILURE(extract(td, decls,
        "compile_time.is_same(MyInt, int32)"));
    auto t = evalExpr(td);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->intVal, 1);

    Extracted box;
    ASSERT_NO_FATAL_FAILURE(extract(box, decls,
        "compile_time.is_same(Box<int32>, Box<int64>)"));
    auto b = evalExpr(box);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->intVal, 0);
}

TEST_F(CompileTimeErgonomics, OffsetOfInheritedField) {
    const std::string decls = R"(
struct C1Base { int32 a; };
struct C1Derived : C1Base { int32 c; };
)";
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex, decls,
        "compile_time.offset_of(C1Derived, \"a\")"));
    auto v = evalExpr(ex);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->intVal, 0);
}

TEST_F(CompileTimeErgonomics, IncompleteAggregateDiagnosed) {
    auto bad = analyze(R"(
class C1Fwd;
int32 f() { return compile_time.size_of(C1Fwd); }
)");
    ASSERT_TRUE(bad.tu != nullptr);
    ASSERT_FALSE(bad.analyzer->getErrors().empty());
    EXPECT_NE(bad.analyzer->getErrors()[0].message.find("incomplete"),
              std::string::npos);

    Extracted ok;
    ASSERT_NO_FATAL_FAILURE(extract(ok, R"(
class C1Done;
class C1Done { public: int32 x; int32 y; };
)",
        "compile_time.size_of(C1Done)"));
    auto v = evalExpr(ok);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->intVal, 8);
}

TEST_F(CompileTimeErgonomics, PaddingWithBitfields) {
    Extracted ex;
    ASSERT_NO_FATAL_FAILURE(extract(ex,
        "struct C1BF { int32 a:3; int8 b; };",
        "compile_time.padding_of(C1BF)"));
    auto v = evalExpr(ex);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->intVal, 3);
}

TEST_F(CompileTimeErgonomics, RepeatRejectsStaticLocal) {
    auto a = analyze(R"(
int32 main() {
    compile_time.repeat(2) { static int32 s = 0; s = s + 1; }
    return 0;
}
)");
    ASSERT_TRUE(a.tu != nullptr);
    ASSERT_FALSE(a.analyzer->getErrors().empty());
    EXPECT_NE(a.analyzer->getErrors()[0].message.find("static/thread_local"),
              std::string::npos);
}

TEST_F(CompileTimeErgonomics, NameOfInStaticAssert) {
    auto a = analyze(R"(
struct C1Name { int32 a; };
compile_time.static_assert(compile_time.name_of(C1Name) == "struct C1Name", "name");
int32 main() { return 0; }
)");
    ASSERT_TRUE(a.tu != nullptr);
    EXPECT_TRUE(a.analyzer->getErrors().empty());
}

TEST_F(CompileTimeErgonomics, VersionQueries) {
    Extracted major;
    ASSERT_NO_FATAL_FAILURE(extract(major, "",
        "compile_time.version.major"));
    auto m = evalExpr(major);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->intVal, 1LL);

    Extracted atLeast;
    ASSERT_NO_FATAL_FAILURE(extract(atLeast, "",
        "compile_time.version.at_least(\"1.0.0\")"));
    auto a = evalExpr(atLeast);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->intVal, 1LL);

    Extracted tooNew;
    ASSERT_NO_FATAL_FAILURE(extract(tooNew, "",
        "compile_time.version.at_least(\"2.0.0\")"));
    auto n = evalExpr(tooNew);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->intVal, 0LL);
}
