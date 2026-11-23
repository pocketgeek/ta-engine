#pragma once
#include <string_view>
// Shared live-unit helper-piece policy. Input is the lowercased 3DO piece name.
inline bool skipLiveModelPiece(std::string_view name,bool root) {
    return root || name.ends_with("gp") || name.ends_with("null") || name.ends_with("off") ||
        name.find("ground")!=name.npos || name.find("gpoly")!=name.npos || name.find("gpoint")!=name.npos;
}
