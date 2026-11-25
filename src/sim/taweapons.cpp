#include "sim/taweapons.h"

#include "hpi/hpi.h"
#include "tdf/tdf.h"
#include "util/virtualpath.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace ta::sim {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// GetInt = atol: decimal, optional sign, stops at the first non-digit, wraps mod 2^32.
int32_t getInt(const tdf::Node& n, const char* key, int32_t def) {
    const std::string* v = n.value(key);
    if (!v) return def;
    return int32_t(uint32_t(std::strtoll(v->c_str(), nullptr, 10)));
}
double getDbl(const tdf::Node& n, const char* key, double def) {
    const std::string* v = n.value(key);
    return v ? std::strtod(v->c_str(), nullptr) : def;
}
// trunc(atof * k) in IEEE double (retail runs the FPU at 53-bit precision).
int32_t scaled(const tdf::Node& n, const char* key, double k, int32_t def = 0) {
    const std::string* v = n.value(key);
    if (!v) return def;
    volatile double x = std::strtod(v->c_str(), nullptr) * k;
    return int32_t(std::trunc(x));
}
bool flag(const tdf::Node& n, const char* key) { return (getInt(n, key, 0) & 1) != 0; }

}  // namespace

Weapon parseTaWeapon(const tdf::Node& w, const std::string& sectionName) {
    Weapon wp;
    wp.section = lower(sectionName);
    wp.taId = getInt(w, "id", -1);
    wp.name = w.valueOr("name", "");
    static const char* kFlagKeys[] = {
        "lineofsight", "ballistic", "shellweapon", "beamweapon", "vlaunch", "meteor", "noradar",
        "paralyzer", "dropped", "startsmoke", "endsmoke", "soundtrigger", "guidance", "tracks",
        "unitsonly", "groundbounce", "waterweapon", "toairweapon", "smoketrail", "turret",
        "selfprop", "propeller", "noexplode", "burnblow", "twophase", "cruise", "commandfire",
        "noautorange", "stockpile", "targetable", "interceptor"};
    for (uint32_t i = 0; i < sizeof kFlagKeys / sizeof *kFlagKeys; ++i)
        if (flag(w, kFlagKeys[i])) wp.taFlags |= 1u << i;

    constexpr double kPerTick = 65536.0 / 30.0, kPerTick2 = 65536.0 / 900.0;
    wp.velocityFx = scaled(w, "weaponvelocity", kPerTick);
    wp.startVelocityFx = scaled(w, "startvelocity", kPerTick);
    wp.accelerationFx = scaled(w, "weaponacceleration", kPerTick2);
    wp.range = getInt(w, "range", 32767);
    wp.coverage = getInt(w, "coverage", 0);
    wp.reloadTicks = uint16_t(scaled(w, "reloadtime", 30.0));
    wp.energyPerShot = float(getDbl(w, "energypershot", 0));
    wp.metalPerShot = float(getDbl(w, "metalpershot", 0));
    wp.aoe = int16_t(getInt(w, "areaofeffect", 0));
    wp.edge = float(getDbl(w, "edgeeffectiveness", 0));
    wp.weaponTimerTicks = uint16_t(scaled(w, "weapontimer", 30.0));
    wp.turnRatePerTick = uint16_t(scaled(w, "turnrate", 1.0 / 30.0));
    wp.burst = uint16_t(getInt(w, "burst", 0));
    wp.burstRateTicks = uint16_t(scaled(w, "burstrate", 30.0));
    wp.sprayAngle = uint16_t(getInt(w, "sprayangle", 0));
    wp.durationTicks16 = uint16_t(scaled(w, "duration", 30.0));
    wp.randomDecayTicks = uint16_t(scaled(w, "randomdecay", 30.0));
    wp.smokeDelayTicks = uint16_t(scaled(w, "smokedelay", 30.0));
    wp.flightTimeTicks = uint16_t(scaled(w, "flighttime", 30.0));
    wp.holdTimeTicks = uint16_t(scaled(w, "holdtime", 30.0));
    if (w.value("minbarrelangle"))
        wp.minBarrelAngle = float(getDbl(w, "minbarrelangle", 0) * (3.14159265358979323846 / 180.0));
    wp.fireStarter = getInt(w, "firestarter", 0) != 0;
    wp.renderType = uint8_t(getInt(w, "rendertype", 0));
    wp.color = uint8_t(getInt(w, "color", 0));
    wp.color2 = uint8_t(getInt(w, "color2", 0));
    wp.accuracy = uint16_t(getInt(w, "accuracy", 0));
    wp.tolerance = uint16_t(getInt(w, "tolerance", 0));
    wp.pitchTolerance = uint16_t(getInt(w, "pitchtolerance", 0));
    wp.shakeMag = getInt(w, "shakemagnitude", 0);
    wp.shakeTicks = scaled(w, "shakeduration", 30.0);
    wp.shakeDur = float(wp.shakeTicks) / 30.0f;
    wp.shotModel = lower(w.valueOr("model", ""));
    if (auto dot = wp.shotModel.rfind(".3do"); dot != std::string::npos) wp.shotModel.erase(dot);
    wp.explosionGaf = lower(w.valueOr("explosiongaf", ""));
    wp.explosionArt = lower(w.valueOr("explosionart", ""));
    wp.waterExplosionGaf = lower(w.valueOr("waterexplosiongaf", ""));
    wp.waterExplosionArt = lower(w.valueOr("waterexplosionart", ""));
    wp.lavaExplosionGaf = lower(w.valueOr("lavaexplosiongaf", ""));
    wp.lavaExplosionArt = lower(w.valueOr("lavaexplosionart", ""));
    wp.soundStart = lower(w.valueOr("soundstart", ""));
    wp.soundHit = lower(w.valueOr("soundhit", ""));
    wp.soundWater = lower(w.valueOr("soundwater", ""));
    if (const tdf::Node* dmg = w.child("DAMAGE")) {
        wp.damage = uint16_t(getInt(*dmg, "default", 0));   // stored as u16
        for (const auto& [k, v] : dmg->values)
            if (k != "default") wp.dmgVs[lower(k)] = int32_t(std::strtoll(v.c_str(), nullptr, 10));
    }

    // Engine-side views of the record (the sim's shared weapon model).
    wp.reload = float(wp.reloadTicks) / 30.0f;

    wp.projVel = float(getDbl(w, "weaponvelocity", 0));
    wp.subSteps = wp.projVel > 0.0f ? int((wp.projVel + 479.0f) / 480.0f) : 1;
    wp.ballistic = wp.hasTaFlag(kWBallistic);
    wp.beam = wp.hasTaFlag(kWBeamWeapon);
    wp.groundBounce = wp.hasTaFlag(kWGroundBounce);
    wp.waterWeapon = wp.hasTaFlag(kWWaterWeapon);
    wp.unitsOnly = wp.hasTaFlag(kWUnitsOnly);
    wp.toAir = wp.hasTaFlag(kWToAirWeapon);
    wp.commandFire = wp.hasTaFlag(kWCommandFire);
    wp.cursorAirstrike = wp.hasTaFlag(kWDropped);
    if (wp.hasTaFlag(kWGuidance) || wp.hasTaFlag(kWTracks)) wp.kind = Weapon::Kind::Guided;
    else if (wp.hasTaFlag(kWDropped)) wp.kind = Weapon::Kind::Dropped;
    // TA turnrate is angle units (65536 per turn) per second.
    wp.turnRate = float(getDbl(w, "turnrate", 0) * (2.0 * 3.14159265358979323846 / 65536.0));
    wp.aimTol = wp.tolerance;
    if (wp.hasTaFlag(kWParalyzer)) wp.status = Weapon::Status::Paralyzed;
    wp.emitTime = wp.durationTicks16;
    // 0x49e010: turret > vlaunch > lineofsight|selfprop > dropped > none.
    if (wp.hasTaFlag(kWTurret)) wp.taAim = Weapon::TaAim::Turret;
    else if (wp.hasTaFlag(kWVLaunch)) wp.taAim = Weapon::TaAim::VLaunch;
    else if (wp.hasTaFlag(kWLineOfSight) || wp.hasTaFlag(kWSelfProp)) wp.taAim = Weapon::TaAim::Direct;
    else if (wp.hasTaFlag(kWDropped)) wp.taAim = Weapon::TaAim::Dropped;
    return wp;
}

