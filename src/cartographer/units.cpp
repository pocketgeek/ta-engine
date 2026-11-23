#include "cartographer/units.h"
#include "cartographer/scenarioinfo.h"
#include "sim/sim.h"

#include "hpi/hpi.h"
#include "tdf/tdf.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "util/virtualpath.h"

namespace cart {

tak::crt::Scenario loadScenario(const tak::hpi::Vfs& vfs, const std::string& crtPath) {
    if(!vfs.has(crtPath))return {};
    auto scenario=tak::crt::parse(vfs.read(crtPath));
    if(scenario.version==0)throw std::runtime_error("Scenario file is damaged: "+crtPath);
    try {const auto info=vfs.read(tak::vpath::replaceExtension(crtPath,".editor"));applyScenarioInfo(scenario,std::string(info.begin(),info.end()));}
    catch(const std::exception&) {} // Optional editor-only metadata.
    return scenario;
}

std::vector<PlacedUnit> toPlaced(const tak::crt::Scenario& s, const tak::sim::TypeRegistry* registry) {
    std::vector<PlacedUnit> out;
    out.reserve(s.units.size());
    for (const auto& u : s.units) {
        PlacedUnit p;
        p.type = u.objectName;
        p.player = u.player;
        std::string id=u.objectName;
        std::transform(id.begin(),id.end(),id.begin(),[](unsigned char c){return char(std::tolower(c));});
        if (registry) if (const auto* type=registry->find(id)) {p.footX=type->footX;p.footZ=type->footZ;}
        p.x = float(u.x) * 16.0f + p.footX*8.0f;
        p.z = float(u.z) * 16.0f + p.footZ*8.0f;
        p.vertical = u.y;
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

std::vector<uint8_t> saveScenario(tak::crt::Scenario base,
                                  const std::vector<PlacedUnit>& units) {
    base.version = 1.0f;
    if (base.players.empty()) base.players.resize(9);   // retail always writes 9
    base.units.clear();
    base.units.reserve(units.size());
    for (const auto& p : units) {
        tak::crt::Unit u;
        u.objectName = p.type;
        u.uniqueName = p.name;
        u.x = int32_t(std::floor((p.x-p.footX*8.0f) / 16.0f));   // pixel -> cell
        u.z = int32_t(std::floor((p.z-p.footZ*8.0f) / 16.0f));
        u.y = p.vertical;                         // preserve, but do not interpret
        u.player = p.player;
        u.health = std::clamp(p.health, 0, 100);
        u.armor = std::clamp(p.armor, 0, 1000);
        u.weapon = std::clamp(p.weapon, 0, 1000);
        u.veteran = std::clamp(p.veteran, 0, 9);
        long a = std::lround(p.angle) % 360; if (a < 0) a += 360;
        u.angle = int32_t(a);
        base.units.push_back(std::move(u));
    }
    return tak::crt::write(base);
}

std::vector<std::string> loadUseOnly(const tak::hpi::Vfs& vfs, const std::string& tdfPath) {
    std::vector<std::string> out;
    std::vector<uint8_t> d;
    try { d = vfs.read(tdfPath); } catch (const std::exception&) { return out; }
    tak::tdf::Node root = tak::tdf::parseText(std::string(d.begin(), d.end()), tdfPath);
    for (const std::string& name : root.childOrder) {   // childOrder is lowercased
        std::string up = name;
        std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c){return char(std::toupper(c));});
        out.push_back(up);
    }
    return out;
}

std::string writeUseOnly(const std::vector<std::string>& types) {
    std::string s;
    for (const auto& t : types) s += "[" + t + "]\t{}\r\n";
    return s;
}

std::vector<std::string> unitTypeNames(const tak::hpi::Vfs& vfs) {
    std::vector<std::string> names;
    for (const std::string& p : vfs.list("units")) {
        std::string ext = tak::vpath::extension(p);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){return char(std::tolower(c));});
        if (ext != ".fbi") continue;
        std::string stem = tak::vpath::stem(p);
        std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c){return char(std::toupper(c));});
        names.push_back(stem);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

} // namespace cart
