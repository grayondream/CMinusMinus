#include "driver/ModuleLoader.h"

#include "ast/Decl.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace fs = std::filesystem;

namespace cmm {

namespace {

// Module path components are identifiers lexically, but a keyword such as
// `atomic` is still a valid component (`import std.atomic;`). Accept any token
// whose lexeme has identifier shape; punctuation/literals are rejected.
bool isModuleNameToken(const Token& token) {
    if (token.type == TokenType::TOKEN_IDENTIFIER) return true;
    const std::string& s = token.lexeme;
    if (s.empty()) return false;
    auto isStart = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    auto isContinue = [&](unsigned char c) {
        return isStart(c) || (c >= '0' && c <= '9');
    };
    if (!isStart(static_cast<unsigned char>(s[0]))) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        if (!isContinue(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string canonicalPath(const std::string& path) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(fs::path(path), ec);
    return ec ? fs::absolute(fs::path(path)).string() : canonical.string();
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Candidate relative paths for a specifier (`std.c` -> `std/c.cmm`).
std::vector<std::string> importCandidates(const std::string& specifier) {
    std::vector<std::string> candidates;
    // Explicit path (has a separator or the module extension): use as-is.
    if (specifier.find('/') != std::string::npos || endsWith(specifier, ".cmm")) {
        candidates.push_back(specifier);
        return candidates;
    }
    // Dotted module path: `std.c` -> `std/c.cmm`.
    std::string dotted = specifier;
    for (char& c : dotted) {
        if (c == '.') c = '/';
    }
    candidates.push_back(dotted + ".cmm");
    candidates.push_back(dotted);
    candidates.push_back(specifier + ".cmm");
    candidates.push_back(specifier);
    return candidates;
}

// `std.c` / `std/c.cmm` / `std/c` -> `std.c` (MOD-13 file <-> module mapping).
std::string specifierToModuleName(const std::string& specifier) {
    std::string name = specifier;
    if (endsWith(name, ".cmm")) name = name.substr(0, name.size() - 4);
    for (char& c : name) {
        if (c == '/' || c == '\\') c = '.';
    }
    return name;
}

// Only dotted module specifiers carry an implicit module name; explicit file
// paths (`import "dir/f.cmm";`) are not name-checked.
bool isDottedSpecifier(const std::string& specifier) {
    return specifier.find('/') == std::string::npos &&
           specifier.find('\\') == std::string::npos &&
           !endsWith(specifier, ".cmm");
}

void tagModuleDecls(std::vector<std::unique_ptr<DeclAST>>& decls,
                    const std::string& moduleName) {
    for (auto& d : decls) {
        if (d && d->moduleName.empty()) d->moduleName = moduleName;
    }
}

constexpr const char* kLegacySmcMessage =
    "legacy .smc source is not supported; rename it to .cmm";

bool isLegacySmcSpecifier(const std::string& specifier) {
    return endsWith(specifier, ".smc");
}

} // namespace

std::string resolveImport(const std::string& specifier, const ModuleSearchPaths& paths) {
    if (specifier.empty()) return "";
    if (isLegacySmcSpecifier(specifier)) return "";
    for (const std::string& dir : paths.dirs) {
        for (const std::string& candidate : importCandidates(specifier)) {
            fs::path full = fs::path(dir) / candidate;
            std::error_code ec;
            if (fs::is_regular_file(full, ec)) {
                return full.string();
            }
        }
    }
    return "";
}

std::vector<std::string> collectImportSpecifiers(const std::vector<Token>& tokens) {
    std::vector<std::string> imports;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].type != TokenType::TOKEN_IMPORT) continue;
        size_t j = i + 1;
        std::string name;
        if (j < tokens.size() && tokens[j].type == TokenType::TOKEN_STRING) {
            const std::string& lex = tokens[j].lexeme;
            if (lex.size() >= 2 && lex.front() == '"' && lex.back() == '"') {
                name = lex.substr(1, lex.size() - 2);
            }
        } else if (j < tokens.size() && isModuleNameToken(tokens[j])) {
            name = tokens[j].lexeme;
            ++j;
            while (j + 1 < tokens.size() && tokens[j].type == TokenType::TOKEN_DOT &&
                   isModuleNameToken(tokens[j + 1])) {
                name += "." + tokens[j + 1].lexeme;
                j += 2;
            }
        }
        if (!name.empty()) imports.push_back(name);
    }
    return imports;
}

void preloadImports(const std::vector<std::string>& specifiers,
                    const std::string& importerDir,
                    const ModuleSearchPaths& paths,
                    std::set<std::string>& loaded,
                    std::vector<std::unique_ptr<DeclAST>>& out,
                    std::vector<std::string>& errors) {
    if (specifiers.empty()) return;
    ModuleSearchPaths localPaths = paths;
    if (!importerDir.empty()) {
        localPaths.dirs.insert(localPaths.dirs.begin(), importerDir);
    }
    std::vector<std::string> active;
    for (const std::string& spec : specifiers) {
        if (isLegacySmcSpecifier(spec)) {
            errors.push_back(kLegacySmcMessage);
            continue;
        }
        std::string resolved = resolveImport(spec, localPaths);
        if (resolved.empty()) {
            errors.push_back("cannot resolve import '" + spec + "'");
            continue;
        }
        loadModuleFile(resolved, spec, localPaths, loaded, active, out, errors);
    }
}

void loadModuleFile(const std::string& path,
                    const std::string& specifier,
                    const ModuleSearchPaths& paths,
                    std::set<std::string>& loaded,
                    std::vector<std::string>& active,
                    std::vector<std::unique_ptr<DeclAST>>& out,
                    std::vector<std::string>& errors) {
    const std::string canonical = canonicalPath(path);

    // Cycle detection (MOD-07). Checked before `loaded` so a back-edge to a
    // module still on the DFS stack is reported instead of silently deduped.
    auto onStack = std::find(active.begin(), active.end(), canonical);
    if (onStack != active.end()) {
        std::string cycle;
        for (auto it = onStack; it != active.end(); ++it) {
            cycle += *it + " -> ";
        }
        cycle += canonical;
        errors.push_back("circular import detected: " + cycle);
        return;
    }
    if (loaded.count(canonical)) {
        return; // already loaded through another (acyclic) path
    }
    loaded.insert(canonical);

    std::string source;
    if (!readFile(canonical, source)) {
        errors.push_back("cannot read module '" + canonical + "'");
        return;
    }

    Lexer lexer(canonical, source);
    auto tokens = lexer.tokenize();

    // LEX-13 / LEX-17: report lexical diagnostics for imported modules too.
    bool lexError = false;
    for (const auto& diag : lexer.getDiagnostics()) {
        errors.push_back(diag.formatWithSeverity());
        if (diag.isError()) {
            lexError = true;
        }
    }
    if (lexError) {
        return;
    }

    Parser parser(tokens);
    auto ast = parser.parse();
    if (!ast) {
        errors.push_back(canonical + ": failed to parse module");
        return;
    }
    if (!parser.getErrors().empty()) {
        for (const auto& diag : parser.getErrors()) {
            errors.push_back(diag.formatWithSeverity());
        }
        return;
    }

    // Module/file binding (MOD-13): a declared `module NAME;` must match the
    // dotted import specifier used to reach this file.
    if (!ast->moduleName.empty() && isDottedSpecifier(specifier)) {
        std::string expected = specifierToModuleName(specifier);
        if (expected != ast->moduleName) {
            errors.push_back(canonical + ": module declares name '" + ast->moduleName +
                             "' but was imported as '" + expected + "'");
            return;
        }
    }

    // Tag declarations of a participating module (has `module NAME;`) so the
    // semantic analyzer can enforce export-based visibility (MOD-05/06).
    if (!ast->moduleName.empty()) {
        tagModuleDecls(ast->declarations, ast->moduleName);
    }

    // Resolve this module's imports first (imported declarations before ours).
    active.push_back(canonical);
    ModuleSearchPaths childPaths = paths;
    childPaths.dirs.insert(childPaths.dirs.begin(),
                           canonicalPath(fs::path(canonical).parent_path().string()));
    for (const std::string& spec : ast->imports) {
        if (isLegacySmcSpecifier(spec)) {
            errors.push_back(kLegacySmcMessage);
            continue;
        }
        std::string resolved = resolveImport(spec, childPaths);
        if (resolved.empty()) {
            errors.push_back(canonical + ": cannot resolve import '" + spec + "'");
            continue;
        }
        loadModuleFile(resolved, spec, childPaths, loaded, active, out, errors);
    }
    active.pop_back();

    for (auto& decl : ast->declarations) {
        out.push_back(std::move(decl));
    }
}

void processImports(TranslationUnitAST& tu,
                    const std::string& importerDir,
                    const ModuleSearchPaths& paths,
                    std::set<std::string>& loaded,
                    std::vector<std::string>& errors) {
    if (tu.imports.empty()) return;

    ModuleSearchPaths localPaths = paths;
    if (!importerDir.empty()) {
        localPaths.dirs.insert(localPaths.dirs.begin(), importerDir);
    }

    std::vector<std::unique_ptr<DeclAST>> imported;
    std::vector<std::string> active;
    for (const std::string& spec : tu.imports) {
        if (isLegacySmcSpecifier(spec)) {
            errors.push_back(kLegacySmcMessage);
            continue;
        }
        std::string resolved = resolveImport(spec, localPaths);
        if (resolved.empty()) {
            errors.push_back("cannot resolve import '" + spec + "'");
            continue;
        }
        loadModuleFile(resolved, spec, localPaths, loaded, active, imported, errors);
    }

    if (!imported.empty()) {
        tu.declarations.insert(
            tu.declarations.begin(),
            std::make_move_iterator(imported.begin()),
            std::make_move_iterator(imported.end()));
    }
}

} // namespace cmm
