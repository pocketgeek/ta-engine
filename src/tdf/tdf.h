#pragma once

#include <filesystem>
#include <map>
#include "util/stoptoken.h"
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tak::tdf {

// TDF: the TA-family text config format used by .fbi (units), .tdf, .ota
// (maps), .gui files. Nested [SECTION] { key=value; ... } blocks.
// Section and key lookups are case-insensitive (the game data is not
// consistent about case).

struct Node {
    std::map<std::string, std::string> values;      // keys lowercased
    std::map<std::string, Node> children;           // names lowercased
    std::vector<std::string> childOrder;            // original order, lowercased
    // Named lookup retains its historical last-definition-wins behavior.
    // Repeated sections (effect emitters, palette ramps) also need lossless order.
    struct ChildEntry { std::string name; int previous=-1; };
    std::vector<ChildEntry> childEntries;
    std::vector<Node> previousChildren;

    const std::string* value(const std::string& key) const;      // nullptr if absent
    std::string valueOr(const std::string& key, const std::string& def) const;
    double numberOr(const std::string& key, double def) const;
    const Node* child(const std::string& name) const;            // nullptr if absent
    std::vector<std::pair<std::string_view,const Node*>> orderedChildren() const;
};

struct ParseUsage {size_t sections=0,nodes=0,memory=0;};
struct ParseLimitError:std::runtime_error {using std::runtime_error::runtime_error;};
struct ParseLimits {
    size_t sections = 65536, nodes = 262144, memory = 64u << 20;
    tak::StopToken stop;
    ParseUsage* usage=nullptr;
};
// Parse a TDF file or an in-memory buffer. Throws on malformed input.
Node parse(const std::filesystem::path& file);
Node parseText(const std::string& text, const std::string& originName = "<memory>", ParseLimits limits = {});

} // namespace tak::tdf
