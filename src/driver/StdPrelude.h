#pragma once

#include <memory>
#include <string>
#include <vector>

class TranslationUnitAST;

namespace smc {

// Built-in C binding layer (TODO MOD-09 / STD-23).
//
// The language has no preprocessor and does not parse real C headers; instead
// libc symbols are made available through `extern` declarations. This is the
// seed of the `std.c` module: it is injected into every translation unit
// (prepended, so its declarations are visible to user code) unless disabled
// with `--no-prelude`.
//
// Source text of the std.c binding layer: the first readable `<dir>/std/c.smc`
// found in `dirs`. The standard library is data, not compiler code: there is
// intentionally no embedded fallback. An empty result means the caller must
// report a missing/incomplete standard-library installation.
std::string loadStdCPrelude(const std::vector<std::string>& dirs);

// Parse prelude source into its own translation unit. Returns nullptr on a
// null/empty source. Parse errors are surfaced through the parser's diagnostics
// but do not throw.
std::unique_ptr<TranslationUnitAST> parseStdCPrelude(const std::string& source,
                                                     const std::string& filename);

// Move the declarations of `prelude` to the front of `target` so that they are
// declared before any user declaration that may reference them.
void prependDeclarations(TranslationUnitAST& target, TranslationUnitAST& prelude);

} // namespace smc
