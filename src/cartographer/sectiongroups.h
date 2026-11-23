#pragma once

// Readable grouping for the terrain section browser. Retail ships sections as
// Sections/<World>/<Folder>/<name>.TNT with terse artist names ("1neclif2trans",
// "hwy_e_n_s01_80", "y_s_rgtb"). Everything here is derived from that structure
// alone -- no data files: folders map to kind groups (cliffs, transitions, water,
// ...), names decompose into kind / direction / corner tokens, and numbered or
// lettered variants of one piece collapse into a single browser entry.

#include "cartographer/sections.h"

#include <string>
#include <vector>

namespace cart {

// One prefab name, decomposed. Context-free except `tag`, which the catalog sets
// when sibling names prove a trailing letter/word is a variant (accoast01a/b/c).
struct SectionName {
    std::string kind;        // "Cliff", "Transition", ... ("" = use the folder's)
    std::string direction;   // "NE", "E+N+S", "NSEW"; "" when undirected
    std::string corner;      // "outer" / "inner" / "type 3" (leading 1/2/3)
    std::string number;      // identity number of undirected pieces ("13")
    std::string variant;     // variant number ("2"); "" for single pieces
    std::string details;     // remaining words/fragments, lowercase words
    std::string suffix;      // trailing letter that is not a variant ("X" in 1netrans1x)
    std::string tag;         // variant tag ("b", "lit", "x", "redo")
    std::string key;         // piece identity in its folder (variants share it)
};
// stripTag also splits a trailing variant letter/"lit" off into `tag`.
SectionName parseSectionName(const std::string& name,bool stripTag=false);
// Folder -> kind group ("Cliffs", "Transitions", ..., "Other") and its noun.
std::string sectionKindGroup(const std::string& folder);
std::string sectionFolderNoun(const std::string& folder);

class SectionCatalog {
public:
    struct Piece {
        std::string folder, kind, direction, corner;   // shared by the variants
        std::vector<int> variants;                      // section indices, ordered
        bool sameNumber = true;                          // variants differ only by tag
    };
    struct Group {
        std::string id;        // "" all, "kind:<group>", "folder:<lowercase folder>"
        std::string label;     // display text without the count
        int depth = 0;         // 0 = all / kind, 1 = folder under its kind
        int sections = 0;      // prefab files in the group
        std::vector<int> pieces;
    };
    void build(const std::vector<SectionRef>& sections);
    const std::vector<Group>& groups() const { return groups_; }
    const std::vector<Piece>& pieces() const { return pieces_; }
    const Group* group(const std::string& id) const;
    int pieceOf(int section) const;
    // "Cliff · NE outer · variant 2" -- the raw name is shown beside it by callers.
    std::string label(int section) const;
    std::string shortLabel(int section) const;   // ~14 chars for the thumbnail grid
    std::string variantText(int section) const;  // "2", "b", "1 lit"
    const SectionName& name(int section) const { return names_[size_t(section)]; }
    // Lowercased text the browser search matches (raw name, label, folder, group).
    std::string searchText(int section) const;
private:
    std::vector<SectionRef> refs_;
    std::vector<SectionName> names_;
    std::vector<int> pieceOf_;
    std::vector<Piece> pieces_;
    std::vector<Group> groups_;
};

} // namespace cart
