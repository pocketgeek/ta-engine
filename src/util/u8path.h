#pragma once
// A filesystem path from UTF-8 text, as std::filesystem::u8path made one. C++20
// deprecates u8path in favour of the char8_t constructor, which reads UTF-8 on every
// platform (Windows converts it to wide characters; elsewhere it is kept as is).
#include <filesystem>
#include <string>
#include <string_view>

namespace tak {

inline std::filesystem::path u8path(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

}  // namespace tak
