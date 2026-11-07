#pragma once

// P1-06 (FMT-04): sema-side UTF-8 validation (RFC 3629 subset). Mirrors the
// codegen-side IR helpers (cmm.utf8.*) — the two implementations must agree:
// accept U+0000..U+10FFFF; reject overlong encodings, surrogates
// U+D800..U+DFFF, and malformed continuation bytes. ASCII is the fast path.
#include <cstddef>
#include <string_view>

namespace cmm_utf8 {

// True when [s] is entirely valid UTF-8.
bool valid(std::string_view s);

// Width of the sequence starting at byte `i` (1..4), or 0 when the lead byte
// is invalid (lone continuation, overlong prefix, or 0xF8..0xFF).
size_t seqLen(std::string_view s, size_t i);

} // namespace cmm_utf8
