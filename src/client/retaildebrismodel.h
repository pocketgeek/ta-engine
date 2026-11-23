#pragma once

#include "tdo/tdo.h"

namespace tak {

// Detached geometry owns its arrays independently of the source visual. Retail
// drops the selected root's authored offset and parent/sibling links, retaining
// child offsets only when the script requests the complete subtree.
inline tdo::Object retailDebrisModel(const tdo::Object& source,bool subtree) {
    tdo::Object result;
    result.name=source.name;
    result.selectionPrimitive=source.selectionPrimitive;
    result.verticesRaw=source.verticesRaw;
    result.vertices=source.vertices;
    result.primitives=source.primitives;
    if(subtree)result.children=source.children;
    return result;
}

} // namespace tak
