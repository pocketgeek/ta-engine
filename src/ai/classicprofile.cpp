#include "ai/classicprofile.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <map>
#include <utility>

#include "hpi/hpi.h"
#include "sim/sim.h"
#include "tdf/tdf.h"
#include "util/virtualpath.h"

namespace tak::ai {

namespace {

// The C "C"-locale isspace the retail tokenizer and number readers use.
bool cSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
std::string lowered(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lowerAscii(c);
    return out;
}

// Retail's per-line tokenizer (0x538a20): whitespace separates tokens and '#'
// ends the line, inside a token too. Retail keeps at most 128 tokens and 126
// bytes of token text (a longer line overruns its buffer); neither limit is
// reached by a sane script, so neither is modelled.
std::vector<std::string_view> tokens(std::string_view line) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && cSpace(line[i])) ++i;
        if (i == line.size() || line[i] == '#') break;
        const size_t start = i;
        while (i < line.size() && !cSpace(line[i]) && line[i] != '#') ++i;
        out.push_back(line.substr(start, i - start));
        if (i < line.size() && line[i] == '#') break;
    }
    return out;
}

// Misspelt unit names in the shipped profiles, read as the unit they mean. Retail
// ignores them: IPData's default.txt and 11 other profiles say `araseige`, so
// its AI builds the Rolling Tower without limit.
constexpr std::pair<std::string_view, std::string_view> kMisspelt[] = {{"araseige", "arasiege"}};

}  // namespace

std::string_view classicUnitSpelling(std::string_view name) {
    for (const auto& [wrong, right] : kMisspelt)
        if (name == wrong) return right;
    return name;
}

float classicAtof(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && cSpace(s[i])) ++i;
    std::string digits;   // a cleaned copy for strtod: sign, mantissa, exponent
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) digits.push_back(s[i++]);
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { digits.push_back(s[i++]); any = true; }
    if (i < s.size() && s[i] == '.') {
        digits.push_back(s[i++]);
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { digits.push_back(s[i++]); any = true; }
    }
    if (!any) return 0.0f;
    // The old Microsoft C library also takes a Fortran 'd' exponent.
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E' || s[i] == 'd' || s[i] == 'D')) {
        size_t j = i + 1;
        std::string exponent = "e";
        if (j < s.size() && (s[j] == '+' || s[j] == '-')) exponent.push_back(s[j++]);
        if (j < s.size() && s[j] >= '0' && s[j] <= '9') {
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') exponent.push_back(s[j++]);
            digits += exponent;
        }
    }
    // atof returns a double; the weight handler stores it as a float.
    return float(std::strtod(digits.c_str(), nullptr));
}

int32_t classicAtoi(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && cSpace(s[i])) ++i;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) negative = s[i++] == '-';
    uint32_t total = 0;   // the C library's accumulator wraps on overflow
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') total = total * 10u + uint32_t(s[i++] - '0');
    return int32_t(negative ? 0u - total : total);
}

uint8_t classicScaledPref(uint8_t pref, float w) {
    // fild/fmul: an int times a float is exact in a double. _ftol truncates to a
    // 64-bit integer (out of range or NaN gives 0x8000000000000000) and the store
    // keeps the low 32 bits, which the clamp then reads as a signed int.
    const double x = double(pref) * double(w);
    int64_t wide = INT64_MIN;
    if (std::isfinite(x) && std::fabs(x) < 9223372036854775808.0) wide = int64_t(x);
    const int32_t v = int32_t(uint32_t(uint64_t(wide)));
    return uint8_t(std::clamp(v, 0, 100));
}

void ClassicCatalogue::add(const tak::sim::UnitType* type, std::string name,
                           const std::string& categoryWords) {
    const auto index = uint16_t(names.size());
    types.push_back(type);
    names.push_back(lowered(name));
    byName.emplace(names.back(), index);
    // The FBI loader's category registration (0x5172c0): each whitespace word of
    // `category=`, lowercased, then ALL.
    for (auto word : tokens(categoryWords)) categories[lowered(word)].push_back(index);
    categories["all"].push_back(index);
}

ClassicCatalogue ClassicCatalogue::from(const tak::sim::TypeRegistry& registry) {
    // Retail's table holds one entry per FBI, keyed and sorted by unitname: the
    // registry's types plus the variants that lost their objectname (tarnecr2).
    std::map<std::string, const tak::sim::UnitType*> table;
    for (const auto& [id, type] : registry.types())
        table.emplace(type.classicFbi.unitName.empty() ? id : type.classicFbi.unitName, &type);
    for (const auto& [name, type] : registry.classicVariants()) table.emplace(name, &type);
    ClassicCatalogue c;
    for (const auto& [name, type] : table) c.add(type, name, type->classicFbi.category);
    return c;
}

