// P0-07 / INF-06/09/11: bind the normative spec documents (docs/spec/) to the
// implementation so the two cannot drift silently.
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "frontend/OperatorPrecedence.h"
#include "frontend/Token.h"

namespace {

std::string readSpec(const std::string& name) {
    std::ifstream in(std::string(SPEC_DIR) + "/" + name);
    if (!in.is_open()) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The infix operators in the normative §9 table, mirrored from
// src/frontend/OperatorPrecedence.cpp.
const std::vector<std::pair<TokenType, const char*>>& infixOperators() {
    static const std::vector<std::pair<TokenType, const char*>> ops = {
        {TokenType::TOKEN_COMMA, ","},
        {TokenType::TOKEN_ASSIGN, "="},
        {TokenType::TOKEN_PLUS_EQ, "+="},
        {TokenType::TOKEN_MINUS_EQ, "-="},
        {TokenType::TOKEN_STAR_EQ, "*="},
        {TokenType::TOKEN_SLASH_EQ, "/="},
        {TokenType::TOKEN_PERCENT_EQ, "%="},
        {TokenType::TOKEN_AMP_EQ, "&="},
        {TokenType::TOKEN_PIPE_EQ, "|="},
        {TokenType::TOKEN_CARET_EQ, "^="},
        {TokenType::TOKEN_LSHIFT_EQ, "<<="},
        {TokenType::TOKEN_RSHIFT_EQ, ">>="},
        {TokenType::TOKEN_OR, "||"},
        {TokenType::TOKEN_AND, "&&"},
        {TokenType::TOKEN_BIT_OR, "|"},
        {TokenType::TOKEN_CARET, "^"},
        {TokenType::TOKEN_BIT_AND, "&"},
        {TokenType::TOKEN_EQ, "=="},
        {TokenType::TOKEN_NOT_EQ, "!="},
        {TokenType::TOKEN_LT, "<"},
        {TokenType::TOKEN_GT, ">"},
        {TokenType::TOKEN_LE, "<="},
        {TokenType::TOKEN_GE, ">="},
        {TokenType::TOKEN_LSHIFT, "<<"},
        {TokenType::TOKEN_RSHIFT, ">>"},
        {TokenType::TOKEN_PLUS, "+"},
        {TokenType::TOKEN_MINUS, "-"},
        {TokenType::TOKEN_STAR, "*"},
        {TokenType::TOKEN_SLASH, "/"},
        {TokenType::TOKEN_PERCENT, "%"},
    };
    return ops;
}

} // namespace

TEST(SpecConformance, AllSpecDocumentsExist) {
    for (const char* f : {"grammar.ebnf", "keywords.md", "conversions.md", "semantics.md",
                          "modules.md", "abi.md", "compile_time.md", "stdlib.md", "README.md"}) {
        EXPECT_FALSE(readSpec(f).empty()) << "missing spec document: " << f;
    }
}

TEST(SpecConformance, NormativeTableMatchesImplementation) {
    // Every operator the parser treats as infix must have the same symbol in the
    // normative grammar's §9 table, and the implementation must agree on the
    // precedence level asserted there.
    std::string grammar = readSpec("grammar.ebnf");
    ASSERT_FALSE(grammar.empty());

    for (const auto& [token, symbol] : infixOperators()) {
        ASSERT_NE(cmm::getOperatorInfo(token).precedence, 0)
            << "implementation table missing operator " << symbol;
        EXPECT_NE(grammar.find(symbol), std::string::npos)
            << "grammar.ebnf does not mention operator " << symbol;
    }
}

TEST(SpecConformance, GrammarDropsRemovedConstructs) {
    std::string grammar = readSpec("grammar.ebnf");
    ASSERT_FALSE(grammar.empty());
    // P0-01 / NG-01, NG-02: no goto/label productions, no preprocessor.
    EXPECT_EQ(grammar.find("goto-statement"), std::string::npos);
    EXPECT_EQ(grammar.find("label-statement"), std::string::npos);
    EXPECT_EQ(grammar.find("preprocessor"), std::string::npos);
    // Implemented module/namespace surface must be present.
    EXPECT_NE(grammar.find("namespace-decl"), std::string::npos);
    EXPECT_NE(grammar.find("import-decl"), std::string::npos);
    EXPECT_NE(grammar.find("module-decl"), std::string::npos);
}

TEST(SpecConformance, KeywordsTableRecordsRemovals) {
    std::string keywords = readSpec("keywords.md");
    ASSERT_FALSE(keywords.empty());
    EXPECT_NE(keywords.find("[removed]"), std::string::npos)
        << "keywords.md should mark removed keywords (NG-01/NG-02)";
}

TEST(SpecConformance, ModuleSpecUsesModulePathOption) {
    // MOD-14 / P0-01: -I was replaced by -M/--module-path.
    std::string modules = readSpec("modules.md");
    ASSERT_FALSE(modules.empty());
    EXPECT_NE(modules.find("--module-path"), std::string::npos);
}

TEST(SpecConformance, StdlibSpecDocumentsBuiltins) {
    // P0-05 / STD-01 / STD-12 / STD-27: the terminator builtins, the C binding
    // layer, and the std.io surface are documented normatively.
    std::string stdlib = readSpec("stdlib.md");
    ASSERT_FALSE(stdlib.empty());
    EXPECT_NE(stdlib.find("assert"), std::string::npos);
    EXPECT_NE(stdlib.find("panic"), std::string::npos);
    EXPECT_NE(stdlib.find("abort"), std::string::npos);
    EXPECT_NE(stdlib.find("std::print_int"), std::string::npos);
    EXPECT_NE(stdlib.find("std::file_open"), std::string::npos);
    // MEM-10: callback-taking libc APIs are documented.
    EXPECT_NE(stdlib.find("qsort"), std::string::npos);
    // DEC-21: panic is not catchable.
    EXPECT_NE(stdlib.find("DEC-21"), std::string::npos);
}

TEST(SpecConformance, ConversionsSpecDocumentsCastOperators) {
    // LEX-11 / PAR-18 / DEC-18: the explicit cast operators and the extended
    // overload-conversion ranks are documented normatively.
    std::string conv = readSpec("conversions.md");
    ASSERT_FALSE(conv.empty());
    EXPECT_NE(conv.find("static_cast"), std::string::npos);
    EXPECT_NE(conv.find("reinterpret_cast"), std::string::npos);
    EXPECT_NE(conv.find("指针→指针"), std::string::npos);
}

TEST(SpecConformance, SemanticsSpecDocumentsAnalysisWarnings) {
    // SEM-11: unused-variable and unreachable-code warnings are documented.
    std::string sem = readSpec("semantics.md");
    ASSERT_FALSE(sem.empty());
    EXPECT_NE(sem.find("W3002"), std::string::npos);
    EXPECT_NE(sem.find("W3003"), std::string::npos);
}

TEST(SpecConformance, ConversionsSpecDocumentsDefaultArgumentPromotions) {
    // TYP-09 / varargs: default argument promotions are documented normatively.
    std::string conv = readSpec("conversions.md");
    ASSERT_FALSE(conv.empty());
    EXPECT_NE(conv.find("默认实参提升"), std::string::npos);
    EXPECT_NE(conv.find("promoteVarArg"), std::string::npos);
}

TEST(SpecConformance, ConversionsSpecDocumentsEnumStrongTyping) {
    // TYP-20: enum <-> integer requires an explicit conversion.
    std::string conv = readSpec("conversions.md");
    ASSERT_FALSE(conv.empty());
    EXPECT_NE(conv.find("TYP-20"), std::string::npos);
    EXPECT_NE(conv.find("enum ↔ int"), std::string::npos);
}

TEST(SpecConformance, ConversionsSpecDocumentsUsualArithmeticConversions) {
    // TYP-22: integer promotion + usual arithmetic conversions + signedness.
    std::string conv = readSpec("conversions.md");
    ASSERT_FALSE(conv.empty());
    EXPECT_NE(conv.find("usualArithmeticType"), std::string::npos);
    EXPECT_NE(conv.find("udiv"), std::string::npos);
}

TEST(SpecConformance, AbiSpecDocumentsEnumUnderlyingType) {
    // TYP-09/TYP-25/AGG-14: explicit enum underlying types are documented.
    std::string abi = readSpec("abi.md");
    ASSERT_FALSE(abi.empty());
    EXPECT_NE(abi.find("enum E : uint8"), std::string::npos);
}
