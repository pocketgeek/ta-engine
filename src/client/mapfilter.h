#pragma once
// The CHANGE MAP panel's map list FILTER: a map stays listed when its title or its name
// contains the typed text, ignoring case. An empty filter lists every map.

#include <cctype>
#include <string>
#include <string_view>

namespace tak::mapfilter {

inline std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// `want` is the filter text already lowered (lower(filter)), so a list of maps lowers
// it once.
inline bool matches(std::string_view title, std::string_view name, const std::string& want) {
    return want.empty() || lower(title).find(want) != std::string::npos ||
           lower(name).find(want) != std::string::npos;
}

}  // namespace tak::mapfilter
