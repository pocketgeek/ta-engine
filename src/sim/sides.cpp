#include "sim/sides.h"

#include "hpi/hpi.h"
#include "tdf/tdf.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>

namespace ta::sides {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<Side> defaults() {
    // Retail Total Annihilation's gamedata/sidedata.tdf (SIDE0 ARM, SIDE1 CORE).
    Side arm{"ARM", "ARM", "armcom", "ARMINT", "console", "armbutt", 208, 224};
    Side core{"CORE", "COR", "corcom", "CORINT", "console", "corbutt", 208, 224};
    return {arm, core};
}

std::mutex mu;
std::vector<Side>& table() {
    static std::vector<Side> t = defaults();
    return t;
}

}  // namespace

int load(const hpi::Vfs& vfs) {
    std::vector<Side> out;
    try {
        auto b = vfs.read("gamedata/sidedata.tdf");
        auto root = tdf::parseText(std::string(b.begin(), b.end()), "gamedata/sidedata.tdf");
        for (int i = 0;; ++i) {
            const tdf::Node* n = root.child("SIDE" + std::to_string(i));
            if (!n) break;
            Side s;
            s.name = n->valueOr("name", "");
            s.prefix = n->valueOr("nameprefix", s.name);
            s.commander = lower(n->valueOr("commander", ""));
            s.intGaf = n->valueOr("intgaf", "");
            s.font = n->valueOr("font", "");
            s.fontGui = n->valueOr("fontgui", "");
            s.energyColor = int(n->numberOr("energycolor", 0));
            s.metalColor = int(n->numberOr("metalcolor", 0));
            if (s.name.empty()) break;
            out.push_back(std::move(s));
        }
    } catch (...) {}
    std::lock_guard<std::mutex> lk(mu);
    if (!out.empty()) table() = std::move(out);
    return int(table().size());
}

const std::vector<Side>& all() { return table(); }
int count() { return int(table().size()); }

const Side& at(int i) {
    const auto& t = table();
    if (i < 0) i = 0;
    if (i >= int(t.size())) i = int(t.size()) - 1;
    return t[size_t(i)];
}

int find(const std::string& key) {
    const std::string k = lower(key);
    const auto& t = table();
    for (size_t i = 0; i < t.size(); ++i)
        if (lower(t[i].name) == k || lower(t[i].prefix) == k) return int(i);
    return -1;
}

std::string lowerPrefix(int i) { return lower(at(i).prefix); }
std::string commander(int i) { return at(i).commander; }

bool isCommander(const std::string& unitId) {
    const std::string k = lower(unitId);
    for (const auto& s : table())
        if (s.commander == k) return true;
    return false;
}

}  // namespace ta::sides