ClassicCatalogue ClassicCatalogue::synthetic(
    std::vector<std::pair<std::string, std::vector<std::string>>> entries) {
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return lowered(a.first) < lowered(b.first); });
    ClassicCatalogue c;
    for (const auto& [name, cats] : entries) {
        std::string words;
        for (const auto& w : cats) words += w + " ";
        c.add(nullptr, name, words);
    }
    return c;
}

void ClassicProfile::reset(size_t n) {
    pref.assign(n, 100);
    limit.assign(n, -1);
    prefLocked.assign(n, 0);
    limitLocked.assign(n, 0);
}

void ClassicProfile::execute(std::string_view script, const ClassicCatalogue& catalogue) {
    if (pref.size() != catalogue.size()) reset(catalogue.size());
    std::vector<uint16_t> one(1);
    for (size_t start = 0; start <= script.size();) {
        size_t end = script.find('\n', start);
        if (end == std::string_view::npos) end = script.size();
        const auto t = tokens(script.substr(start, end - start));
        start = end + 1;
        if (t.empty() || t[0].substr(0, 2) == "//") continue;
        const std::string command = lowered(t[0]);
        const bool weight = command == "weight";
        if (!weight && command != "limit") continue;   // `plan` and the rest: no-ops here
        // 0x517240: an exact name sets one type and locks it; anything else is a
        // category (unknown = empty), which does not lock.
        const std::string word = t.size() > 1 ? lowered(t[1]) : std::string();
        const std::vector<uint16_t>* targets = nullptr;
        bool exact = false;
        auto it = catalogue.byName.find(word);
        if (it == catalogue.byName.end()) it = catalogue.byName.find(std::string(classicUnitSpelling(word)));
        if (it != catalogue.byName.end() && it->second != 0) {
            one[0] = it->second;
            targets = &one;
            exact = true;
        } else if (auto ct = catalogue.categories.find(word); ct != catalogue.categories.end()) {
            targets = &ct->second;
        }
        if (!targets) continue;
        const std::string_view value = t.size() > 2 ? t[2] : std::string_view();
        if (weight) {
            const float w = t.size() > 2 ? classicAtof(value) : 0.0f;
            for (uint16_t i : *targets) {
                if (prefLocked[i]) continue;
                pref[i] = classicScaledPref(pref[i], w);
                if (exact) prefLocked[i] = 1;
            }
        } else {
            const int32_t n = t.size() > 2 ? classicAtoi(value) : 0;
            for (uint16_t i : *targets) {
                if (limitLocked[i]) continue;
                limit[i] = n;
                if (exact) limitLocked[i] = 1;
            }
        }
    }
}

std::string classicMapAiProfile(const tak::hpi::Vfs& vfs, const std::string& mapPath) {
    const std::string ota = tak::vpath::replaceExtension(mapPath, ".ota");
    const auto bytes = vfs.tryRead(ota);
    if (!bytes) return {};
    try {
        const auto root = tak::tdf::parseText(std::string(bytes->begin(), bytes->end()), ota);
        if (const auto* gh = root.child("globalheader"))
            if (const auto* md = gh->child("map data")) return md->valueOr("aiprofile", "");
    } catch (const std::exception&) {}
    return {};
}

ClassicProfileScript classicProfileScript(const tak::hpi::Vfs& vfs, const std::string& key) {
    return classicProfileScript(vfs, key, nullptr, nullptr);
}

ClassicProfileScript classicProfileScript(const tak::hpi::Vfs& vfs, const std::string& key,
                                          const ClassicFiles* gameModFiles, const ClassicFiles* mapFiles,
                                          ClassicProfileLayer* layer) {
    std::string name = key;
    // 0x53ae10 drops the extension (the last '.' after the last path separator).
    if (const auto dot = name.find_last_of('.');
        dot != std::string::npos && name.find_first_of("/\\", dot) == std::string::npos)
        name.erase(dot);
    ClassicProfileScript out;
    if (layer) *layer = ClassicProfileLayer::None;
    for (const std::string& path : {name.empty() ? std::string() : "ai/" + name + ".txt",
                                    std::string("ai/default.txt")}) {
        if (path.empty()) continue;
        const std::string k = tak::hpi::MountSet::key(path);
        for (auto [files, from] : {std::pair{gameModFiles, ClassicProfileLayer::GameMods},
                                   std::pair{mapFiles, ClassicProfileLayer::Map}}) {
            if (!files) continue;
            const auto it = files->find(k);
            if (it == files->end()) continue;
            out.path = path;
            out.text.assign(it->second.begin(), it->second.end());
            if (layer) *layer = from;
            return out;
        }
        if (auto b = vfs.tryRead(path)) {
            out.path = path;
            out.text.assign(b->begin(), b->end());
            if (layer) *layer = ClassicProfileLayer::Base;
            return out;
        }
    }
    return out;
}

}  // namespace tak::ai
