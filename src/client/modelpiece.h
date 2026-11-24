#pragma once
#include <string_view>
// Shared live-unit helper-piece policy. Input is the lowercased 3DO piece name.
//
// TA draws the root piece like any other: the object draw loops (TotalA.exe
// 0x459958, the nanoframe outline 0x458fe1) test only the piece's visible bit
// (+0x28 & 1) and leave out the selection primitive (0x45908a), never a piece by
// position or name. Skipping the root (the Kingdoms engine's ground-plate rule)
// dropped the body of most structures -- armsolar's whole base, 514 root pieces
// with geometry across the shipped models. The name rules match no TA piece that
// has geometry; they stay for the Kingdoms helper pieces.
inline bool skipLiveModelPiece(std::string_view name,bool root) {
    (void)root;
    return name.ends_with("gp") || name.ends_with("null") || name.ends_with("off") ||
        name.find("ground")!=name.npos || name.find("gpoly")!=name.npos || name.find("gpoint")!=name.npos;
}
