#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace ta {

// Case-insensitive suffix test, for matching file extensions against VFS paths.
//
// This exists because TA's archives are not consistent about case -- the same
// install holds `features/archi/METAL.TDF` and `features/all worlds/
// DragonsTeeth.tdf` -- so a `path.extension() != ".tdf"` test silently skips
// every uppercase file. That is exactly how a map carrying 120 features, 10 of
// them metal patches, loaded with none of them and reported no error.
inline bool iendsWith(std::string_view s, std::string_view suffix) {
    if (suffix.size() > s.size()) return false;
    auto si = s.end() - std::ptrdiff_t(suffix.size());
    for (auto ci = suffix.begin(); ci != suffix.end(); ++ci, ++si)
        if (std::tolower(static_cast<unsigned char>(*si)) !=
            std::tolower(static_cast<unsigned char>(*ci)))
            return false;
    return true;
}

} // namespace ta
