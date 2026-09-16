#include "cartographer/units.h"

#include "hpi/hpi.h"
#include "tdf/tdf.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace cart {

ta::crt::Scenario loadScenario(const ta::hpi::Vfs& vfs, const std::string& crtPath) {
    std::vector<uint8_t> d;
    try { d = vfs.read(crtPath); } catch (const std::exception&) { return {}; }
    return ta::crt::parse(d);
}

std::vector<PlacedUnit> toPlaced(const ta::crt::Scenario& s) {
    std::vector<PlacedUnit> out;
    out.reserve(s.units.size());
    for (const auto& u : s.units) {
        PlacedUnit p;
        p.type = u.objectName;
        p.player = u.player;
        p.x = float(u.x) * 16.0f + 8.0f;   // cell -> pixel centre
        p.z = float(u.z) * 16.0f + 8.0f;
        p.health = u.health;
        p.armor = u.armor;
        p.weapon = u.weapon;
        p.veteran = u.veteran;
        p.angle = float(u.angle);
        p.name = u.uniqueName;
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<uint8_t> saveScenario(ta::crt::Scenario base,
                                  const std::vector<PlacedUnit>& units) {
    base.version = 1.0f;
    if (base.players.empty()) base.players.resize(9);   // retail always writes 9
    base.units.clear();
    base.units.reserve(units.size());
    for (const auto& p : units) {
        ta::crt::Unit u;
        u.objectName = p.type;
        u.uniqueName = p.name;
        u.x = int32_t(std::floor(p.x / 16.0f));   // pixel -> cell
        u.z = int32_t(std::floor(p.z / 16.0f));
        u.y = 200;                                // constant in shipped maps
        u.player = p.player;
        u.health = std::clamp(p.health, 0, 100);
        u.armor = std::clamp(p.armor, 0, 1000);
        u.weapon = std::clamp(p.weapon, 0, 1000);
        u.veteran = std::clamp(p.veteran, 0, 9);
        long a = std::lround(p.angle) % 360; if (a < 0) a += 360;
        u.angle = int32_t(a);
        base.units.push_back(std::move(u));
    }
    return ta::crt::write(base);
}

std::vector<std::string> loadUseOnly(const ta::hpi::Vfs& vfs, const std::string& tdfPath) {
    std::vector<std::string> out;
    std::vector<uint8_t> d;
    try { d = vfs.read(tdfPath); } catch (const std::exception&) { return out; }
    ta::tdf::Node root = ta::tdf::parseText(std::string(d.begin(), d.end()), tdfPath);
    for (const std::string& name : root.childOrder) {   // childOrder is lowercased
        std::string up = name;
        std::transform(up.begin(), up.end(), up.begin(), ::toupper);
        out.push_back(up);
    }
    return out;
}

std::string writeUseOnly(const std::vector<std::string>& types) {
    std::string s;
    for (const auto& t : types) s += "[" + t + "]\t{}\r\n";
    return s;
}

std::vector<std::string> unitTypeNames(const ta::hpi::Vfs& vfs) {
    std::vector<std::string> names;
    for (const std::string& p : vfs.list("units")) {
        std::filesystem::path fp(p);
        std::string ext = fp.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".fbi") continue;
        std::string stem = fp.stem().string();
        std::transform(stem.begin(), stem.end(), stem.begin(), ::toupper);
        names.push_back(stem);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

} // namespace cart
