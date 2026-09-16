#include "tdf/sidedata.h"

#include "hpi/hpi.h"
#include "tdf/tdf.h"

#include <algorithm>
#include <cctype>

namespace ta::tdf {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Every section under a side that is not one of its scalar keys is a panel rect.
PanelRect readRect(const Node& n) {
    PanelRect r;
    r.x1 = int(n.numberOr("x1", 0));
    r.y1 = int(n.numberOr("y1", 0));
    r.x2 = int(n.numberOr("x2", 0));
    r.y2 = int(n.numberOr("y2", 0));
    return r;
}

}  // namespace

const PanelRect* Side::panel(const std::string& name) const {
    auto it = panels.find(upper(name));
    return it == panels.end() ? nullptr : &it->second;
}

const Side* SideData::side(const std::string& name) const {
    std::string want = upper(name);
    for (const auto& s : sides)
        if (upper(s.name) == want) return &s;
    return nullptr;
}

const Side* SideData::sideByPrefix(const std::string& p) const {
    std::string want = upper(p);
    for (const auto& s : sides)
        if (upper(s.namePrefix) == want) return &s;
    return nullptr;
}

SideData SideData::parse(const std::string& text, const std::string& origin) {
    SideData out;
    Node root;
    try {
        root = parseText(text, origin);
    } catch (const std::exception&) {
        return out;
    }

    // Sides are [SIDE0], [SIDE1], ... -- read in numeric order so a side's index
    // is stable, because that index is what a lobby slot stores.
    for (int i = 0;; ++i) {
        const Node* sn = root.child("side" + std::to_string(i));
        if (!sn) break;
        Side s;
        s.name = sn->valueOr("name", "");
        s.namePrefix = sn->valueOr("nameprefix", "");
        s.commander = lower(sn->valueOr("commander", ""));
        s.intGaf = sn->valueOr("intgaf", "");
        s.font = sn->valueOr("font", "");
        s.fontGui = sn->valueOr("fontgui", "");
        s.energyColor = int(sn->numberOr("energycolor", 0));
        s.metalColor = int(sn->numberOr("metalcolor", 0));
        for (const std::string& child : sn->childOrder)
            if (const Node* c = sn->child(child))
                s.panels[upper(child)] = readRect(*c);
        out.sides.push_back(std::move(s));
    }

    // [CANBUILD] { [ARMCOM] { canbuild1=...; canbuild2=...; } ... }
    if (const Node* cb = root.child("canbuild")) {
        for (const std::string& builder : cb->childOrder) {
            const Node* b = cb->child(builder);
            if (!b) continue;
            std::vector<std::string> list;
            // Numbered keys, read in order and stopping at the first gap -- the
            // menu order IS the numbering, so walking the map's own key order
            // would put canbuild10 between canbuild1 and canbuild2.
            for (int n = 1;; ++n) {
                const std::string* v = b->value("canbuild" + std::to_string(n));
                if (!v) break;
                if (!v->empty()) list.push_back(lower(*v));
            }
            if (!list.empty()) out.canBuild[lower(builder)] = std::move(list);
        }
    }
    return out;
}

SideData SideData::load(const hpi::Vfs& vfs) {
    const char* kPath = "gamedata/sidedata.tdf";
    if (!vfs.has(kPath)) return {};
    try {
        auto b = vfs.read(kPath);
        return parse(std::string(b.begin(), b.end()), kPath);
    } catch (const std::exception&) {
        return {};
    }
}


const Side* SideData::byIndex(int i) const {
    if (sides.empty()) return nullptr;
    if (i < 0 || i >= int(sides.size())) i = 0;
    return &sides[size_t(i)];
}

std::string SideData::nameForIndex(int i, const std::string& fallback) const {
    const Side* s = byIndex(i);
    if (!s) return fallback;
    std::string n = s->name;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    return n;
}

int SideData::indexOfName(const std::string& name) const {
    for (size_t i = 0; i < sides.size(); ++i) {
        const std::string& n = sides[i].name;
        if (n.size() != name.size()) continue;
        bool same = true;
        for (size_t k = 0; k < n.size(); ++k)
            if (std::tolower(uint8_t(n[k])) != std::tolower(uint8_t(name[k]))) { same = false; break; }
        if (same) return int(i);
    }
    return -1;
}

} // namespace ta::tdf
