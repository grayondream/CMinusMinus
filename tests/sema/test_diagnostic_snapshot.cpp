// INF-03 / INF-13: diagnostic snapshot tests.
//
// Each case formats the analyzer diagnostics for a source and compares them to
// a golden file under tests/diagnostics/snapshots/<name>.txt. Regenerate with:
//   CMM_UPDATE_SNAPSHOTS=1 ./build/bin/compiler_tests --gtest_filter='*Snapshot*'
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "sema/SemanticAnalyzer.h"

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace {

class DiagnosticSnapshotTest : public ::testing::Test {
protected:
    void SetUp() override { spdlog::set_level(spdlog::level::off); }
};

std::string analyzeDiagnostics(const std::string& source) {
    Lexer lexer("snapshot.c", source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();

    std::ostringstream out;
    // LEX-13 / LEX-17: include lexical diagnostics (E0001/E0002/E0003) so the
    // pipeline snapshot covers the whole front end.
    for (const auto& d : lexer.getDiagnostics()) {
        out << d.formatWithSeverity() << "\n";
    }
    if (!ast) {
        out << "<parse failed>\n";
        return out.str();
    }
    for (const auto& d : parser.getErrors()) {
        out << d.formatWithSeverity() << "\n";
    }
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    for (const auto& d : analyzer.getErrors()) {
        out << d.formatWithSeverity() << "\n";
    }
    for (const auto& d : analyzer.getWarnings()) {
        out << d.formatWithSeverity() << "\n";
    }
    return out.str();
}

void expectSnapshot(const std::string& name, const std::string& actual) {
    fs::path file = fs::path(SNAPSHOT_DIR) / (name + ".txt");

    if (std::getenv("CMM_UPDATE_SNAPSHOTS")) {
        fs::create_directories(file.parent_path());
        std::ofstream out(file);
        out << actual;
        return;
    }

    ASSERT_TRUE(fs::exists(file)) << "missing snapshot: " << file
                                  << " (regenerate with CMM_UPDATE_SNAPSHOTS=1)";
    std::ifstream in(file);
    std::string expected((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(actual, expected) << "snapshot mismatch: " << file;
}

} // namespace

TEST_F(DiagnosticSnapshotTest, UndeclaredIdentifier) {
    expectSnapshot("undeclared_identifier", analyzeDiagnostics(R"(
int32 main() {
    return missing;
}
)"));
}

TEST_F(DiagnosticSnapshotTest, LocalArrayReturnDangling) {
    expectSnapshot("local_array_return_dangling", analyzeDiagnostics(R"(
int32[] bad() {
    int32 a[2] = {1, 2};
    return a;
}
)"));
}

TEST_F(DiagnosticSnapshotTest, SliceMemberError) {
    expectSnapshot("slice_member_error", analyzeDiagnostics(R"(
int32 main() {
    int32[] s;
    return s.ptr;
}
)"));
}

TEST_F(DiagnosticSnapshotTest, PrivateMemberAccess) {
    expectSnapshot("private_member_access", analyzeDiagnostics(R"(
class C {
private:
    int32 secret;
};

int32 main() {
    C c;
    c.secret = 1;
    return 0;
}
)"));
}

TEST_F(DiagnosticSnapshotTest, IncompatibleAssignment) {
    expectSnapshot("incompatible_assignment", analyzeDiagnostics(R"(
struct S { int32 x; };
int32 main() {
    struct S s;
    int32 a = 1;
    a = s;
    return 0;
}
)"));
}

TEST_F(DiagnosticSnapshotTest, FunctionRedefinition) {
    expectSnapshot("function_redefinition", analyzeDiagnostics(R"(
int32 f() { return 1; }
int32 f() { return 2; }
)"));
}

TEST_F(DiagnosticSnapshotTest, IncompatibleCastWarning) {
    expectSnapshot("incompatible_cast", analyzeDiagnostics(R"(
struct S { int32 x; };
int32 main() {
    struct S s;
    float32 f = (float32)s;
    return 0;
}
)"));
}

TEST_F(DiagnosticSnapshotTest, UninitializedVariableWarning) {
    // SEM-01: `y` is read before any assignment along every path.
    expectSnapshot("uninitialized_variable", analyzeDiagnostics(R"(
int32 main() {
    int32 y;
    return y;
}
)"));
}

TEST_F(DiagnosticSnapshotTest, LexicalInvalidCharacter) {
    expectSnapshot("lexical_invalid_character", analyzeDiagnostics(R"(
int32 f() { return 0; @ }
)"));
}

TEST_F(DiagnosticSnapshotTest, LexicalDigitSeparator) {
    expectSnapshot("lexical_digit_separator", analyzeDiagnostics(R"(
int32 f() { return 1__0; }
)"));
}

TEST_F(DiagnosticSnapshotTest, BranchInitializationIsPathSensitive) {
    // Assigned on both branches -> no warning; only one branch -> join loses it.
    expectSnapshot("uninitialized_branch", analyzeDiagnostics(R"(
int32 main() {
    int32 a;
    int32 b;
    int32 c = 1;
    if (c) { a = 1; b = 1; } else { a = 2; }
    return a + b;
}
)"));
}
