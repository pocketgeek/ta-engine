#pragma once

// One UTF-8 character at a time, under the chat box's rule: only a well-formed
// sequence (no overlongs, surrogates or values past U+10FFFF) is one character; any
// other byte is a character on its own and stands for itself (Latin-1). The chat
// font's decoder (client/uifont.h) and the chat word-wrap (client/chatbox.h) both
// step with this, so wrap can break wherever the font draws a separate glyph.

#include <cstddef>
#include <cstdint>
#include <string>

namespace tak::utf8 {

// The byte length (1..4) of the character starting at s[i] (i < s.size()); its code
// point goes to *cp when given.
inline size_t next(const std::string& s, size_t i, uint32_t* cp = nullptr) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const int n = c >= 0xC2 && c < 0xE0 ? 1 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xF0 && c < 0xF5 ? 3 : 0;
    uint32_t v = c & (0x3F >> n);
    bool good = n > 0 && i + size_t(n) < s.size();
    for (int k = 1; good && k <= n; ++k) {
        const unsigned char d = static_cast<unsigned char>(s[i + size_t(k)]);
        if ((d & 0xC0) != 0x80) good = false;
        else v = (v << 6) | (d & 0x3F);
    }
    if (good && ((n == 2 && v < 0x800) || (n == 3 && (v < 0x10000 || v > 0x10FFFF)) ||
                 (v >= 0xD800 && v <= 0xDFFF)))
        good = false;
    if (cp) *cp = good ? v : c;
    return good ? size_t(n) + 1 : 1;
}

}  // namespace tak::utf8