void TaWeaponTable::load(const hpi::Vfs& vfs) {
    slots_.assign(256, Weapon{});
    byName_.clear();
    for (const std::string& path : vfs.list("weapons")) {
        if (ta::vpath::extension(path) != ".tdf") continue;
        try {
            auto b = vfs.read(path);
            auto root = tdf::parseText(std::string(b.begin(), b.end()), path);
            for (const auto& name : root.childOrder) {
                const tdf::Node* n = root.child(name);
                if (!n) continue;
                Weapon wp = parseTaWeapon(*n, name);
                if (wp.taId < 0 || wp.taId > 255) continue;
                slots_[size_t(wp.taId)] = std::move(wp);
            }
        } catch (const std::exception&) {}
    }
    for (int i = 0; i < 256; ++i) {
        const auto& sec = slots_[size_t(i)].section;
        if (!sec.empty()) byName_.try_emplace(sec, i);   // lowest ID first
    }
}

const Weapon& TaWeaponTable::slot(int id) const {
    return slots_[size_t(id < 0 || id > 255 ? 0 : id)];
}

const Weapon& TaWeaponTable::resolve(const std::string& name) const {
    auto it = byName_.find(lower(name));
    return it == byName_.end() ? slots_[0] : slots_[size_t(it->second)];
}

}  // namespace ta::sim
