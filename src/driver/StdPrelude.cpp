#include "driver/StdPrelude.h"

#include "ast/Decl.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

namespace cmm {

std::string loadStdCPrelude(const std::vector<std::string>& dirs) {
    for (const std::string& dir : dirs) {
        std::filesystem::path candidate =
            std::filesystem::path(dir) / "std" / "c.cmm";
        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidate, ec)) {
            continue;
        }
        std::ifstream in(candidate, std::ios::binary);
        if (!in.is_open()) {
            continue;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    return {};
}

std::unique_ptr<TranslationUnitAST> parseStdCPrelude(const std::string& source,
                                                     const std::string& filename) {
    if (source.empty()) {
        return nullptr;
    }
    Lexer lexer(filename, source);
    auto tokens = lexer.tokenize();

    // LEX-13 / LEX-17: the prelude is compiler-provided; a lexical error means a
    // corrupted binding layer, so report it rather than parsing garbage.
    bool lexError = false;
    for (const auto& diag : lexer.getDiagnostics()) {
        std::cerr << diag.formatWithSeverity() << "\n";
        if (diag.isError()) {
            lexError = true;
        }
    }
    if (lexError) {
        return nullptr;
    }

    Parser parser(tokens);
    return parser.parse();
}

void prependDeclarations(TranslationUnitAST& target, TranslationUnitAST& prelude) {
    if (prelude.declarations.empty()) {
        return;
    }
    target.declarations.insert(
        target.declarations.begin(),
        std::make_move_iterator(prelude.declarations.begin()),
        std::make_move_iterator(prelude.declarations.end()));
    prelude.declarations.clear();
}

} // namespace cmm
