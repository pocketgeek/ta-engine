#pragma once
#include <string>
#include <string_view>

namespace ta::vpath {
// Archive names are byte strings, not native OS paths. Never route lexical
// operations through Windows' locale-dependent narrow/wide conversion.
inline std::string_view filename(std::string_view p) {
    auto i=p.find_last_of("/\\");
    return i==p.npos ? p : p.substr(i+1);
}
// The extension LOWERCASED ("METAL.TDF" -> ".tdf"): Total Annihilation's archives
// mix case freely (720 upper-case extensions in totala1.hpi alone) and every caller
// compares extensions as file types, never as display text.
inline std::string extension(std::string_view p) {
    p=filename(p);
    auto i=p.find_last_of('.');
    std::string e = i==p.npos || i==0 || p==".." ? std::string{} : std::string(p.substr(i));
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return e;
}
inline std::string stem(std::string_view p) {
    p=filename(p);
    auto i=p.find_last_of('.');
    const size_t n = i==p.npos || i==0 || p==".." ? p.size() : i;
    return std::string(p.substr(0,n));
}
inline std::string replaceExtension(std::string_view p, std::string_view ext) {
    std::string out(p.substr(0,p.size()-extension(p).size()));
    if (!ext.empty() && ext.front()!='.') out+='.';
    out+=ext;
    return out;
}
}
