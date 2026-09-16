#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ta::tdf {

// TDF: the TA-family text config format used by .fbi (units), .tdf, .ota
// (maps), .gui files. Nested [SECTION] { key=value; ... } blocks.
// Section and key lookups are case-insensitive (the game data is not
// consistent about case).

struct Node {
    std::map<std::string, std::string> values;      // keys lowercased
    std::map<std::string, Node> children;           // names lowercased
    std::vector<std::string> childOrder;            // original order, lowercased

    const std::string* value(const std::string& key) const;      // nullptr if absent
    std::string valueOr(const std::string& key, const std::string& def) const;
    double numberOr(const std::string& key, double def) const;
    const Node* child(const std::string& name) const;            // nullptr if absent
};

// Parse a TDF file or an in-memory buffer. Throws on malformed input.
Node parse(const std::filesystem::path& file);
Node parseText(const std::string& text, const std::string& originName = "<memory>");

} // namespace ta::tdf
