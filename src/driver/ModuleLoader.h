#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "frontend/Token.h"

class TranslationUnitAST;
class DeclAST;

namespace cdd {

// Ordered directories used to resolve `import` specifiers (P0-04 / MOD-04).
struct ModuleSearchPaths {
    std::vector<std::string> dirs;
};

// Resolve an import specifier to a readable file path, or "" when not found.
// A specifier containing '/' (or ending in ".cdd") is treated as a relative
// path; otherwise dots map to directory separators and ".cdd" is appended
// (`std.c` -> `std/c.cdd`). `paths.dirs` are searched in order.
std::string resolveImport(const std::string& specifier, const ModuleSearchPaths& paths);

// P0-04/MOD-04: collect top-level `import` specifiers from a token stream
// before parsing the importing unit. Imported module types must be registered
// before the importer's parser resolves type names (`S s;`).
std::vector<std::string> collectImportSpecifiers(const std::vector<Token>& tokens);

// Load the given import specifiers (searching `importerDir` first) into `out`
// in dependency order. This is the pre-parse counterpart of processImports.
void preloadImports(const std::vector<std::string>& specifiers,
                    const std::string& importerDir,
                    const ModuleSearchPaths& paths,
                    std::set<std::string>& loaded,
                    std::vector<std::unique_ptr<DeclAST>>& out,
                    std::vector<std::string>& errors);

// Load `path` and, recursively, its imports. Declarations are appended to `out`
// in dependency order (imports before importers). `loaded` holds canonical
// paths already processed (deduplication for diamond imports); `active` is the
// current DFS stack, used to detect import cycles (MOD-07). `specifier` is the
// import specifier that named this file, used to validate `module NAME;`
// bindings (MOD-13); pass "" for the entry file. Diagnostics append to `errors`.
void loadModuleFile(const std::string& path,
                    const std::string& specifier,
                    const ModuleSearchPaths& paths,
                    std::set<std::string>& loaded,
                    std::vector<std::string>& active,
                    std::vector<std::unique_ptr<DeclAST>>& out,
                    std::vector<std::string>& errors);

// Resolve and load `tu`'s imports (searching `importerDir` first), splicing the
// imported declarations in front of `tu.declarations` so they are visible to
// the importing unit during semantic analysis.
void processImports(TranslationUnitAST& tu,
                    const std::string& importerDir,
                    const ModuleSearchPaths& paths,
                    std::set<std::string>& loaded,
                    std::vector<std::string>& errors);

} // namespace cdd
