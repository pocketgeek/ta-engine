#pragma once
#include <cstdint>
#include <iterator>
namespace cart {
namespace widthdata {
#include "cartographer/fontwidth.inc"
}
// Text columns a code point occupies in the editor font: 2 for East Asian Width wide/fullwidth
// (CJK, Hangul, fullwidth forms, emoji-style symbols), else 1. A pure function of the code point,
// so layout does not depend on which glyphs the embedded font happens to carry.
inline int glyphCells(uint32_t code) {
    if(code<widthdata::fontWideRanges[0][0])return 1;
    size_t lo=0,hi=std::size(widthdata::fontWideRanges);
    while(lo<hi) {
        const size_t mid=(lo+hi)/2;
        if(code>widthdata::fontWideRanges[mid][1])lo=mid+1;else hi=mid;
    }
    return lo<std::size(widthdata::fontWideRanges) && code>=widthdata::fontWideRanges[lo][0]?2:1;
}
} // namespace cart
