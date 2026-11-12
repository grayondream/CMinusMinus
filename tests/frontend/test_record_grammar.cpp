// PAR-05/PAR-22/AGG: struct and class share one record grammar. Both accept
// fields, methods, static members, nested types, operators, and access
// sections; only the default access level differs.
#include <gtest/gtest.h>

#include "ast/Decl.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"

#include <spdlog/spdlog.h>

namespace {

std::vector<Token> lex(const std::string& source) {
    Lexer lexer("record_grammar.cdd", source);
    return lexer.tokenize();
}

auto parse(const std::string& source) {
    Parser parser(lex(source));
    return parser.parse();
}

} // namespace

TEST(RecordGrammarTest, StructParsesAccessSectionsAndMethods) {
    auto tu = parse("struct S { private: int32 x; public: int32 get() { return x; } };");
    ASSERT_TRUE(tu != nullptr);
    auto* s = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->isClassDecl);
    EXPECT_TRUE(s->hasAccessSections);
    EXPECT_EQ(s->memberAccess.at("x"), AccessLevel::Private);
    ASSERT_EQ(s->methods.size(), 1u);
}

TEST(RecordGrammarTest, ClassParsesSameMemberSet) {
    auto tu = parse("class C { int32 x; public: int32 get() { return x; } };");
    ASSERT_TRUE(tu != nullptr);
    auto* c = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(c->isClassDecl);
    EXPECT_EQ(c->memberAccess.at("x"), AccessLevel::Private);
    ASSERT_EQ(c->methods.size(), 1u);
}

TEST(RecordGrammarTest, StructDefaultAccessIsPublic) {
    auto tu = parse("struct S { int32 x; int32 get() { return x; } };");
    ASSERT_TRUE(tu != nullptr);
    auto* s = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->hasAccessSections);
    EXPECT_EQ(s->memberAccess.at("x"), AccessLevel::Public);
}

TEST(RecordGrammarTest, StructParsesStaticNestedAndOperator) {
    auto tu = parse(
        "struct S { static int32 count; struct Inner { int32 v; }; "
        "int32 operator+(int32 other) { return count + other; } };");
    ASSERT_TRUE(tu != nullptr);
    auto* s = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->staticMembers.size(), 1u);
    EXPECT_EQ(s->nestedTypes.size(), 1u);
    ASSERT_EQ(s->methods.size(), 1u);
}
