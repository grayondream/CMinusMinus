#include "support/Utf8.h"

namespace cmm_utf8 {

size_t seqLen(std::string_view s, size_t i) {
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) return 2; // C0/C1 are overlong
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xF0 && lead <= 0xF4) return 4; // F5..FF are out of range
    return 0;
}

static bool isContinuation(unsigned char b) {
    return b >= 0x80 && b <= 0xBF;
}

bool valid(std::string_view s) {
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char lead = static_cast<unsigned char>(s[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        const size_t w = seqLen(s, i);
        if (w == 0 || i + w > n) return false;
        // Bound checks per RFC 3629.
        if (w == 2) {
            // lead >= C2 already excludes overlong.
        } else if (w == 3) {
            const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            if (lead == 0xE0 && b1 < 0xA0) return false;             // overlong
            if (lead == 0xED && b1 >= 0xA0) return false;            // surrogate
        } else { // w == 4
            const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            if (lead == 0xF0 && b1 < 0x90) return false;             // overlong
            if (lead == 0xF4 && b1 > 0x8F) return false;             // > U+10FFFF
        }
        for (size_t k = 1; k < w; ++k) {
            if (!isContinuation(static_cast<unsigned char>(s[i + k]))) return false;
        }
        i += w;
    }
    return true;
}

} // namespace cmm_utf8
