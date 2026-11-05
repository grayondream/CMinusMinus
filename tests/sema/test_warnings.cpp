// SEM-11: unused-variable (W3002) and unreachable-code (W3003) warnings.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/Diagnostic.h"
#include "sema/SemanticAnalyzer.h"

#include <spdlog/spdlog.h>

namespace {

class WarningAnalysisTest : public ::testing::Test {
protected:
    void SetUp() override { spdlog::set_level(spdlog::level::off); }
};

struct Result {
    std::vector<std::string> errors;
    std::vector<Diagnostic> warnings;

    int count(DiagnosticCode code) const {
        int n = 0;
        for (const auto& w : warnings) {
            if (w.code == code) ++n;
        }
        return n;
    }

    bool mentions(DiagnosticCode code, const std::string& needle) const {
        for (const auto& w : warnings) {
            if (w.code == code && w.message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

Result analyze(const std::string& source) {
    Lexer lexer("warn.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    Result r;
    if (!ast) {
        r.errors.push_back("<parse failed>");
        return r;
    }
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    for (const auto& d : analyzer.getErrors()) r.errors.push_back(d.message);
    r.warnings = analyzer.getWarnings();
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// W3002: unused variable
// ---------------------------------------------------------------------------

TEST_F(WarningAnalysisTest, WarnsOnUnusedLocal) {
    auto r = analyze("int32 main() { int32 x = 5; return 0; }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_TRUE(r.mentions(DiagnosticCode::WarnUnusedVariable, "'x'"));
}

TEST_F(WarningAnalysisTest, NoWarningWhenRead) {
    auto r = analyze("int32 main() { int32 x = 5; return x - 5; }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnUnusedVariable), 0);
}

TEST_F(WarningAnalysisTest, NoWarningForUnusedParameter) {
    auto r = analyze("int32 f(int32 a) { return 0; } int32 main() { return f(1); }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnUnusedVariable), 0);
}

TEST_F(WarningAnalysisTest, NoWarningForUnusedGlobal) {
    auto r = analyze("int32 g; int32 main() { return 0; }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnUnusedVariable), 0);
}

TEST_F(WarningAnalysisTest, WarnsOnUnusedLocalArray) {
    auto r = analyze("int32 main() { int32 a[3]; return 0; }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_TRUE(r.mentions(DiagnosticCode::WarnUnusedVariable, "'a'"));
}

TEST_F(WarningAnalysisTest, ReferenceFromAssignmentCountsAsUse) {
    // `-Wunused-variable` semantics: any reference counts, so a set-but-never-read
    // variable does not warn (that would be -Wunused-but-set-variable).
    auto r = analyze("int32 main() { int32 x; x = 5; return 0; }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnUnusedVariable), 0);
}

TEST_F(WarningAnalysisTest, WarnsForUnusedInNestedScope) {
    auto r = analyze("int32 main() { { int32 y = 1; } return 0; }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_TRUE(r.mentions(DiagnosticCode::WarnUnusedVariable, "'y'"));
}

TEST_F(WarningAnalysisTest, ReadFromNestedScopeCountsAsUse) {
    auto r = analyze("int32 main() { int32 y = 1; { return y; } }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnUnusedVariable), 0);
}

TEST_F(WarningAnalysisTest, AddressTakenCountsAsUse) {
    auto r = analyze("void set(int32* p) { *p = 7; } int32 main() { int32 x; set(&x); return 0; }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnUnusedVariable), 0);
}

TEST_F(WarningAnalysisTest, ExplicitCaptureCountsAsUse) {
    auto r = analyze(
        "int32 main() { int32 x = 1; "
        "auto f = [x]() -> int32 { return x; }; return f() - 1; }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnUnusedVariable), 0)
        << "resolving [x] is a use of the outer local";
}

TEST_F(WarningAnalysisTest, CaptureAllCountsAsUse) {
    auto r = analyze(
        "int32 main() { int32 x = 1; "
        "auto f = [=]() -> int32 { return x; }; return f() - 1; }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnUnusedVariable), 0)
        << "an implicit capture is a use of the outer local";
}

// ---------------------------------------------------------------------------
// W3003: unreachable code
// ---------------------------------------------------------------------------

TEST_F(WarningAnalysisTest, WarnsAfterReturn) {
    auto r = analyze("int32 main() { return 0; int32 x = 1; return x; }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnUnreachableCode), 1);
}

TEST_F(WarningAnalysisTest, WarnsOncePerBlock) {
    auto r = analyze("int32 main() { return 0; int32 a = 1; int32 b = 2; }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnUnreachableCode), 1);
}

TEST_F(WarningAnalysisTest, WarnsAfterBreak) {
    auto r = analyze("int32 main() { while (1) { break; int32 x = 1; } return 0; }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnUnreachableCode), 1);
}

TEST_F(WarningAnalysisTest, NoWarningWhenBranchMayFallThrough) {
    auto r = analyze("int32 main() { if (1) { return 1; } return 0; }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnUnreachableCode), 0);
}

TEST_F(WarningAnalysisTest, BothBranchesReturnMakesTailUnreachable) {
    auto r = analyze("int32 main() { if (1) { return 1; } else { return 2; } return 3; }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnUnreachableCode), 1);
}

TEST_F(WarningAnalysisTest, DeferAfterReturnIsNotUnreachable) {
    auto r = analyze("void cleanup() {} int32 main() { return 0; defer cleanup(); }");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnUnreachableCode), 0);
}

TEST_F(WarningAnalysisTest, UnreachableWarningHasSourceLocation) {
    auto r = analyze("int32 main() { return 0; int32 x = 1; return x; }");
    ASSERT_TRUE(r.errors.empty());
    ASSERT_EQ(r.count(DiagnosticCode::WarnUnreachableCode), 1);
    for (const auto& w : r.warnings) {
        if (w.code == DiagnosticCode::WarnUnreachableCode) {
            EXPECT_EQ(w.file, "warn.c");
            EXPECT_EQ(w.line, 1);
            EXPECT_GT(w.column, 0);
        }
    }
}
