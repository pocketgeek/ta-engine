#pragma once
#include <string>
#include <string_view>

namespace tak::vpath {
// Archive names are byte strings, not native OS paths. Never route lexical
// operations through Windows' locale-dependent narrow/wide conversion.
inline std::string_view filename(std::string_view p) {
    auto i=p.find_last_of("/\\");
    return i==p.npos ? p : p.substr(i+1);
}
inline std::string extension(std::string_view p) {
    p=filename(p);
    auto i=p.find_last_of('.');
    return i==p.npos || i==0 || p==".." ? std::string{} : std::string(p.substr(i));
}
inline std::string stem(std::string_view p) {
    p=filename(p);
    return std::string(p.substr(0,p.size()-extension(p).size()));
}
inline std::string replaceExtension(std::string_view p, std::string_view ext) {
    std::string out(p.substr(0,p.size()-extension(p).size()));
    if (!ext.empty() && ext.front()!='.') out+='.';
    out+=ext;
    return out;
}
}
