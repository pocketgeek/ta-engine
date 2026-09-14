#include "sim/sim.h"

#include "hpi/hpi.h"
#include "sim/detmath.h"
#include "sim/mission.h"
#include "sim/scenario.h"
#include "tdf/tdf.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <atomic>
#include <thread>

namespace tak::sim {


bool gInstantBuild = false;

// TAK_PHASE sim profiler globals (zero cost when unset). The accumulators are
// thread_local: the server may tick several games in parallel, one World::tick per
// thread, so each thread accumulates (and prints) its own room's timing -- both
// race-free and correctly attributed.
static const bool g_phase = getenv("TAK_PHASE") != nullptr;
static double g_visMs = 0, g_burnMs = 0, g_gridMs = 0;   // finer "other" split
static thread_local double g_tcomb = 0;

namespace {

constexpr float kPi = 3.14159265358979f;
// How long a released bomb takes to reach the ground from a flyer's cruise
// altitude. Retail drives it down under gravity from the terrain height it looks
// up at the release point; our projectiles are 2-D, so the fall is a fixed window
// after which the bomb detonates where it has drifted to.
constexpr float kBombFall = 0.8f;
constexpr float kTick = 30.0f;               // FBI per-tick values -> per-second
constexpr float kCobAngle = 2 * kPi / 65536.0f;

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

float angleDiff(float a, float b) {
    float d = std::fmod(a - b + kPi, 2 * kPi);
    if (d < 0) d += 2 * kPi;
    return d - kPi;
}

} // namespace

void TypeRegistry::loadMoveInfo(const hpi::Vfs& vfs, const std::string& path) {
    try {
        auto b = vfs.read(path);
        auto root = tdf::parseText(std::string(b.begin(), b.end()), path);
        for (const auto& cn : root.childOrder) {
            const tdf::Node& c = root.children.at(cn);
            std::string name = lower(c.valueOr("Name", ""));
            if (name.empty()) continue;
            MoveClass m;
            m.footX = int(c.numberOr("FootprintX", 0));
            m.footZ = int(c.numberOr("FootprintZ", 0));
            m.maxSlope = float(c.numberOr("MaxSlope", 255));
            m.maxWaterSlope = float(c.numberOr("MaxWaterSlope", 255));
            m.maxWaterDepth = float(c.numberOr("MaxWaterDepth", 255));
            m.minWaterDepth = float(c.numberOr("MinWaterDepth", 0));
            moveClasses_[name] = m;
        }
    } catch (const std::exception&) {}   // no MOVEINFO -> units keep FBI defaults
}

void TypeRegistry::loadDir(const hpi::Vfs& vfs, const std::string& prefix) {
    for (const std::string& path : vfs.list(prefix)) {
        if (lower(std::filesystem::path(path).extension().string()) != ".fbi") continue;
        try {
            auto bytes = vfs.read(path);
            auto root = tdf::parseText(std::string(bytes.begin(), bytes.end()), path);
            const auto* info = root.child("UNITINFO");
            if (!info) continue;
            UnitType t;
            std::string stem = lower(std::filesystem::path(path).stem().string());
            t.id = lower(info->valueOr("objectname", stem));
            // Two files can claim the same objectname (e.g. the Iron Plague campaign's
            // tarnecr2.fbi also declares objectname TARNECRO, but drops builder=1). The
            // CANONICAL definition is the one whose filename matches the objectname
            // (tarnecro.fbi); it must win even though a variant may sort earlier. So:
            // keep the first def UNLESS this is the canonical file and the existing def
            // was only a variant -- then replace it.
            bool canonical = (stem == t.id);
            if (types_.count(t.id)) {
                if (!canonical || canonicalTypes_.count(t.id)) continue;   // keep existing
            }
            if (canonical) canonicalTypes_.insert(t.id);
            t.name = info->valueOr("name", t.id);
            t.side = info->valueOr("side", "");
            // Buildings often declare canmove=1; bmcode (0 = building,
            // 1 = mobile unit) is the authoritative mobility flag.
            t.canMove = info->numberOr("canmove", 0) != 0 &&
                        info->numberOr("bmcode", 1) != 0;
            t.maxVel = float(info->numberOr("maxvelocity", 0)) * kTick;
            // Velocity is px/tick (*kTick => px/s); acceleration and braking are
            // px/tick^2, so they need kTick^2. Using kTick left accel 30x too
            // small, so high-maxVel flyers never reached speed and all crawled.
            t.accel = float(info->numberOr("acceleration", 0.5)) * kTick * kTick;
            t.brake = float(info->numberOr("brakerate", 0.5)) * kTick * kTick;
            // FBI turnrate is COB angle units per tick.
            float tr = float(info->numberOr("turnrate", 500));
            t.turnRate = tr * kCobAngle * kTick;
            // See the header: retail defaults this to 0; we default it to turnrate so
            // a unit that omits the key can still turn on the spot.
            t.turnInPlaceRate = float(info->numberOr("turninplacerate", tr))
                                * kCobAngle * kTick;
            t.maxHp = float(info->numberOr("maxdamage", 100));
            t.isBuilder = info->numberOr("builder", 0) != 0;
            t.commander = info->numberOr("commander", 0) != 0;   // the Monarch
            t.buildCost = float(info->numberOr("buildcost", 0));
            t.buildTime = float(info->numberOr("buildtime", 0));
            t.workerTime = float(info->numberOr("workertime", 1));
            t.income = float(info->numberOr("mogriumincome", 0));
            t.storage = float(info->numberOr("mogriumstorage", 0));
            t.footX = int(info->numberOr("footprintx", 1));
            t.footZ = int(info->numberOr("footprintz", 1));
            {
                std::string ym = info->valueOr("yardmap", "");
                std::erase_if(ym, [](char c) { return c == ' ' || c == '\t'; });
                // A stray 'S' (not a full footprint map) marks a lodestone that
                // must sit on a mana deposit.
                t.onMana = ym.find('S') != std::string::npos ||
                           ym.find('s') != std::string::npos;
                if (int(ym.size()) == t.footX * t.footZ) t.yardMap = ym;
            }
            // Every lodestone (Lodestone / Divine Lodestone) must sit on a mana
            // deposit, even ones whose FBI omits the 'S' yardmap (e.g. crelode).
            if (t.id.find("lode") != std::string::npos ||
                t.id.find("mana") != std::string::npos)
                t.onMana = true;
            t.canTransport = info->numberOr("cantransport", 0) != 0;
            // Prefer the size-based capacity when present (it caps summed
            // transportsize, which is what we compare); else the plain count.
            t.transportCap = int(info->numberOr("transportsizecapacity",
                                                info->numberOr("transportcapacity", 0)));
            t.transportDist = float(info->numberOr("transportdistance", 0));
            t.buildDist = float(info->numberOr("builddistance", 0));
            t.soundClass = lower(info->valueOr("soundcategory",
                                               info->valueOr("soundclass", "")));
            t.sight = float(info->numberOr("sightdistance", 180));
            t.corpse = lower(info->valueOr("corpse", ""));
            t.stoneFeat = lower(info->valueOr("stone", ""));
            t.frozenFeat = lower(info->valueOr("frozen", ""));
            t.corpseAdjX = int(info->numberOr("corpseadjustx", 0));
            t.corpseAdjZ = int(info->numberOr("corpseadjustz", 0));
            t.canAnimate = info->numberOr("cananimate", 0) != 0;
            t.animName_ = lower(info->valueOr("animatetype", ""));
            t.shadowArt = lower(info->valueOr("shadowart", ""));
            t.noShadow = info->numberOr("noshadow", 0) != 0;
            t.floater = info->numberOr("floater", 0) != 0;
            t.canHover = info->numberOr("canhover", 0) != 0;
            t.ghost = info->numberOr("ghost", 0) != 0;
            t.waterline = int(info->numberOr("waterline", 0));
            t.veteranModel = lower(info->valueOr("veteranmodel", ""));
            t.bodyType = lower(info->valueOr("bodytype", "default"));
            // Extended stats.
            t.healTime = float(info->numberOr("healtime", 0));
            t.leash = float(info->numberOr("maneuverleashlength", 0));
            // Standing orders, derived the way retail's parser does (icd 0x4c005d):
            // standingunitorder is a front-end that expands into the two fields
            // that actually drive behaviour, and its ABSENCE is a distinct case --
            // the sentinel default 3 falls through to standingmoveorder /
            // standingfireorder (both default 2), i.e. roam + fire at will.
            {
                int suo = int(info->numberOr("standingunitorder", 3));
                if (suo == 2)      { t.defaultMove = 1; t.defaultFire = 2; }
                else if (suo == 1) { t.defaultMove = 0; t.defaultFire = 2; }
                else if (suo == 0) { t.defaultMove = 0; t.defaultFire = 0; }
                else {
                    t.defaultMove = uint8_t(int(info->numberOr("standingmoveorder", 2)) & 3);
                    t.defaultFire = uint8_t(int(info->numberOr("standingfireorder", 2)) & 3);
                }
            }
            t.waterMult = float(info->numberOr("watermultiplier",
                                info->numberOr("watermultipliser", 1)));
            // Exact key only: the icd's parser knows no typo fallback, so
            // verpult's "roadmultplier" never counted in retail either. The
            // retail default is ~1.2 (16.16 0x13333), NOT 1.0.
            t.roadMult = float(info->numberOr("roadmultiplier", 1.2));
            t.maxWaterDepth = float(info->numberOr("maxwaterdepth", 0));
            t.maxSlope = float(info->numberOr("maxslope", 255));
            // Retail keeps this in 3 bits (icd 0x4c09e8: `& 7`, shifted into
            // UnitType+0x264 bits 21..23), so 0..7 seconds is the whole range.
            // Default 2, not 0: when the key is absent retail's parser clears
            // bit 23 and sets bit 22 of the three-bit field (icd 0x4c0a1f),
            // which leaves 2. No shipped FBI declares the key, so 2 is what
            // every unit in the game actually uses.
            t.selfDestructCountdown =
                std::clamp(int(info->numberOr("selfdestructcountdown", 2)), 0, 7);
            t.radar = float(info->numberOr("radardistance", 0));
            t.noVeteran = info->numberOr("noveteran", 0) != 0;
            t.maxMana = float(info->numberOr("maxmana", 0));
            t.manaRegen = float(info->numberOr("manarechargerate", 0));
            t.canReclaim = info->numberOr("canreclaim", 0) != 0;
            t.canResurrect = info->numberOr("canresurrect", 0) != 0;
            t.canCapture = info->numberOr("cancapture", 0) != 0;
            t.canCloak = info->numberOr("cancloak", 0) != 0;
            t.cloakCost = float(info->numberOr("cloakcost", 0));
            t.cloakCostMove = float(info->numberOr("cloakcostmoving", t.cloakCost));
            t.minCloakDist = float(info->numberOr("mincloakdistance", 0));
            // Boolean in retail too: the parser does `and eax,1`.
            t.fireAtWillRandom = (int(info->numberOr("fireatwillrandom", 0)) & 1) != 0;
            t.attractsGods = info->numberOr("attractsgods", 0) != 0;
            t.weaponSwitching = info->numberOr("weaponswitching", 0) != 0;
            t.onOffable = info->numberOr("onoffable", 0) != 0;
            t.activateWhenBuilt = info->numberOr("activatewhenbuilt", 1) != 0;
            t.cantBeStoned = info->numberOr("cantbestoned", 0) != 0;
            t.cantBeFrozen = info->numberOr("cantbefrozen", 0) != 0;
            // Retail forces both on Monarchs (icd: def bit18 -> cantbestoned +
            // cantbefrozen) -- else petrify would be an instant Monarch kill.
            if (t.commander) t.cantBeStoned = t.cantBeFrozen = true;
            t.cantBeCaptured = info->numberOr("cantbecaptured", 0) != 0;
            t.cantBeTransported = info->numberOr("cantbetransported", 0) != 0;
            t.transportSize = int(info->numberOr("transportsize", 1));
            {   // bloodcolor1 = "r g b"
                std::string bc = info->valueOr("bloodcolor1", "");
                int r = 150, g = 30, b = 10;
                if (std::sscanf(bc.c_str(), "%d %d %d", &r, &g, &b) >= 1) {
                    t.blood[0] = uint8_t(std::clamp(r, 0, 255));
                    t.blood[1] = uint8_t(std::clamp(g, 0, 255));
                    t.blood[2] = uint8_t(std::clamp(b, 0, 255));
                }
            }
            t.canFly = info->numberOr("canfly", 0) != 0;
            std::string mc = lower(info->valueOr("movementclass", ""));
            if (mc.rfind("water", 0) == 0) t.domain = UnitType::Domain::Water;
            else if (mc.rfind("hover", 0) == 0) t.domain = UnitType::Domain::Hover;
            // Inherit terrain limits from the MOVEINFO movement class (the real
            // source of slope/water limits; most FBIs don't carry their own).
            if (auto mci = moveClasses_.find(mc); mci != moveClasses_.end()) {
                t.maxSlope = mci->second.maxSlope;
                t.maxWaterSlope = mci->second.maxWaterSlope;
                t.maxWaterDepth = mci->second.maxWaterDepth;
                t.minWaterDepth = mci->second.minWaterDepth;
                // The movement class WINS over the FBI, which is retail's precedence
                // and is safe here: no shipped unit declares both.
                if (mci->second.footX > 0) t.footX = mci->second.footX;
                if (mci->second.footZ > 0) t.footZ = mci->second.footZ;
            }
            // RAW, no divisor. Retail adds cruisealt straight onto the terrain
            // height byte to get the flyer's world Y (icd 0x4e42ee:
            // `eax = groundHeight + cruisealt`, then <<16 into the unit's Y),
            // so it is in the same units as the heightmap -- and our terrain
            // lift already matches retail's height/2. The `/ 4` sitting here
            // was an undocumented render fudge with no other reader (nothing in
            // the sim consumes cruiseAlt), and it put flyers at a quarter of
            // their height: 18.75px of lift for a cruisealt of 150 where retail
            // gives 75. It also pulled every flyer's shadow four times too close,
            // since the shadow offset is alt/4 off the same number.
            t.cruiseAlt = float(info->numberOr("cruisealt", 0));
            t.bankScale = float(info->numberOr("bankscale", 0));
            t.canSetStance = int(info->numberOr("unitstandorders", 1)) != 0;
            {
                std::string dmt = lower(info->valueOr("defaultmissiontype", ""));
                t.wanders = dmt == "standby_wander";
                t.vtolStandby = dmt == "vtol_standby";
            }
            t.pitchScale = float(info->numberOr("pitchscale", 0));
            // One weapon block -> a Weapon. Shared by WEAPON1..3 and by
            // [EXPLODEAS] (the death blast), which is the same block shape.
            auto parseWeapon = [](const tdf::Node* w) {
                Weapon wp;
                wp.name = w->valueOr("name", "");
                wp.range = float(w->numberOr("range", 0));
                wp.reload = float(w->numberOr("reloadtime", 1));
                wp.projVel = float(w->numberOr("weaponvelocity", 0));
                // See Weapon::subSteps. Integer ceil: no shipped weaponvelocity is a
                // multiple of 480, so this is bit-identical to retail's fixed-point
                // chain on every shipped weapon (a mod landing exactly on 480n would
                // differ by one sub-step).
                wp.subSteps = wp.projVel > 0.0f
                                  ? int((wp.projVel + 479.0f) / 480.0f) : 1;
                if (wp.subSteps < 1) wp.subSteps = 1;
                wp.noLead = w->numberOr("dontleadtargets", 0) != 0;
                wp.gravityAdj = float(w->numberOr("gravityadjustment", 1.0));
                wp.lobPreferred = w->numberOr("lobpreferred", 0) != 0;
                wp.melee = lower(w->valueOr("type", "")) == "melee";
                // Visual family: FBI hweffect is authoritative (it names the hit
                // effect); fall back to subtype/damagetype/name when it is absent.
                std::string hwe = lower(w->valueOr("hweffect", ""));
                std::string wtag = hwe + " " +
                                   lower(w->valueOr("subtype", "")) + " " +
                                   lower(w->valueOr("damagetype", "")) + " " +
                                   lower(wp.name);
                if (wtag.find("lightning") != std::string::npos)
                    wp.fx = WeaponFx::Lightning;
                else if (wtag.find("fire") != std::string::npos ||
                         wtag.find("flame") != std::string::npos ||
                         wtag.find("breath") != std::string::npos)
                    wp.fx = WeaponFx::Fire;
                // FBI weapon type = "Line of Sight" is a sustained hitscan beam
                // (the Gold Dragon's Fire Breath: range 600, emittime 45). It is
                // NOT a lobbed projectile -- earlier code capped every fire weapon's
                // range to 170px on the false premise that a breath is a short
                // emission, which forced the drake to dive to point-blank and spit a
                // single slow comet. Use the FBI range and drive the flame from
                // emittime instead. (emittime is in 30Hz frames.)
                wp.beam = lower(w->valueOr("type", "")) == "line of sight";
                wp.emitTime = float(w->numberOr("emittime", 0)) / 30.0f;
                // The remaining retail weapon classes. `type` is authoritative;
                // subtype adds the mind-control behaviour on top of either a
                // line-of-sight shot (Individual) or a Remote Effect (Area).
                {
                    std::string ty = lower(w->valueOr("type", ""));
                    if (ty == "guided") wp.kind = Weapon::Kind::Guided;
                    else if (ty == "remote effect") wp.kind = Weapon::Kind::Remote;
                    else if (ty == "wandering") wp.kind = Weapon::Kind::Wandering;
                    // turnrate is in degrees/second (a guided shot at 180 flips its
                    // heading in a second, which matches the shipped 120..600 range
                    // against 250-1200 px/s speeds).
                    wp.turnRate = float(w->numberOr("turnrate", 0)) * (kPi / 180.0f);
                    wp.buildUp = float(w->numberOr("builduptime", 0));
                    wp.decay = float(w->numberOr("decaytime", 0));
                    wp.duration = float(w->numberOr("duration", 0));
                    // maxvariation is NOT an angle: it is the wander jitter's
                    // half-width in PIXELS PER TICK (2..8 across the shipped
                    // storms, which dwarfs their 45..80 px/s drift -- that is what
                    // makes the path genuinely wander rather than curve).
                    wp.maxVariation = float(w->numberOr("maxvariation", 0));
                    wp.variationTime = float(w->numberOr("variationtime", 0));
                    wp.unitsOnly = w->numberOr("unitsonly", 0) != 0;
                    wp.particlesPerSec = float(w->numberOr("particlespersecond", 0));
                    std::string st = lower(w->valueOr("subtype", ""));
                    wp.mindControl = st == "mindcontrol";
                    // Which Remote Effect subclass this is (retail dispatches on
                    // subtype); it decides the damage cadence, not just the visuals.
                    // subtype=Dropped rides on a Ballistic weapon but is its own
                    // retail class: the bomb is let go, not launched.
                    if (st == "dropped") wp.kind = Weapon::Kind::Dropped;
                    if (st == "earthquake") wp.remote = Weapon::RemoteKind::Earthquake;
                    else if (st == "hailstorm") wp.remote = Weapon::RemoteKind::Hailstorm;
                    else if (st == "mindcontrol") wp.remote = Weapon::RemoteKind::MindCtl;
                    else if (st == "turntofrozen") wp.remote = Weapon::RemoteKind::Freeze;
                }
                // aimtolerance is in COB angle units; convert to radians. Ballistic
                // weapons lob an arc (viewer draws it); soundhitclass = impact sound.
                wp.aimTol = float(w->numberOr("aimtolerance",
                                  w->numberOr("aimtolerence", 1024))) * float(kCobAngle);
                wp.ballistic = lower(w->valueOr("type", "")) == "ballistic";
                wp.soundHit = lower(w->valueOr("soundhitclass",
                                               w->valueOr("soundhit", "")));
                // Projectile art (display only).
                wp.weaponArt = lower(w->valueOr("weaponart", ""));
                wp.shotModel = lower(w->valueOr("model", ""));
                if (auto dot = wp.shotModel.rfind(".3do"); dot != std::string::npos)
                    wp.shotModel.erase(dot);          // some FBIs spell the extension
                wp.nimbus = w->numberOr("nimbus", 0) != 0;
                {
                    // innercolor/middlecolor/outercolor are "R G B" triples that
                    // tint a lightning bolt's core, body and halo.
                    auto rgb = [&](const char* key, uint8_t out[3]) {
                        const std::string* v = w->value(key);
                        if (!v) return false;
                        int c[3] = {255, 255, 255};
                        std::sscanf(v->c_str(), "%d %d %d", &c[0], &c[1], &c[2]);
                        for (int i = 0; i < 3; ++i)
                            out[i] = uint8_t(std::clamp(c[i], 0, 255));
                        return true;
                    };
                    bool a = rgb("innercolor", wp.inner);
                    bool b = rgb("middlecolor", wp.middle);
                    bool c = rgb("outercolor", wp.outer);
                    wp.hasBoltColor = a || b || c;
                }
                // spinheading is in COB angle units per second.
                wp.spinRate = float(w->numberOr("spinheading", 0)) * float(kCobAngle);
                // shadowgaf is always "shadows"; shadowart names the sequence in it.
                wp.shadowArt = lower(w->valueOr("shadowart", ""));
                wp.shotArt = lower(w->valueOr("shotart", ""));
                {
                    std::string lm = lower(w->valueOr("lightmap", ""));
                    wp.lightMap = lm == "small" ? 1 : lm == "medium" ? 2
                                : lm == "large" ? 3 : 0;
                }
                wp.wanderStart = lower(w->valueOr("wanderstartart", ""));
                wp.wanderLoop = lower(w->valueOr("wanderloopart", ""));
                wp.wanderEnd = lower(w->valueOr("wanderendart", ""));
                wp.explosionClass = lower(w->valueOr("explosionclass", ""));
                wp.waterExplosionClass = lower(w->valueOr("waterexplosionclass", ""));
                wp.radiusArt[0] = lower(w->valueOr("radiusart0", ""));
                wp.radiusArt[1] = lower(w->valueOr("radiusart1", ""));
                wp.radiusArt[2] = lower(w->valueOr("radiusart2", ""));
                wp.ringCount = int(w->numberOr("ringcount", 0));
                wp.ringDelay = float(w->numberOr("ringdelay", 0.2));
                wp.ringDur = float(w->numberOr("ringduration", 1.0));
                wp.spriteCount = int(w->numberOr("spritecount", 24));
                wp.shakeMag = float(w->numberOr("shakemagnitude", 0));
                wp.shakeDur = float(w->numberOr("shakeduration", 0));
                wp.fireStarter = w->numberOr("firestarter", 0) != 0;
                {
                    std::string dtp = lower(w->valueOr("damagetype", "normal"));
                    wp.dmgType = dtp == "fire" ? 2 : dtp == "explosion" ? 3
                               : dtp == "paralyzer" ? 4 : 1;
                }
                wp.minRange = float(w->numberOr("minrange", 0));
                wp.noAir = w->numberOr("noairweapon", 0) != 0;
                wp.manaCost = float(w->numberOr("manapershot", 0));
                // Status-effect weapons (Creon freeze, medusa/paralyzer, petrify),
                // inferred from the hit-effect / damagetype / name.
                {
                    // Freeze and petrify come from the SUBTYPE, never from the name.
                    // Retail selects them by subtype=turntofrozen/turntostone (6
                    // weapons, all named accordingly), and a name-substring
                    // heuristic also swept up the three HAILSTORMS -- "Hail Shower",
                    // "Ice Storm", "Ice Storms" -- which do not freeze. Because our
                    // freeze is an instant statue death, that quietly turned the
                    // Acolyte's 70-damage Hail Shower into a 200-radius instant kill.
                    std::string st = lower(w->valueOr("subtype", ""));
                    std::string s = hwe + " " + lower(w->valueOr("damagetype", "")) +
                                    " " + lower(w->valueOr("soundhitclass", "")) + " " +
                                    lower(wp.name);
                    if (st == "turntofrozen") wp.status = Weapon::Status::Frozen;
                    else if (st == "turntostone") wp.status = Weapon::Status::Stoned;
                    else if (s.find("paraly") != std::string::npos)
                        wp.status = Weapon::Status::Paralyzed;
                    if (wp.status != Weapon::Status::None)
                        wp.statusDur = float(w->numberOr("duration", 5.0));
                }
                wp.aoe = float(w->numberOr("areaofeffect", 0));
                // The FBI data misspells this key both ways; accept either.
                wp.edge = float(w->numberOr("edgeeffectiveness",
                                w->numberOr("edgeeffectivness", 1.0)));
                if (const auto* dmg = w->child("DAMAGE")) {
                    wp.damage = float(dmg->numberOr("default", 0));
                    // Per-target-category damage (every DAMAGE key but `default`).
                    for (const auto& [k, v] : dmg->values)
                        if (k != "default")
                            wp.dmgVs[k] = float(std::atof(v.c_str()));
                }
                return wp;
            };
            for (int slot = 1; slot <= 3; ++slot) {
                const auto* w = root.child("WEAPON" + std::to_string(slot));
                if (!w) continue;
                Weapon wp = parseWeapon(w);
                if (wp.damage > 0) t.weapons.push_back(wp);
            }
            if (!t.weapons.empty()) t.weapon = t.weapons[0];
            // [EXPLODEAS]: the weapon a unit detonates at its own position when it
            // dies -- the Kamikaze Rat's whole purpose (areaofeffect 203, 8000
            // damage), plus the Grenadier, Fire Demon, Balloon, crebomb, creshoc.
            if (const auto* ea = root.child("EXPLODEAS")) {
                t.explodeAs = parseWeapon(ea);
                t.hasExplodeAs = t.explodeAs.damage > 0;
            }
            // totalallowed: per-player cap on live units of this type (the five
            // dragons, the five gods and the Aerial Juggernaut all carry 1).
            t.totalAllowed = int(info->numberOr("totalallowed", 0));
            // Stat auras: [AdjustArmor]/[AdjustAttack]/[AdjustJoy] blocks under
            // UNITINFO make the unit a continuous buff/debuff field on nearby units.
            {
                struct AK { const char* tag; Aura::Kind kind; };
                for (const AK& ak : {AK{"AdjustArmor", Aura::Kind::Armor},
                                     AK{"AdjustAttack", Aura::Kind::Attack},
                                     AK{"AdjustJoy", Aura::Kind::Joy}}) {
                    if (const auto* a = info->child(ak.tag)) {
                        Aura au;
                        au.kind = ak.kind;
                        au.amount = float(a->numberOr("adjustment", 1));
                        au.affectsEnemy = a->numberOr("affectsenemy", 0) != 0;
                        au.radius = float(a->numberOr("radius", 200));
                        au.edge = float(a->numberOr("edgeeffectiveness", 1));
                        t.auras.push_back(au);
                    }
                }
            }
            // Target-category tokens for weapons' per-category damage: split
            // `category`, plus `damagecategory` and `tedclass`, all lowercased.
            {
                std::string cat = lower(info->valueOr("category", "")) + " " +
                                  lower(info->valueOr("damagecategory", "")) + " " +
                                  lower(info->valueOr("tedclass", ""));
                std::string tok;
                for (char c : cat + " ") {
                    if (c == ' ' || c == '\t') {
                        if (!tok.empty()) { t.categories.push_back(tok); tok.clear(); }
                    } else tok.push_back(c);
                }
            }
            types_[t.id] = std::move(t);
        } catch (const std::exception&) {
            // Skip malformed definitions rather than fail the registry.
        }
    }
    // Fix up cross-type references now that every unit is in the table
    // (animatetype=MONGHOUL names another unit the animator raises).
    for (auto& [id, t] : types_)
        if (!t.animName_.empty())
            if (auto it = types_.find(t.animName_); it != types_.end())
                t.animateType = &it->second;
}

void TypeRegistry::loadBuildTree(const hpi::Vfs& vfs, const std::string& prefix) {
    namespace fs = std::filesystem;
    // The VFS enumerates <prefix>/<builder>/<buildable>.tdf recursively; group by
    // the builder subdirectory (the path segment right after `prefix`).
    std::string pre = lower(prefix);
    while (!pre.empty() && (pre.back() == '/' || pre.back() == '\\')) pre.pop_back();
    std::map<std::string, std::vector<std::pair<double, std::string>>> byBuilder;
    std::vector<std::string> order;   // builders in first-seen order
    for (const std::string& path : vfs.list(prefix)) {
        if (lower(fs::path(path).extension().string()) != ".tdf") continue;
        // Split into segments; require exactly <prefix>/<builder>/<file>.
        std::vector<std::string> segs;
        for (const auto& part : fs::path(path)) {
            std::string s = part.string();
            if (!s.empty() && s != "/") segs.push_back(s);
        }
        size_t i = 0;
        while (i < segs.size() && lower(segs[i]) != pre) ++i;
        if (i + 3 != segs.size()) continue;   // require exactly <prefix>/<builder>/<file>
        std::string builder = lower(segs[i + 1]);
        double prio = 99;
        try {
            auto bytes = vfs.read(path);
            auto root = tdf::parseText(std::string(bytes.begin(), bytes.end()), path);
            if (const auto* m = root.child("Menu")) prio = m->numberOr("priority", 99);
        } catch (const std::exception&) {}
        if (!byBuilder.count(builder)) order.push_back(builder);
        byBuilder[builder].push_back({prio, lower(fs::path(path).stem().string())});
    }
    for (const std::string& builder : order) {
        auto entries = byBuilder[builder];
        std::sort(entries.begin(), entries.end());
        auto& list = buildTree_[builder];
        // First source to define a builder wins it whole -- so a Crusades-balance
        // canbuildcb loaded before canbuild replaces that builder's menu rather
        // than merging with it. (No effect on the usual single load.)
        if (!list.empty()) continue;
        for (auto& [p, id] : entries)
            if (std::find(list.begin(), list.end(), id) == list.end())
                list.push_back(id);
    }
}

const std::vector<std::string>& TypeRegistry::buildable(const std::string& builderId) const {
    static const std::vector<std::string> kEmpty;
    auto it = buildTree_.find(lower(builderId));
    return it == buildTree_.end() ? kEmpty : it->second;
}

const UnitType* TypeRegistry::find(const std::string& id) const {
    auto it = types_.find(lower(id));
    return it == types_.end() ? nullptr : &it->second;
}

std::vector<const UnitType*> TypeRegistry::combatUnits(const std::string& side) const {
    std::vector<const UnitType*> out;
    // types_ is a std::map keyed by lowercased id, so this walk is name-sorted and
    // identical on every peer -> a deterministic stress-test roster.
    for (const auto& [id, t] : types_) {
        if (t.side != side) continue;
        if (!t.canMove || t.isBuilder || t.isStructure() || t.commander) continue;
        if (t.weapons.empty()) continue;   // must be able to fight (drives real load)
        out.push_back(&t);
    }
    return out;
}

int World::spawn(const UnitType* type, float x, float z, float heading, int player) {
    Unit u;
    u.id = nextId_++;
    if (type) {                      // the type's standing orders are the unit's
        u.moveState = type->defaultMove;
        u.fireState = type->defaultFire;
        // The displayed stance is whichever button retail would light up.
        u.stance = u.fireState == 0 ? 2 : (u.moveState == 0 ? 1 : 0);
    }
    // Clamp to a valid player slot: every players_[u.player] index downstream is
    // unchecked, so an out-of-range owner would read/write past the vector.
    if (player < 0 || player >= int(players_.size())) {
        std::fprintf(stderr, "sim: spawn player %d out of range [0,%d) -> clamped to 0\n",
                     player, int(players_.size()));
        player = 0;
    }
    players_[size_t(player)].unitCount++;   // keep the cap count exact within a tick
    players_[size_t(player)].built++;        // end-of-game "Units" column
    u.player = player;
    u.type = type;
    u.x = x;
    u.z = z;
    u.heading = heading;
    u.hp = type ? type->maxHp : 100;
    u.mana = type ? type->maxMana : 0;   // casters start with a full pool
    u.active = type ? type->activateWhenBuilt : true;
    u.homeX = x;
    u.homeZ = z;
    units_.push_back(u);
    return u.id;
}

Unit* World::unit(int id) {
    // units_ is append-only with sequential ids (u.id = nextId_++ then push_back,
    // never erased), so id == index + 1 -- an O(1) lookup. This is called per unit
    // every tick in combat (unit(targetId)); the old linear scan made a big battle
    // O(n^2) and stalled the sim to single-digit fps. The scan stays as a fallback
    // in case that invariant is ever broken.
    if (id >= 1 && size_t(id) <= units_.size()) {
        Unit& u = units_[size_t(id) - 1];
        if (u.id == id) return &u;
    }
    for (auto& u : units_)
        if (u.id == id) return &u;
    return nullptr;
}

void World::setTerrain(const std::vector<uint8_t>& heights, int w, int h, int seaLevel,
                       const std::vector<uint16_t>* features) {
    // The fog worker reads heights_; never let a reload pull the map out from under it.
    if (visRunning_) { visWorker_.join(); visRunning_ = false; visDone_.store(false); }
    // EVERY cached LoS mask is a function of the terrain we are about to replace, and
    // its contents are flat cell indices computed against the OLD visW_/visH_. Keeping
    // them across a map change is not merely stale fog: load a smaller map and those
    // indices address past the end of visBack_. The cache key is (sight, radar, cx, cz)
    // with no terrain identity in it, so nothing else would ever invalidate them.
    visMaskCache_.clear();
    visHavePass_ = false;   // the next pass is a first pass again (see updateVisibility)
    seaLevel_ = seaLevel;
    heights_ = heights;   // keep raw heights for fog line-of-sight
    hW_ = w; hH_ = h;
    // Road cells: the retail map format marks them as 0xFFFB in the feature
    // plane (verified against Two Castles -- the mask traces the road network).
    roads_.clear();
    if (features && features->size() == size_t(w) * size_t(h)) {
        bool any = false;
        roads_.assign(size_t(w) * size_t(h), 0);
        for (size_t i = 0; i < roads_.size(); ++i)
            if ((*features)[i] == 0xFFFB) { roads_[i] = 1; any = true; }
        if (!any) roads_.clear();   // no roads on this map: skip the per-tick test
    }
    // Passability, retail's rule (see NavGrid::Limits). One grid per DOMAIN still,
    // with each domain's limits taken from the movement class that dominates it in
    // the shipped data: GROUND2/GROUND3 (67+20 of the 125 movers) for ground,
    // WATER* for water, HOVER2 for hover. Per-CLASS grids -- so a Catapult's
    // MaxSlope 15 differs from a Swordsman's 30 -- are the next step; this leg is
    // about the rule's SHAPE, which is where the connectivity was being lost.
    {
        NavGrid::Limits ground;      // GROUND2 / GROUND3
        ground.maxSlope = 30; ground.maxWaterSlope = 30;
        ground.maxWaterDepth = 20; ground.minWaterDepth = -10000;
        nav_ = NavGrid(heights, w, h, seaLevel, ground);

        NavGrid::Limits water;       // WATER* declare no limits: retail's defaults,
        water.maxSlope = 255; water.maxWaterSlope = 255;   // except they must be IN
        water.maxWaterDepth = 10000; water.minWaterDepth = 13;   // water to float.
        navWater_ = NavGrid(heights, w, h, seaLevel, water);

        NavGrid::Limits hover;       // HOVER2: crosses land and water alike
        hover.maxSlope = 30; hover.maxWaterSlope = 255;
        hover.maxWaterDepth = 10000; hover.minWaterDepth = -10000;
        navHover_ = NavGrid(heights, w, h, seaLevel, hover);
        // One obstacle overlay, shared by every grid. Buildings, blocking features
        // and wrecks go here rather than into a single grid's cells.
        obst_.assign(size_t(w) * size_t(h), 0);
        nav_.setObstacles(&obst_);
        navWater_.setObstacles(&obst_);
        navHover_.setObstacles(&obst_);
        navClasses_.clear();
        navIdx_.clear();
    }
    // Occlusion block: RETAIL NEVER LETS A UNIT STAND BEHIND TERRAIN. A tall
    // cell's painted face leans up-screen over the lower ground to its north, so
    // a unit stopping there would be drawn inside the rock; retail keeps it out
    // of those cells rather than fixing the draw order. Block them.
    //
    // This pass existed before, was deleted this morning for blocking 12% of the
    // map and stranding armies (2 of 24 arriving), and was right all along -- it
    // was the PROJECTION CONSTANT that was wrong. It used 1.1 px of northward
    // lean per height unit; retail's figure is 0.5, proven off 0x511140 and
    // 0x426820 (screenY = z*16 - camY - height/2, and no X term at all). At more
    // than twice the real lean it condemned more than twice the ground.
    //
    // Into the SHARED overlay, so the per-class grids see it -- the legacy grids
    // are not what navFor() hands a unit in a real match.
    {
        long hist[256] = {0};
        for (uint8_t v : heights) hist[v]++;
        int ref = 0;
        for (int i = 1; i < 256; ++i) if (hist[i] > hist[ref]) ref = i;
        const float kProj = 0.5f;          // retail's lift, not the old 1.1
        for (int z = 0; z < h; ++z)
            for (int x = 0; x < w; ++x) {
                int hu = heights[size_t(z) * w + x];
                for (int d = 1; d <= 7; ++d) {
                    int nz = z + d;
                    if (nz >= h) break;
                    int hw = heights[size_t(nz) * w + x];
                    if (hw <= hu + 24) continue;   // not a wall relative to this cell
                    if (float(hw - ref) * kProj > float(d) * 16.0f) {
                        obst_[size_t(z) * size_t(w) + size_t(x)] = 1;
                        break;
                    }
                }
            }
    }
    nav_.setRoads(&roads_);   // ground domain only: marches prefer highways
    // 0xFFFC cells are hard blockers (retail rates them impassable for every
    // movement domain AND rejects building placement on them -- icd 0x508190 /
    // 0x507705; they sit under castle wall/gate/dock art baked into terrain).
    if (features && features->size() == size_t(w) * size_t(h))
        for (int z = 0; z < h; ++z)
            for (int x = 0; x < w; ++x)
                if ((*features)[size_t(z) * w + x] == 0xFFFC)
                    // The SHARED overlay, not each grid's own cells. block() writes
                    // cells_, and buildNavClasses() -- which runs after setTerrain and
                    // builds the grids navFor() actually hands every unit in a real
                    // match -- constructs its grids from heights alone. Stamping the
                    // legacy grids published this to nobody from 9b037c6 onward, and
                    // these are the cells under castle wall, gate and dock art: units
                    // walked straight through them. 2538 cells on Inner Circle.
                    obst_[size_t(z) * size_t(w) + size_t(x)] = 1;
    // The overlay changed after setObstacles(), so clearance derived from it is
    // stale. The per-class grids are built after this point and start clean.
    nav_.markClearanceDirty();
    navWater_.markClearanceDirty();
    navHover_.markClearanceDirty();
    // Per-cell slope (max 3x3 height spread) and water depth, for per-unit
    // maxSlope / maxWaterDepth checks on top of the shared domain grids.
    terW_ = w; terH_ = h;
    slope_.assign(size_t(w) * h, 0);
    depth_.assign(size_t(w) * h, 0);
    for (int z = 0; z < h; ++z)
        for (int x = 0; x < w; ++x) {
            int lo = 255, hi = 0;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = std::clamp(x + dx, 0, w - 1);
                    int nz = std::clamp(z + dz, 0, h - 1);
                    int v = heights[size_t(nz) * w + nx];
                    lo = std::min(lo, v); hi = std::max(hi, v);
                }
            slope_[size_t(z) * w + x] = uint8_t(std::min(255, hi - lo));
            int d = seaLevel - int(heights[size_t(z) * w + x]);
            depth_[size_t(z) * w + x] = uint8_t(std::clamp(d, 0, 255));
        }
    // Fog starts FULLY UNEXPLORED the moment terrain exists: before this, vis_ was
    // allocated lazily by the first updateVisibility (0.25s into the sim), and an
    // empty vis_ makes drawFog skip and cellVisible() report everything visible --
    // so the first rendered frames flashed the whole bare map (masked historically
    // by the synchronous load stall; exposed once terrain composited async). Local
    // display only (never hashed); the headless referee (visPlayer_ < 0) skips fog
    // entirely. Sized to THIS terrain, so loading a different map into the same
    // world also re-fits the grid instead of keeping stale dimensions.
    if (visPlayer_ >= 0) {
        visW_ = w;
        visH_ = h;
        vis_.assign(size_t(w) * size_t(h), 0);
        ++visGen_;   // the renderer re-uploads the (all-fogged) texture
    }
}

void blockFootprint(NavGrid& nav, const UnitType& t, float x, float z, bool blocked) {
    int cx = int(x) / 16 - t.footX / 2, cz = int(z) / 16 - t.footZ / 2;
    if (t.yardMap.empty()) {
        nav.block(cx, cz, t.footX, t.footZ, blocked);
        return;
    }
    for (int j = 0; j < t.footZ; ++j)
        for (int i = 0; i < t.footX; ++i) {
            char c = t.yardMap[size_t(j) * t.footX + i];
            if (c == 'o' || c == 'O')
                nav.block(cx + i, cz + j, 1, 1, blocked);
        }
}




void NavGrid::block(int cx, int cz, int w, int h, bool blocked) {
    for (int z = cz; z < cz + h; ++z)
        for (int x = cx; x < cx + w; ++x)
            if (x >= 0 && z >= 0 && x < w_ && z < h_)
                cells_[size_t(z) * w_ + x] = blocked ? 0 : 1;
    ++version_;   // walkability changed: component caches over this grid are stale
    // Footprint clearance depends on the walkability grid. When it has already been
    // built, refresh just the affected rect (values are clamped at kClearMax, so a
    // cell change can influence clearance at most kClearMax cells down-left) instead
    // of dirtying the whole map -- a full rebuild is a w*h DP that every building
    // placement used to re-trigger on the next fits() query.
    if (!clearDirty_ && !clear_.empty())
        updateClearanceRect(cx, cz, w, h);
    else
        clearDirty_ = true;
}

// Stored clearance saturates here. fits() only ever asks about footprints <= 15
// cells (footCells clamps there), so every value >= 15 answers identically -- and
// the clamp is what bounds how far a walkability edit can propagate (see block()).
static constexpr uint16_t kClearMax = 15;

// Largest all-walkable square with each cell as its min (bottom-left) corner, by the
// classic DP from the far corner inward, saturated at kClearMax. Deterministic
// (pure function of cells_).
void NavGrid::rebuildClearance() const {
    clear_.assign(size_t(w_) * size_t(h_), 0);
    for (int z = h_ - 1; z >= 0; --z)
        for (int x = w_ - 1; x >= 0; --x) {
            if (!walkable(x, z)) continue;   // blocked (cells_ or the overlay) -> 0
            uint16_t r  = (x + 1 < w_)               ? clear_[size_t(z) * w_ + x + 1]     : 0;
            uint16_t u  = (z + 1 < h_)               ? clear_[size_t(z + 1) * w_ + x]     : 0;
            uint16_t ru = (x + 1 < w_ && z + 1 < h_) ? clear_[size_t(z + 1) * w_ + x + 1] : 0;
            clear_[size_t(z) * w_ + x] =
                std::min<uint16_t>(kClearMax, uint16_t(1 + std::min({r, u, ru})));
        }
    clearDirty_ = false;
}

// Recompute the clearance DP over just the cells a walkability edit in the given
// rect can influence: the rect itself plus kClearMax cells down-left (the DP reads
// up-right neighbours, and saturation stops the influence beyond that band). Same
// order and arithmetic as the full rebuild, so the result is bit-identical to it.
void NavGrid::updateClearanceRect(int cx, int cz, int w, int h) const {
    int x0 = std::max(0, cx - int(kClearMax));
    int z0 = std::max(0, cz - int(kClearMax));
    int x1 = std::min(w_ - 1, cx + w - 1);
    int z1 = std::min(h_ - 1, cz + h - 1);
    for (int z = z1; z >= z0; --z)
        for (int x = x1; x >= x0; --x) {
            uint16_t v = 0;
            if (walkable(x, z)) {
                uint16_t r  = (x + 1 < w_)               ? clear_[size_t(z) * w_ + x + 1]     : 0;
                uint16_t u  = (z + 1 < h_)               ? clear_[size_t(z + 1) * w_ + x]     : 0;
                uint16_t ru = (x + 1 < w_ && z + 1 < h_) ? clear_[size_t(z + 1) * w_ + x + 1] : 0;
                v = std::min<uint16_t>(kClearMax, uint16_t(1 + std::min({r, u, ru})));
            }
            clear_[size_t(z) * w_ + x] = v;
        }
}

bool NavGrid::fits(int cx, int cz, int foot) const {
    if (foot <= 1) return walkable(cx, cz);
    if (clearDirty_) rebuildClearance();
    int off = foot / 2;
    int bx = cx - off, bz = cz - off;   // min corner of the foot x foot block
    if (bx < 0 || bz < 0 || bx + foot > w_ || bz + foot > h_) return false;
    return clear_[size_t(bz) * w_ + size_t(bx)] >= foot;
}

NavGrid::NavGrid(const std::vector<uint8_t>& heights, int w, int h, int cliff)
    : w_(w), h_(h) {
    cells_.assign(size_t(w) * h, 1);
    for (int z = 0; z < h; ++z)
        for (int x = 0; x < w; ++x) {
            // Block a cell only if a NEIGHBOUR rises more than `cliff` above it (a
            // face you can't climb). A cell that merely sits beside a DROP -- the
            // flat top edge of a cliff/plateau/wall -- stays walkable, so units can
            // be commanded right to the rim, not just the interior. (Using the local
            // max-min spread instead wrongly blocked the whole edge ring of every
            // plateau, leaving only its middle reachable.)
            int self = heights[size_t(z) * w + x];
            int hi = self;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = std::clamp(x + dx, 0, w - 1);
                    int nz = std::clamp(z + dz, 0, h - 1);
                    hi = std::max(hi, int(heights[size_t(nz) * w + nx]));
                }
            if (hi - self > cliff) cells_[size_t(z) * w + x] = 0;
        }
}

NavGrid::NavGrid(const std::vector<uint8_t>& heights, int w, int h, int sea,
                 const Limits& lim)
    : w_(w), h_(h) {
    cells_.assign(size_t(w) * h, 1);
    auto H = [&](int x, int z) {
        return int(heights[size_t(std::min(z, h - 1)) * size_t(w) + size_t(std::min(x, w - 1))]);
    };
    for (int z = 0; z < h; ++z)
        for (int x = 0; x < w; ++x) {
            // The cell's own quad, anchored down-right -- retail's exact sample set.
            int a = H(x, z), b = H(x + 1, z), c = H(x, z + 1), d = H(x + 1, z + 1);
            int lo = std::min(std::min(a, b), std::min(c, d));
            int hi = std::max(std::max(a, b), std::max(c, d));
            bool blocked = false;
            if (lo < sea - lim.maxWaterDepth) blocked = true;          // too deep
            else if (hi > sea - lim.minWaterDepth) blocked = true;     // too shallow
            else if (hi - lo > (lo < sea ? lim.maxWaterSlope : lim.maxSlope)) blocked = true;
            if (blocked) cells_[size_t(z) * w + x] = 0;
        }
}

bool NavGrid::lineClear(int x0, int z0, int x1, int z1) const {
    int dx = std::abs(x1 - x0), dz = -std::abs(z1 - z0);
    int sx = x0 < x1 ? 1 : -1, sz = z0 < z1 ? 1 : -1, err = dx + dz;
    while (true) {
        if (!walkable(x0, z0)) return false;
        if (x0 == x1 && z0 == z1) return true;
        int e2 = 2 * err;
        if (e2 >= dz) { err += dz; x0 += sx; }
        if (e2 <= dx) { err += dx; z0 += sz; }
    }
}

bool NavGrid::lineFits(int x0, int z0, int x1, int z1, int foot) const {
    // Like lineClear, but asks whether a body of `foot` cells FITS at every cell the
    // line crosses -- and, on a diagonal step, at both cells it passes between, so a
    // shortcut can never squeeze a body through a corner the mover would be stopped by
    // (the movers themselves refuse diagonal corner-cutting; see the step loop in the
    // search). Integer Bresenham, so it is exact and identical on every peer.
    int dx = std::abs(x1 - x0), dz = -std::abs(z1 - z0);
    int sx = x0 < x1 ? 1 : -1, sz = z0 < z1 ? 1 : -1, err = dx + dz;
    for (int guard = 0; guard < 8192; ++guard) {
        if (!fits(x0, z0, foot)) return false;
        if (x0 == x1 && z0 == z1) return true;
        const int e2 = 2 * err;
        const bool stepX = e2 >= dz, stepZ = e2 <= dx;
        if (stepX && stepZ) {   // diagonal: both orthogonal neighbours must fit too
            if (!fits(x0 + sx, z0, foot) || !fits(x0, z0 + sz, foot)) return false;
        }
        if (stepX) { err += dz; x0 += sx; }
        if (stepZ) { err += dx; z0 += sz; }
    }
    return false;
}

bool NavGrid::losBetween(float wx0, float wz0, float wx1, float wz1,
                         int skip0, int skip1) const {
    if (empty()) return true;
    int ex0 = int(wx0) / 16, ez0 = int(wz0) / 16;
    int ex1 = int(wx1) / 16, ez1 = int(wz1) / 16;
    int x0 = ex0, z0 = ez0;
    int dx = std::abs(ex1 - x0), dz = -std::abs(ez1 - z0);
    int sx = x0 < ex1 ? 1 : -1, sz = z0 < ez1 ? 1 : -1, err = dx + dz;
    while (true) {
        if (x0 == ex1 && z0 == ez1) return true;   // reached the target cell
        int e2 = 2 * err;
        if (e2 >= dz) { err += dz; x0 += sx; }
        if (e2 <= dx) { err += dx; z0 += sz; }
        if (x0 == ex1 && z0 == ez1) return true;   // stepped onto the endpoint
        // Ignore cells inside either endpoint's own footprint (Chebyshev radius).
        if (std::max(std::abs(x0 - ex0), std::abs(z0 - ez0)) <= skip0) continue;
        if (std::max(std::abs(x0 - ex1), std::abs(z0 - ez1)) <= skip1) continue;
        if (!walkable(x0, z0)) return false;       // a wall stands between them
    }
}









// A unit's footprint size in cells (square approximation) for footprint-aware nav.
static int footCells(const UnitType* t) {
    return t ? std::clamp(std::max(t->footX, t->footZ), 1, 15) : 1;
}

void World::replaceLeg(Unit& u, const std::vector<Order>& path) {
    if (u.orders.empty() || path.empty()) return;
    const size_t end = currentLeg(u.orders);
    const Order tmpl = u.orders[end];        // keep attack-move / patrol / flags
    std::vector<Order> next;
    next.reserve(path.size() + u.orders.size() - end - 1);
    for (size_t i = 0; i < path.size(); ++i) {
        Order o = path[i];
        o.attackMove = tmpl.attackMove;
        o.patrol = tmpl.patrol;
        o.goal = (i + 1 == path.size());
        if (o.goal) { o.clickX = tmpl.clickX; o.clickZ = tmpl.clickZ; }
        next.push_back(o);
    }
    next.insert(next.end(), u.orders.begin() + long(end) + 1, u.orders.end());
    u.orders.swap(next);
    // KNOWN LIMITATION. This resets the no-headway tracker, and the pathfinder
    // re-routes the same leg about once a second, so the tracker cannot build up
    // while a unit is being re-routed -- which is why a unit circling between
    // routes to the same place is never noticed and never gives up. Preserving
    // the tracker when the destination is unchanged looks like the fix and is
    // not: units held up briefly in a crowd then accumulate across re-routes and
    // abandon perfectly good orders (it halves "two columns pass through each
    // other", 6 of 12 instead of 12). The real answer is probably a separate
    // per-destination timer that re-routing does not touch, rather than reusing
    // this one. See docs/retail-engine.md.
    u.goalStuckD = 1e30f;                    // new leg -> the tracker starts over
    u.goalStuckT = 0;
}

void World::dropLeg(Unit& u) {
    if (u.orders.empty()) return;
    const size_t end = currentLeg(u.orders);
    u.orders.erase(u.orders.begin(), u.orders.begin() + long(end) + 1);
    u.goalStuckD = 1e30f;
    u.goalStuckT = 0;
}

// A move/attack/patrol order aimed at a PRODUCTION BUILDING sets its rally instead:
// the building cannot go anywhere itself, but the units it makes can. Returns true
// when the order was consumed as a rally, so the caller stops there.
//
// The orders are stored raw and never enter the mover -- see Unit::rally. `queue`
// appends, so a player can lay out "move here, then fight-move there, then patrol"
// and every unit off the line follows the whole plan.
bool World::setRally(Unit& u, const Order& o, bool queue) {
    if (!u.type || !u.type->producesUnits()) return false;
    if (!queue) u.rally.clear();
    u.rally.push_back(o);
    u.rally.back().goal = true;          // each issued rally step is its own leg
    if (u.rally.size() > 32) u.rally.erase(u.rally.begin());   // bound it
    return true;
}

void World::order(int unitId, float x, float z, bool queue) {
    Unit* u = unit(unitId);
    // isStructure(), NOT canMove: the Keep and both Taros/Veruna walls declare
    // canmove=1 with no velocity (the CLAUDE.md gotcha). Gating on canMove let a
    // BUILDING accept a move order -- it could not go anywhere, but the mover still
    // turned its heading toward the goal, so you could spin a keep by right-clicking.
    if (!u || !u->alive() || !u->type) return;
    if (u->type->isStructure()) { setRally(*u, Order{x, z, 0}, queue); return; }
    if (!queue) u->orders.clear();
    auto markGoal = [&] {
        if (u->orders.empty()) return;
        u->orders.back().goal = true;
        u->orders.back().issuedTick = tickCounter_;
    };
    if (u->type->canFly) {
        u->orders.push_back({x, z, 0});
        markGoal();
        return;
    }
    // Ground/water units steer STRAIGHT at the front order's point. There is no local
    // obstacle avoidance in the steering: going around terrain is entirely the job of
    // the background path search, which splices its route in as further order legs.
    //
    // That search is retail's ported boundary tracer (sim/pathsearch.h), NOT an A* --
    // no open list, no heuristic; it marches at the goal and traces an obstacle's
    // outline from both sides when blocked. It is the sim's ONLY pathfinder: the
    // A* that used to route transports to their unload point resolved its grid with
    // the same navFor() and bought nothing over the tracer, so it is gone.
    //
    // This comment said "steer by a shared flow field ... fall back to A* waypoints",
    // and neither half survived: the flow fields are gone, and what replaced them is
    // not an A* either. Left stale it sends anyone debugging a unit that walks into a
    // cliff looking for two mechanisms that do not exist.
    // Snap the destination onto ground this unit can actually stand on. A click
    // on a mountain is a click on a cell no ground unit fits in: without this the
    // unit walks up to the cliff and shoves at it indefinitely, because the goal
    // is never reached and nothing declares it unreachable -- the reachability
    // test resolves an unstandable goal to the nearest walkable cell, which is
    // usually on the unit's OWN side, so it reports "reachable" quite correctly
    // and no watchdog fires. Retail behaves the way this makes us behave: ordered
    // at a mountain, the unit walks as close as it can get and stops.
    //
    // Deterministic: a fixed outward scan in a fixed order, and it only moves a
    // goal that was already impossible.
    const float clickX = x, clickZ = z;   // what the player aimed at
    {
        const NavGrid& g = navFor(u->type);
        const int foot = footCells(u->type);
        int cx = int(x) / 16, cz = int(z) / 16;
        if (!g.empty() && !g.fits(cx, cz, foot)) {
            bool found = false;
            for (int r = 1; r <= 24 && !found; ++r)
                for (int dz = -r; dz <= r && !found; ++dz)
                    for (int dx = -r; dx <= r && !found; ++dx) {
                        if (std::abs(dx) != r && std::abs(dz) != r) continue;
                        if (!g.fits(cx + dx, cz + dz, foot)) continue;
                        x = float(cx + dx) * 16.0f + 8.0f;
                        z = float(cz + dz) * 16.0f + 8.0f;
                        found = true;
                    }
        }
    }

    // Retail pushes the straight goal in and starts steering at it IMMEDIATELY,
    // while a path request runs in the background; when the search returns, the
    // real route replaces the straight segment (docs/retail-engine.md). So the
    // two-point segment is the interim, not the mechanism -- exactly what this
    // engine did permanently until the search was ported.
    u->orders.push_back({x, z, 0});
    u->orders.back().clickX = clickX;
    u->orders.back().clickZ = clickZ;
    markGoal();
    // Only ask for a route when this order is the one the unit is about to
    // WALK. Requests are keyed by unit id, so asking for a queued order cancels
    // the pending request for the leg in progress and then installs the queued
    // destination's route into that leg -- the unit sets off for the last thing
    // you queued and skips everything before it. Orders behind the current one
    // get their route when they become current, via the retry sweep in tick().
    if (!queue || u->orders.size() == 1) requestPath(*u, x, z);
}

// Queue a background path for `u` toward (x,z). Flyers ignore the ground, and a
// goal a couple of cells away is not worth a search -- the straight segment
// already covers it.
// Drop any path search still queued for this unit. Call it wherever a unit's order
// list is REPLACED wholesale and nothing immediately re-requests, because a search
// outlives the orders that asked for it: the PathService keys on the unit id, not on
// the order it came from.
//
// A stale route is not merely wasted work. When it lands, replaceLeg() rebuilds the
// current leg out of its waypoints -- and currentLeg() falls through to the LAST order
// when nothing carries `goal`, so for a bare command list the leg it rewrites is the
// command itself. replaceLeg carries only movement flags across, so `load` and `unload`
// are dropped: the transport quietly stops being a transport and drives off along a
// route to wherever it was going before. Both were reachable by changing your mind
// while a route was in flight.
void World::cancelPath(Unit& u) {
    paths_.cancel(u.id);
    pathRetryAt_.erase(u.id);
}

void World::requestPath(Unit& u, float x, float z) {
    if (!pathService_) return;
    pathRetryAt_.erase(u.id);   // a new order deserves a fresh attempt
    if (!u.type || u.type->canFly || u.type->isStructure()) return;
    const NavGrid& g = navFor(u.type);
    if (g.empty()) return;
    const PathCell from{int(u.x) / 16, int(u.z) / 16};
    const PathCell to{int(x) / 16, int(z) / 16};
    if (pathDist(from, to) < 3) { paths_.cancel(u.id); return; }
    paths_.request(u.id, from, to, g.width(), g.height(), x, z,
                   /*priority=*/u.type->commander);
}

// Label the connected components of the cells a `foot`-wide unit can occupy, using
// the same adjacency the movers use (8-neighbour, fits(), no diagonal corner-cut),
// so "same component" means a body of this size can actually walk between them. Rebuilt
// only when the grid's walkability version moves; one fill serves every goal.
const World::CompGrid* World::components(const NavGrid& g, int foot) const {
    if (g.empty()) return nullptr;
    auto& cg = compCache_[{&g, foot}];
    if (cg.ver == g.version() && cg.w == g.width() && cg.h == g.height()) return &cg;
    g.ensureClearance();
    const int w = g.width(), h = g.height();
    cg.ver = g.version(); cg.w = w; cg.h = h;
    cg.label.assign(size_t(w) * size_t(h), -1);
    static const int dcx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int dcz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    std::vector<int> stack;
    int32_t next = 0;
    for (int z0 = 0; z0 < h; ++z0)
        for (int x0 = 0; x0 < w; ++x0) {
            size_t seed = size_t(z0) * size_t(w) + size_t(x0);
            if (cg.label[seed] != -1 || !g.fits(x0, z0, foot)) continue;
            const int32_t id = next++;
            cg.label[seed] = id;
            stack.push_back(int(seed));
            while (!stack.empty()) {
                int idx = stack.back(); stack.pop_back();
                int cx = idx % w, cz = idx / w;
                for (int k = 0; k < 8; ++k) {
                    int nx = cx + dcx[k], nz = cz + dcz[k];
                    if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
                    if (!g.fits(nx, nz, foot)) continue;
                    if (k >= 4 && (!g.fits(cx + dcx[k], cz, foot) ||
                                   !g.fits(cx, cz + dcz[k], foot)))
                        continue;                    // no diagonal corner-cutting
                    size_t ni = size_t(nz) * size_t(w) + size_t(nx);
                    if (cg.label[ni] != -1) continue;
                    cg.label[ni] = id;
                    stack.push_back(int(ni));
                }
            }
        }
    return &cg;
}


// Reachability: can a body of this type get from (fx,fz) to (gx,gz) at all? The AI
// scores targets with it and the movement watchdogs use it to give up on a leg
// rather than run a whole-map search that would scan the grid before failing.
// Deterministic -- the labelling is a pure function of the nav grid -- so the sim
// may use it, not only the AI.
bool World::lineOpen(const UnitType* t, int selfId, int x0, int z0, int x1, int z1) const {
    // Can this unit travel the straight line between two CELLS without meeting
    // anything it cannot get past? Used to shortcut a traced route.
    //
    // Stricter than the SEARCH's own rule on purpose. cellScore grades a parked body
    // as kCellOccupied, which equals kCellThreshold -- passable, just expensive -- so
    // the trace prefers to go around one but is allowed through. The MOVER has no such
    // latitude: bodies are solid and there is no local avoidance, so a unit sent
    // through a parked crowd simply stops in it. Shortcutting on terrain alone
    // (NavGrid::lineFits) therefore undid exactly the detours the trace had just made
    // around parked troops, and put the mover back where it would jam. Here a cell
    // must be genuinely clear -- ground or road, not merely "passable".
    int dx = std::abs(x1 - x0), dz = -std::abs(z1 - z0);
    int sx = x0 < x1 ? 1 : -1, sz = z0 < z1 ? 1 : -1, err = dx + dz;
    // How many OCCUPIED cells a shortcut may cross. Terrain is never crossed.
    //
    // ONE, and the reason is worth reading before changing it, because the obvious
    // measurement gives the wrong answer. The search now refuses to route through a
    // parked body (see the score callback in tick()), so a traced route genuinely goes
    // AROUND a standing crowd -- and a shortcut that straightens back through it puts
    // the mover right back where it jams. Measured on a wall of parked bodies 1-4 cells
    // deep, with that search in place:
    //
    //   tolerance 1   all four walls passed, 1262-1288 travelled (1100 straight-line)
    //   tolerance 6   walls 1 and 2 STUCK -- thin enough to fit inside the budget
    //
    // Earlier, with the OLD search -- which routed through parked bodies because
    // kCellOccupied equals kCellThreshold -- a strict shortcut measured WORSE than a
    // loose one, and 6 was chosen on that evidence. That result was an artifact: the
    // only routes available then went through the crowd, so refusing them left nothing.
    // Once the search was fixed the ordering reversed. A tuning number is only as good
    // as the thing it was tuned against.
    //
    // Not zero: the GOAL cell is routinely occupied -- ordering a unit to where
    // something already stands is an ordinary thing to do -- and the search's goal
    // exemption does not apply here. One cell lets the last hop reach it without
    // letting a shortcut cross a wall. Zero measures identically on arrivals and
    // marginally worse on travel.
    constexpr int kShortcutOccupied = 1;
    int budget = kShortcutOccupied;
    auto ok = [&](int x, int z) {
        const int sc = cellScore(t, x, z, selfId);
        if (sc < kCellThreshold) return false;         // terrain: never
        if (sc <= kCellOccupied && --budget < 0) return false;   // too many bodies
        return true;
    };
    for (int guard = 0; guard < 8192; ++guard) {
        if (!ok(x0, z0)) return false;
        if (x0 == x1 && z0 == z1) return true;
        const int e2 = 2 * err;
        const bool stepX = e2 >= dz, stepZ = e2 <= dx;
        // A diagonal step passes between two cells; both must be clear, matching the
        // movers' refusal to cut a corner.
        if (stepX && stepZ && (!ok(x0 + sx, z0) || !ok(x0, z0 + sz))) return false;
        if (stepX) { err += dz; x0 += sx; }
        if (stepZ) { err += dx; z0 += sz; }
    }
    return false;
}

bool World::pathExists(const UnitType* type, float gx, float gz, float fx, float fz) const {
    // Answered from connectivity, not a distance field. This used to build a full-map
    // Dijkstra (~40ms) for a single yes/no, and that WAS the per-second AI hitch:
    // measured ai=40.7ms of which pathExists=40.7ms for one build. The component
    // labelling costs one flood fill per grid+footprint and serves every goal until
    // the terrain changes.
    const NavGrid& grid = navFor(type);
    if (grid.empty()) return false;
    const int foot = type ? std::clamp(std::max(type->footX, type->footZ), 1, 15) : 1;
    const CompGrid* cg = components(grid, foot);
    if (!cg || cg->label.empty()) return false;
    const int w = cg->w, h = cg->h;
    auto labelAt = [&](float wx, float wz) -> int32_t {
        int cx = int(wx) / 16, cz = int(wz) / 16;
        if (cx < 0 || cz < 0 || cx >= w || cz >= h) return -1;
        return cg->label[size_t(cz) * size_t(w) + size_t(cx)];
    };
    // Nearest label to a point whose own cell has none: spiral out to the first cell
    // a body of this size fits in. Used for BOTH ends -- see below.
    auto nearestLabel = [&](float wx, float wz) -> int32_t {
        int cx = std::clamp(int(wx) / 16, 0, w - 1);
        int cz = std::clamp(int(wz) / 16, 0, h - 1);
        for (int r = 1; r < 24; ++r)
            for (int j = -r; j <= r; ++j)
                for (int i = -r; i <= r; ++i) {
                    if (std::max(std::abs(i), std::abs(j)) != r) continue;
                    int nx = cx + i, nz = cz + j;
                    if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
                    int32_t l = cg->label[size_t(nz) * size_t(w) + size_t(nx)];
                    if (l >= 0) return l;
                }
        return -1;
    };
    // The asker may not FIT where it stands -- shoved by a crowd, clipped into a
    // corner, squeezed past a building -- while still being on walkable ground. That
    // is a transient position, not a verdict about the goal, so resolve it to the
    // nearest cell the body does fit exactly as the goal is resolved below. Returning
    // false here instead made the movement watchdogs read "goal unreachable" and DROP
    // the order, so a unit that got briefly wedged abandoned its march for good.
    int32_t from = labelAt(fx, fz);
    if (from < 0) from = nearestLabel(fx, fz);
    if (from < 0) return false;
    int32_t to = labelAt(gx, gz);
    if (to < 0) {
        // A goal the unit cannot occupy resolves to the nearest cell it can, so
        // asking about a spot on a wall still answers usefully.
        to = nearestLabel(gx, gz);
    }
    return to >= 0 && to == from;
}

void World::loadInto(int unitId, int transportId) {
    Unit* u = unit(unitId);
    Unit* t = unit(transportId);
    if (!u || !t || !u->alive() || !t->alive() || u->embarked()) return;
    if (!u->type || !u->type->canMove || u->type->domain != UnitType::Domain::Ground)
        return;
    if (u->type->cantBeTransported) return;   // e.g. heavy/anchored units
    if (!t->type || !t->type->canTransport || u->player != t->player) return;
    // transportsize: sum the slot cost of the current cargo, not just the count.
    int used = 0;
    for (int id : t->cargo)
        if (const Unit* c = unit(id)) used += c->type ? c->type->transportSize : 1;
    if (used + u->type->transportSize > t->type->transportCap) return;
    u->orders.clear();
    cancelPath(*u);          // a route for the orders just discarded would eat the load
    Order o;
    o.targetId = transportId;
    o.load = true;
    u->orders.push_back(o);
}

void World::unloadAt(int transportId, float x, float z) {
    Unit* t = unit(transportId);
    if (!t || !t->alive() || t->cargo.empty()) return;
    t->orders.clear();
    // A search for the orders just discarded is still queued, and cases 1 and 3 below
    // re-request nothing -- so it would land on the bare unload and rewrite it.
    cancelPath(*t);
    // Sail within unloading range of the drop point, then disembark.
    //
    // The drop point is normally LAND -- that is the whole point of the order -- and a
    // boat can never stand on it, so the approach cannot aim at it. tickTransport
    // unloads from kUnloadRange away; the approach has to end somewhere the transport
    // actually fits, inside that range.
    //
    // Getting this wrong is what the first version of this function did after the
    // inline A* came out: it queued a move goal AT the drop point. A final move goal
    // completes within max(16px, footprint) of its point, so a boat would push at
    // unreachable ground for ever -- or until some watchdog dropped the leg -- while
    // standing well inside the range it could have unloaded from. The old A* hid this
    // by accident: it required the GOAL to fit the unit, so a land drop point simply
    // returned no path and the order list was the bare unload, governed by
    // tickTransport's range check. That accident was the real behaviour, and it has
    // to survive the A*'s removal.
    //
    // Three cases, in order:
    //   1. Already in range -- no approach at all; the unload fires on the next tick.
    //   2. A cell the transport fits, within range of the drop point -- approach that,
    //      routed by the tracer like any other move.
    //   3. Nothing suitable (a drop point far inland, a landlocked lake) -- no approach
    //      leg, exactly as before: sail straight at it and let the range check decide.
    //      An unreachable approach leg would be strictly worse than none.
    const float ddx = x - t->x, ddz = z - t->z;
    const bool inRange = ddx * ddx + ddz * ddz <= kUnloadRange * kUnloadRange;
    if (!inRange) {
        float ax = 0, az = 0;
        if (approachCell(*t, x, z, ax, az)) {
            // The approach leg has to be its own order rather than routing the unload
            // order itself: replaceLeg rebuilds the current leg from route waypoints
            // and carries only the leg's movement flags across, so an unload order used
            // as the leg would come back as plain waypoints with the flag gone and the
            // cargo would never leave. As a trailing order it is untouched (replaceLeg
            // keeps everything queued behind the leg) and reaches the front once the
            // approach completes, where the tick dispatch hands it to tickTransport.
            Order mv;
            mv.x = ax;
            mv.z = az;
            mv.goal = true;      // so currentLeg() picks THIS order as the leg to route
            t->orders.push_back(mv);
        }
    }
    Order o;
    o.x = x;
    o.z = z;
    o.unload = true;
    t->orders.push_back(o);
    // Route the approach with the same async boundary tracer every other move order
    // uses. This used to call NavGrid::findPath -- a synchronous 400k-expansion A* on
    // the sim thread, and the last caller of it. It resolved its grid with the same
    // navFor(), so it bought nothing over the tracer that an ordinary move order on the
    // same transport already went through; findPath is gone with it. No approach leg
    // means nothing to route.
    if (t->orders.size() > 1) requestPath(*t, t->orders.front().x, t->orders.front().z);
}

// The nearest point the transport can actually sit in AND get to, within kUnloadRange
// of the drop point. Spirals out in cell rings from the drop cell so the first hit is
// the closest, which keeps the boat as near the shore as its own grid allows; bounded
// by the range because a cell outside it is no use -- arriving there would not satisfy
// the unload. Deterministic: a fixed scan order over a deterministic grid.
//
// Fitting is NOT reaching, and the ring order makes the difference bite. A landlocked
// pond on the far side of the drop point is water the boat fits in, so an unfiltered
// scan accepts it -- and because the pond can easily sit nearer the drop point than the
// open sea, it is found FIRST and a perfectly good coastal cell one ring further out is
// never considered. The boat then has an approach it can never reach. So candidates are
// filtered by the same component labelling pathExists() answers from (cached per
// grid+footprint, so this costs a lookup, not a search).
bool World::approachCell(const Unit& t, float x, float z, float& outX, float& outZ) const {
    const NavGrid& g = navFor(t.type);
    if (g.empty()) return false;
    const int foot = footCells(t.type);
    const int cx = int(x) / 16, cz = int(z) / 16;
    const int maxR = int(kUnloadRange) / 16;      // rings beyond this cannot be in range
    // The transport's own component. Its cell is normally occupiable, but a boat can be
    // mid-nudge or on a cell its footprint no longer fits; fall back to accepting any
    // fitting cell rather than refusing to unload at all.
    const CompGrid* cg = components(g, foot);
    int32_t here = -1;
    if (cg && !cg->label.empty()) {
        const int tx = std::clamp(int(t.x) / 16, 0, cg->w - 1);
        const int tz = std::clamp(int(t.z) / 16, 0, cg->h - 1);
        here = cg->label[size_t(tz) * size_t(cg->w) + size_t(tx)];
    }
    auto reachable = [&](int nx, int nz) {
        if (here < 0 || !cg || cg->label.empty()) return true;   // no labelling to use
        if (nx < 0 || nz < 0 || nx >= cg->w || nz >= cg->h) return false;
        return cg->label[size_t(nz) * size_t(cg->w) + size_t(nx)] == here;
    };
    for (int r = 0; r <= maxR; ++r) {
        for (int j = -r; j <= r; ++j)
            for (int i = -r; i <= r; ++i) {
                if (std::max(std::abs(i), std::abs(j)) != r) continue;   // ring only
                const int nx = cx + i, nz = cz + j;
                if (!g.fits(nx, nz, foot)) continue;
                if (!reachable(nx, nz)) continue;   // fits, but not from here
                const float wx = float(nx) * 16 + 8, wz = float(nz) * 16 + 8;
                // Ring distance is Chebyshev; the range test is Euclidean, so a corner
                // of the last ring can still fall outside it. Check the real distance.
                const float dx = wx - x, dz = wz - z;
                if (dx * dx + dz * dz > kUnloadRange * kUnloadRange) continue;
                outX = wx;
                outZ = wz;
                return true;
            }
    }
    return false;
}

void World::tickTransport(Unit& u, float dt) {
    (void)dt;
    Order& o = u.orders.front();
    if (o.load) {
        Unit* t = unit(o.targetId);
        if (!t || !t->alive() || !t->type ||
            int(t->cargo.size()) >= t->type->transportCap) {
            u.orders.erase(u.orders.begin());
            return;
        }
        float dx = t->x - u.x, dz = t->z - u.z;
        o.x = t->x;
        o.z = t->z;
        // The pickup radius is the TRANSPORT's own transportdistance, not a constant.
        // Fall back to the old 70 when a type declares none, so a modded transport
        // without the key still works rather than never completing a load.
        float tr = t->type->transportDist > 0 ? t->type->transportDist : 70.0f;
        if (dx * dx + dz * dz <= tr * tr) {
            u.inTransport = t->id;
            t->cargo.push_back(u.id);
            u.orders.clear();
            u.speed = 0;
        }
        return;
    }
    // unload: sail close to the point, then place cargo on nearby land.
    float dx = o.x - u.x, dz = o.z - u.z;
    if (dx * dx + dz * dz > kUnloadRange * kUnloadRange) return;   // keep sailing
    int cx = int(o.x) / 16, cz = int(o.z) / 16;
    for (int id : u.cargo) {
        Unit* c = unit(id);
        if (!c) continue;
        // Spiral out for a free ground cell.
        for (int r = 0; r < 12 && c->inTransport; ++r)
            for (int j = -r; j <= r && c->inTransport; ++j)
                for (int i = -r; i <= r && c->inTransport; ++i) {
                    if (std::max(std::abs(i), std::abs(j)) != r) continue;
                    if (!nav_.walkable(cx + i, cz + j)) continue;
                    c->x = float(cx + i) * 16 + 8;
                    c->z = float(cz + j) * 16 + 8;
                    c->inTransport = 0;
                }
    }
    std::erase_if(u.cargo, [&](int id) {
        Unit* c = unit(id);
        return !c || !c->inTransport;
    });
    if (u.cargo.empty()) u.orders.erase(u.orders.begin());
}

void World::attackMove(int unitId, float x, float z, bool queue) {
    Unit* u = unit(unitId);
    // isStructure(), NOT canMove: the Keep and both Taros/Veruna walls declare
    // canmove=1 with no velocity (the CLAUDE.md gotcha). Gating on canMove let a
    // BUILDING accept a move order -- it could not go anywhere, but the mover still
    // turned its heading toward the goal, so you could spin a keep by right-clicking.
    if (!u || !u->alive() || !u->type) return;
    if (u->type->isStructure()) {
        Order o{x, z, 0};
        o.attackMove = true;
        setRally(*u, o, queue);
        return;
    }
    size_t before = queue ? u->orders.size() : 0;
    order(unitId, x, z, queue);
    for (size_t i = before; i < u->orders.size(); ++i) u->orders[i].attackMove = true;
}

void World::patrol(int unitId, float x, float z) {
    Unit* u = unit(unitId);
    // isStructure(), NOT canMove: the Keep and both Taros/Veruna walls declare
    // canmove=1 with no velocity (the CLAUDE.md gotcha). Gating on canMove let a
    // BUILDING accept a move order -- it could not go anywhere, but the mover still
    // turned its heading toward the goal, so you could spin a keep by right-clicking.
    if (!u || !u->alive() || !u->type) return;
    if (u->type->isStructure()) {
        // The rally patrols between the building and the clicked point: a unit off the
        // line walks out and then loops, which is what a patrol rally means.
        Order b{x, z, 0};   b.patrol = true; b.attackMove = true;
        Order a{u->x, u->z, 0}; a.patrol = true; a.attackMove = true;
        setRally(*u, b, /*queue=*/false);
        setRally(*u, a, /*queue=*/true);
        return;
    }
    u->orders.clear();
    cancelPath(*u);          // the route for the replaced orders is not this patrol's
    Order a;
    a.x = u->x; a.z = u->z; a.patrol = true; a.attackMove = true;
    Order b;
    b.x = x; b.z = z; b.patrol = true; b.attackMove = true;
    u->orders.push_back(b);
    u->orders.push_back(a);
}

void World::patrolTo(int unitId, float x, float z, bool queue) {
    Unit* u = unit(unitId);
    // isStructure(), NOT canMove: the Keep and both Taros/Veruna walls declare
    // canmove=1 with no velocity (the CLAUDE.md gotcha). Gating on canMove let a
    // BUILDING accept a move order -- it could not go anywhere, but the mover still
    // turned its heading toward the goal, so you could spin a keep by right-clicking.
    if (!u || !u->alive() || !u->type) return;
    if (u->type->isStructure()) {
        Order o{x, z, 0};
        o.patrol = true;
        o.attackMove = true;
        setRally(*u, o, queue);
        return;
    }
    size_t before = queue ? u->orders.size() : 0;
    order(unitId, x, z, queue);
    // Mark every waypoint of this move as a looping, engage-en-route patrol leg; a
    // chain of patrolTo calls then cycles the unit through all of them.
    for (size_t i = before; i < u->orders.size(); ++i) {
        u->orders[i].patrol = true;
        u->orders[i].attackMove = true;
    }
}

void World::orderWait(int unitId, float seconds, bool queue) {
    Unit* u = unit(unitId);
    if (!u || !u->alive() || !u->type) return;
    if (!queue) u->orders.clear();
    Order o;
    o.x = u->x;
    o.z = u->z;
    o.wait = seconds > 0 ? seconds : 0.0f;
    u->orders.push_back(o);
}

void World::orderWaitAttack(int unitId, bool queue) {
    Unit* u = unit(unitId);
    if (!u || !u->alive() || !u->type) return;
    if (!queue) u->orders.clear();
    Order o;
    o.x = u->x;
    o.z = u->z;
    o.waitAttack = true;
    u->orders.push_back(o);
}

void World::guard(int unitId, int targetId, bool queue) {
    Unit* u = unit(unitId);
    Unit* t = unit(targetId);
    if (!u || !t || !u->alive() || !t->alive() || u->id == t->id) return;
    if (!u->type || !u->type->canMove || u->player != t->player) return;
    if (!queue) u->orders.clear();
    Order o;
    o.targetId = targetId;
    o.guard = true;
    o.x = t->x;
    o.z = t->z;
    u->orders.push_back(o);
}

void World::stop(int unitId) {
    Unit* u = unit(unitId);
    if (!u) return;
    u->orders.clear();
    cancelPath(*u);          // stop means stop: do not finish routing where it was going
    // Stop also halts a conjurer: cancel the infinite loop and drain the queue.
    u->repeatType = nullptr;
    u->buildQueue.clear();
    u->buildProgress = 0;
}

void World::destroy(int unitId) {
    // Self-destruct: TOGGLE the countdown. Arming a live unit starts the timer;
    // pressing again while it counts down cancels it. On expiry (in tick) the
    // unit leaves your command -- it fades, without an explosion, without a
    // wreck, and without giving anyone kill credit.
    //
    // The length is the type's `selfdestructcountdown` in SECONDS. Retail packs
    // it into three bits of UnitType+0x264 (0x4c09e8 masks it & 7, shifts 21)
    // and the mission at 0x4017e0 steps it once a second.
    //
    // Nothing in the shipped data declares that key -- not one of the 558 FBIs
    // across every archive -- which is exactly why the DEFAULT matters, and the
    // default is not zero. When the key is absent the parser clears bit 23 and
    // sets bit 22 (0x4c0a1f), leaving the field at 2. Emulating the mission
    // confirms it: at 2 it reschedules twice at 30 ticks, expires, and applies
    // its 30000 damage on the run after -- two seconds, which is what the game
    // gives you. See tools/re/emuself.py.
    Unit* u = unit(unitId);
    if (!u || !u->alive() || !u->type) return;
    const float len = float(u->type->selfDestructCountdown);
    u->selfDestructT = (u->selfDestructT < 0.0f) ? len : -1.0f;
}

void World::setWeapon(int unitId, int slot) {
    Unit* u = unit(unitId);
    if (!u || !u->type) return;
    int n = int(u->type->weapons.size());
    if (n > 0) {
        u->weaponSlot = std::clamp(slot, 0, n - 1);
        u->weaponAuto = false;   // the player has taken the choice over
    }
}

void World::setStance(int unitId, int stance) {
    Unit* u = unit(unitId);
    if (!u || !u->alive()) return;
    // A type with no stance cannot be given one. The HUD already hides the buttons;
    // this is the authoritative half, so a modified client cannot set a stance on
    // something retail would not offer it for.
    if (u->type && !u->type->canSetStance) return;
    u->stance = std::clamp(stance, 0, 2);
    // Retail's Standing_UnitOrder setter (icd 0x5198a0) writes the move AND fire
    // fields together, and note what it CANNOT write: move 2, the unlimited-chase
    // roam state. A unit that spawned roaming (its type sets no standingunitorder)
    // gives that up permanently the first time the player touches these buttons.
    // That is retail's behaviour, not an oversight of ours.
    switch (u->stance) {
        case 0: u->moveState = 1; u->fireState = 2; break;   // Offensive
        case 1: u->moveState = 0; u->fireState = 2; break;   // Defensive
        default: u->moveState = 0; u->fireState = 0; break;  // Passive
    }
}

void World::setCloak(int unitId, bool on) {
    Unit* u = unit(unitId);
    if (!u || !u->alive() || !u->type || !u->type->canCloak) return;
    u->cloakOn = on;
}

void World::setActive(int unitId, bool on) {
    Unit* u = unit(unitId);
    if (!u || !u->alive() || !u->type || !u->type->onOffable) return;
    u->active = on;
}

void World::setSquad(int unitId, int squad) {
    Unit* u = unit(unitId);
    if (!u || !u->alive()) return;
    u->squad = int8_t(std::clamp(squad, -10, 10));   // 0 none, +N group N, -N formation N
}

void World::attack(int unitId, int targetId, bool queue) {
    Unit* u = unit(unitId);
    if (!u || !u->alive() || !u->type) return;
    // A production building sends its OUTPUT at the target rather than shooting it
    // itself -- and it may well have no weapon, so this is checked before the
    // weapon gate below.
    if (u->type->isStructure() && u->type->producesUnits()) {
        Order o{0, 0, targetId};
        setRally(*u, o, queue);
        return;
    }
    if (u->type->weapon.damage <= 0) return;
    if (!queue) u->orders.clear();
    u->orders.push_back({0, 0, targetId});
    u->orders.back().goal = true;                 // an attack order IS its own leg
    u->orders.back().issuedTick = tickCounter_;
}

float Weapon::damageVs(const UnitType* t) const {
    // A per-category DAMAGE entry is a MULTIPLIER on `default`, not a damage figure.
    // This returned the entry directly, which is how the Barracks became unkillable:
    // arakeep is damagecategory=factory, the Crusades swordsman declares factory=0.5,
    // and 0.5 was read as half a hit point. Against 17162 HP with healtime=1.25 (0.8
    // HP/s of regen) the building out-healed a besieging army by three orders of
    // magnitude -- every arrow visibly connecting, the health bar never moving.
    //
    // The data says multiplier plainly once you look at all of it. Across both
    // datasets the values cluster on 0.04/0.08/0.2/0.25/0.5/0.75/1.1/1.25/1.5/2/3 --
    // 0.25 and 0.5 alone are 129 of the 249 entries in unitscb. Read as absolute
    // damage those are fractions of one HP, i.e. every override in the game would
    // mean "this weapon does nothing", which is not a balance system. Read as
    // multipliers they are exactly the expected table: siege weapons strong against
    // structures (arasmith factory=2.0, fort=2.0), infantry weak against them
    // (factory=0.5), an assassin devastating against soft targets (araspy human=6,
    // tier1=6), anti-air multiplied against flyers (verball airship=4). The clincher
    // is npcemen, a campaign duellist with buri=100 against Lord Buriash (2940 HP):
    // as a multiplier that is the scripted one-shot kill the mission wants, as
    // absolute damage it is 100 and the duel takes thirty hits.
    if (t && !dmgVs.empty())
        for (const auto& c : t->categories) {
            auto it = dmgVs.find(c);
            if (it != dmgVs.end()) return damage * it->second;
        }
    return damage;
}

void World::applyHit(const Weapon& w, float hx, float hz, int fromPlayer, int fromId,
                     Unit* primary) {

    // Record the impact for the viewer (hit sound / effect).
    {
        HitFx hf{hx, hz, &w, primary ? primary->type : nullptr};
        if (primary) { hf.victimId = primary->id; hf.damage = w.damageVs(primary->type); }
        const Unit* from = fromId ? unit(fromId) : nullptr;
        hf.fromX = from ? from->x : hx;
        hf.fromZ = from ? from->z : hz;
        hits_.push_back(hf);
    }
    // Attacker's veteran attack multiplier boosts damage dealt (retail scales the
    // attacker's attack stat by vetMul); the victim's boosts armour (below).
    const Unit* attacker = fromId ? unit(fromId) : nullptr;
    // Attack multiplier = veterancy × live aura buff (AdjustAttack).
    float atkMul = attacker ? attacker->vetMul() * attacker->atkBuff : 1.0f;
    // Damage + status one victim, honouring veterancy, auras and immunities.
    auto hurt = [&](Unit& e, float scale) {
        if (e.stonedFor > 0) return;   // petrified units are impervious
        // subtype=mindcontrol (the Taros Mind Mage's Individual + Area Mind
        // Control): the shot CONVERTS the victim instead of hurting it. Retail
        // encodes eligibility in the weapon's own [DAMAGE] table -- it zeroes
        // monarch/god/dragon/fort/factory/naval/lodestone -- so "damage > 0
        // against this category" is exactly "can be charmed", which damageVs()
        // already computes. Conversion is permanent, like contact capture.
        if (w.mindControl) {
            // Retail's charm roll (icd 0x52da10), gates in this exact order so the
            // RNG stream can never diverge between peers. Note the [DAMAGE] table is
            // the ACQUISITION filter -- it decides what you may TARGET -- so a
            // Monarch caught inside an area charm is stopped by the commander gate
            // here, not by its zeroed damage row.
            if (e.embarked()) return;                 // safe inside a transport
            if (e.type->commander) return;            // Monarchs are never charmed
            if (e.type->cantBeCaptured) return;
            if (e.hp <= 0) return;
            if (atUnitCap(fromPlayer)) return;        // no room on the new side
            // Chance rises with the victim's veterancy -- a green unit is ~80%, a
            // 10-star veteran is capped at 99%. Scaled by the area falloff so the
            // rim of an Area Mind Control is less reliable than its centre.
            int chance = std::min(99, (int(e.veteran) + 16) * 5);
            if (burnRand(100) >= int(float(chance) * std::clamp(scale, 0.0f, 1.0f)))
                return;                               // it shrugged the spell off
            captureUnit(e, fromPlayer);
            return;
        }
        if (benchmarkMode() && e.type && e.type->commander) return;   // benchmark: Monarchs are invincible
        // base × attacker-attack (↑) ÷ victim-armour (↓); armour = veterancy × aura.
        float armour = std::max(e.vetMul() * e.armBuff, 0.01f);
        float dealt = w.damageVs(e.type) * atkMul / armour * scale;
        e.hp -= dealt;
        if (e.hp <= 0) {
            e.overkill = std::max(e.overkill, -e.hp);          // retail severity input
            e.deathType = uint8_t(w.dmgType);                  // 3 = explosion -> gib
        }
        if (fromId) e.lastHitBy = fromId;
        if (w.status != Weapon::Status::None && e.type) {
            bool immune =
                (w.status == Weapon::Status::Frozen && e.type->cantBeFrozen) ||
                (w.status == Weapon::Status::Stoned && e.type->cantBeStoned);
            if (!immune) {
                if (w.status == Weapon::Status::Paralyzed) {
                    e.paralyzedFor = std::max(e.paralyzedFor, w.statusDur);
                    e.speed = 0;
                } else {
                    // Retail petrify/freeze (icd 0x51a61a): HP zeroes INSTANTLY
                    // regardless of amount -- the victim dies on the spot and
                    // the death edge places its stone=/frozen= STATUE (blocking,
                    // permanent, resurrectable back to life). The old temporary
                    // stoned/frozen debuff was our pre-RE guess.
                    bool freeze = w.status == Weapon::Status::Frozen;
                    if (freeze) e.frozenFor = 1.0f; else e.stonedFor = 1.0f;
                    e.hp = 0;
                    e.deathType = freeze ? 15 : 14;
                    e.speed = 0;
                    static const bool kStatLog = std::getenv("TAK_BURNLOG") != nullptr;
                    if (kStatLog)
                        std::fprintf(stderr, "statue kill: %s %s at %.0f,%.0f\n",
                                     e.type->id.c_str(), freeze ? "frozen" : "stoned",
                                     e.x, e.z);
                }
            }
        }
    };
    if (primary) hurt(*primary, 1.0f);
    // Features under fire (retail icd 0x529dc0 -> DamageFeature 0x4961a0):
    // every feature head cell within aoe/2 of the impact -- the impact cell
    // exempt from the radius check, so a no-splash weapon (Fire Breath, aoe 0)
    // still strikes the cell it hits. A firestarter hit on a flamable feature
    // IGNITES it instead of damaging it that hit; everything else accumulates
    // weapon damage, and at the def's `damage` value the feature is destroyed
    // -- swapped to its `featuredead` stage (npcwreck chains) or removed.
    // Deterministic in every peer's sim (state + RNG hashed). Must run BEFORE
    // the aoe<=0 early-out below.
    // unitsonly=1: the effect touches UNITS only, never the scenery. Retail sets it
    // on the big area spells (Earthen/Wind Wave, Death Aura, Energy Blast, both
    // Mind Controls) precisely so a 500px nuke doesn't also level every forest and
    // mana-bearing prop inside it.
    if (!featTypes_.empty() && terW_ > 0 && !w.unitsOnly) {
        float r = std::max(w.aoe * 0.5f, 8.0f);
        int icx = int(hx) / 16, icz = int(hz) / 16;
        int rc = int(r) / 16 + 1;
        for (int dz = -rc; dz <= rc; ++dz)
            for (int dx = -rc; dx <= rc; ++dx) {
                int cx = icx + dx, cz = icz + dz;
                if (cx < 0 || cz < 0 || cx >= terW_ || cz >= terH_) continue;
                auto it = featureIdx_.find(cz * terW_ + cx);
                if (it == featureIdx_.end()) continue;
                Feature& f = features_[it->second];
                if (!f.alive || f.type < 0) continue;
                if (!(dx == 0 && dz == 0)) {   // impact cell is struck regardless
                    float fdx = f.x - hx, fdz = f.z - hz;
                    if (fdx * fdx + fdz * fdz > r * r) continue;
                }
                const FeatType& ft = featTypes_[size_t(f.type)];
                if (ft.indestructible) continue;
                if (w.fireStarter && ft.flamable && !f.burn && ft.hasBurnAnim) {
                    igniteFeature(f);
                    continue;   // ignition replaces the damage this hit
                }
                if (ft.hp <= 0) continue;   // no damage value: indestructible-by-damage
                f.dmg += w.damage;
                if (f.dmg >= ft.hp) swapFeature(f, ft.deadType);
            }
    }
    if (w.aoe <= 0) return;
    // Splash the surrounding enemies, scaled from full at the centre to `edge`
    // at the rim (so an area weapon actually hits a crowd, per its FBI aoe).
    // Retail's splash radius is areaofeffect/2, and the falloff is QUADRATIC:
    //   frac = (1 - d/r)^2 * (1 - edge) + edge
    // We used the full aoe as the radius with a linear ramp, which made every
    // splash weapon in the game twice as wide as retail and far too strong at the
    // rim -- and it disagreed with our own FEATURE pass below, which already halved
    // it. Both halves of applyHit now use the same radius.
    const float r = w.aoe * 0.5f;
    int splashed = 0;
    forEachNear(hx, hz, r, [&](int idx) {
        Unit& e = units_[size_t(idx)];
        if (!e.alive() || e.embarked() || !e.type || allied(e.player, fromPlayer)) return;
        if (&e == primary) return;   // already took the direct hit
        float dx = e.x - hx, dz = e.z - hz;
        float d = std::sqrt(dx * dx + dz * dz);
        if (d >= r) return;
        float t = 1.0f - d / r;
        float scale = t * t * (1.0f - w.edge) + w.edge;
        hurt(e, scale);
        ++splashed;
    });
    static const bool kLog = std::getenv("TAK_SPLASHLOG") != nullptr;
    if (splashed && kLog)
        std::fprintf(stderr, "splash %s aoe=%.0f hit %d extra\n",
                     w.name.c_str(), r, splashed);
}

// Retail leads a MOVING unit target: the aim point is the target's centre plus its
// velocity times a fraction of the flight time (icd 0x51aa50). The fraction is the
// fixed-point literal 0xCCCC/65536, so retail deliberately under-leads by 20% --
// shots land slightly behind a runner rather than perfectly on it, which is what
// makes a siege shell miss a moving blob and crater instead.
// A ground-point target, a melee swing and a hitscan beam never lead; nor does a
// weapon carrying `dontleadtargets`.
static constexpr float kLeadFrac = float(0xCCCC) / 65536.0f;   // 0.79998779296875

static void leadAim(const Unit& shooter, const Unit& tgt, const Weapon& w,
                    float& ax, float& az) {
    ax = tgt.x;
    az = tgt.z;
    if (w.noLead || w.melee || w.beam || w.projVel <= 0.0f || tgt.speed == 0.0f) return;
    // Distance is the PRE-lead one, matching retail: a single pass, no iteration.
    float d = detmath::len(tgt.x - shooter.x, tgt.z - shooter.z);
    float t = kLeadFrac * d / w.projVel;
    // Velocity from heading x speed is exact -- it is literally what the mover
    // integrates each tick.
    ax += detmath::sin(tgt.heading) * tgt.speed * t;
    az += detmath::cos(tgt.heading) * tgt.speed * t;
}

void World::fire(Unit& u, Unit& target, int slot) {
    const Weapon& w = u.type->weapons[size_t(slot)];
    // manapershot: a caster spends personal mana to fire; if it can't pay, the
    // shot doesn't happen (reload not consumed, so it fires the moment it can).
    if (w.manaCost > 0 && u.type->maxMana > 0) {
        if (u.mana < w.manaCost) return;
        u.mana -= w.manaCost;
    }
    // Veterans reload faster (retail divides the cooldown by the veteran multiplier).
    float rl = w.reload / std::max(u.vetMul(), 0.01f);
    u.reloads[slot] = rl;
    u.justFired = true;
    // Remote Effect: nothing travels. The spell materialises at the AIMED GROUND
    // POINT and lands after builduptime -- so walking aside doesn't dodge an
    // Earthquake, you have to leave its (up to 500px) radius. decaytime is the
    // visual fade after the hit.
    if (w.kind == Weapon::Kind::Remote) {
        PendingEffect e;
        e.w = &w;
        e.x = target.x; e.z = target.z;
        e.player = u.player; e.fromId = u.id;
        // shotart: this spell is DELIVERED. A visible shot flies to the aim point
        // first and the channel only starts when it lands, so the Acolyte lobs its
        // Earthquake rather than conjuring it under your feet. The shot is purely
        // cosmetic -- targetId 0 means it collides with nothing and simply expires
        // on arrival; the PendingEffect below carries all the actual damage.
        float deliver = 0.0f;
        if (!w.shotArt.empty() && w.projVel > 0) {
            float sdx = target.x - u.x, sdz = target.z - u.z;
            float sdist = std::max(detmath::len(sdx, sdz), 1e-3f);
            float svel = w.projVel * kTick / 30.0f;
            deliver = sdist / svel;
            Projectile s;
            s.x = u.x; s.z = u.z;
            s.vx = sdx / sdist * svel;
            s.vz = sdz / sdist * svel;
            s.wsrc = &w;
            s.targetId = 0;              // cosmetic: hits nothing, just flies
            s.fromPlayer = u.player;
            s.fromId = u.id;
            s.fx = w.fx;
            s.life = deliver;
            s.flight = deliver;
            projectiles_.push_back(s);
        }
        switch (w.remote) {
            case Weapon::RemoteKind::Earthquake:
                // Pulses every `shakeduration` from impact, right through buildup
                // AND decay -- for the drake's Earthquake the single pulse actually
                // lands inside the decay window.
                e.period = std::max(w.shakeDur, 0.1f);
                e.at = e.period;
                e.endAt = w.buildUp + w.decay;
                break;
            case Weapon::RemoteKind::Hailstorm: {
                // The rain: buildup/decay don't apply. It starts falling shortly
                // after the cast and pulses particlespersecond times a second for
                // `duration` -- so the Acolyte's Hail Shower is ~12 small hits, not
                // one 70-damage tap.
                float pps = w.particlesPerSec > 0 ? w.particlesPerSec : 5.0f;
                e.period = 1.0f / pps;
                e.at = 20.0f / 30.0f;                     // retail's ~20-tick lead-in
                e.endAt = 20.0f / 30.0f + std::max(w.duration, e.period);
                break;
            }
            case Weapon::RemoteKind::MindCtl:
            case Weapon::RemoteKind::Freeze:
                // One sweep when the channel completes -- and killing the caster
                // mid-channel ABORTS it, which is real counterplay against a Mind
                // Mage winding up an area charm.
                e.at = w.buildUp;
                e.endAt = w.buildUp + w.decay;
                e.casterGated = true;
                break;
            case Weapon::RemoteKind::Plain:
            default:
                e.at = w.buildUp;
                e.endAt = w.buildUp + w.decay;
                break;
        }
        // The channel clock starts when the shot LANDS, not when it is thrown.
        e.at += deliver;
        e.endAt += deliver;
        pendingEffects_.push_back(e);
        return;
    }
    // Wandering: spawn a roaming storm at the aim point. It drifts for `duration`,
    // grinding whatever it passes over, instead of landing one tap.
    if (w.kind == Weapon::Kind::Wandering) {
        // The storm is born just IN FRONT OF THE CASTER facing the aim point -- it
        // is not delivered to the target. It then drifts along that launch heading
        // for good (a storm never re-aims), weaving as it goes, so the caster is
        // aiming a slow moving hazard rather than placing one.
        Storm s;
        float dx = target.x - u.x, dz = target.z - u.z;
        float dl = std::max(detmath::len(dx, dz), 1e-3f);
        s.dirX = dx / dl; s.dirZ = dz / dl;
        s.w = &w;
        s.x = u.x + s.dirX * 32.0f;
        s.z = u.z + s.dirZ * 32.0f;
        s.player = u.player; s.fromId = u.id;
        s.id = ++stormSeq_;
        s.arm = w.buildUp;            // wind-up: visible and moving, but harmless
        s.left = w.duration > 0 ? w.duration : 6.0f;
        s.nextVary = 0.0f;            // roll the first wander offset immediately
        storms_.push_back(s);
        return;
    }
    if (w.melee || w.beam || w.projVel <= 0) {
        // Instant hit: a melee swing, a hitscan bolt, or a Line-of-Sight beam (the
        // drake's Fire Breath -- a sustained flame emission, not a lobbed shot). The
        // damage lands now along the sightline; the flame stream itself is a
        // client-side visual driven by emitTime, so no traveling projectile spawns.
        applyHit(w, target.x, target.z, u.player, u.id, &target);
        return;
    }
    // Dropped: the bomb is RELEASED, not fired. It leaves the bomber with only the
    // bomber's own forward drift and falls onto the ground beneath -- so it lands
    // where the bomber IS, and a bomber has to overfly its target (see the release
    // gate in tickCombat). Detonates on landing wherever it ended up, hit or miss.
    if (w.kind == Weapon::Kind::Dropped) {
        Projectile b;
        b.x = u.x;
        b.z = u.z;
        // Retail solves the release velocity so the bomb arrives over the aim point
        // exactly as it finishes falling: vel = (aim - release) / fallTicks, with
        // ZERO vertical speed. weaponvelocity is never read here and neither is the
        // flyer's heading -- release point and timing do not change where the bomb
        // lands. (It LOOKS like inherited momentum only because a closing bomber is
        // already pointed at its target.)
        float bdx = target.x - u.x, bdz = target.z - u.z;
        b.vx = bdx / kBombFall;
        b.vz = bdz / kBombFall;
        b.damage = w.damage;
        b.wsrc = &w;
        b.targetId = target.id;
        b.fromPlayer = u.player;
        b.fromId = u.id;
        b.fx = w.fx;
        b.life = kBombFall;      // time to fall from cruise altitude
        b.flight = kBombFall;    // the viewer arcs it down over the same window
        projectiles_.push_back(b);
        return;
    }
    Projectile p;
    p.x = u.x;
    p.z = u.z;
    // Aim where the target WILL be, not where it is (see leadAim). The projectile
    // keeps targetId so the swept proximity test still resolves a direct hit; what
    // changes is that a shot at a runner now lands behind it and detonates there.
    float aimX = target.x, aimZ = target.z;
    leadAim(u, target, w, aimX, aimZ);
    float dx = aimX - u.x, dz = aimZ - u.z;
    float dist = std::max(std::sqrt(dx * dx + dz * dz), 1e-3f);
    float vel = w.projVel * kTick / 30.0f;   // weaponvelocity is already px/s-ish
    p.vx = dx / dist * vel;
    p.vz = dz / dist * vel;
    p.damage = w.damage;
    p.wsrc = &w;
    p.targetId = target.id;
    p.fromPlayer = u.player;
    p.fromId = u.id;
    p.fx = w.fx;
    // Fuel. A dumb shot only needs to reach where it was aimed. A GUIDED one is
    // fuelled from the weapon's RANGE instead (retail fixes its expiry tick at
    // launch as one full range of travel), so a homer that has to curve after a
    // fleeing target doesn't run dry halfway -- fuelling it from the launch
    // distance would make the chase it exists for fizzle. Out of fuel is a dud
    // either way: no damage, no splash.
    // A dumb shot is fuelled to EXACTLY its aim point, because that is where it now
    // detonates: the old half-second of slack would put an arapult's crater ~375px
    // (23 cells) beyond where the shell was drawn landing. A GUIDED one keeps the
    // slack and is fuelled from the weapon's RANGE instead.
    p.life = w.kind == Weapon::Kind::Guided ? (std::max(w.range, dist) / vel + 0.5f)
                                            : (dist / vel);
    p.flight = dist / vel;
    projectiles_.push_back(p);
}

// Retail melee ignores its TDF `range` entirely: MeleeWeapon::inRange (icd
// @0x52b980) passes when the two units' footprint boxes are within half a cell
// (8px) of TOUCHING on both axes -- edge adjacency, not a radial distance.
// (Ranged weapons use the base test, icd @0x530580: 2D centre-to-centre,
// inclusive.) Treating melee `range` as radial left Zhon beasts (range 250)
// halting 15 cells out and biting air, and the Sailor (range 10) unable to ever
// reach a legal firing distance past the 13px separation floor.
static bool meleeInRange(const UnitType* a, const UnitType* b, float dx, float dz) {
    return std::abs(dx) < 8.0f * float(a->footX + b->footX) + 8.0f &&
           std::abs(dz) < 8.0f * float(a->footZ + b->footZ) + 8.0f;
}

void World::tickCombat(Unit& u, float dt) {
    for (auto& r : u.reloads)
        if (r > 0) r -= dt;
    if (!u.active) return;   // onoffable unit powered down: no acquisition/fire

    // Auto-acquire: idle armed units engage the nearest enemy in reach;
    // attack-movers and patrollers interrupt their route to fight. A PASSIVE unit
    // (stance 2, hold fire) never auto-acquires -- it only fights when ordered.
    // A builder mid-job -- constructing a site, repairing, reclaiming, holding
    // queued builds, or producing from a queue -- stays on task: it does NOT
    // auto-acquire a nearby enemy and wander off to fight while it should build.
    bool busyBuilding = u.buildSiteId != 0 || u.repairId != 0 || u.reclaimId != 0 ||
                        hasQueuedWork(u) ||
                        (u.type->canMove && !u.buildQueue.empty());   // mobile conjurer producing
    // Auto-acquisition belongs to the FIRE order: only Fire At Will goes looking.
    // Hold Fire and Return Fire both wait to be handed a target (an explicit attack
    // order still works -- retail's hold-fire gate has a "forced" bypass for
    // exactly that).
    bool acquiring = !busyBuilding && u.fireState == 2 &&
                     (u.orders.empty() ||
                      (u.orders.front().targetId == 0 &&
                       (u.orders.front().attackMove || u.orders.front().patrol)) ||
                      u.orders.front().guard);
    // Can any of this unit's weapons deal real damage to `e`? (noairweapon gates
    // flyers.) Requiring damage > 0 stops an army from piling onto -- and firing
    // forever at -- a building/target its weapons cannot scratch (a per-category
    // damage.<cat>=0 override), which never dies so the attack order never clears.
    auto canDamage = [&](const Unit& e) {
        for (const auto& wp : u.type->weapons)
            if (!(e.type->canFly && wp.noAir) && wp.damageVs(e.type) > 0.0f) return true;
        return false;
    };
    auto canTarget = [&](const Unit& e) {
        if (e.cloaked) return false;   // cloaked units are invisible to auto-acquire
        return canDamage(e);
    };
    // Auto-acquisition is staggered across ticks by unit id: an idle armed unit
    // rescans for a target every kAcqStride ticks (~0.13s at 30Hz), not every
    // tick. That turns the dense-crowd O(n^2) neighbour scan into O(n^2/stride)
    // -- the dominant sim cost when thousands of idle armed units pile together.
    // Deterministic (id+tick, identical on every lockstep peer), so no desync. The
    // stride widens with the crowd (acqStride_, set at tick start) so massive battles
    // rescan less often -- the acquisition cost scales sub-linearly instead of O(n).
    bool acqTurn = (uint32_t(u.id) + tickCounter_) % acqStride_ == 0;
    if (acquiring && u.type->weapon.damage > 0 && acqTurn) {
        // The +90 is approach margin: room to notice something and walk to it. A
        // unit whose move order forbids leaving has no use for it -- it should
        // acquire only what it can already shoot, or it would lock onto something
        // it will never reach and then drop it again next tick.
        float ar = u.type->maxRange() + (u.moveState == 0 ? 0.0f : 90.0f);
        int best = 0;
        float bestD = ar * ar;
        // fireatwillrandom: spread fire across whatever is in range instead of
        // every unit converging on the nearest body. Only while AUTO-acquiring --
        // an explicit attack order still goes exactly where it was pointed, which
        // is retail's gate too (its scorer is reached only from the fire-at-will
        // path, and every shipped unit is fire-at-will by default since no FBI
        // sets standingfireorder).
        const bool scatter = u.type->fireAtWillRandom;
        float bestScore = 1e30f;
        // maneuverleashlength limits how far an IDLE defender will chase from its
        // post. It must NOT apply while attack-moving/patrolling — those orders
        // mean "advance and engage everything en route", so an army that has
        // travelled far from its spawn still acquires (incl. just-conjured foes).
        // An OFFENSIVE unit (stance 0) ignores its leash entirely and chases freely.
        // ... while whether you may LEAVE to engage belongs to the move order:
        //   0 = never (shoot what wanders into range, but hold the post),
        //   1 = only within maneuverleashlength of where you were posted,
        //   2 = no limit.
        // The leash is measured from home, and an attack-move or patrol overrides
        // it entirely -- those orders mean "advance and engage everything en route".
        float leash2 = (u.moveState == 1 && u.orders.empty() && u.type->leash > 0)
                           ? u.type->leash * u.type->leash : 1e30f;
        // A ranged unit only auto-acquires enemies it can actually see, so it
        // doesn't charge a target hidden behind a wall (which caused pile-ups at
        // corners). Melee/flyers acquire regardless.
        bool ranged = u.type->maxRange() > 64.0f && !u.type->canFly;
        int uFoot = std::max(u.type->footX, u.type->footZ) / 2;
        const bool lobber = u.type->lobs();   // shoots OVER obstacles: skip the LoS gate
        forEachNear(u.x, u.z, ar, [&](int idx) {
            const Unit& e = units_[size_t(idx)];
            if (!e.alive() || e.embarked() || allied(e.player, u.player) || !e.type) return;
            if (e.underConstruction) return;   // don't auto-react to a site still conjuring
            if (!canTarget(e)) return;
            float hx = e.x - u.homeX, hz = e.z - u.homeZ;
            if (hx * hx + hz * hz > leash2) return;   // outside the leash
            float dx = e.x - u.x, dz = e.z - u.z;
            float d = dx * dx + dz * dz;
            // A scatter-firing unit still only considers what is in RANGE (retail
            // gathers by radius the same way), but inside that set it no longer
            // prefers the nearest -- so the early-out on distance is skipped.
            if (!scatter && d >= bestD) return;
            if (d >= ar * ar) return;
            if (ranged && !e.type->canFly && !lobber &&
                !nav_.losBetween(u.x, u.z, e.x, e.z, uFoot,
                                 std::max(e.type->footX, e.type->footZ) / 2))
                return;                         // no clear shot: don't acquire it
            if (scatter) {
                // Retail's scorer, with the flag's substitution in place: n is
                // INT_MAX/damage instead of dist^2/damage, and the winner is the
                // lowest of rand(n)/2 + rand(n). Dividing by damage keeps the
                // draw biased toward whatever this weapon hits hardest, which is
                // the half of the original scoring the flag does NOT remove.
                float dmg = 0;
                for (const auto& wp : u.type->weapons)
                    if (!(e.type->canFly && wp.noAir))
                        dmg = std::max(dmg, wp.damageVs(e.type));
                if (dmg <= 0) return;
                uint32_t n = uint32_t(float(0x7FFFFFFF) / dmg);
                float score = float(fireRand(n)) / 2.0f + float(fireRand(n));
                if (score >= bestScore) return;
                bestScore = score; best = e.id;
                return;
            }
            bestD = d; best = e.id;
        });
        if (best) {
            u.orders.insert(u.orders.begin(), {0, 0, best});
            u.orders.front().autoTarget = true;
        }
    }
    if (u.orders.empty() || u.orders.front().targetId == 0) return;

    if (u.orders.front().guard) {
        Order& o = u.orders.front();
        Unit* t = unit(o.targetId);
        if (!t || !t->alive()) {
            u.orders.erase(u.orders.begin());
            return;
        }
        o.x = t->x;
        o.z = t->z;
        float dx = t->x - u.x, dz = t->z - u.z;
        if (dx * dx + dz * dz <= 70 * 70)
            u.speed = std::max(0.0f, u.speed - u.type->brake * dt);
        return;   // movement walks toward o when out of reach
    }

    Unit* target = unit(u.orders.front().targetId);
    if (!target || !target->alive()) {
        u.orders.erase(u.orders.begin());
        return;
    }
    // Give up a target this unit can neither damage NOR capture, instead of
    // firing at it forever -- it will never die or convert. Covers a converter
    // (0-damage charm weapon) ordered onto a cantBeCaptured building, and any
    // unit whose weapons all do 0 to the target's category. Capturable targets
    // are exempt: a charmer keeps contact until the capture branch converts it.
    bool canConvert = u.type->canCapture && target->type &&
                      !target->type->cantBeCaptured && !allied(u.player, target->player);
    if (target->type && !canDamage(*target) && !canConvert) {
        u.orders.erase(u.orders.begin());
        return;
    }
    float dx = target->x - u.x, dz = target->z - u.z;
    float dist = std::sqrt(dx * dx + dz * dz);
    int slot = u.type->weapons.empty()
                   ? 0 : std::clamp(u.weaponSlot, 0, int(u.type->weapons.size()) - 1);
    // Which weapon(s) this unit fights with. Retail splits multi-weapon units in
    // two, on the FBI `weaponswitching` flag (unitdef+0x264 bit 26):
    //   weaponswitching=1 (24 units, incl. every Monarch and dragon) -- ONE active
    //     weapon at a time, chosen by the PLAYER from the command panel and held
    //     until switched. There is no cleverness: it starts on WEAPON1 and stays
    //     there. (An earlier pass here had the sim auto-pick the biggest usable
    //     weapon, which made these units markedly stronger than retail.)
    //   no flag (10 units: araat, aragod, arasiege, creaeri, cregod, creiron,
    //     targod, verat, verball, vergod) -- every weapon fires INDEPENDENTLY, each
    //     with its own reload, its own range band and its own mana check, so an
    //     AA/ground turret really does work both barrels at once.
    // (`fireatwillrandom` is unrelated to this -- it scatters auto-acquired TARGET
    // choice; see UnitType::fireAtWillRandom.)
    bool allWeapons = !u.type->weaponSwitching && u.type->weapons.size() > 1;
    // Approach on the reach this unit actually fights at: the selected weapon for a
    // switcher, the longest of them when they all fire.
    const Weapon* sel = slot < int(u.type->weapons.size()) ? &u.type->weapons[slot] : nullptr;
    float best = allWeapons ? u.type->maxRange() : (sel ? sel->range : u.type->maxRange());
    // Range is to the target's footprint EDGE, not its centre. A building's centre is
    // deep inside a blocked footprint, so a centre-distance check leaves a short-range
    // attacker grinding the edge (never "in range") or a flyer buried inside it. For a
    // STRUCTURE target, pad the effective range by its footprint half-extent plus a
    // small margin for where nav actually halts at the edge (mirrors the builder reach
    // in tickConstruction). Mobile targets keep centre-distance, so unit-vs-unit combat
    // is unchanged.
    float pad = target->type->maxVel <= 0.0f
                    ? 8.0f * float(std::max(target->type->footX, target->type->footZ)) + 24.0f
                    : 0.0f;
    float reach = best + pad;
    // Melee: retail closes to footprint adjacency (see meleeInRange above);
    // `range` and LoS play no part.
    bool adj = sel && sel->melee && target->type &&
               meleeInRange(u.type, target->type, dx, dz);
    // Ranged units need a clear line to shoot; a wall between them means close
    // in / reposition rather than firing through it (melee & flyers are exempt).
    // A lobbing weapon arcs over what is in the way, which is the whole point of a
    // mortar. Our projectiles are still 2D, so this is an approximation of retail's
    // high-arc solve rather than the mechanism: it lets a lobber shoot over a cliff
    // it could not genuinely clear. Ported faithfully, the arc would decide.
    bool needLoS = best > 64.0f && !u.type->canFly &&
                   target->type && !target->type->canFly && !u.type->lobs();
    // LoS gates ONLY in-range firing and in-range repositioning; an out-of-range
    // chaser advances regardless (the `dist > best*0.95` move test below
    // short-circuits before `los` is read, and the fire gate is never reached out of
    // range). So defer the raycast until we're actually close enough for it to
    // matter -- identical result, but a marching army (the bulk of a big battle, all
    // out of range) stops paying for a per-tick line-of-sight cast it never uses.
    bool los = true;
    if (needLoS && dist <= reach * 0.95f)
        los = nav_.losBetween(u.x, u.z, target->x, target->z,
                              std::max(u.type->footX, u.type->footZ) / 2,
                              std::max(target->type->footX, target->type->footZ) / 2);
    // A static unit cannot chase -- and neither may one whose move standing order
    // says hold position. Both drop an AUTO-acquired target that walks out of
    // reach; an explicitly ordered attack is unaffected, because being told to go
    // kill something is exactly the case the standing order does not govern.
    const bool mayChase = u.type->canMove &&
                          (u.moveState != 0 || !u.orders.front().autoTarget);
    if (!mayChase && dist > reach) {
        u.orders.erase(u.orders.begin());
        return;
    }
    if ((sel && sel->melee) ? !adj
                            : (dist > reach * 0.95f || (!los && mayChase))) {
        // Advance toward the target, steering around impassable terrain.
        u.repathLeft -= dt;
        Order& o = u.orders.front();
        o.x = target->x;
        o.z = target->z;
        // A chase target moves, so there is nothing worth pre-computing: steer
        // straight at it and let the background pathfinder deal with whatever is
        // in the way. This used to run a full-grid A* every 0.7s behind a shared
        // per-tick budget; the budget is gone, and so is the A*.
        return;   // movement handled by the normal move logic
    }
    // In range: stop and face the target. Retail brakes to a halt FIRST and only
    // then pivots, at turninplacerate rather than the moving turn rate (icd
    // 0x4d9b65), so a unit doesn't spin on the spot while it is still sliding.
    u.speed = std::max(0.0f, u.speed - u.type->brake * dt);
    float want = detmath::atan2(dx, dz);
    float diff = angleDiff(want, u.heading);
    if (u.speed <= 0.0f) {
        float maxTurn = u.type->turnInPlaceRate * dt;
        u.heading += std::clamp(diff, -maxTurn, maxTurn);
    }
    // Fire a weapon when the target is in ITS [minrange, range] band, within its
    // aimtolerance, with a clear shot, and (unless noairweapon) legal against air.
    // Each weapon is gated on its own terms, because a non-switching unit runs all
    // of them at once and they rarely share a range band.
    auto tryFire = [&](int sl) {
        const Weapon& sw = u.type->weapons[size_t(sl)];
        if (sw.noAir && target->type && target->type->canFly) return;
        if (!(sw.melee || los)) return;
        if (u.reloads[size_t(sl)] > 0) return;
        if (sw.melee ? !adj : dist > sw.range + pad) return;
        if (dist < sw.minRange) return;
        // A bomb is let go, not aimed -- retail's Dropped weapon overrides its aim
        // virtual with a no-op, so the bomber is always considered on target (which
        // a hovering flyer with momentum could otherwise almost never satisfy).
        if (sw.kind != Weapon::Kind::Dropped &&
            std::abs(diff) >= std::max(sw.aimTol, 0.03f)) return;
        fire(u, *target, sl);
    };
    if (!u.type->weapons.empty()) {
        if (allWeapons)
            for (int sl = 0; sl < int(u.type->weapons.size()); ++sl) tryFire(sl);
        else if (sel)
            tryFire(slot);
    }
    // cancapture: a charmer converts the target after sustained contact (~3s) or
    // once it is worn down, rather than killing it.
    if (u.type->canCapture && target->type && !target->type->cantBeCaptured &&
        !allied(u.player, target->player)) {
        u.captureProg += dt;
        if (u.captureProg > 3.0f || target->hp < target->type->maxHp * 0.25f) {
            captureUnit(*target, u.player);
            u.captureProg = 0;
            u.orders.erase(u.orders.begin());   // done with this one
        }
    }
}

// Switch a unit's allegiance: contact charm (cancapture) and mind-control weapons
// both land here, so a converted unit behaves identically either way -- it drops
// its old orders, comes up at half health if it was nearly dead, and forgets who
// last hit it (so the new owner isn't credited a kill on its own unit).
void World::captureUnit(Unit& t, int newPlayer) {
    if (!t.type || t.player == newPlayer) return;
    // Already dead, just not swept yet (alive() reads deadFor, which tick() sets):
    // capturing would raise it back to half health and steal a corpse.
    if (t.hp <= 0) return;
    // Move the unit between the two owners' live counts NOW, the way spawn() does.
    // The counts only re-sync from a full walk at end of tick, so without this an
    // area mind control -- which converts a whole blob inside ONE applyHit -- reads
    // the same stale count for every victim and can carry its new owner far past
    // the lobby unit cap.
    if (t.alive()) {
        if (t.player >= 0 && t.player < int(players_.size()) &&
            players_[size_t(t.player)].unitCount > 0)
            players_[size_t(t.player)].unitCount--;
        if (newPlayer >= 0 && newPlayer < int(players_.size()))
            players_[size_t(newPlayer)].unitCount++;
    }
    t.player = newPlayer;
    t.orders.clear();
    t.hp = std::max(t.hp, t.type->maxHp * 0.5f);
    t.lastHitBy = 0;
    t.squad = 0;             // no longer in its old owner's control group
    t.buildQueue.clear();    // and not still producing for them
    t.buildProgress = 0;
}

// Stamp every PARKED ground unit into the occupancy layer. O(n), one cell each:
// our movers are all one nav cell, and this deliberately does NOT go through
// NavGrid::block, whose clearance DP and cache invalidation (it bumps the grid
// version, staling every component labelling over it) would be ruinous at a
// per-tick cadence. Rebuilt wholesale in unit-index order so the last writer on a
// contested cell is the same on every peer.
void World::blockCells(int cx, int cz, int w, int h, bool blocked) {
    if (obst_.empty() || terW_ <= 0) return;
    int x0 = std::max(0, cx), z0 = std::max(0, cz);
    int x1 = std::min(terW_ - 1, cx + w - 1), z1 = std::min(terH_ - 1, cz + h - 1);
    bool any = false;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            uint8_t& o = obst_[size_t(z) * size_t(terW_) + size_t(x)];
            if (o != (blocked ? 1 : 0)) { o = blocked ? 1 : 0; any = true; }
        }
    if (!any) return;
    // Every grid reads the same overlay, so they all need their clearance redone
    // over the touched band. markDirty is cheap; the DP is lazy.
    nav_.markClearanceDirty();
    navWater_.markClearanceDirty();
    navHover_.markClearanceDirty();
    for (auto& g : navClasses_) g.markClearanceDirty();
}

void World::blockFoot(const UnitType& t, float x, float z, bool blocked) {
    int cx = int(x) / 16 - t.footX / 2, cz = int(z) / 16 - t.footZ / 2;
    if (t.yardMap.empty()) { blockCells(cx, cz, t.footX, t.footZ, blocked); return; }
    for (int j = 0; j < t.footZ; ++j)
        for (int i = 0; i < t.footX; ++i) {
            char c = t.yardMap[size_t(j) * t.footX + i];
            if (c == 'o' || c == 'O') blockCells(cx + i, cz + j, 1, 1, blocked);
        }
}

void World::buildNavClasses(const TypeRegistry& reg) {
    navClasses_.clear();
    navIdx_.clear();
    if (heights_.empty() || terW_ <= 0) return;
    // Collapse the registry's types onto their distinct limit tuples. Ordered by the
    // tuple so the table is built identically on every peer regardless of map order.
    auto tupleOf = [](const UnitType& t) {
        NavGrid::Limits l;
        l.maxSlope = int(t.maxSlope);
        l.maxWaterSlope = int(t.maxWaterSlope);
        // Domain decides the sense of the water band, exactly as the three legacy
        // grids did: a ground unit may wade only so deep, a boat needs a minimum
        // depth to float, a hoverer ignores both.
        if (t.domain == UnitType::Domain::Water) {
            l.maxWaterDepth = 10000;
            l.minWaterDepth = int(t.minWaterDepth > 0 ? t.minWaterDepth : 13);
        } else if (t.domain == UnitType::Domain::Hover) {
            l.maxWaterDepth = 10000;
            l.minWaterDepth = -10000;
        } else {
            l.maxWaterDepth = int(t.maxWaterDepth > 0 ? t.maxWaterDepth : 20);
            l.minWaterDepth = -10000;
        }
        return l;
    };
    auto key = [](const NavGrid::Limits& l) {
        return std::make_tuple(l.maxSlope, l.maxWaterSlope, l.maxWaterDepth, l.minWaterDepth);
    };
    std::map<std::tuple<int, int, int, int>, int> seen;
    for (const auto& [id, t] : reg.types()) {
        if (t.canFly) continue;             // flyers ignore the ground entirely
        NavGrid::Limits l = tupleOf(t);
        auto k = key(l);
        auto it = seen.find(k);
        if (it == seen.end()) {
            it = seen.emplace(k, int(navClasses_.size())).first;
            navClasses_.push_back(NavGrid(heights_, terW_, terH_, seaLevel_, l));
        }
        navIdx_[&t] = it->second;
    }
    // Obstacles are NOT baked in: every grid reads the one shared overlay, so a
    // building blocked before or after this call is seen by all of them.
    for (auto& g : navClasses_) { g.setObstacles(&obst_); g.setRoads(&roads_); }
}

float World::bodyPenetration(const Unit& u, float nx, float nz) const {
    if (occW_ <= 0 || !u.type) return 0.0f;
    const float hs = float(std::max(u.type->footX, u.type->footZ)) * 8.0f;
    // occ_ stamps a body's whole footprint, so any unit we could overlap has a
    // stamped cell within hs of us: |nx-o.x| < hs+ohs and its cells reach o.x
    // +/- ohs, leaving the nearest one inside hs. One cell of slack for the
    // truncation in int(nx)/16.
    const int span = int(hs) / 16 + 2;
    const int cx = int(nx) / 16, cz = int(nz) / 16;
    float worst = 0.0f;
    int32_t last = 0;
    for (int j = -span; j <= span; ++j) {
        int z = cz + j;
        if (z < 0 || z >= occH_) continue;
        const int32_t* row = &occ_[size_t(z) * size_t(occW_)];
        for (int i = -span; i <= span; ++i) {
            int x = cx + i;
            if (x < 0 || x >= occW_) continue;
            int32_t id = row[x];
            // A footprint stamps a run of identical ids; skipping the repeat is
            // most of the scan for anything bigger than a single cell.
            if (id == 0 || id == u.id || id == last) continue;
            last = id;
            const Unit* o = unit(id);
            if (!o || !o->type) continue;
            // Retail's occupancy rule, read off 0x4db640 (the unit half of the
            // passability query) at 0x4db767-0x4db7c9. An occupant is IGNORED --
            // the step passes straight through it -- only when all three hold:
            //   1. it is genuinely under way (retail tests a live movement
            //      object at its +8, and bails to "blocked" when there is none),
            //   2. it is not slower than us ([[+8]+0x20] >= our own),
            //   3. its heading (+0x7e) is within 0x4000 of ours -- 90 degrees.
            // Anything else falls to 0x4db893, which returns 2; the threshold at
            // every call site is 4, so the step is refused.
            //
            // Phrased in English: you may close up behind someone going your way
            // who is not slower than you. Head-on traffic blocks. Slower traffic
            // ahead of you blocks. Parked blocks. This is the rule that lets
            // retail run with no separation pass at all -- overlap barely forms,
            // and the single case that creates it (following a faster leader)
            // unwinds itself as the leader pulls away.
            //
            // Ignoring EVERY mover, which is what stood here, is far looser than
            // retail and is precisely what made a de-overlap pass necessary.
            if (o->speed > 0.0f && o->speed >= u.speed &&
                std::fabs(angleDiff(o->heading, u.heading)) <= kPi * 0.5f)
                continue;
            float sep = hs + float(std::max(o->type->footX, o->type->footZ)) * 8.0f;
            // Bodies we are ALREADY inside are not ours to arbitrate. Units spawn
            // in tight ranks, a finished building lands under its builder, a push
            // overlaps a pair -- and a rule phrased on the deepest overlap freezes
            // the lot: nobody can reduce every overlap at once, so nobody moves,
            // and each frozen (speed 0) body then blocks its neighbours in turn.
            // So the mover's job is the narrow one: never enter a body you are
            // currently clear of. NOTHING pulls existing overlap apart any more --
            // the separation pass that did was deleted with the move to retail's
            // occupancy predicate, and the point of that change is that overlap
            // barely forms in the first place (see the note further up). Do not
            // re-read this as "something else will sort it out".
            if (std::min(sep - std::fabs(u.x - o->x), sep - std::fabs(u.z - o->z)) > 0.0f)
                continue;
            float p = std::min(sep - std::fabs(nx - o->x), sep - std::fabs(nz - o->z));
            if (p > worst) worst = p;
        }
    }
    return worst;
}

int World::cellScore(const UnitType* t, int cx, int cz, int selfId) const {
    const NavGrid& g = navFor(t);
    const int foot = footCells(t);
    if (!g.empty() && !g.fits(cx, cz, foot)) return kCellImpassable;
    if (occW_ > 0) {
        // Only PARKED bodies hold a cell against a search, matching the mover:
        // a unit under way is something to fall in behind, not a wall.
        const int x0 = cx - foot / 2, z0 = cz - foot / 2;
        for (int j = 0; j < foot; ++j)
            for (int i = 0; i < foot; ++i) {
                const int x = x0 + i, z = z0 + j;
                if (x < 0 || z < 0 || x >= occW_ || z >= occH_) continue;
                const int32_t o = occ_[size_t(z) * size_t(occW_) + size_t(x)];
                if (o == 0 || o == selfId) continue;
                const Unit* u = unit(o);
                if (u && u->alive() && u->speed == 0.0f) return kCellOccupied;
            }
    }
    if (!g.empty() && g.roadAt(cx, cz)) return kCellRoad;
    return kCellGround;
}

void World::rebuildOccupancy() {
    occW_ = terW_;
    occH_ = terH_;
    if (occW_ <= 0 || occH_ <= 0) { occ_.clear(); occW_ = occH_ = 0; return; }
    occ_.assign(size_t(occW_) * size_t(occH_), 0);
    // Every mobile body is stamped, moving or parked, matching retail's "exclusive
    // per-cell occupancy layer" (080d288's RE).
    //
    // MOVING BODIES ARE STAMPED SECOND -- i.e. a parked body wins a contested cell.
    // A cell holds exactly one id, so whoever stamps last is the only one anybody
    // can see there, and the mover now gates on PARKED bodies specifically (a
    // moving one is merely expensive, per retail's graded area score). Let a mover
    // overwrite a parked stamp and the parked body becomes invisible to the test
    // that exists to respect it -- units would walk through precisely the bodies
    // that are standing still, and only while someone happened to be passing.
    // Two passes rather than one, in fixed unit order, so this stays deterministic.
    for (int pass = 0; pass < 2; ++pass) {
        const bool wantParked = (pass == 1);
        for (const auto& u : units_) {
            if (!u.alive() || u.embarked() || !u.type) continue;
            if (u.type->canFly || u.type->isStructure()) continue;   // structures are in nav_
            if (u.underConstruction) continue;
            if ((u.speed == 0.0f) != wantParked) continue;
            // Stamp the whole footprint, centre-anchored to match NavGrid::fits.
            int f = footCells(u.type);
            int cx = int(u.x) / 16 - f / 2, cz = int(u.z) / 16 - f / 2;
            for (int j = 0; j < f; ++j)
                for (int i = 0; i < f; ++i) {
                    int x = cx + i, z = cz + j;
                    if (x < 0 || z < 0 || x >= occW_ || z >= occH_) continue;
                    occ_[size_t(z) * size_t(occW_) + size_t(x)] = u.id;
                }
        }
    }
}

void World::rebuildGrid() {
    gCell_ = 32.0f;
    float minx = 1e30f, minz = 1e30f, maxx = -1e30f, maxz = -1e30f;
    bool any = false;
    // Include ALL alive units (buildings too) so combat can target structures.
    auto inGrid = [](const Unit& u) {
        return u.alive() && !u.embarked() && u.type;
    };
    for (const auto& u : units_) {
        if (!inGrid(u)) continue;
        any = true;
        minx = std::min(minx, u.x); maxx = std::max(maxx, u.x);
        minz = std::min(minz, u.z); maxz = std::max(maxz, u.z);
    }
    if (!any) { gW_ = gH_ = 0; return; }
    gOx_ = minx - gCell_;
    gOz_ = minz - gCell_;
    gW_ = int((maxx - minx) / gCell_) + 3;
    gH_ = int((maxz - minz) / gCell_) + 3;
    gHead_.assign(size_t(gW_) * gH_, -1);
    gNext_.assign(units_.size(), -1);
    for (size_t i = 0; i < units_.size(); ++i) {
        const Unit& u = units_[i];
        if (!inGrid(u)) continue;
        int cx = std::clamp(int((u.x - gOx_) / gCell_), 0, gW_ - 1);
        int cz = std::clamp(int((u.z - gOz_) / gCell_), 0, gH_ - 1);
        int c = cz * gW_ + cx;
        gNext_[i] = gHead_[size_t(c)];
        gHead_[size_t(c)] = int(i);
    }
}

bool World::canPlace(const UnitType* type, float x, float z) const {
    if (!type) return false;
    // Lodestones must sit on a mana deposit — but only on maps that have any
    // (deposit-less maps let them build on open ground). And only ONE lodestone
    // per deposit: reject if another already occupies the target deposit.
    if (type->onMana && !manaSpots_.empty()) {
        int spot = -1;
        float best = 24.0f * 24.0f;
        for (size_t i = 0; i < manaSpots_.size(); ++i) {
            float dx = manaSpots_[i].first - x, dz = manaSpots_[i].second - z;
            float d = dx * dx + dz * dz;
            if (d < best) { best = d; spot = int(i); }
        }
        if (spot < 0) return false;   // not on any deposit
        float sx = manaSpots_[size_t(spot)].first, sz = manaSpots_[size_t(spot)].second;
        // One lodestone per deposit. Some Sacred Stones register as two adjacent
        // spots (~22-40px apart); a 44px exclusion merges those into one deposit
        // so a second lodestone can't squeeze onto the same stone.
        for (const auto& u : units_) {
            if (!u.alive() || !u.type || !u.type->onMana) continue;
            float dx = u.x - sx, dz = u.z - sz;
            if (dx * dx + dz * dz < 44.0f * 44.0f) return false;   // deposit taken
        }
    }
    // Check the domain-appropriate grid so water units (Kraken) require water
    // and land units require land, rather than always testing the ground grid.
    const NavGrid& grid = navFor(type);
    int cx = int(x) / 16 - type->footX / 2, cz = int(z) / 16 - type->footZ / 2;
    // Water structures (Veruna's Sea Fort / Floating Tower) encode a shoreline
    // footprint in their yardmap: 'w' cells are the slipway that MUST sit over water
    // (where the ships launch), solid cells ('o'/'c'/'C') the land-side base, '.' is
    // off-footprint. Honouring that stops a naval building from being placed on dry
    // land (all its 'w' cells would fail the water test) AND lets it sit correctly on
    // a coast (which the old whole-footprint-on-land check wrongly rejected). Only
    // yardmaps that actually contain 'w' take this path, so every land building keeps
    // its exact previous placement rule (and hash).
    bool waterYard = false;
    if (!type->yardMap.empty())
        for (char c : type->yardMap) if (c == 'w' || c == 'W') { waterYard = true; break; }
    if (waterYard) {
        for (int j = 0; j < type->footZ; ++j)
            for (int i = 0; i < type->footX; ++i) {
                char c = type->yardMap[size_t(j) * type->footX + i];
                if (c == 'w' || c == 'W') {
                    if (!navWater_.walkable(cx + i, cz + j)) return false;   // slipway needs water
                } else if (c == '.' || c == ' ') {
                    continue;                                                // not part of the footprint
                } else if (!nav_.walkable(cx + i, cz + j)) {
                    return false;                                           // land base needs land
                }
            }
    } else if (!type->yardMap.empty()) {
        // Honour the yardmap: a '.' cell is NOT part of the footprint and retail
        // runs no test on it at all (its per-cell mask is zero). We were testing
        // every cell of the bounding box, so a tree under one of the Keep's TWO
        // leading '.' rows -- 14 of its 84 cells -- denied a placement retail
        // allows. blockFootprint already honoured the yardmap, so nav and
        // placement disagreed about the same building.
        for (int j = 0; j < type->footZ; ++j)
            for (int i = 0; i < type->footX; ++i) {
                char c = type->yardMap[size_t(j) * type->footX + i];
                if (c == '.' || c == ' ') continue;
                if (!grid.walkable(cx + i, cz + j)) return false;
            }
    } else {
        for (int j = 0; j < type->footZ; ++j)
            for (int i = 0; i < type->footX; ++i)
                if (!grid.walkable(cx + i, cz + j)) return false;
    }
    for (const auto& u : units_) {
        if (!u.alive()) continue;
        float dx = u.x - x, dz = u.z - z;
        float min = 16.0f * float(std::max(type->footX, type->footZ)) / 2 + 12;
        if (dx * dx + dz * dz < min * min) return false;
    }
    return true;
}

bool World::clearableForPlacement(const UnitType* type, float x, float z,
                                  std::vector<int>& out) const {
    out.clear();
    if (!type || type->maxVel > 0.0f) return false;   // buildings only
    if (canPlace(type, x, z)) return true;            // nothing in the way already
    const NavGrid& grid = navFor(type);
    if (grid.empty()) return false;
    int cx = int(x) / 16 - type->footX / 2, cz = int(z) / 16 - type->footZ / 2;
    // Which live features could be cleared, indexed by the cells they cover.
    auto featureAt = [&](int gx, int gz) -> const Feature* {
        for (const auto& f : features_) {
            if (!f.alive || !f.blocks || f.work <= 0.0f) continue;   // not reclaimable
            int fx0 = int(f.x) / 16 - f.fx / 2, fz0 = int(f.z) / 16 - f.fz / 2;
            if (gx >= fx0 && gx < fx0 + f.fx && gz >= fz0 && gz < fz0 + f.fz) return &f;
        }
        return nullptr;
    };
    bool anyBlocked = false;
    for (int j = 0; j < type->footZ; ++j)
        for (int i = 0; i < type->footX; ++i) {
            if (!type->yardMap.empty()) {
                char c = type->yardMap[size_t(j) * type->footX + i];
                if (c == '.' || c == ' ') continue;   // not part of the footprint
            }
            int gx = cx + i, gz = cz + j;
            if (grid.walkable(gx, gz)) continue;                 // clear already
            if (!grid.terrainWalkable(gx, gz)) return false;     // the GROUND says no
            const Feature* f = featureAt(gx, gz);
            if (!f) return false;   // an obstacle that isn't a clearable doodad
            anyBlocked = true;
            if (std::find(out.begin(), out.end(), f->id) == out.end()) out.push_back(f->id);
        }
    if (!anyBlocked) return false;   // blocked by something the cell walk didn't see
    // A unit standing on the site still refuses, exactly as canPlace does -- clearing
    // doodads can't move a body, and retail refuses here too.
    for (const auto& u : units_) {
        if (!u.alive()) continue;
        float dx = u.x - x, dz = u.z - z;
        float min = 16.0f * float(std::max(type->footX, type->footZ)) / 2 + 12;
        if (dx * dx + dz * dz < min * min) return false;
    }
    return true;
}

int World::startBuild(int builderId, const UnitType* type, float x, float z,
                      Approach approach) {
    Unit* b = unit(builderId);
    // A mobile builder only: a building may carry canMove=1 in its FBI but can never
    // place structures, and a site still under construction is not yet a builder.
    if (!b || !b->alive() || !b->type || !b->type->isBuilder || b->type->isStructure() ||
        b->underConstruction)
        return 0;
    if (atUnitCap(b->player)) return 0;   // at the unit cap: can't start a new build
    if (atTypeCap(b->player, type)) return 0;   // totalallowed: already fielding one
    if (!canPlace(type, x, z)) return 0;
    int8_t bsquad = b->squad;   // capture before spawn may realloc units_
    int id = spawn(type, x, z, 3.14159f, b->player);
    Unit* site = unit(id);
    site->underConstruction = true;
    site->beingBuilt = true;   // its builder owns it this tick (no instant decay)
    site->hp = type->maxHp * 0.05f;
    // Auto-join: a conjured MOBILE unit inherits the builder's squad (a building never does).
    if (bsquad && !type->isStructure()) site->squad = bsquad;
    // Key off maxVel, NOT the FBI canmove flag: verwall and tarwall both declare
    // canmove=1 with no velocity, so a canMove gate left Veruna and Taros walls
    // blocking neither pathing nor line of sight while Aramon's and Creon's did.
    // (The CLAUDE.md gotcha, biting for real.)
    if (type->isStructure()) {
        blockFoot(*type, x, z, true);
    }
    b = unit(builderId);   // spawn may have reallocated units_
    b->buildSiteId = id;
    b->buildStuckT = 0; b->buildStuckD = 1e30f;   // fresh job: reset the reach watchdog
    if (approach == Approach::Replace)
        order(builderId, x, z + float(type->footZ) * 8 + 24, false);
    return id;
}

void World::queueBuild(int builderId, const UnitType* type, float x, float z, bool queue) {
    Unit* b = unit(builderId);
    if (!b || !type) return;
    if (!canPlace(type, x, z)) return;
    // ONE queue. A build is an order like any other and lives in Unit::orders, in
    // the position the player clicked it -- which is what makes "move here, build
    // that, move there" mean what it says, and what lets the order line draw it.
    // Retail works the same way: its order list holds MobileBuild orders alongside
    // the moves, and the order-line iterator (icd 0x4d6340) dispatches per kind.
    // A non-queued build replaces the lot, exactly as a non-queued move does.
    if (!queue) b->orders.clear();
    Order o;
    // Park it where the builder must stand to work, so arriving at this order
    // means "in position to build" rather than "standing on the site".
    o.x = x;
    o.z = z + float(type->footZ) * 8 + 24;
    o.goal = true;
    o.issuedTick = tickCounter_;
    o.buildType = type;
    b->orders.push_back(o);
}

void World::cancelBuilds(int builderId) {
    Unit* b = unit(builderId);
    if (!b) return;
    // Drop every pending build from the order queue (see queueBuild): a fresh
    // move/attack/stop cancels queued construction.
    b->orders.erase(std::remove_if(b->orders.begin(), b->orders.end(),
                                   [](const Order& o) {
                                       return o.buildType != nullptr || o.reclaimFeat != 0 ||
                                              o.repairTarget != 0;
                                   }),
                    b->orders.end());
    b->reclaimId = 0;            // a fresh move/attack/stop drops any reclaim job
    b->repairId = 0;             // ...and any repair job
    if (b->buildSiteId) {
        Unit* site = unit(b->buildSiteId);
        // A site that never actually started building is just a ghost — remove
        // it so its marker/ghost doesn't linger.
        if (site && site->underConstruction && !site->buildBegun) {
            if (site->type && site->type->isStructure()) {
                blockFoot(*site->type, site->x, site->z, false);
            }
            site->underConstruction = false;
            site->deadFor = 1000.0f;   // fully gone (painter skips deadFor>=4)
        }
        b->buildSiteId = 0;
    }
}

void World::assist(int builderId, int siteId, bool queue) {
    Unit* b = unit(builderId);
    Unit* site = unit(siteId);
    if (!b || !b->alive() || !b->type || !b->type->isBuilder || b->type->isStructure() ||
        b->underConstruction)
        return;
    if (!site || !site->alive() || !site->underConstruction ||
        !allied(site->player, b->player))
        return;
    // Latch onto the existing site; tickConstruction walks there and resumes at
    // this builder's rate (buildTime / workerTime) from the site's current HP.
    b->buildSiteId = siteId;
    b->buildStuckT = 0; b->buildStuckD = 1e30f;   // fresh job: reset the reach watchdog
    order(builderId, site->x, site->z + float(site->type->footZ) * 8 + 24, queue);
}

void World::addFeature(int id, float x, float z, float manaYield, float work,
                       int fx, int fz, bool blocks, int type) {
    featureIdx_[id] = features_.size();
    Feature f{id, x, z, fx, fz, manaYield,
              std::max(work, 1.0f), std::max(work, 1.0f), blocks, true};
    f.type = type;
    features_.push_back(f);
    bumpFeatGen();   // a new feature needs a visual instance
}

const Feature* World::feature(int id) const {
    auto it = featureIdx_.find(id);
    return it == featureIdx_.end() ? nullptr : &features_[it->second];
}

const Feature* World::featureAt(float x, float z) const {
    if (terW_ <= 0) return nullptr;
    return feature((int(z) / 16) * terW_ + int(x) / 16);
}

bool World::featureAliveAt(float x, float z) const {
    if (terW_ <= 0) return true;
    int cx = int(x) / 16, cz = int(z) / 16;
    const Feature* f = feature(cz * terW_ + cx);
    return !f || f->alive;   // decorative (untracked) cells are always "alive"
}

// --- Feature burning (retail mechanic; icd 0x494b40 StartBurning, 0x495110
// BurnSpread, 0x495300 BurnOut). Retail synced fire as host-authority events;
// our lockstep equivalent runs it deterministically in every peer's sim. The
// retail burnweapon ("TreeBurn") is DEAD DATA -- the engine wants a [BurnWeapon]
// subsection which no shipped feature has, so burning never damages units, and
// neither does ours. Burn duration in retail is the burn-anim GAF length; the
// sim can't read art, so a fixed 5s stands in (typical tree-burn length).
static constexpr int kBurnTicks = 150;

// Swap a feature IN PLACE to another stage of its chain (featureburnt on
// burn-out, featuredead on destruction; retail places the replacement neutral
// -- our features carry no owner). newType < 0 = the feature is simply gone.
void World::swapFeature(Feature& f, int newType) {
    bumpFeatGen();   // burnt-stage swap (or removal): art changes
    f.burn = 0;
    f.dmg = 0;
    if (f.blocks)   // old stage's footprint frees first
        blockCells(int(f.x) / 16 - f.fx / 2, int(f.z) / 16 - f.fz / 2,
                   f.fx, f.fz, false);
    if (newType < 0) { f.alive = false; f.blocks = false; f.type = -1; return; }
    const FeatType& nt = featTypes_[size_t(newType)];
    f.type = newType;
    f.fx = nt.fx; f.fz = nt.fz;
    f.blocks = nt.blocking;
    if (f.blocks)
        blockCells(int(f.x) / 16 - f.fx / 2, int(f.z) / 16 - f.fz / 2,
                   f.fx, f.fz, true);
    f.manaYield = nt.energy;
    f.work = f.workFull = std::max(nt.energy, 60.0f);
}

void World::igniteFeature(Feature& f) {
    if (f.burn || !f.alive || f.type < 0) return;
    const FeatType& ft = featTypes_[size_t(f.type)];
    if (!ft.flamable || !ft.hasBurnAnim) return;
    static const bool kBurnLog = std::getenv("TAK_BURNLOG") != nullptr;
    if (kBurnLog)
        std::fprintf(stderr, "ignite %s at %.0f,%.0f (spark %d)\n",
                     ft.name.c_str(), f.x, f.z, ft.sparkTicks);
    f.burn = 1;
    bumpFeatGen();   // ignition edge: flame overlay + smoke start
    // Retail spread timer: sparktime30/2 + rand(sparktime30/2), one LCG draw.
    int half = std::max(ft.sparkTicks / 2, 1);
    f.spreadIn = half + burnRand(half);
    f.burnLeft = kBurnTicks;
}

void World::tickBurning() {
    if (featTypes_.empty()) return;
    // Index-based: igniteFeature never reallocates (spread only flips state).
    for (size_t i = 0; i < features_.size(); ++i) {
        Feature& f = features_[i];
        if (!f.alive || !f.burn) continue;
        // Spread fires ONCE per burning feature (retail slot+0x44 never reloads):
        // a 7x7 box around the head cell, each flamable neighbour rolls
        // rand(100) < its own spreadchance. (Retail's downwind spark phase moves
        // <1 cell at shipped wind speeds -- omitted.)
        if (f.spreadIn > 0 && --f.spreadIn == 0) {
            int cx = int(f.x) / 16, cz = int(f.z) / 16;
            for (int dz = -3; dz <= 3; ++dz)
                for (int dx = -3; dx <= 3; ++dx) {
                    if (dx == 0 && dz == 0) continue;
                    int nx = cx + dx, nz = cz + dz;
                    if (nx < 0 || nz < 0 || nx >= terW_ || nz >= terH_) continue;
                    auto it = featureIdx_.find(nz * terW_ + nx);
                    if (it == featureIdx_.end()) continue;
                    Feature& nf = features_[it->second];
                    if (!nf.alive || nf.burn || nf.type < 0) continue;
                    const FeatType& nft = featTypes_[size_t(nf.type)];
                    if (!nft.flamable || !nft.hasBurnAnim) continue;
                    if (burnRand(100) < nft.spreadChance) igniteFeature(nf);
                }
        }
        if (--f.burnLeft <= 0)
            // Burn-out: swap to the featureburnt stage IN PLACE (same id/cell --
            // the client watches f.type to swap art), or die outright.
            swapFeature(f, featTypes_[size_t(f.type)].burntType);
    }
}

// Reclaim rate: work is consumed at a flat rate (retail ties reclaim duration to
// the feature's `energy`, not the builder's worktime). ~2s for a 250-value tree.
static constexpr float kReclaimRate = 120.0f;   // work units per second

void World::reclaim(int builderId, int featureId, bool queue) {
    Unit* b = unit(builderId);
    if (!b || !b->alive() || !b->type || !b->type->isBuilder ||
        !b->type->canMove || !b->type->canReclaim)
        return;
    // Negative target = a CORPSE (dead-unit record: bodies, statues, building
    // rubble), so the right-drag box and clicks can clear wrecks like features.
    float tx, tz;
    if (featureId < 0) {
        const Unit* c = unit(-featureId);
        // Accept anything dead or dying this tick whose def can be reclaimed
        // (the walk takes longer than the death anim anyway).
        if (!c || c->id == builderId || !c->type || (c->alive() && c->hp > 0))
            return;
        int ct = c->corpseStatue >= 0 ? c->corpseStatue : corpseTypeOf(c->type);
        if (ct < 0 || !featTypes_[size_t(ct)].reclaimable) return;
        tx = c->x; tz = c->z;
        static const bool kRcLog = std::getenv("TAK_BURNLOG") != nullptr;
        if (kRcLog)
            std::fprintf(stderr, "corpse-reclaim ORDER: b%d -> %s (%d)\n",
                         builderId, c->type->id.c_str(), -featureId);
    } else {
        const Feature* f = feature(featureId);
        if (!f || !f->alive) return;
        tx = f->x; tz = f->z;
    }
    // A reclaim is an ORDER, in the sequence the player gave it -- the same fix the
    // build queue needed. It used to go to a separate reclaimQueue that only ever
    // drained from inside tickReclaim, so unless a reclaim was ALREADY running
    // nothing started it: an area reclaim queued behind a move or a build sat
    // there for the rest of the game.
    if (!queue) b->orders.clear();
    Order o;
    o.x = tx; o.z = tz;
    o.goal = true;
    o.issuedTick = tickCounter_;
    o.reclaimFeat = featureId;
    b->orders.push_back(o);
}

void World::tickReclaim(Unit& b, float dt) {
    auto advance = [&] {
        // The job is over. Retire its order; the next one (which may be another
        // reclaim) simply becomes current.
        b.reclaimId = 0;
        if (!b.orders.empty() && b.orders.front().reclaimFeat) b.orders.erase(b.orders.begin());
    };
    if (b.reclaimId < 0) {
        // Ordered corpse reclaim: walk to the body/wreck and consume it. Yields
        // the corpse def's energy (0 for everything shipped) -- this is about
        // clearing rubble and denying resurrection, not income.
        Unit* c = unit(-b.reclaimId);
        if (!c || !c->type || c->deadFor >= c->corpseUntil) { advance(); return; }
        if (c->alive()) {
            // hp<=0 = dying THIS tick (the death edge may run after us): hold
            // the job. A genuinely healthy target is an invalid order: drop it.
            if (c->hp > 0) advance();
            return;
        }
        // Body still mid-death-anim: stand by until it settles (statues settle
        // instantly).
        if (c->deadFor < (c->corpseStatue >= 0 ? 0.0f : 4.0f)) return;
        float dx = c->x - b.x, dz = c->z - b.z;
        float reach = 24.0f + 8.0f * float(std::max(c->type->footX, c->type->footZ)) +
                      (b.type->buildDist > 0 ? b.type->buildDist : 0.0f);
        if (dx * dx + dz * dz > reach * reach) return;   // still walking there
        b.speed = 0;
        int ct = c->corpseStatue >= 0 ? c->corpseStatue : corpseTypeOf(c->type);
        const FeatType* cd = ct >= 0 ? &featTypes_[size_t(ct)] : nullptr;
        float step = kReclaimRate * dt;
        // Proportional drip against the INITIAL work (= max(energy,60), set at
        // death). Shipped corpse defs all have energy 0, so this grants nothing.
        if (cd && cd->energy > 0 && c->corpseWork > 0)
            players_[size_t(b.player)].mana +=
                cd->energy * std::min(step, c->corpseWork) /
                std::max(cd->energy, 60.0f);
        c->corpseWork -= step;
        if (c->corpseWork <= 0) {
            c->deadFor = 1000.0f;   // consumed
            if (c->corpseBlocks) {
                c->corpseBlocks = false;
                blockFoot(*c->type, c->x, c->z, false);
            }
            static const bool kRecLog = std::getenv("TAK_BURNLOG") != nullptr;
            if (kRecLog)
                std::fprintf(stderr, "corpse reclaimed: %s at %.0f,%.0f\n",
                             c->type->id.c_str(), c->x, c->z);
            advance();
        }
        return;
    }
    auto it = featureIdx_.find(b.reclaimId);
    if (it == featureIdx_.end()) { advance(); return; }
    Feature& f = features_[it->second];
    if (!f.alive) { advance(); return; }   // someone else got it (RECLAIMFAILED)
    float dx = f.x - b.x, dz = f.z - b.z;
    float reach = 24.0f + 8.0f * float(std::max(f.fx, f.fz)) +
                  (b.type->buildDist > 0 ? b.type->buildDist : 0.0f);
    if (dx * dx + dz * dz > reach * reach) return;   // still walking there
    // Never the RECLAIM order itself -- that entry IS the job, and it is what
    // holds the queue back until the feature is gone (advance() retires it).
    if (b.orders.empty() || !b.orders.front().reclaimFeat) dropLeg(b);
    b.speed = 0;
    float want = detmath::atan2(dx, dz);   // face the feature (deterministic)
    // Stopped (b.speed was zeroed just above), so this is a pivot: turninplacerate.
    float turn = std::clamp(angleDiff(want, b.heading), -b.type->turnInPlaceRate * dt,
                            b.type->turnInPlaceRate * dt);
    b.heading += turn;
    float d = std::min(f.work, kReclaimRate * dt);
    f.work -= d;
    players_[size_t(b.player)].mana +=
        f.manaYield * (d / f.workFull) * players_[size_t(b.player)].manaMult;   // drip (income-cheat scaled)
    if (f.work <= 0) {
        f.alive = false;
        bumpFeatGen();   // reclaimed away: stop drawing it
        if (f.blocks) {   // free the ground cells it occupied (setupMatch blocked nav_)
            blockCells(int(f.x) / 16 - f.fx / 2, int(f.z) / 16 - f.fz / 2, f.fx, f.fz, false);
        }
        advance();
    }
}

void World::repair(int builderId, int targetId, bool queue) {
    Unit* b = unit(builderId);
    Unit* t = unit(targetId);
    if (!b || !b->alive() || !b->type || !b->type->isBuilder || !b->type->canMove) return;
    if (!t || !t->alive() || !t->type || t->id == b->id || t->underConstruction ||
        !allied(t->player, b->player) || t->hp >= t->type->maxHp)
        return;
    // An order, in sequence, like construction and reclaim.
    if (!queue) b->orders.clear();
    Order o;
    o.x = t->x; o.z = t->z;
    o.goal = true;
    o.issuedTick = tickCounter_;
    o.repairTarget = targetId;
    b->orders.push_back(o);
}

// A builder repairs a damaged friendly: restores HP at the same rate it would build
// the unit (buildTime / workerTime), draining the owner's mana proportional to the
// HP restored -- pauses if the mana runs out.
void World::tickRepair(Unit& b, float dt) {
    Unit* t = unit(b.repairId);
    auto endRepair = [&] {
        b.repairId = 0;
        if (!b.orders.empty() && b.orders.front().repairTarget) b.orders.erase(b.orders.begin());
    };
    if (!b.type) { endRepair(); return; }
    // `t->hp <= 0` is NOT covered by alive(): that reads deadFor, which is only set
    // by the death sweep in tick(). A unit whose hp has already reached zero this
    // tick still looks alive here, and tickRepair runs BEFORE the sweep -- so
    // without this a builder repairs a unit back off zero and it never dies.
    if (!t || !t->alive() || !t->type || t->hp <= 0 || t->underConstruction ||
        t->embarked() || !allied(t->player, b.player) || t->hp >= t->type->maxHp) {
        endRepair();
        return;
    }
    float dx = t->x - b.x, dz = t->z - b.z;
    float half = 16.0f * float(std::max(t->type->footX, t->type->footZ)) / 2;
    float reach = std::max(half + 40.0f,
                           b.type->buildDist > 0 ? b.type->buildDist + half : 0.0f);
    if (dx * dx + dz * dz > reach * reach) {
        if (b.orders.empty()) order(b.id, t->x, t->z, false);   // (re)walk toward it
        return;
    }
    // ...but never the REPAIR order itself: that entry is the job.
    if (b.orders.empty() || !b.orders.front().repairTarget) dropLeg(b);
    b.speed = 0;
    float want = detmath::atan2(dx, dz);
    // Stopped: pivot at turninplacerate, not the moving turn rate.
    b.heading += std::clamp(angleDiff(want, b.heading), -b.type->turnInPlaceRate * dt,
                            b.type->turnInPlaceRate * dt);
    float total = t->type->buildTime / std::max(b.type->workerTime, 0.01f);
    Player& tm = players_[size_t(b.player)];
    float cost = t->type->buildCost * dt / std::max(total, 0.01f);
    if (tm.mana < cost) return;   // can't afford: pause the repair
    tm.mana -= cost;
    t->hp = std::min(t->type->maxHp, t->hp + t->type->maxHp * dt / std::max(total, 0.01f));
    if (t->hp >= t->type->maxHp) endRepair();   // mended: on to the next order
}

void World::startDisco(int player) {
    if (player < 0 || player >= int(players_.size())) return;
    auto& p = players_[size_t(player)];
    // One emote at a time: ignore if this player is already dancing OR headbanging
    // (so you can't stack or restart them). Deterministic across peers.
    if (p.discoLeft <= 0 && p.headbangLeft <= 0) p.discoLeft = 10.0f;
}

bool World::discoActive(int player) const {
    return player >= 0 && player < int(players_.size()) &&
           players_[size_t(player)].discoLeft > 0;
}

void World::startHeadbang(int player) {
    if (player < 0 || player >= int(players_.size())) return;
    auto& p = players_[size_t(player)];
    if (p.discoLeft <= 0 && p.headbangLeft <= 0) p.headbangLeft = 10.0f;
}

bool World::headbangActive(int player) const {
    return player >= 0 && player < int(players_.size()) &&
           players_[size_t(player)].headbangLeft > 0;
}

// Retire the queued build order a builder is sitting on, whatever ended the job
// (finished, cancelled, or the site turned out to be impossible). Until this runs
// the order blocks the queue, which is exactly what keeps the builder in place.
static void popBuildOrder(Unit& b) {
    if (!b.orders.empty() && b.orders.front().buildType) b.orders.erase(b.orders.begin());
}

void World::tickConstruction(Unit& b, float dt) {
    Unit* site = unit(b.buildSiteId);
    if (!site || !site->alive() || !site->underConstruction) {
        b.buildSiteId = 0;
        popBuildOrder(b);
        return;
    }
    site->beingBuilt = true;   // a builder is assigned (walking or working): no decay
    float dx = site->x - b.x, dz = site->z - b.z;
    float half = 16.0f * float(std::max(site->type->footX, site->type->footZ)) / 2;
    // Reach = the builder's FBI builddistance (to the site edge) when it has one,
    // else the default footprint-derived range.
    float reach = std::max(half + 40.0f,
                           b.type->buildDist > 0 ? b.type->buildDist + half : 0.0f);
    float gd = dx * dx + dz * dz;
    if (gd > reach * reach) {                          // out of range: still walking there
        // Give-up watchdog: if the builder gets no closer (~20px, squared 400) for ~8s,
        // it can't reach the site -- abandon it and pop the next queued build. Uses the
        // same fixed-dt / squared-distance idiom as the movement goalStuck watchdog, so
        // it stays deterministic (buildStuck* are non-hashed scratch, like goalStuck*).
        if (gd < b.buildStuckD - 400.0f) {             // real progress: reset the timer
            b.buildStuckD = gd; b.buildStuckT = 0;
        } else if ((b.buildStuckT += dt) > 8.0f) {
            b.buildStuckT = 0; b.buildStuckD = 1e30f;
            // Drop an un-started ghost site (as cancelBuilds does) so its marker and
            // blocked footprint don't linger.
            if (!site->buildBegun) {
                if (site->type && site->type->isStructure()) {
                    blockFoot(*site->type, site->x, site->z, false);
                }
                site->underConstruction = false;
                site->deadFor = 1000.0f;
            }
            b.buildSiteId = 0;
            popBuildOrder(b);
            
        }
        return;
    }
    b.buildStuckT = 0; b.buildStuckD = 1e30f;          // in range: reset the watchdog
    site->buildBegun = true;   // in range: the site starts materialising now
    // Drop the APPROACH leg, but never the queued build order itself: that entry
    // IS the job, and it is what holds the rest of the queue back until the
    // building is finished (popBuildOrder retires it when the job ends).
    if (b.orders.empty() || !b.orders.front().buildType) dropLeg(b);   // never the build order
    b.speed = 0;
    // Face what we're building/conjuring: turn toward the site at the unit's turn
    // rate (a building has turnRate 0, so it simply doesn't rotate).
    float want = detmath::atan2(dx, dz);
    // Stopped (b.speed was zeroed just above), so this is a pivot: turninplacerate.
    float turn = std::clamp(angleDiff(want, b.heading), -b.type->turnInPlaceRate * dt,
                            b.type->turnInPlaceRate * dt);
    b.heading += turn;
    float total = site->type->buildTime / std::max(b.type->workerTime, 0.01f);
    // Record the current build rate so an interrupted conjure decays at this speed.
    site->conjureRate = site->type->maxHp * 0.95f / std::max(total, 0.01f);
    Player& tm = players_[size_t(b.player)];
    if (gInstantBuild) {
        site->hp = site->type->maxHp;   // finishes this tick, free
    } else {
        float cost = site->type->buildCost * dt / std::max(total, 0.01f);
        if (tm.mana < cost) return;
        tm.mana -= cost;
        site->hp += site->type->maxHp * 0.95f * dt / std::max(total, 0.01f);
    }
    if (site->hp >= site->type->maxHp) {
        site->hp = site->type->maxHp;
        site->underConstruction = false;
        b.buildSiteId = 0;
        popBuildOrder(b);
    }
}

void World::decayConstruction(Unit& u, float dt) {
    // No builder worked this conjure this tick: it "un-conjures", losing HP at the
    // rate it was last built, and vanishes at zero (no corpse -- it was never
    // finished). A site that never materialised falls back to its nominal rate.
    float rate = u.conjureRate > 0 ? u.conjureRate
                 : u.type->maxHp * 0.95f / std::max(u.type->buildTime, 0.01f);
    u.hp -= rate * dt;
    if (u.hp > 0) return;
    if (u.type->isStructure()) {
        blockFoot(*u.type, u.x, u.z, false);
    }
    u.underConstruction = false;
    u.deadFor = 1000.0f;   // fully gone (painter skips deadFor>=4), no death anim
}

void World::train(int builderId, const UnitType* type, int count) {
    Unit* b = unit(builderId);
    if (!b || !b->alive() || !type) return;
    for (int i = 0, n = std::max(1, count); i < n; ++i) b->buildQueue.push_back(type);
}

void World::dequeue(int builderId, const UnitType* type, int count) {
    Unit* b = unit(builderId);
    if (!b || !b->alive() || !type) return;
    // Remove up to `count` copies of `type`, newest first (from the back), so
    // subtracting cancels the most-recently-queued rather than the in-progress front.
    for (int removed = 0; removed < count; ++removed) {
        int idx = -1;
        for (int i = int(b->buildQueue.size()) - 1; i >= 0; --i)
            if (b->buildQueue[size_t(i)] == type) { idx = i; break; }
        if (idx < 0) break;
        if (idx == 0) b->buildProgress = 0;   // canceling the in-progress front
        b->buildQueue.erase(b->buildQueue.begin() + idx);
    }
    // If the queue no longer holds a type set to infinite-repeat, stop repeating it
    // so the count actually reaches zero instead of refilling next tick.
    if (b->repeatType == type) {
        bool any = false;
        for (const auto* q : b->buildQueue) if (q == type) { any = true; break; }
        if (!any) b->repeatType = nullptr;
    }
}

int World::queuedCount(int builderId, const UnitType* type) const {
    const Unit* b = unit(builderId);
    if (!b || !type) return 0;
    int n = 0;
    for (const auto* q : b->buildQueue) if (q == type) ++n;
    return n;
}

void World::setRepeat(int builderId, const UnitType* type) {
    Unit* b = unit(builderId);
    if (!b || !b->alive() || !type) return;
    if (b->repeatType == type) {
        // ctrl+click the +++ icon again: stop now and clear what's pending.
        b->repeatType = nullptr;
        b->buildQueue.clear();
        b->buildProgress = 0;
    } else {
        b->repeatType = type;
        if (b->buildQueue.empty()) b->buildQueue.push_back(type);   // kick it off
    }
}

// [AdjustJoy]: a passive repair aura, not morale. Once a second an emitter counts the
// eligible units in its radius, then repairs each DAMAGED one at build power
// `adjustment * falloff / N`, paying its own owner's mana at the target's
// buildcost/buildtime rate (KINGDOMS.icd 0x51d3e0 -> 0x51f7b0 -> 0x429d90). Dividing
// by the full count -- damaged or not -- is what stops it being absurd in a crowd.
void World::tickHealAuras() {
    for (size_t i = 0; i < units_.size(); ++i) {
        Unit& s = units_[i];
        if (!s.alive() || s.embarked() || !s.type || s.type->auras.empty() ||
            s.underConstruction || !s.active || s.incapacitated())
            continue;
        for (const Aura& a : s.type->auras) {
            if (a.kind != Aura::Kind::Joy || a.radius <= 0 || a.amount <= 0) continue;
            float r2 = a.radius * a.radius;
            // A target must be alive, fully built, on the right side, and not the
            // emitter itself. Pass 1 counts them all; pass 2 heals the damaged ones.
            auto eligible = [&](const Unit& e, float d2) {
                return e.alive() && !e.embarked() && e.type && e.id != s.id &&
                       !e.underConstruction && d2 <= r2 &&
                       (!allied(e.player, s.player)) == a.affectsEnemy;
            };
            int n = 0;
            forEachNear(s.x, s.z, a.radius, [&](int idx) {
                const Unit& e = units_[size_t(idx)];
                float dx = e.x - s.x, dz = e.z - s.z;
                if (eligible(e, dx * dx + dz * dz)) ++n;
            });
            if (n == 0) continue;
            Player& tm = players_[size_t(s.player)];
            forEachNear(s.x, s.z, a.radius, [&](int idx) {
                Unit& e = units_[size_t(idx)];
                float dx = e.x - s.x, dz = e.z - s.z;
                float d2 = dx * dx + dz * dz;
                if (!eligible(e, d2)) return;
                if (e.hp >= e.type->maxHp) return;   // retail heals only the damaged
                // ... and never anything already at zero: alive() reads deadFor, so a
                // unit killed this tick still looks alive until the sweep in tick()
                // runs, and the aura pulse fires AFTER that loop. Healing it here
                // resurrects it (a self-destruct inside a friendly aura never died).
                if (e.hp <= 0) return;
                float power = a.amount * a.falloff(std::sqrt(d2)) / float(n);
                float prog = power / std::max(e.type->buildTime, 0.01f);
                // Deliberate divergence: retail bills the whole pulse even when only a
                // sliver of it lands, so topping off one unit can drain a player's
                // pool. Charge for the repair actually done instead.
                float need = 1.0f - e.hp / std::max(e.type->maxHp, 1.0f);
                prog = std::min(prog, need);
                float cost = e.type->buildCost * prog;
                if (cost > tm.mana) {   // short on mana: heal proportionally less
                    prog *= tm.mana / std::max(cost, 1e-6f);
                    cost = tm.mana;
                }
                tm.mana -= cost;
                e.hp = std::min(e.type->maxHp, e.hp + e.type->maxHp * prog);
            });
        }
    }
}

void World::tickAuras(float dt) {
    // The heal aura runs at its own 1 Hz cadence, hoisted above the crowd stride
    // below so a big battle can never swallow its tick.
    if (tickCounter_ % 30 == 0) tickHealAuras();
    // Projecting every aura through a dense crowd every tick is the top sim cost in a
    // massive battle (~50ms at 30k units). Buffs change slowly -- they take ~0.5s to
    // fade -- so above a crowd threshold refresh them every kAuraStride ticks instead
    // of every tick: a ~0.1s update granularity that's imperceptible in a huge melee
    // but cuts the aura cost by the stride. Below the threshold it runs every tick, so
    // normal play stays byte-identical. Deterministic (tick-count + unit-count gated,
    // identical on every peer). The relax scales with the stride so a buff still fades
    // over the same wall-clock ~0.5s after its projector stops refreshing it.
    uint32_t stride = units_.size() > 8000 ? 3u : 1u;
    if (tickCounter_ % stride != 0) return;   // buffs held between refreshes
    // Every unit's buffs relax back toward 1.0; aura projectors then refresh the
    // units in their radius, so a buff holds while in range and fades on leaving.
    float relax = std::min(1.0f, 2.0f * dt * float(stride));   // catch up skipped ticks
    for (auto& u : units_) {
        if (!u.alive()) continue;
        u.atkBuff += (1.0f - u.atkBuff) * relax;
        u.armBuff += (1.0f - u.armBuff) * relax;
    }
    for (size_t i = 0; i < units_.size(); ++i) {
        Unit& s = units_[i];
        if (!s.alive() || s.embarked() || !s.type || s.type->auras.empty() ||
            s.underConstruction || !s.active || s.incapacitated())
            continue;
        for (const Aura& a : s.type->auras) {
            if (a.kind == Aura::Kind::Joy || a.radius <= 0) continue;  // morale: not modelled
            float r2 = a.radius * a.radius;
            forEachNear(s.x, s.z, a.radius, [&](int idx) {
                Unit& e = units_[size_t(idx)];
                if (!e.alive() || e.embarked() || !e.type) return;
                bool enemy = !allied(e.player, s.player);
                if (enemy != a.affectsEnemy) return;   // buff friends OR debuff foes
                float dx = e.x - s.x, dz = e.z - s.z;
                float d2 = dx * dx + dz * dz;
                if (d2 > r2) return;
                // Retail's falloff is edge-at-the-CENTRE, full-at-the-RIM (see the
                // Aura comment); we had it the other way round, so a Shaman's +20%
                // was strongest where it should have been weakest.
                float amt = 1.0f + (a.amount - 1.0f) * a.falloff(std::sqrt(d2));
                float& buff = a.kind == Aura::Kind::Armor ? e.armBuff : e.atkBuff;
                buff = a.affectsEnemy ? std::min(buff, amt) : std::max(buff, amt);
            });
        }
    }
}

void World::tickAbilities(float dt) {
    // A corpse is a dead unit whose body still lies on the field: the death
    // anim has finished (4s) and decomposetime hasn't expired. Whether it can
    // be RAISED again is the corpse def's `resurrectable` (81 of the 150
    // shipped corpse defs; reclaiming works on any corpse).
    auto isCorpse = [](const Unit& c) {
        // Statues stand from the instant of death (retail: severity 0, no Dying
        // anim); ordinary bodies appear after the 4s death animation.
        float from = c.corpseStatue >= 0 ? 0.0f : 4.0f;
        return c.type && !c.alive() && c.deadFor >= from && c.deadFor < c.corpseUntil;
    };
    auto corpseDef = [&](const Unit& c) -> const FeatType* {
        int ct = c.corpseStatue >= 0 ? c.corpseStatue : corpseTypeOf(c.type);
        return ct >= 0 ? &featTypes_[size_t(ct)] : nullptr;
    };
    auto retire = [&](Unit& c) {
        c.deadFor = 1000.0f;
        if (c.corpseBlocks) {
            c.corpseBlocks = false;
            blockFoot(*c.type, c.x, c.z, false);
        }
    };
    struct Revive { const UnitType* type; float x, z; int player; bool animate; };
    std::vector<Revive> revives;
    const float kR = 56.0f;
    for (size_t i = 0; i < units_.size(); ++i) {
        Unit& u = units_[i];
        if (!u.alive() || !u.type || u.underConstruction || u.incapacitated() ||
            !u.orders.empty()) {
            if (u.reviveTarget) u.reviveTarget = 0;   // interrupted: channel drops
            continue;
        }
        bool caster = u.type->canResurrect || (u.type->canAnimate && u.type->animateType);
        if (!caster && !u.type->canReclaim) continue;

        // A revive is a CHANNEL, not an instant (retail order-0xA task states:
        // work = target buildtime / caster workertime, x0.3 for animate; mana
        // drains across the channel at the same total as the build cost).
        if (u.reviveTarget) {
            Unit* c = unit(u.reviveTarget);
            if (!c || !isCorpse(*c)) { u.reviveTarget = 0; continue; }
            float dx = c->x - u.x, dz = c->z - u.z;
            if (dx * dx + dz * dz > kR * kR * 4) { u.reviveTarget = 0; continue; }
            bool animate = u.reviveMode == 2;
            const UnitType* out = animate ? u.type->animateType : c->type;
            float totalMana = out->buildCost * (animate ? 0.3f : 1.0f);
            float inc = totalMana * dt / std::max(u.reviveTotal, 0.01f);
            Player& tm = players_[size_t(u.player)];
            if (tm.mana < inc) continue;   // starved: the channel stalls, not drops
            tm.mana -= inc;
            u.reviveLeft -= dt;
            if (u.reviveLeft <= 0) {
                revives.push_back({out, c->x, c->z, u.player, animate});
                retire(*c);
                u.reviveTarget = 0;
            }
            continue;
        }

        // Corpses are dead (not in the spatial grid), so scan directly.
        for (size_t j = 0; j < units_.size(); ++j) {
            Unit& c = units_[j];
            if (j == i || !isCorpse(c)) continue;
            float dx = c.x - u.x, dz = c.z - u.z;
            if (dx * dx + dz * dz > kR * kR) continue;
            const FeatType* cd = corpseDef(c);
            if (!cd) continue;
            if (u.type->canResurrect && c.player == u.player && cd->resurrectable) {
                // Channel length: buildtime / workertime seconds (retail
                // 0x4201e9), mana drained over it; unit returns at 10% HP.
                u.reviveTarget = c.id;
                u.reviveMode = 1;
                u.reviveTotal = u.reviveLeft =
                    std::max(c.type->buildTime / std::max(u.type->workerTime, 0.01f),
                             0.5f);
                break;
            } else if (u.type->canAnimate && u.type->animateType &&
                       cd->resurrectable) {
                // Animate (tarpries animatetype=MONGHOUL): ANY resurrectable
                // corpse -- friend or foe -- rises as the caster's creature,
                // at 0.3x the work and FULL HP (retail 0x420257/0x4206ba).
                u.reviveTarget = c.id;
                u.reviveMode = 2;
                u.reviveTotal = u.reviveLeft =
                    std::max(u.type->animateType->buildTime /
                                 std::max(u.type->workerTime, 0.01f) * 0.3f,
                             0.5f);
                break;
            } else if (u.type->canReclaim && cd->reclaimable) {
                // Retail yield = the corpse def's energy -- 0 for every shipped
                // corpse. You reclaim bodies to DENY resurrection, not for mana.
                players_[size_t(u.player)].mana += cd->energy;
                retire(c);
            }
        }
    }
    for (const auto& r : revives) {
        int id = spawn(r.type, r.x, r.z, 3.14159f, r.player);
        // Retail HP: resurrect returns the unit at 10% (min 1); an animated
        // creature rises at FULL health (icd 0x420666-0x4206ba).
        if (Unit* nu = unit(id))
            if (!r.animate) nu->hp = std::max(nu->type->maxHp * 0.1f, 1.0f);
        static const bool kRevLog = std::getenv("TAK_BURNLOG") != nullptr;
        if (kRevLog)
            std::fprintf(stderr, "%s: %s for p%d at %.0f,%.0f\n",
                         r.animate ? "animate" : "resurrect",
                         r.type->id.c_str(), r.player, r.x, r.z);
    }
}

bool World::sightClear(int ux, int uz, float eyeH, int tx, int tz) const {
    if (heights_.empty()) return true;
    auto H = [&](int x, int z) { return float(heights_[size_t(z) * hW_ + x]); };
    float tH = H(tx, tz);
    float dxf = float(tx - ux), dzf = float(tz - uz);
    float D = std::sqrt(dxf * dxf + dzf * dzf);
    if (D < 1.5f) return true;                    // adjacent cell: always visible
    // Walk the cells between unit and target (integer DDA); a cell blocks the view
    // if its ground rises above the straight eye->target sight line at that point.
    // Terrain must rise this far ABOVE the sight line to block it. Tuned (with the
    // eye height below) so walls/hills/cliffs cast shadows but small rock clutter
    // does not -- on athri cay real walls are height 60-220, so an obstacle must
    // clear ~eye+margin ~= 56 to block. Live-tunable via TAK_FOG_MARGIN.
    static float MARGIN = [] {
        const char* e = std::getenv("TAK_FOG_MARGIN"); return e ? float(std::atof(e)) : 16.0f;
    }();
    int steps = int(D);
    for (int i = 1; i < steps; ++i) {
        float t = float(i) / D;
        int x = int(std::lround(float(ux) + dxf * t));
        int z = int(std::lround(float(uz) + dzf * t));
        if (x < 0 || z < 0 || x >= hW_ || z >= hH_) continue;
        float lineH = eyeH + (tH - eyeH) * t;
        if (H(x, z) > lineH + MARGIN) return false;   // terrain pokes above the line
    }
    return true;
}

// Collect a finished fog pass and start the next one. Called every tick from the sim;
// the cadence timer decides when a new pass is DUE, this decides when one can actually
// start (never two at once -- visGather's placeholder dedup and the worker's cache
// writes both assume exclusive ownership of visMaskCache_).
void World::visPump() {
    if (visRunning_ && visDone_.load(std::memory_order_acquire)) {
        visWorker_.join();
        visRunning_ = false;
        visDone_.store(false, std::memory_order_relaxed);
        vis_.swap(visBack_);
        ++visGen_;   // renderer: fog content may have changed; re-upload once
        visHavePass_ = true;
    }
}

void World::updateVisibility() {
    if (nav_.empty()) return;
    if (visPlayer_ < 0) return;   // headless referee: nothing renders, no fog needed
    if (visRunning_) return;      // previous pass still going; skip this beat
    // The FIRST pass runs inline. Async fog lands a beat late, which is invisible mid-game
    // but would leave the opening frames (and a --shot capture) with an unpopulated fog
    // buffer -- the whole map dark. At world start there are only the handful of starting
    // units, so this one is cheap; it is the crowded steady state that needed the worker.
    //
    // The test is "have we ever PRODUCED a pass", not "is the buffer allocated": setTerrain
    // pre-fills vis_ with zeros so the opening frames render fully fogged rather than fully
    // bare, so vis_.empty() is already false by the time we first get here and the inline
    // path never ran. The safeguard was dead code, and --shot only looked right because it
    // waits seconds before capturing.
    bool first = !visHavePass_;
    if (vis_.empty()) {
        visW_ = nav_.width();
        visH_ = nav_.height();
        vis_.assign(size_t(visW_) * visH_, 0);
    }
    visGather();
    // The back buffer starts from the current fog so EXPLORED memory carries over; the
    // demote and the stamping both happen on it, off-thread, leaving vis_ readable.
    visBack_ = vis_;
    if (first || serialThreads_) {   // first pass, and single-threaded callers: inline
        visCompute();
        vis_.swap(visBack_);
        ++visGen_;
        visHavePass_ = true;
        return;
    }
    visRunning_ = true;
    visDone_.store(false, std::memory_order_relaxed);
    visWorker_ = std::thread([this] {
        visCompute();
        visDone_.store(true, std::memory_order_release);
    });
}

void World::visGather() {
    // PASS 1a (serial, cheap): gather every revealer's stamp parameters, and collect the
    // set of NEW LoS masks that need computing (deduped via an empty cache placeholder --
    // the ray-march that fills them runs in parallel in pass 1b). Bound the cache up front;
    // clearing mid-pass would desync the placeholder dedup. Fog is client-only display (the
    // referee returned above), so NONE of this is hashed.
    if (visMaskCache_.size() >= 8192) visMaskCache_.clear();
    std::vector<Reveal>& reveals = visReveals_;
    std::vector<Miss>&   misses  = visMisses_;
    reveals.clear();
    misses.clear();
    reveals.reserve(units_.size());
    for (const auto& u : units_) {
        // Shared team vision: every allied, built, living unit reveals fog.
        if (!u.alive() || !u.type || u.underConstruction || !allied(u.player, visPlayer_))
            continue;
        // Reveal to the greater of sight and radar range. Radar sees THROUGH terrain, so
        // the LoS test applies only to the sight area not already covered by radar; a unit
        // with radar >= sight does zero LoS work.
        int rSight = int(u.type->sight) / 16;
        int rRadar = int(u.type->radar) / 16;
        int r = std::max(rSight, rRadar) + 1;
        int rRadar2 = rRadar * rRadar;
        int cx = int(u.x) / 16, cz = int(u.z) / 16;
        bool losBlocks = !heights_.empty() && cx >= 0 && cz >= 0 && cx < hW_ && cz < hH_ &&
                         rSight > rRadar;   // only worth testing where sight exceeds radar
        uint64_t key = 0;
        if (losBlocks) {
            // LoS mask key: the visible-cell set for (sight, radar, cell) is a pure function
            // of the immutable heightmap -- computed ONCE, then re-stamped while a unit of
            // this class stands on the cell.
            key = (uint64_t(uint32_t(rSight)) << 52) | (uint64_t(uint32_t(rRadar)) << 40) |
                  (uint64_t(uint32_t(cx)) << 20) | uint32_t(cz);
            // emplace an empty placeholder: .second==true means this key is new this pass,
            // so it needs the ray-march (and dedups repeats of the same class+cell).
            if (visMaskCache_.emplace(key, std::vector<uint32_t>{}).second)
                misses.push_back({key, cx, cz, r, rRadar2});
        }
        reveals.push_back({cx, cz, r, rRadar2, losBlocks, key});
    }
}

// Everything below runs on visWorker_: it reads the immutable heightmap and the gathered
// reveal list, and writes only visMaskCache_ (exclusively owned while a pass is running)
// and visBack_ (not the buffer the renderer is reading).
void World::visCompute() {
    std::vector<Reveal>& reveals = visReveals_;
    std::vector<Miss>&   misses  = visMisses_;
    // Eye above the unit's ground cell: sees over small bumps, not over real
    // walls/hills. Paired with the sight-line MARGIN in sightClear so small rock
    // clutter stops casting fog shadows. Live-tunable via TAK_FOG_EYE.
    static float EYE = [] {
        const char* e = std::getenv("TAK_FOG_EYE"); return e ? float(std::atof(e)) : 40.0f;
    }();
    // Demote last pass's visible cells: to EXPLORED (1, dimmed but remembered) normally, or
    // straight to hidden (0) when fog memory is off, so they go dark again once out of sight.
    for (auto& v : visBack_)
        if (v == 2) v = fogExplored_ ? 1 : 0;
    // PASS 1b (PARALLEL): the O(r^3) LoS ray-march for each new mask -- the expensive part
    // a moving army keeps retriggering. Each computes independently, reading only the
    // immutable heightmap into a local buffer (no shared-map writes); we install them
    // serially afterwards. This is what makes fog cheap at scale for a seated player too.
    if (!misses.empty()) {
        std::vector<std::vector<uint32_t>> built(misses.size());
        auto march = [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) {
                const Miss& m = misses[i];
                std::vector<uint32_t>& mask = built[i];
                float eyeH = float(heights_[size_t(m.cz) * hW_ + m.cx]) + EYE;
                for (int dz = -m.r; dz <= m.r; ++dz)
                    for (int dx = -m.r; dx <= m.r; ++dx) {
                        int dd = dx * dx + dz * dz;
                        if (dd > m.r * m.r) continue;
                        int x = m.cx + dx, z = m.cz + dz;
                        if (x < 0 || z < 0 || x >= visW_ || z >= visH_) continue;
                        // Inside radar range: revealed unconditionally. Beyond it, LoS-gated.
                        if (dd > m.rRadar2 && !sightClear(m.cx, m.cz, eyeH, x, z)) continue;
                        mask.push_back(uint32_t(size_t(z) * visW_ + x));
                    }
            }
        };
        size_t mn = misses.size();
        unsigned hw = serialThreads_ ? 1u : std::thread::hardware_concurrency();
        unsigned nth = std::min<unsigned>(hw ? hw : 1u, unsigned((mn + 15) / 16));  // heavy: ~16/thread
        if (nth <= 1) {
            march(0, mn);
        } else {
            size_t chunk = (mn + nth - 1) / nth;
            std::vector<std::thread> th;
            th.reserve(nth - 1);
            for (unsigned k = 1; k < nth; ++k) {
                size_t bb = std::min(mn, size_t(k) * chunk), ee = std::min(mn, bb + chunk);
                if (bb < ee) th.emplace_back(march, bb, ee);
            }
            march(0, std::min(mn, chunk));
            for (auto& t : th) t.join();
        }
        for (size_t i = 0; i < mn; ++i) visMaskCache_[misses[i].key] = std::move(built[i]);
    }
    // PASS 2 (parallel): stamp visBack_[i]=2 for every revealer's cells (cached mask or
    // circle fill). It only READS the now-warm cache and writes the constant 2, and two
    // threads can land on the same cell where reveal circles overlap.
    //
    // This used to claim that was "a benign race, no locks needed" because both threads
    // store the same value. That is not a thing C++ says: concurrent non-atomic writes to
    // the same object are a data race and therefore UB, no matter how equal the values --
    // and it would trip any race detector pointed at this. Do a relaxed atomic store
    // instead: on every target we build for, a relaxed 1-byte store is the same
    // instruction as the plain one, so this is free at runtime and merely makes the
    // guarantee real. Relaxed is enough because nothing ORDERS off these writes; the
    // worker's join in visPump() is what publishes the finished buffer.
    //
    // Not std::atomic_ref, which is the obvious spelling and does not exist in Apple's
    // libc++ -- it broke the macOS release build and nothing local caught it, since
    // libstdc++ has had it since GCC 10. __atomic_store_n is the same store and is
    // available on both compilers we build with.
    auto put = [&](size_t i) {
        __atomic_store_n(&visBack_[i], uint8_t(2), __ATOMIC_RELAXED);
    };
    auto stamp = [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i) {
            const Reveal& rv = reveals[i];
            if (!rv.los) {
                for (int dz = -rv.r; dz <= rv.r; ++dz)
                    for (int dx = -rv.r; dx <= rv.r; ++dx) {
                        if (dx * dx + dz * dz > rv.r * rv.r) continue;
                        int x = rv.cx + dx, z = rv.cz + dz;
                        if (x < 0 || z < 0 || x >= visW_ || z >= visH_) continue;
                        put(size_t(z) * visW_ + x);
                    }
                continue;
            }
            auto mi = visMaskCache_.find(rv.key);   // guaranteed present after pass 1
            if (mi != visMaskCache_.end())
                for (uint32_t idx : mi->second) put(idx);
        }
    };
    size_t n = reveals.size();
    unsigned hw = serialThreads_ ? 1u : std::thread::hardware_concurrency();
    unsigned nth = std::min<unsigned>(hw ? hw : 1u, unsigned((n + 255) / 256));  // >=256/thread
    if (nth <= 1) {
        stamp(0, n);
    } else {
        size_t chunk = (n + nth - 1) / nth;
        std::vector<std::thread> th;
        th.reserve(nth - 1);
        for (unsigned k = 1; k < nth; ++k) {
            size_t bb = std::min(n, size_t(k) * chunk), ee = std::min(n, bb + chunk);
            if (bb < ee) th.emplace_back(stamp, bb, ee);
        }
        stamp(0, std::min(n, chunk));
        for (auto& t : th) t.join();
    }
}

void World::summonReadyGods() {
    if (!godsEnabled_) return;
    for (size_t t = 0; t < players_.size(); ++t) {
        if (!godReady(int(t))) continue;
        // The god appears among the player's forces: the centroid of everything it
        // has standing. Accumulated in unit order over a container every peer builds
        // identically, so the sum -- and therefore the spawn point -- is bit-identical.
        float cx = 0, cz = 0;
        int n = 0;
        for (const auto& u : units_)
            if (u.alive() && u.player == int(t) && u.type && !u.underConstruction) {
                cx += u.x;
                cz += u.z;
                ++n;
            }
        // Mark it handled either way: a player with nothing left on the map does not
        // get to bank the summon until it rebuilds.
        players_[t].godSummoned = true;
        if (!n || !players_[t].godType) continue;
        spawn(players_[t].godType, cx / float(n), cz / float(n), 3.14159f, int(t));
    }
}

// Nearest point to (fx,fz) that a `t`-sized body fits in AND nothing is standing on.
// Returns false (leaving out* at the requested point) when the whole neighbourhood is
// taken, so the caller can decide whether to wait or to proceed anyway.
bool World::exitSpot(const UnitType* t, float fx, float fz, float& outX, float& outZ) const {
    outX = fx;
    outZ = fz;
    if (!t) return false;
    const NavGrid& g = navFor(t);
    if (g.empty()) return false;
    const int foot = footCells(t);
    // Clearance to keep from the nearest body: our own half-width plus a little, so
    // the spot is somewhere this unit can actually come to rest.
    const float clr = float(std::max(t->footX, t->footZ)) * 8.0f + 10.0f;
    const int cx = int(fx) / 16, cz = int(fz) / 16;
    for (int r = 0; r <= 12; ++r)
        for (int j = -r; j <= r; ++j)
            for (int i = -r; i <= r; ++i) {
                if (std::max(std::abs(i), std::abs(j)) != r) continue;   // ring only
                const int nx = cx + i, nz = cz + j;
                if (!g.fits(nx, nz, foot)) continue;
                const float wx = float(nx) * 16 + 8, wz = float(nz) * 16 + 8;
                bool taken = false;
                forEachNear(wx, wz, clr, [&](int idx) {
                    const Unit& e = units_[size_t(idx)];
                    if (!e.alive() || !e.type) return;
                    const float dx = e.x - wx, dz = e.z - wz;
                    if (dx * dx + dz * dz < clr * clr) taken = true;
                });
                if (taken) continue;
                outX = wx;
                outZ = wz;
                return true;
            }
    return false;
}

void World::tickProduction(Unit& u, float dt) {
    if (u.underConstruction || u.buildQueue.empty()) return;
    const UnitType* t = u.buildQueue.front();
    float total = t->buildTime / std::max(u.type->workerTime, 0.01f);
    Player& tm = players_[size_t(u.player)];
    // Accumulate work (spending mana) until complete. Once complete, buildProgress
    // holds at `total` and grows only as a wait timer below.
    if (u.buildProgress < total) {
        if (gInstantBuild) {
            u.buildProgress = total;   // finishes this tick, free
        } else {
            float cost = t->buildCost * dt / std::max(total, 0.01f);
            if (tm.mana < cost) return;   // stalled: no mana
            tm.mana -= cost;
            u.buildProgress += dt;
        }
        if (u.buildProgress < total) return;   // not done yet
    }
    // At the per-player unit cap: hold the finished unit inside the building (no mana
    // spent) until a slot frees, rather than popping the queue and spawning it.
    if (atUnitCap(u.player)) return;
    // Same for the FBI totalallowed cap (one dragon/god/juggernaut per player): hold
    // the finished unit until the one already fielded dies, instead of spawning a
    // second. The mana is already spent, so it emerges the moment a slot frees.
    if (atTypeCap(u.player, t)) return;
    // Finished: the conjured unit emerges just south of the footprint and walks to
    // a rally point. Hold it until that tile is clear so a repeat/queued build
    // doesn't stack units on top of each other -- but never wait forever (2.5s cap)
    // if the exit is jammed.
    // The exit tile, and the spot the unit will actually EMERGE on. Both used to be
    // the one fixed tile just south of the footprint: production waited up to 2.5s for
    // it to clear and then spawned there anyway, so a factory running continuously
    // stacked bodies on one another. Search for a free spot instead, and only fall
    // back to waiting when the whole neighbourhood is genuinely full.
    const float ex = u.x, ez = u.z + float(u.type->footZ) * 8 + 20;
    float sx = ex, sz = ez;
    const bool haveSpot = exitSpot(t, ex, ez, sx, sz);
    if (!haveSpot && u.buildProgress < total + 2.5f) {
        u.buildProgress += dt;   // nowhere to put it yet (no mana spent)
        return;
    }
    u.buildProgress = 0;
    u.buildQueue.erase(u.buildQueue.begin());
    int producerId = u.id, player = u.player;
    // spawn() may reallocate units_, invalidating `u`; capture the id and re-fetch.
    int id = spawn(t, sx, sz, 3.14159f, player);
    // Then walk it clear of the exit, to a spot that is free RIGHT NOW -- computed
    // after the spawn, so it also avoids the unit we just put down.
    //
    // This used to be `order(id, sx + ((id % 5) - 2) * 22, sz + 60, false)`: a fan of
    // five fixed points keyed on the unit id. With five lanes and no memory of what is
    // already standing there, the sixth unit out of a factory was ordered onto the
    // exact spot the first was still occupying. Bodies are solid and the steering has
    // no local avoidance (that is the router's job, and the router cannot route INTO
    // an occupied cell either), so it pushed at its own countryman for ever. The
    // no-headway watchdog does not save it: replaceLeg resets the tracker and the
    // router re-issues about once a second, so the tracker can never build up (see the
    // KNOWN LIMITATION note on replaceLeg). Reported as "built units always try to go
    // to the same spot, forever", which is exactly what it did.
    //
    // Deterministic: a fixed outward ring scan over the nav grid and the unit grid,
    // both of which every peer builds identically.
    float gx = sx, gz = sz + 60.0f;
    if (exitSpot(t, gx, gz, gx, gz)) order(id, gx, gz, false);
    // ...then hand it the factory's RALLY plan, queued behind that step. A production
    // building accepts move / fight-move / patrol / attack orders (and queues them) --
    // it cannot act on them itself, so they describe what its OUTPUT should do. The
    // step above still runs first so the unit clears the doorway before setting off.
    if (const Unit* pb = unit(producerId))
        for (const Order& ro : pb->rally) {
            Unit* nu = unit(id);
            if (!nu) break;
            // A targeted rally (attack) whose target is already dead is skipped rather
            // than handed over as an order pointing at nothing.
            if (ro.targetId) {
                const Unit* tg = unit(ro.targetId);
                if (!tg || !tg->alive()) continue;
            }
            nu->orders.push_back(ro);
            nu->orders.back().issuedTick = tickCounter_;
        }
    if (Unit* pu = unit(producerId)) {
        pu->justBuilt = id;
        // Auto-join: a squad-member producer's new MOBILE unit joins its squad.
        if (pu->squad && t && !t->isStructure())
            if (Unit* nu = unit(id)) nu->squad = pu->squad;
        // Infinite build: re-queue so the next one starts once this one clears.
        if (pu->buildQueue.empty() && pu->repeatType)
            pu->buildQueue.push_back(pu->repeatType);
    }
}

// How close to the start or the goal a parked body stops blocking the search. Two
// cells: measured against 4 and 8 on a 24-unit convergence, 2 is the only one where
// every unit arrives (24/24 in 246s; 4 and 8 both stall at 23/24 and time out).
constexpr int kParkedFreeCells = 2;

void World::tick(float dt) {
    ++tickCounter_;
#ifndef NDEBUG
    hashTrace();   // TAK_HASHTRACE=lo:hi -- per-component dump, EVERY tick on both peers
#endif
    {
        auto _b = std::chrono::steady_clock::now();
        tickBurning();   // feature fire: spread + burn-out (deterministic, hashed)
        if (g_phase) g_burnMs += std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - _b).count();
    }
    // Benchmark: fire the staged spawn plan at its scheduled ticks. Deterministic -- both
    // the client sim and the referee run the identical plan (built in setupMatch), so
    // lockstep holds. It IS hashed (real sim state), which is fine: every peer agrees.
    while (benchCursor_ < benchPlan_.size() && benchPlan_[benchCursor_].tick <= tickCounter_) {
        for (const auto& bs : benchPlan_[benchCursor_].units)
            spawn(bs.type, bs.x, bs.z, 0, bs.player);
        ++benchCursor_;
    }
    std::chrono::steady_clock::time_point _tk0, _sep0;
    if (g_phase) { _tk0 = std::chrono::steady_clock::now();
                   g_tcomb = 0;
                   g_visMs = g_burnMs = g_gridMs = 0; }
    // Cap repaths per tick: a big group that jams while moving can trip the
    // blocked/stuck watchdogs en masse, and hundreds of path searches in one tick
    // stall the sim. Deferred units retry a later tick. Deterministic (fixed budget,
    // unit-index order), so lockstep peers stay in sync.
    // RETAIL FIDELITY: zero. Retail has no pathfinder at all -- its PathNavigator
    // keeps a two-point segment [position, goal] and searches for nothing
    // (docs/retail-engine.md) -- so neither do we. A* repathing was ours.
    //
    // Know what this costs before changing it back. Measured, 30 units sent the
    // length of a map:
    //                    Angvir's Maze   Athri Cay   Inner Circle
    //     budget 24         15/30          22/30        24/30
    //     budget  0          0/30          21/30         0/30
    // At zero, every unit keeps its order (they no longer give up, see the
    // pathExists fix) but the whole group wedges ~31% along and never moves again:
    // straight-line steering slides along a wall and never commits to moving AWAY
    // from the goal, so concave geometry traps it for good.
    //
    // That is WORSE than retail, not equal to it. Retail units wedged occasionally;
    // ours wedge universally, because we copied retail's absence of a search without
    // yet matching its local avoidance (the mover's clamp-and-slide, and the ~6-tick
    // re-anchor of the segment). The fix is to improve the mover until units stop
    // needing a search -- at which point this zero costs nothing -- NOT to put the
    // budget back. Deliberate call: be faithful now, sharpen the steering later.
    for (auto& u : units_) { u.justFired = false; u.justBuilt = 0; }
    if (mission_ || scenario_) justDied_.clear();   // deaths this tick, fed to mission/scenario below
    hits_.clear();   // per-tick weapon impacts (drained by the viewer for sounds/fx)
    clock_ += dt;    // wall-clock since the match started (for god timing)
    // Cosmetic disco emote countdown (Shift+D). Deterministic across peers but not
    // hashed -- drives client-side monarch dancing only.
    for (auto& tm : players_) {
        if (tm.discoLeft > 0) tm.discoLeft = std::max(0.0f, tm.discoLeft - dt);
        if (tm.headbangLeft > 0) tm.headbangLeft = std::max(0.0f, tm.headbangLeft - dt);
    }

    // Economy: recompute income/storage, apply income.
    for (auto& tm : players_) { tm.income = 0; tm.storage = 0; }
    std::vector<int> godPriests(players_.size(), 0);
    for (auto& u : units_) {
        // A unit under construction contributes no economy until it finishes -- a
        // half-built lodestone must not add its mana income or storage capacity yet.
        if (!u.alive() || !u.type || u.underConstruction) continue;
        auto& tm = players_[size_t(u.player)];
        tm.income += u.type->income;
        tm.storage += u.type->storage;
        if (u.type->attractsGods) godPriests[size_t(u.player)]++;
    }
    // Difficulty income cheat: scale the summed income so the boost flows through the
    // mana accrual below, allied surplus sharing, and god-favour alike. manaMult is 1
    // for everyone but an Absurd AI, so this is an exact no-op (x1.0) otherwise.
    for (auto& tm : players_) tm.income *= tm.manaMult;
    // Apply income, then share the economy across allies: mana that would
    // overflow a player's storage flows to teammates that still have headroom,
    // so a maxed-out ally feeds the team instead of wasting mogrium. It is truly
    // wasted only when the whole team is capped. Deterministic -- collected and
    // handed out in player-index order. A solo/FFA player (a team of one) has no
    // teammate to receive the surplus, so this reduces exactly to the old clamp.
    for (auto& tm : players_) tm.mana += tm.income * dt;
    const int np = int(players_.size());
    auto cap = [&](int i) { return std::max(players_[size_t(i)].storage, 100.0f); };
    bool teamDone[kMaxPlayers] = {};
    for (int lead = 0; lead < np; ++lead) {
        int team = players_[size_t(lead)].team;
        if (team < 0 || team >= kMaxPlayers || teamDone[team]) continue;
        teamDone[team] = true;
        float pool = 0;   // surplus above caps, gathered from the whole team
        for (int i = 0; i < np; ++i)
            if (players_[size_t(i)].team == team && players_[size_t(i)].mana > cap(i)) {
                pool += players_[size_t(i)].mana - cap(i);
                players_[size_t(i)].mana = cap(i);
            }
        for (int i = 0; i < np && pool > 0; ++i)
            if (players_[size_t(i)].team == team) {
                float give = std::min(cap(i) - players_[size_t(i)].mana, pool);
                if (give > 0) { players_[size_t(i)].mana += give; pool -= give; }
            }
        // Any pool left (every member capped) is wasted, as before.
    }
    // God favour: a player's priests channel its mana income into favour while any
    // is present; it fills toward kGodFavorNeeded, then godReady() lets the god come.
    if (godsEnabled_)
        for (size_t t = 0; t < players_.size(); ++t)
            if (godPriests[t] > 0)
                players_[t].godFavor = std::min(kGodFavorNeeded,
                    players_[t].godFavor + std::max(players_[t].income, 20.0f) * dt);

    // ...and once it has filled, the god manifests. This has to happen HERE, in the
    // shared sim, and used to happen in the client instead (GameView::simStep polled
    // godReady() and called its own summonGod()). The referee never did it, so from
    // the first summon the server's world held one fewer unit than every client's and
    // the hashes split for the rest of the match -- a desync arriving tens of minutes
    // in, with nothing in the command stream to explain it.
    summonReadyGods();

    // Index-based: tickProduction can spawn a trained unit, reallocating
    // units_ and invalidating any range-for iterator over it.
    for (size_t i = 0, n = units_.size(); i < n; ++i) {
        Unit& u = units_[i];
        if (u.alive() && u.type) tickProduction(u, dt);
    }
    // Construction: clear the per-tick "worked" flag, let builders add HP (which
    // re-sets it on the sites they walk to / work), then bleed any orphaned conjure
    // that no builder touched this tick until it dies and vanishes.
    for (auto& u : units_)
        if (u.alive() && u.underConstruction) u.beingBuilt = false;
    for (size_t i = 0; i < units_.size(); ++i) {
        Unit& u = units_[i];
        if (u.alive() && u.type && u.buildSiteId) tickConstruction(u, dt);
        else if (u.alive() && u.type && u.reclaimId) tickReclaim(u, dt);
        else if (u.alive() && u.type && u.repairId) tickRepair(u, dt);
    }
    for (auto& u : units_)
        if (u.alive() && u.type && u.underConstruction && !u.beingBuilt)
            decayConstruction(u, dt);

    visPump();   // land a finished async fog pass (cheap; a swap or nothing at all)
    visTimer_ -= dt;
    if (visTimer_ <= 0) {
        // Flat 0.25s. This used to widen to 0.5s with the crowd, to halve the RATE of the
        // periodic fog spike -- and at 3800 units it sat pinned at the 0.5s ceiling, which
        // is exactly the half-second hitch that showed up on hardware. The pass is async
        // now, so there is no spike left to space out and no reason to make a big battle
        // (the case that most wants current fog) reveal at half rate. A pass still in
        // flight simply skips the beat. Fog is client-only display, never hashed.
        visTimer_ = 0.25f;
        {
        auto _v = std::chrono::steady_clock::now();
        updateVisibility();
        if (g_phase) g_visMs += std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - _v).count();
    }
    }

    // Projectiles.
    for (auto& p : projectiles_) {
        // Guided (FBI type=Guided): steer toward the target's CURRENT position,
        // clamped to the weapon's turnrate, so a homing shot (Tracking Arrow, Ball
        // Lightning, the dragons' fireballs) chases a target that keeps walking
        // instead of flying through where it used to be. Retail's answer to kiting;
        // without it these 576-9000 damage shots simply missed anything mobile.
        // A homer whose target is gone just flies on straight and fizzles.
        if (p.wsrc && p.wsrc->kind == Weapon::Kind::Guided && p.wsrc->turnRate > 0) {
            if (const Unit* gt = unit(p.targetId); gt && gt->alive() && !gt->embarked()) {
                float speed = detmath::len(p.vx, p.vz);
                if (speed > 0.01f) {
                    float cur = detmath::atan2(p.vx, p.vz);
                    float want = detmath::atan2(gt->x - p.x, gt->z - p.z);
                    float d = angleDiff(want, cur);
                    float maxTurn = p.wsrc->turnRate * dt;
                    float nh;
                    if (std::abs(d) <= maxTurn) {
                        // Correction complete: retail SNAPS onto the bearing and, in
                        // the same store, rebuilds the velocity from the per-sub-step
                        // speed -- which its mover then applies once per sub-step. The
                        // shot really does accelerate to subSteps x nominal while it
                        // is tracking, and stays there: the clamped branch below only
                        // rotates the vector, preserving whatever magnitude it has.
                        nh = want;
                        speed = p.wsrc->projVel * float(p.wsrc->subSteps);
                    } else {
                        nh = cur + (d > 0 ? maxTurn : -maxTurn);
                    }
                    p.vx = detmath::sin(nh) * speed;
                    p.vz = detmath::cos(nh) * speed;
                }
            }
        }
        float ox = p.x, oz = p.z;        // segment start (before this step)
        p.x += p.vx * dt;
        p.z += p.vz * dt;
        p.life -= dt;
        p.age += dt;
        Unit* t = unit(p.targetId);
        // A falling bomb is ABOVE everything until it lands, so it takes no
        // in-flight collision -- it detonates once, on the ground, below.
        bool bomb = p.wsrc && p.wsrc->kind == Weapon::Kind::Dropped;
        if (!bomb && t && t->alive() && !t->embarked()) {
            // Distance from the target to the segment travelled this tick, so a
            // fast projectile (e.g. the totem's lightning, ~50px/tick) can't
            // skip past the small hit radius between ticks.
            float sx = p.x - ox, sz = p.z - oz;
            float seg = sx * sx + sz * sz;
            float u = seg > 0 ? ((t->x - ox) * sx + (t->z - oz) * sz) / seg : 0.0f;
            u = std::clamp(u, 0.0f, 1.0f);
            float cx = ox + u * sx, cz = oz + u * sz;
            float dx = t->x - cx, dz = t->z - cz;
            float r = t->type ? 8.0f + 8.0f * float(std::max(t->type->footX,
                                                             t->type->footZ)) : 8.0f;
            if (dx * dx + dz * dz < r * r) {
                // Apply the impact: direct hit + area splash (per the weapon's
                // FBI areaofeffect), using the grid from the previous rebuild.
                if (p.wsrc) applyHit(*p.wsrc, t->x, t->z, p.fromPlayer, p.fromId, t);
                else if (!(benchmarkMode() && t->type && t->type->commander)) {
                    t->hp -= p.damage;
                    if (t->hp <= 0) {
                        t->overkill = std::max(t->overkill, -t->hp);
                        t->deathType = p.wsrc ? uint8_t(p.wsrc->dmgType) : 1;
                    }
                }
                p.life = -1;
                p.spent = true;   // connected: don't also crater at expiry
            }
        }
    }
    // A released bomb detonates the moment it reaches the ground, wherever it has
    // drifted to -- unlike a fired shot, which is a dud if it runs out of life.
    // Whatever it lands ON takes the direct hit: several bombs carry no
    // areaofeffect at all (tarbeak's Egg Bomb), and a splash-only detonation would
    // make those deal nothing whatsoever.
    // ...and so does an ordinary BALLISTIC shell that missed. A retail ballistic
    // projectile has no lifetime at all: it flies its arc under gravity and always
    // comes down, and the terrain impact runs the ordinary area-damage path with no
    // hit unit (KINGDOMS.icd 0x52a6f3 -> 0x529c10). Ours simply vanished, so every
    // missed siege shell -- 32 ballistic weapon blocks with a real blast radius,
    // the catapults, trebuchets and mortars -- dealt nothing whatsoever.
    // A guided or hitscan shot that runs out is still a silent dud, which is retail
    // too: those DO carry a range-derived expiry that kills them without damage.
    for (size_t bi = 0; bi < projectiles_.size(); ++bi) {
        const Projectile bp = projectiles_[bi];
        if (bp.spent || bp.life > 0 || !bp.wsrc) continue;
        bool dropped = bp.wsrc->kind == Weapon::Kind::Dropped;
        // Ballistic = the plain arcing shot: not guided, not a hitscan beam, not melee.
        bool ballistic = bp.wsrc->kind == Weapon::Kind::Normal &&
                         !bp.wsrc->beam && !bp.wsrc->melee;
        // Splash weapons only. Retail also strikes whatever feature sits in the
        // impact cell even for a shot with no areaofeffect, but our applyHit damages
        // that centre cell unconditionally -- so porting it literally would have
        // every stray arrow chopping and igniting forests. Deliberate divergence.
        if (!dropped && !(ballistic && bp.wsrc->aoe > 0.0f)) continue;
        // A bomb is let go over its target, so whatever it lands on takes the DIRECT
        // hit -- several bombs carry no areaofeffect at all and a splash-only
        // detonation would deal nothing. A missed shell has no such victim: retail's
        // terrain impact carries no hit unit, and a unit genuinely standing there
        // would have been caught by the in-flight test instead.
        Unit* under = nullptr;
        if (dropped) {
            float bestD = 1e30f;
            forEachNear(bp.x, bp.z, 40.0f, [&](int idx) {
                Unit& e = units_[size_t(idx)];
                if (!e.alive() || e.embarked() || !e.type || allied(e.player, bp.fromPlayer)) return;
                float dx = e.x - bp.x, dz = e.z - bp.z;
                float d = dx * dx + dz * dz;
                if (d < bestD) { bestD = d; under = &e; }
            });
        }
        applyHit(*bp.wsrc, bp.x, bp.z, bp.fromPlayer, bp.fromId, under);
    }
    std::erase_if(projectiles_, [](const Projectile& p) { return p.life <= 0; });

    rebuildGrid();   // spatial hash for this tick (combat acquire + separation)
    rebuildOccupancy();   // who is parked where: units are solid (see sim.h)

    // Retail's pathfinder: one integer work budget split across every pending
    // request, each search resuming where it left off (icd 0x416430). Runs
    // after occupancy so a search scores exactly what the mover will see.
    if (pathService_) paths_.tick(
        [&](int unitId, int cx, int cz) {
            const Unit* u = unit(unitId);
            if (!u || !u->type) return int(kCellImpassable);
            const int sc = cellScore(u->type, cx, cz, unitId);
            if (sc != kCellOccupied) return sc;   // terrain or clear: unchanged
            // A PARKED BODY BLOCKS THE SEARCH.
            //
            // cellScore grades one kCellOccupied, which EQUALS kCellThreshold --
            // retail's model, where every icd call site compares against 4, so a
            // parked body is passable-but-costly. That is why a unit ordered through
            // a standing crowd walked into it and stopped: the tracer routed straight
            // through, because doing so was legal by its own scoring, and the mover --
            // for which bodies are solid and which has no local avoidance -- had
            // nowhere to go. Measured before this: a wall of parked units 1-8 cells
            // deep stopped the mover dead every time, and no amount of tightening the
            // route SHORTCUT helped, because the shortcut was never the cause.
            //
            // Retail fidelity is knowingly traded here. Retail's mover nudges its way
            // past a body; ours cannot, so a cost the original could afford to ignore
            // is a wall for us.
            //
            // Two exemptions, and the change does not work without them:
            //   * near the START, or a unit standing inside its own idle army could
            //     not path OUT of it -- every neighbouring cell would be blocked;
            //   * near the GOAL, or ordering units onto ground where anything already
            //     stands would fail to route at all, which is most move orders in a
            //     base.
            const int ux = int(u->x) / 16, uz = int(u->z) / 16;
            if (std::max(std::abs(cx - ux), std::abs(cz - uz)) <= kParkedFreeCells)
                return sc;
            if (!u->orders.empty()) {
                const Order& leg = u->orders[currentLeg(u->orders)];
                const int gx = int(leg.x) / 16, gz = int(leg.z) / 16;
                if (std::max(std::abs(cx - gx), std::abs(cz - gz)) <= kParkedFreeCells)
                    return sc;
            }
            return int(kCellImpassable);
        },
        [&](int unitId, const std::vector<PathCell>& route, float gx, float gz) {
            Unit* u = unit(unitId);
            if (route.empty()) {           // failed: back off before retrying
                pathRetryAt_[unitId] = tickCounter_ + kPathFailBackoff;
                return;
            }
            pathRetryAt_.erase(unitId);
            if (!u || !u->alive() || u->orders.empty()) return;
            // Only the leg this search was issued for; anything queued behind
            // it stays untouched.
            // Snap the final waypoint to the caller's exact goal ONLY when the
            // route actually reached the goal cell. A route clipped at the
            // 64-waypoint limit stops short, and pointing its last waypoint at
            // the distant goal sends the unit charging straight at whatever lies
            // between -- it gets blocked, re-asks, gets another clipped route,
            // and ping-pongs. Measured: a unit oscillating between two points
            // 2426px of travel later, having never arrived.
            const bool reachedGoal =
                !route.empty() && route.back().x == int(gx) / 16 &&
                route.back().z == int(gz) / 16;
            // SHORTCUT the traced route before installing it ("string pulling").
            //
            // The tracer emits a waypoint at every change of direction, and a march
            // across open ground alternates orthogonal and diagonal steps -- so a
            // completely unobstructed trip came back as a staircase of ~one waypoint
            // per cell, hit the 64-waypoint cap before reaching the goal, and made the
            // unit visibly jink at every one of them (the mover aims within 3px of an
            // intermediate waypoint). Going around an obstacle had the same problem in
            // the large: the trace hugs the outline, so a unit followed the far side of
            // a plateau instead of cutting the corner once it was past.
            //
            // Keep only the waypoints that are actually needed: from where we are, take
            // the FARTHEST waypoint still reachable in a straight line this body fits
            // through, jump to it, repeat. Scanning from the end means open ground
            // costs one test and collapses to a single waypoint.
            //
            // Starting from the unit's CURRENT cell (not the cell it occupied when the
            // search was requested) also drops the waypoints it has already walked past
            // while the search was in flight -- which used to reconnect the route behind
            // the unit and send it backtracking.
            const NavGrid& ng = navFor(u->type);
            std::vector<PathCell> pulled;
            if (!ng.empty()) {
                PathCell at{int(u->x) / 16, int(u->z) / 16};
                size_t from = 0;
                while (from < route.size() && pulled.size() < 64) {
                    size_t take = from;
                    for (size_t j = route.size(); j-- > from;)
                        if (lineOpen(u->type, unitId, at.x, at.z, route[j].x, route[j].z)) {
                            take = j;
                            break;
                        }
                    pulled.push_back(route[take]);
                    at = route[take];
                    if (take + 1 >= route.size()) break;
                    from = take + 1;
                }
            }
            const std::vector<PathCell>& useRoute = pulled.empty() ? route : pulled;
            std::vector<Order> path;
            path.reserve(useRoute.size());
            for (size_t i = 0; i < useRoute.size(); ++i) {
                Order o;
                o.x = float(useRoute[i].x) * 16.0f + 8.0f;
                o.z = float(useRoute[i].z) * 16.0f + 8.0f;
                if (i + 1 == useRoute.size() && reachedGoal) { o.x = gx; o.z = gz; }
                path.push_back(o);
            }
            // A route clipped at 64 waypoints stops short of where the player
            // actually sent the unit, and replaceLeg marks the LAST waypoint as
            // the leg's goal -- so installing a clipped route silently threw the
            // real destination away. The unit then walked to the end of each
            // clipped route, asked for another, and shuffled between them for
            // ever: measured 2426px of travel in 60s, never arriving, and the
            // no-headway watchdog never fired because from its point of view the
            // unit kept reaching its goal. Keep the destination on the end.
            if (!reachedGoal) {
                Order last;
                last.x = gx;
                last.z = gz;
                path.push_back(last);
            }
            replaceLeg(*u, path);
        });

    // Re-request, the way retail's navigator re-anchors instead of asking once.
    // A search is capped at (w+h)*20 cell visits (icd 0x414797), so a long haul
    // legitimately fails -- but the unit is meanwhile walking its straight
    // segment, and from closer in the same search succeeds. Without this a unit
    // that failed once never routes at all, which is most of a big map.
    //
    // Staggered by unit id so the queue does not spike on one tick, and driven
    // off the tick counter, so it stays identical on every peer.
    if (pathService_ && !nav_.empty()) {
        for (auto& u : units_) {
            if (!u.alive() || u.embarked() || !u.type) continue;
            if (u.type->canFly || u.type->isStructure() || !u.type->canMove) continue;
            if (u.orders.empty()) continue;
            // CHEAP TESTS FIRST. currentLeg() scans the order queue, and a unit
            // carrying a fresh 64-waypoint route makes that scan 64 long -- doing
            // it for every unit on every tick cost more than the searches it was
            // scheduling (41.5ms/tick against 19.5 baseline, almost all of it
            // here). The stagger already discards 29 ticks in 30.
            if ((tickCounter_ + uint32_t(u.id)) % kPathRetryTicks != 0) continue;
            if (paths_.pending(u.id)) continue;       // a search is already running
            if (auto it = pathRetryAt_.find(u.id);
                it != pathRetryAt_.end() && tickCounter_ < it->second) continue;
            const Order& leg = u.orders[currentLeg(u.orders)];
            if (leg.targetId != 0) continue;          // chasing, not travelling
            // Deliberately NOT skipped when the unit is already moving: the
            // periodic re-ask IS the mechanism. A search is capped at (w+h)*20
            // cell visits, so one route rarely spans a long trip -- the unit
            // follows what it got, asks again from further along, and chains its
            // way there. Skipping progressing units to save budget dropped a
            // journey from 97% of the way to 53%.
            requestPath(u, leg.x, leg.z);
        }
    }

    // Auto-acquire re-scan period, widened with the crowd: target acquisition is the
    // dominant sim cost in a huge battle (each idle armed unit scans its neighbourhood
    // every acqStride_ ticks), so as unit counts climb we rescan LESS often -- a 0.5s
    // re-acquire delay is imperceptible in a 5000-unit melee but roughly halves the
    // acquisition cost there. Deterministic: derived from the live-unit count, which is
    // identical on every lockstep peer.
    {
        uint32_t live = 0;
        for (const auto& u : units_) if (u.alive() && u.type) ++live;
        acqStride_ = std::clamp<uint32_t>(4 + live / 700, 4, 16);
        // (Goal quantization used to coarsen with the crowd here as well -- 2x2 cell
        // blocks up to 8x8 -- so that fewer distinct goal blocks meant fewer full-map
        // flow-field builds per tick, which was the dominant cost at 10k+ units. Both
        // the quantization and the flow fields are gone; only acqStride_ above
        // survives. Deterministic: it keys off the live count.)
    }


    // Formations (Unit::squad < 0): each tick, compute the group's centre + slowest
    // member speed, then walk idle stragglers back toward the centre so they congregate.
    // The mover below paces grouped members to `slowest`; members behind the centre
    // (relative to the goal) keep their own speed to catch up. Deterministic: the sums
    // accumulate in unit-index order and use only basic arithmetic + detmath::len.
    constexpr float kFormBehind = 48.0f;   // sprint if this far behind the group (goal-relative)
    constexpr float kFormRejoin = 140.0f;  // an idle member this far from the centre rejoins
    struct FormAgg { double sx = 0, sz = 0; int n = 0; float slowest = 1e9f; };
    FormAgg forms[kMaxPlayers][11] = {};   // [player][1..10]; slot 0 unused
    auto formOf = [&](const Unit& u) -> FormAgg* {
        if (u.squad >= 0 || u.player < 0 || u.player >= kMaxPlayers) return nullptr;
        return &forms[u.player][-u.squad];
    };
    for (const auto& u : units_)
        if (u.alive() && u.type && u.squad < 0)
            if (FormAgg* f = formOf(u)) {
                f->sx += u.x; f->sz += u.z; ++f->n;
                f->slowest = std::min(f->slowest, u.type->maxVel);
            }
    for (auto& u : units_) {
        if (!u.alive() || !u.type || u.squad >= 0 || !u.orders.empty()) continue;
        if (u.type->isStructure() || u.underConstruction) continue;   // buildings don't rejoin
        FormAgg* f = formOf(u);
        if (!f || f->n <= 1) continue;
        float cx = float(f->sx / f->n), cz = float(f->sz / f->n);
        if (detmath::len(u.x - cx, u.z - cz) > kFormRejoin) order(u.id, cx, cz, false);
    }

    for (auto& u : units_) {
        if (!u.type) continue;
        if (!u.alive()) {
            u.deadFor += dt;
            // CorpseAdjustX/Z: the wreck art sits offset from the building's
            // centre (arakeep corpseadjustz=2). Shift the record -- and its nav
            // block -- once, the moment the death anim ends and the corpse
            // appears (retail places the corpse feature at the adjusted cell).
            if (u.deadFor >= 4.0f && u.deadFor - dt < 4.0f &&
                u.deadFor < u.corpseUntil &&
                (u.type->corpseAdjX != 0 || u.type->corpseAdjZ != 0)) {
                if (u.corpseBlocks) blockFoot(*u.type, u.x, u.z, false);
                u.x = std::clamp(u.x + float(u.type->corpseAdjX) * 16.0f,
                                 8.0f, float(terW_) * 16.0f - 8.0f);
                u.z = std::clamp(u.z + float(u.type->corpseAdjZ) * 16.0f,
                                 8.0f, float(terH_) * 16.0f - 8.0f);
                if (u.corpseBlocks) blockFoot(*u.type, u.x, u.z, true);
            }
            // The body decomposed (or was never a corpse): fully gone. Records
            // explicitly retired at 1000 stay put.
            if (u.deadFor >= u.corpseUntil && u.deadFor < 999.0f) {
                u.deadFor = 1000.0f;
                if (u.corpseBlocks) {   // blocking wreck finally clears the ground
                    u.corpseBlocks = false;
                    blockFoot(*u.type, u.x, u.z, false);
                }
            }
            continue;
        }
        if (u.hp <= 0) {
            if (u.player >= 0 && u.player < int(players_.size()))
                players_[size_t(u.player)].losses++;   // end-of-game "Losses" column
            // Award the destroyed unit's experiencepoints to the killer, then set
            // its veteran level = accumulatedXP / the killer's OWN experiencepoints,
            // capped at 10 (retail KINGDOMS.icd). No HP-pool change — veterancy
            // works through the attack/armor multipliers, not a bigger health bar.
            if (u.lastHitBy && u.type) {
                Unit* k = unit(u.lastHitBy);
                // Credit the killer's player with an enemy kill (F4 overlay). Counts
                // even if the killer is a structure or has since died -- what
                // matters is who landed the fatal blow, not that it still lives.
                if (k && k->type && !allied(k->player, u.player) &&
                    k->player >= 0 && k->player < int(players_.size()))
                    players_[size_t(k->player)].kills++;
                if (k && k->alive() && k->type && k->type->canMove &&
                    !k->type->noVeteran) {
                    // One veteran level per kill, capped at 10 (retail counts
                    // kills, not the victim's value — so a cheap unit that lands
                    // a single big kill doesn't jump straight to max veterancy).
                    k->xp += 1;
                    k->veteran = std::min(10, k->xp);
                }
            }
            if (mission_ || scenario_) justDied_.push_back(u.id);
            // [EXPLODEAS]: the unit detonates its own death weapon where it stands
            // (Kamikaze Rat, Grenadier, Fire Demon, Balloon...). Credit the blast to
            // the dying unit's OWNER so it can't hurt its own side (applyHit skips
            // allies) and so kills count for the player who built it. Deferred to a
            // queue: applyHit walks units_ and can kill others, and we are mid-sweep
            // over units_ -- draining after the loop keeps that safe and keeps the
            // order deterministic (sweep order, which is id order).
            if (u.type->hasExplodeAs)
                deathBlasts_.push_back({&u.type->explodeAs, u.x, u.z, u.player, u.id});
            // Corpse window: the body lies reclaimable (and, if its corpse def
            // says so, resurrectable) until decomposetime runs out. Gibbed
            // (overkill >= maxHp -- placeholder severity rule pending the icd
            // Killed RE) or corpse-less units vanish with the death anim.
            {
                // Retail severity (icd 0x512610): ((overkill% + HP% one second
                // before death) / 2), clamped 1..100. Passed to the COB Killed.
                float okPct = u.overkill * 100.0f / std::max(u.type->maxHp, 1.0f);
                u.severity = uint8_t(std::clamp((okPct + float(u.hpPct1s)) * 0.5f,
                                                1.0f, 100.0f));
                // Dying while petrified/frozen leaves the FBI stone=/frozen=
                // STATUE feature instead of the corpse (retail deathType 0xE/0xF
                // path, 0x512d2a) -- blocking, permanent, and resurrectable
                // (raising a statue un-petrifies the unit).
                u.corpseStatue = u.stonedFor > 0 ? statueTypeOf(u.type, false)
                              : u.frozenFor > 0 ? statueTypeOf(u.type, true) : -1;
                static const bool kStatLog2 = std::getenv("TAK_BURNLOG") != nullptr;
                if (kStatLog2 && (u.stonedFor > 0 || u.frozenFor > 0))
                    std::fprintf(stderr, "statue edge: %s statue=%d stoned=%.1f\n",
                                 u.type->id.c_str(), u.corpseStatue, u.stonedFor);
                int ct = u.corpseStatue >= 0 ? u.corpseStatue : corpseTypeOf(u.type);
                // Retail gib rule (icd 0x512610): deathType = the killing blow's
                // FBI damagetype; 3 (explosion) makes Killed refuse the corpse
                // and EXPLODE every piece. An unfinished conjure never leaves a
                // corpse (corpseType forced 0 at 0x5127f5). Statues place
                // unconditionally.
                bool gib = (u.deathType == 3 || u.underConstruction) &&
                           u.corpseStatue < 0;
                // A self-destructed unit leaves nothing: it is not gibbed (that
                // is the explosion type) and it does not lie there as a wreck
                // either -- it fades. Statues still place, as they do for every
                // other death.
                if (u.deathType == Unit::kDeathSelfDestruct && u.corpseStatue < 0)
                    ct = -1;
                if (ct >= 0 && !gib) {
                    if (u.corpseStatue < 0 && isWater(u.x, u.z)) {
                        // Retail water graves sink and fade in seconds, never
                        // decompose, never get reclaimed (icd 0x512fbe).
                        u.corpseUntil = 4.0f + 2.5f;
                    } else {
                        int d30 = featTypes_[size_t(ct)].decomposeTicks;
                        // decomposetime 0 = never rots (building wrecks linger
                        // until reclaimed, like retail).
                        u.corpseUntil = d30 > 0 ? 4.0f + float(d30) / 30.0f : 1e9f;
                    }
                } else {
                    u.corpseUntil = 4.0f;
                }
                u.corpseWork = std::max(ct >= 0 ? featTypes_[size_t(ct)].energy : 0.0f,
                                        60.0f);   // ~0.5s minimum consume time
                // A dead structure frees its nav footprint -- unless its wreck
                // BLOCKS (arakeep_dead blocking=1; a destroyed wall's ARAWALL
                // feature likewise), which keeps the cells occupied until the
                // wreck is reclaimed or rots. (Fixes a long-standing gap:
                // destroyed buildings never unblocked at all.)
                if (u.type->isStructure()) {
                    bool wreckBlocks =
                        u.corpseUntil > 4.0f &&
                        ct >= 0 && featTypes_[size_t(ct)].blocking;
                    if (wreckBlocks) {
                        u.corpseBlocks = true;
                    } else {
                        blockFoot(*u.type, u.x, u.z, false);
                    }
                }
            }
            u.deadFor = 0; u.orders.clear(); u.speed = 0; continue;
        }

        // Retail 1-second HP-percent samples (unit+0x110/+0x111): the Killed
        // severity reads the PREVIOUS sample, i.e. your health ~1s before death.
        if (tickCounter_ % 30 == 0) {
            u.hpPct1s = u.hpPctCur;
            u.hpPctCur = uint8_t(std::clamp(u.hp / std::max(u.type->maxHp, 1.0f)
                                            * 100.0f, 0.0f, 100.0f));
        }
        // Status timers count down; HP regenerates (healtime); mana recharges.
        if (u.frozenFor > 0) u.frozenFor = std::max(0.0f, u.frozenFor - dt);
        if (u.stonedFor > 0) u.stonedFor = std::max(0.0f, u.stonedFor - dt);
        if (u.paralyzedFor > 0) u.paralyzedFor = std::max(0.0f, u.paralyzedFor - dt);
        if (u.selfDestructT >= 0.0f) {   // armed self-destruct: tick down, then blow up
            u.selfDestructT -= dt;
            if (u.selfDestructT <= 0.0f) {
                u.selfDestructT = -1.0f;
                // NOT an explosion. Retail's self-destruct mission (icd
                // 0x4017e0, the handler behind SelfDestruct /
                // UNITMISSIONCODE_SELFDESTRUCT) ends by applying 30000 damage of
                // DAMAGE TYPE 5 to the unit itself. Type 5 is not the explosion
                // type -- 3 is, and 3 is the one that gibs -- and it takes its
                // own branch in the death handler (0x5126a9), distinct from
                // every other type, setting a flag none of them set.
                //
                // What the player sees, reported from retail: the unit does not
                // blow up. It quietly leaves your command and fades, with no
                // wreck left behind.
                u.hp = 0; u.lastHitBy = 0; u.deathType = Unit::kDeathSelfDestruct;
            }
        }
        // Deadly water (.ota waterdoesdamage): a ground unit standing in it is
        // burned down. Flyers are over it, not in it; ships and amphibians belong
        // there, so anything that can cross water is exempt.
        if (waterDamage_ > 0 && !u.type->canFly && u.type->maxWaterDepth <= 0 &&
            isWater(u.x, u.z)) {
            u.hp -= waterDamage_ * dt;
            if (u.hp <= 0) { u.overkill = std::max(u.overkill, -u.hp); u.deathType = 1; }
        }
        // `u.hp > 0` matters because of WHERE hp gets zeroed, not by how much.
        // Every other death path puts hp <= 0 outside this loop body, so the sweep
        // at the top (3912) catches it and `continue`s -- never reaching this line.
        // The self-destruct expiry a few lines up is the one that fires INSIDE the
        // body, after this unit's own sweep has already gone by, so its kill waits
        // for the next tick and regen gets to run first. Without the guard a
        // regenerating unit healed straight back off zero and never died at all,
        // which is nearly every unit: of the 202 types this registry loads from a
        // retail install, 200 have healtime > 0 (only aranull and npcwagon do not).
        // That is why Ctrl+Shift+D self-destruct appeared to do nothing.
        if (u.type->healTime > 0 && u.hp > 0 && u.hp < u.type->maxHp)
            u.hp = std::min(u.type->maxHp, u.hp + dt / u.type->healTime);
        if (u.type->maxMana > 0 && u.mana < u.type->maxMana)
            u.mana = std::min(u.type->maxMana, u.mana + u.type->manaRegen * dt);
        // A WANDERER keeps its spawn point as home -- that anchor is what stops its
        // stroll turning into a migration.
        if (u.orders.empty() && !u.type->wanders) { u.homeX = u.x; u.homeZ = u.z; }
        // Wildlife and villagers roam. Retail gives them Standby_wander and they
        // amble; ours stood like statues on every campaign and scenario map. An idle
        // wanderer strolls every ~8 seconds, staggered by id so a herd doesn't move
        // as one, rolled on the sim's deterministic RNG -- and always to a point
        // near HOME, so they mill about their patch instead of drifting off it.
        if (u.type->wanders && u.orders.empty() && !u.underConstruction &&
            u.type->maxVel > 0 && (uint32_t(u.id) + tickCounter_) % 240 == 0) {
            float ang = float(burnRand(628)) / 100.0f;
            float r = float(burnRand(96));
            order(u.id, u.homeX + detmath::sin(ang) * r, u.homeZ + detmath::cos(ang) * r, false);
        }

        // Cloaking: drains player mana; an enemy within mincloakdistance forces a
        // decloak, and so does running dry of mana.
        if (u.type->canCloak && u.cloakOn) {
            bool enemyNear = false;
            float md = std::max(u.type->minCloakDist, 1.0f);
            forEachNear(u.x, u.z, md, [&](int idx) {
                const Unit& e = units_[size_t(idx)];
                if (e.alive() && !e.embarked() && !allied(e.player, u.player) && e.type) {
                    float dx = e.x - u.x, dz = e.z - u.z;
                    if (dx * dx + dz * dz <= md * md) enemyNear = true;
                }
            });
            float cost = (u.speed > 3.0f ? u.type->cloakCostMove : u.type->cloakCost) * dt;
            Player& tm = players_[size_t(u.player)];
            if (!enemyNear && tm.mana >= cost) { tm.mana -= cost; u.cloaked = true; }
            else u.cloaked = false;
        } else if (u.cloaked) {
            u.cloaked = false;   // toggled off (or no longer a cloaker): decloak now
        }

        if (u.underConstruction) continue;   // silent until finished
        if (u.embarked()) {                  // riding a transport
            Unit* t = unit(u.inTransport);
            if (t && t->alive()) { u.x = t->x; u.z = t->z; }
            else { if (mission_ || scenario_) justDied_.push_back(u.id); u.deadFor = 0; }  // transport lost with all hands
            continue;
        }
        // Frozen / petrified / paralyzed: the unit is inert this tick.
        if (u.incapacitated()) { u.speed = 0; continue; }
        // A build order that has reached the front claims its ground NOW, before
        // the builder has walked anywhere. Waiting until arrival would leave the
        // spot unreserved for the whole walk, so two builders sent to the same
        // place would both pass canPlace and one would arrive to find it taken.
        // The actual startBuild happens after this loop: it spawns the site, which
        // can reallocate units_ and leave every Unit& here dangling -- an earlier
        // version called it inline and read the order queue out of freed memory.
        if (!u.orders.empty() && u.orders.front().buildType && u.buildSiteId == 0)
            buildDue_.push_back(u.id);
        // Reclaim needs no deferral: consuming a feature spawns nothing, so
        // nothing can reallocate units_ underneath us.
        if (!u.orders.empty() && u.orders.front().reclaimFeat && u.reclaimId == 0)
            u.reclaimId = u.orders.front().reclaimFeat;
        if (!u.orders.empty() && u.orders.front().repairTarget && u.repairId == 0)
            u.repairId = u.orders.front().repairTarget;

        if (!u.orders.empty() && (u.orders.front().load || u.orders.front().unload))
            tickTransport(u, dt);
        else if (g_phase) { auto _c0=std::chrono::steady_clock::now(); tickCombat(u, dt); g_tcomb += std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-_c0).count(); }
        else tickCombat(u, dt);

        bool combatHold =
            !u.orders.empty() && u.orders.front().targetId != 0 &&
            !u.orders.front().load && !u.orders.front().guard && [&] {
                Unit* t = unit(u.orders.front().targetId);
                if (!t) return false;
                float dx = t->x - u.x, dz = t->z - u.z;
                // Melee holds at footprint adjacency (must agree with tickCombat's
                // gates, else a unit standing at contact is dragged back into
                // walking by the radial test below).
                int slot = u.type->weapons.empty()
                               ? 0 : std::clamp(u.weaponSlot, 0, int(u.type->weapons.size()) - 1);
                const Weapon* sel =
                    slot < int(u.type->weapons.size()) ? &u.type->weapons[slot] : nullptr;
                if (sel && sel->melee && t->type)
                    return meleeInRange(u.type, t->type, dx, dz);
                // Pad by a structure target's footprint half-extent so "holding in
                // range" agrees with tickCombat's fire gate (else a unit stopped at a
                // building's edge gets dragged back into moving).
                float pad = t->type->maxVel <= 0.0f
                    ? 8.0f * float(std::max(t->type->footX, t->type->footZ)) + 24.0f : 0.0f;
                if (std::sqrt(dx * dx + dz * dz) > (u.type->maxRange() + pad) * 0.95f) return false;
                // Don't sit still with no shot: a ranged unit whose line to the
                // target is blocked keeps moving to get around the wall.
                bool needLoS = u.type->maxRange() > 64.0f && !u.type->canFly &&
                               t->type && !t->type->canFly && u.type->canMove &&
                               !u.type->lobs();
                return !needLoS ||
                       nav_.losBetween(u.x, u.z, t->x, t->z,
                                       std::max(u.type->footX, u.type->footZ) / 2,
                                       std::max(t->type->footX, t->type->footZ) / 2);
            }();
        if (combatHold) continue;

        if (u.orders.empty()) {
            u.speed = std::max(0.0f, u.speed - u.type->brake * dt);
        } else if (u.orders.front().wait > 0.0f) {
            // SetMission "w N": hold position while the scripted wait counts down.
            u.speed = std::max(0.0f, u.speed - u.type->brake * dt);
            u.orders.front().wait -= dt;
            if (u.orders.front().wait <= 0.0f) u.orders.erase(u.orders.begin());
        } else if (u.orders.front().waitAttack) {
            // SetMission "wa": ambush -- hold until a non-allied unit is within sight,
            // then release to the next order (usually an attack).
            u.speed = std::max(0.0f, u.speed - u.type->brake * dt);
            float sight = u.type->sight > 0 ? u.type->sight : 200.0f;
            bool threat = false;
            for (const auto& e : units_)
                if (e.alive() && !e.embarked() && e.type && !allied(e.player, u.player)) {
                    float dx = e.x - u.x, dz = e.z - u.z;
                    if (dx * dx + dz * dz <= sight * sight) { threat = true; break; }
                }
            if (threat) u.orders.erase(u.orders.begin());
        } else {
            const Order& o = u.orders.front();
            float dx = o.x - u.x, dz = o.z - u.z;
            float dist = std::sqrt(dx * dx + dz * dz);
            if (o.guard && dist <= 70.0f) continue;   // in escort position
            // A FINAL move goal completes a little short, so a crowd sharing one
            // destination settles into a blob (spread by separation) instead of every
            // unit fighting for the exact same point. (Described as "flow-field orders"
            // when the arrival radius came from the field; it is the o.goal flag now.)
            // Retail move goals are AREAS -- NavGoalCircle / NavGoalRect / NavGoalRing
            // in the RTTI, not points (docs/retail-engine.md). A FINAL goal therefore
            // completes on a circle wide enough to hold a body: 16px, or the unit's own
            // footprint if that is bigger, so a 4x4 trebuchet is not asked to stand on
            // the same pixel a swordsman would. Intermediate route waypoints keep the tight
            // 3px so a route is actually followed.
            // (This was `o.flow ? 16 : 3`, which quietly became 3 for EVERY unit once the
            // retail-nav experiment stopped setting o.flow -- a whole group then fought
            // over one pixel, which is exactly what it looked like.)
            float foot = float(std::max(u.type->footX, u.type->footZ)) * 8.0f;
            float arrive = o.goal ? std::max(16.0f, foot) : 3.0f;
            if (dist < arrive) {
                if (o.buildType || o.reclaimFeat || o.repairTarget)
                    continue;   // the job is claimed above; its order blocks the queue
                if (o.targetId == 0) {
                    Order done = o;
                    u.orders.erase(u.orders.begin());
                    if (done.patrol) u.orders.push_back(done);
                }
                continue;
            }
            float want = detmath::atan2(dx, dz);
            float diff = angleDiff(want, u.heading);
            float maxTurn = u.type->turnRate * dt;
            u.heading += std::clamp(diff, -maxTurn, maxTurn);

            // Brake into the waypoint if it's the last one; slow for big turns.
            float target = u.type->maxVel;
            // Formation pacing: a pure-move member keeps to the group's slowest speed,
            // UNLESS it's behind the centre relative to the goal (a straggler), in which
            // case it sprints at its own speed to catch up (see the FormAgg pass above).
            if (u.squad < 0 && o.targetId == 0) {
                if (FormAgg* f = formOf(u); f && f->n > 1) {
                    float cx = float(f->sx / f->n), cz = float(f->sz / f->n);
                    // "Behind the centre" is measured against the leg the group is
                    // walking NOW -- against the last QUEUED leg a straggler check
                    // would compare everyone to a point nobody is heading for yet.
                    const Order& legEnd = u.orders[currentLeg(u.orders)];
                    float gx = legEnd.x, gz = legEnd.z;
                    float uToGoal = detmath::len(u.x - gx, u.z - gz);
                    float cToGoal = detmath::len(cx - gx, cz - gz);
                    if (uToGoal <= cToGoal + kFormBehind) target = std::min(target, f->slowest);
                }
            }
            // Terrain speed factors, retail-style (icd 0x51be12): ROAD first --
            // whole footprint on road cells -- else shallow WATER, never both
            // (a road bridge over the river keeps the road bonus).
            if (!u.type->canFly) {
                if (u.type->roadMult != 1.0f &&
                    onRoad(u.x, u.z, u.type->footX, u.type->footZ)) {
                    target *= u.type->roadMult;
                    static const bool kRoadLog = std::getenv("TAK_ROADLOG") != nullptr;
                    if (kRoadLog) {
                        static int logged = 0;
                        if (logged < 5)
                            std::fprintf(stderr, "road boost: %s x%.2f (%d)\n",
                                         u.type->id.c_str(), u.type->roadMult, ++logged);
                    }
                } else if (u.type->waterMult != 1.0f && !depth_.empty()) {
                    int cx = int(u.x) / 16, cz = int(u.z) / 16;
                    if (cx >= 0 && cz >= 0 && cx < terW_ && cz < terH_ &&
                        depth_[size_t(cz) * terW_ + cx] > 0)
                        target *= u.type->waterMult;
                }
            }
            // Retail couples turning to braking (icd 0x4da4ce): it works out how far
            // the unit will travel while it finishes the turn it still owes, and
            // brakes when the remaining distance is inside twice that arc -- so a
            // sharp corner slows you down in proportion to how sharp it is, instead
            // of our old flat "over 0.8 rad, drop to 30%" cliff.
            float turnRate = std::max(u.type->turnRate, 1e-4f);
            float arcDist = u.speed * std::abs(diff) / turnRate;
            if (dist < 2.0f * arcDist) target = 0;
            // Retail's stop-distance test measures to a DIFFERENT path point than the
            // arc test does, and we have only one `dist` (to the current order point).
            // Feeding it both tests unguarded would stop the unit at every route node, so
            // the stop test stays on the final leg, where our `dist` means what its
            // does.
            bool last = u.orders.size() == 1;
            if (last) {
                float stopDist = u.speed * u.speed / (2 * u.type->brake);
                if (dist < stopDist) target = 0;
            }
            if (u.speed < target)
                u.speed = std::min(u.speed + u.type->accel * dt, target);
            else
                u.speed = std::max(u.speed - u.type->brake * dt, target);

            float mx = detmath::sin(u.heading) * u.speed * dt;
            float mz = detmath::cos(u.heading) * u.speed * dt;
            // Collide ground/water units with the nav grid so they can't walk
            // through walls and buildings (pathfinding routes around, but direct
            // steering in combat did not). Slide along a blocked axis.
            if (u.type->canFly) {
                u.x += mx; u.z += mz;
            } else {
                const NavGrid& g = navFor(u.type);
                // The old test was "is the destination CELL free", plus an escape
                // hatch skipping it entirely while a unit stayed inside its own
                // cell -- so between crossings a body crept into its neighbour
                // unopposed, measured 13px in where footprints touch at 32.
                auto free = [&](float nx, float nz) {
                    // Footprint-aware, and the SAME grid the pathfinder used, so a unit
                    // never stalls on a cell its own path routed it through.
                    if (!(g.empty() || g.fits(int(nx) / 16, int(nz) / 16, footCells(u.type))))
                        return false;
                    // ...and units are solid, tested against the bodies themselves
                    // rather than the cells they happen to be stamped into.
                    return bodyPenetration(u, nx, nz) <= 0.0f;
                };
                if (free(u.x + mx, u.z + mz)) { u.x += mx; u.z += mz; }
                else if (free(u.x + mx, u.z)) { u.x += mx; }
                else if (free(u.x, u.z + mz)) { u.z += mz; }
                else {
                    // Fully blocked (a wall dead-ahead the straight path clipped):
                    // stop and repath around it toward the final destination, so
                    // the unit routes around instead of wedging permanently.
                    // Retail does NOT stop a blocked unit dead -- it clamps the move
                    // and caps speed (0.5x on the first refusal, 0.4x once it has
                    // been refused twice running), so the unit keeps pressing and
                    // resumes the instant the way clears. Stopping outright is what
                    // turns a momentary jam into a permanent one.
                    u.speed = std::min(u.speed, u.type->maxVel * 0.4f);
                    u.repathLeft -= dt;
                    if (!g.empty() && u.repathLeft <= 0 &&
                        u.orders.front().targetId == 0) {
                        u.repathLeft = 0.5f;
                        // The CURRENT leg's endpoint, not orders.back() -- that is
                        // the last thing the player queued, and routing to it here
                        // deleted every leg in front of it.
                        const Order& legEnd = u.orders[currentLeg(u.orders)];
                        float tx = legEnd.x, tz = legEnd.z;
                        // Check reachability BEFORE repathing: if this unit cannot get
                        // there, give up rather than queue a search that scans the
                        // whole map before failing -- hundreds of units doing that is
                        // the sim stall. pathExists answers it from the component
                        // labelling (it read the flow field's reachable set when that
                        // existed). Only repath when reachable, and within the budget.
                        bool onWalkable = g.walkable(int(u.x) / 16, int(u.z) / 16);
                        if (onWalkable && !pathExists(u.type, tx, tz, u.x, u.z))
                            dropLeg(u);      // give up THIS leg; honour the rest
                    }
                }
            }

            // Stuck watchdog: a unit that wants to move but makes no headway
            // (crowd jam at a corner/chokepoint) gets nudged sideways and
            // repathed to break the deadlock. Half the units nudge each way so
            // a crowd splits around an obstacle instead of piling up.
            if (!u.type->canFly) {
                float moved = detmath::len(u.x - u.stuckX, u.z - u.stuckZ);
                if (moved > 11.0f) {
                    u.stuckFor = 0; u.stuckX = u.x; u.stuckZ = u.z;
                } else {
                    u.stuckFor += dt;
                    if (u.stuckFor > 1.0f) {
                        u.stuckFor = 0; u.stuckX = u.x; u.stuckZ = u.z;
                        const NavGrid& g = navFor(u.type);
                        auto free = [&](float nx, float nz) {
                            return (g.empty() ||
                                    g.fits(int(nx) / 16, int(nz) / 16, footCells(u.type))) &&
                                   bodyPenetration(u, nx, nz) <= 0.0f;
                        };
                        float px = detmath::cos(u.heading), pz = -detmath::sin(u.heading);
                        // Which way to dodge. `id & 1` alone makes two units of the
                        // same parity meeting head-on pick the SAME side and collide
                        // again -- the exact failure solidity would otherwise turn
                        // into a permanent corridor lock. Fold in the tick so a pair
                        // that keeps re-colliding eventually picks opposite sides,
                        // and the choice stays deterministic.
                        float s = ((u.id ^ int(tickCounter_ >> 5)) & 1) ? 1.0f : -1.0f;
                        for (float d : {20.0f, 34.0f}) {
                            if (free(u.x + px * s * d, u.z + pz * s * d)) {
                                u.x += px * s * d * 0.5f; u.z += pz * s * d * 0.5f; break;
                            }
                            if (free(u.x - px * s * d, u.z - pz * s * d)) {
                                u.x -= px * s * d * 0.5f; u.z -= pz * s * d * 0.5f; break;
                            }
                        }
                        // Guard emptiness: the fully-blocked branch above may have
                        // just u.orders.clear()'d this unit (gave up an unreachable
                        // goal), and back()/front() on an empty deque is undefined --
                        // it computes a wild pointer that segfaults on some heap
                        // layouts (the release referee sim) while reading harmless
                        // garbage on others, which is exactly how this surfaced as an
                        // 8-AI server crash. A unit with no order has nothing to repath.
                        if (!g.empty() && !u.orders.empty() && u.orders.front().targetId == 0) {
                            const Order& legEnd = u.orders[currentLeg(u.orders)];
                            float tx = legEnd.x, tz = legEnd.z;
                            bool onWalkable = g.walkable(int(u.x) / 16, int(u.z) / 16);
                            if (onWalkable && !pathExists(u.type, tx, tz, u.x, u.z))
                                dropLeg(u);     // unreachable leg -> skip it, keep the queue
                        }
                    }
                }
            }
        }

        // Progress-based give-up: a ground unit heading to a POINT (move /
        // fight-move, targetId == 0) that has not gotten meaningfully closer for
        // a long time re-asks the background pathfinder, and eventually abandons
        // the leg
        // or in the unit's own base without ever tripping the fully-blocked path, so a
        // lone scout could sit forever. A traced route (which we know exists when the
        // goal is reachable) replaces the order and steers it around. Fliers and
        // target-locked (attack) orders are exempt; idle units reset the tracker.
        if (!u.type->canFly && !u.orders.empty()) {
            const Order& fo = u.orders.front();
            bool pointMove = fo.targetId == 0 && !fo.load && !fo.unload &&
                             fo.wait <= 0.0f && !fo.waitAttack;
            if (pointMove) {
                // Progress is measured against the leg being walked NOW. Against
                // orders.back() an out-and-back queue looks permanently stuck on
                // its outbound leg -- it IS moving away from the final one -- so
                // this fired after 2s, routed straight to the last leg, and
                // deleted the leg the player was watching the unit walk.
                const Order& legEnd = u.orders[currentLeg(u.orders)];
                float gx = legEnd.x, gz = legEnd.z;
                // LINEAR distance, not squared. This compared squared distances
                // and subtracted 400 for "20px closer" -- which only means 20px
                // when the goal is a few tens of pixels away. At 2900px the
                // squared distance is ~8.6 million, so a sub-pixel gain cleared
                // the bar, the tracker reset, and the no-headway timer could
                // never build up at all. A unit circling between two routes
                // 2900px from its goal therefore never gave up: measured 2426px
                // of travel in 60s with no arrival and no stop.
                float gd = detmath::len(u.x - gx, u.z - gz);
                if (gd < u.goalStuckD - 20.0f) {         // >20px closer -> real progress
                    u.goalStuckD = gd; u.goalStuckT = 0;
                } else {
                    u.goalStuckT += dt;
                    // No headway toward the goal. Ask the background search
                    // again from where we actually are -- the original request
                    // was made from somewhere else and may have failed there.
                    if (u.goalStuckT > 2.0f && pathService_ &&
                        !paths_.pending(u.id) &&
                        (tickCounter_ + uint32_t(u.id)) % kPathRetryTicks == 0)
                        requestPath(u, gx, gz);

                    // Still nothing after a long while: this leg cannot be
                    // satisfied -- the goal is inside terrain, behind a barrier,
                    // or down a corridor we cannot solve -- so stop shoving at
                    // it and honour whatever the player queued behind it.
                    //
                    // OURS, not retail's: the original just keeps pressing
                    // (0x4e545b does nothing at all once the path-failed bits
                    // are set), and a unit wedged against a cliff stays there.
                    // That reads as broken rather than characterful.
                    // ...but only when the search has actually FAILED from here.
                    // Time alone is not evidence: a unit following a long
                    // wandering route legitimately goes many seconds without
                    // getting nearer the goal, and dropping the leg then cuts a
                    // journey short -- measured, it turned a 97%-of-the-way trip
                    // into 53%. A live backoff entry means we tried to find a
                    // route from here and could not, which is the real signal.
                    // Three things must all hold, because each alone gives a
                    // false positive: time (not getting closer), a live failure
                    // backoff (we looked for a route from here and found none),
                    // and actually being WEDGED rather than crawling. Without the
                    // last one a unit grinding slowly along a wall gets its order
                    // cancelled -- measured, a trip that reaches 97% of the way by
                    // pressing was being abandoned at 53%.
                    // "Wedged" has two shapes. Pressed against something and
                    // barely moving is one (stuckFor). The other is standing on
                    // ground the unit does not fit on at all: back when the
                    // unstick pass existed (deleted 2026-09-12, see the note at
                    // the end of this file) it nudged such a unit toward legal
                    // ground every tick, that nudge counted as movement and kept
                    // resetting stuckFor, and the unit wandered for ever while
                    // never reaching anything --
                    // measured 2426px of wandering in 60s with no arrival.
                    // No extra conditions. Not getting closer for this long IS
                    // the signal, whatever the unit is doing meanwhile -- wedged
                    // against rock, or circling between two routes that each
                    // lead back to the other. Earlier versions demanded the unit
                    // be wedged, or standing off the grid, or to have had a
                    // search fail; a unit oscillating on perfectly legal ground
                    // is none of those, and wandered 2426px in 60s without ever
                    // arriving. Retail stops: ordered at a mountain it walks as
                    // close as it can and comes to rest.
                    if (u.goalStuckT > kGoalGiveUpSecs) dropLeg(u);
                }
            } else {
                u.goalStuckD = 1e30f; u.goalStuckT = 0;
            }
        } else if (u.orders.empty()) {
            u.goalStuckD = 1e30f; u.goalStuckT = 0;
        }
        // An idle aircraft looks for somewhere to put down. Retail's VTOL_Standby
        // (icd 0x417350) hands off to VTOL_LandIfCan (0x416cd0), which tests the
        // spot underneath and, if it will not do, samples TWELVE grid-snapped
        // candidates through a window that grows from +/-64 to +/-240 world units,
        // taking the first that is landable. Only if all twelve fail does it give
        // up and orbit its anchor at radius 160, stepping about -120 degrees each
        // time round. Without this a flyer settles wherever it happened to stop --
        // over water, or on the roof of a building.
        if (u.type->canFly && u.type->vtolStandby && u.alive() && u.orders.empty() &&
            u.buildSiteId == 0 && !hasQueuedWork(u) && u.reclaimId == 0 &&
            u.repairId == 0 && !u.embarked()) {
            const NavGrid& g = navFor(u.type);
            auto landable = [&](float x, float z) {
                if (g.empty()) return true;
                int cx = int(x) / 16, cz = int(z) / 16;
                return g.walkable(cx, cz) && cellFree(x, z, u.id, footCells(u.type));
            };
            if (!landable(u.x, u.z)) {
                bool found = false;
                // The window grows exactly as retail's does: 12 draws, centre
                // 64..240, so a flyer boxed in by its own airfield keeps widening
                // its search instead of giving up on the first miss.
                for (int i = 0; i < 12 && !found; ++i) {
                    float half = 64.0f + float(i) * 16.0f;
                    float ox = float(int(fireRand(uint32_t(half * 2))) ) - half;
                    float oz = float(int(fireRand(uint32_t(half * 2))) ) - half;
                    float tx = u.x + ox, tz = u.z + oz;
                    if (tx < 16 || tz < 16 || !landable(tx, tz)) continue;
                    order(u.id, tx, tz, false);
                    found = true;
                }
                if (!found) {
                    // Nowhere to land: circle the spot instead of grinding on it.
                    u.standbyTheta = uint16_t(u.standbyTheta - 21845);   // ~-120 deg
                    float a = float(u.standbyTheta) * (2.0f * 3.14159265f / 65536.0f);
                    order(u.id, u.x - detmath::sin(a) * 160.0f,
                          u.z - detmath::cos(a) * 160.0f, false);
                }
            }
        }
    }

    // Queued builds that came due this tick, started now that nothing holds a
    // reference into units_. Re-fetch by id each pass for the same reason.
    for (int bid : buildDue_) {
        Unit* b = unit(bid);
        if (!b || !b->alive() || b->buildSiteId != 0 || b->orders.empty()) continue;
        const Order& o = b->orders.front();
        if (!o.buildType) continue;
        startBuild(bid, o.buildType, o.x, o.z - float(o.buildType->footZ) * 8 - 24,
                   Approach::None);
        // startBuild may have failed (the spot is taken now, or mana ran out). Drop
        // the order rather than parking the builder on it forever.
        if (Unit* b2 = unit(bid); b2 && b2->buildSiteId == 0 && !b2->orders.empty() &&
                                  b2->orders.front().buildType)
            b2->orders.erase(b2->orders.begin());
    }
    buildDue_.clear();

    // Remote Effect spells channelling toward their landing point. The effect is
    // pinned to the ground where it was aimed, so it lands whether or not the
    // original target moved -- you dodge an Earthquake by leaving its radius, not
    // by taking a step. Ticked before the death-blast drain so a kill it causes is
    // swept next tick like any other.
    for (size_t i = 0; i < pendingEffects_.size();) {
        PendingEffect& e = pendingEffects_[i];
        // A charm/freeze channel dies with its caster: kill the Mind Mage during
        // its wind-up and nothing is converted.
        if (e.casterGated) {
            const Unit* c = e.fromId ? unit(e.fromId) : nullptr;
            if (!c || !c->alive()) {
                pendingEffects_.erase(pendingEffects_.begin() + std::ptrdiff_t(i));
                continue;
            }
        }
        e.at -= dt;
        e.endAt -= dt;
        bool pulse = e.at <= 0.0f;
        if (pulse) {
            if (e.period > 0.0f) e.at += e.period;
            else e.at = 1e9f;          // single-pulse: never again
        }
        bool done = e.endAt <= 0.0f;
        const PendingEffect cur = e;   // applyHit walks/kills units_; copy first
        if (done) pendingEffects_.erase(pendingEffects_.begin() + std::ptrdiff_t(i));
        else ++i;
        if (pulse && cur.w) applyHit(*cur.w, cur.x, cur.z, cur.player, cur.fromId, nullptr);
    }
    // Wandering storms: drift along the launch heading, weave, and grind whatever
    // they touch. The FBI `damage` is a PER-TICK rate, not a per-hit figure -- the
    // Tornado's 50 is 1500/sec inside its small radius (it wears a unit down over
    // its 9 seconds), while a god vortex's 12500 is simply death to stand in, which
    // its `monarch = 0.01` row scales back so a Monarch has a few seconds to escape.
    for (size_t i = 0; i < storms_.size();) {
        Storm& s = storms_[i];
        if (!s.w) { storms_.erase(storms_.begin() + std::ptrdiff_t(i)); continue; }
        // Wander offset: NOT an angle. maxvariation is a jitter half-width in
        // pixels per tick, applied PERPENDICULAR to the launch direction (x gets
        // |dirZ|, z gets |dirX|), so the storm weaves across its own path while
        // still advancing. Re-rolled every variationtime on the sim's Lehmer RNG,
        // so every peer weaves identically.
        s.nextVary -= dt;
        if (s.nextVary <= 0.0f) {
            s.nextVary += s.w->variationTime > 0 ? s.w->variationTime : 2.0f;
            float mv = s.w->maxVariation;
            if (mv > 0) {
                float vx = mv * std::abs(s.dirZ), vz = mv * std::abs(s.dirX);
                s.jitX = (float(burnRand(2001)) / 1000.0f - 1.0f) * vx;
                s.jitZ = (float(burnRand(2001)) / 1000.0f - 1.0f) * vz;
            }
        }
        float vel = s.w->projVel > 0 ? s.w->projVel : 50.0f;
        s.x += s.dirX * vel * dt + s.jitX;
        s.z += s.dirZ * vel * dt + s.jitZ;
        s.x = std::clamp(s.x, 0.0f, float(terW_) * 16.0f);
        s.z = std::clamp(s.z, 0.0f, float(terH_) * 16.0f);
        // builduptime is a harmless wind-up: the storm is already visible and
        // moving, which is the only warning a victim gets to walk out of its path.
        if (s.arm > 0.0f) { s.arm -= dt; ++i; continue; }
        s.left -= dt;
        if (s.left <= 0.0f) { storms_.erase(storms_.begin() + std::ptrdiff_t(i)); continue; }
        const Storm hit = s;   // applyHit walks/kills units_; copy what we need
        static const bool kStormLog = std::getenv("TAK_STORMLOG") != nullptr;
        if (kStormLog) std::fprintf(stderr, "storm t=%u at %.0f,%.0f jit=%.1f,%.1f left=%.1f\n",
                                    tickCounter_, hit.x, hit.z, hit.jitX, hit.jitZ, hit.left);
        applyHit(*hit.w, hit.x, hit.z, hit.player, hit.fromId, nullptr);
        ++i;
    }
    // Drain the [EXPLODEAS] death blasts queued by the sweep above. Done here, after
    // the loop, because applyHit can kill further units (chain-detonating a pack of
    // Kamikaze Rats) and must not mutate units_ while it is being walked. A blast may
    // queue more blasts; the index walk picks those up in the same tick, capped so a
    // pathological chain can't spin forever. Fully deterministic (id-ordered).
    for (size_t i = 0; i < deathBlasts_.size() && i < 4096; ++i) {
        const DeathBlast b = deathBlasts_[i];
        if (b.w) applyHit(*b.w, b.x, b.z, b.player, b.fromId, nullptr);
    }
    deathBlasts_.clear();

    if (g_phase) _sep0 = std::chrono::steady_clock::now();
    // Separation: push overlapping mobile units apart. The spatial hash limits
    // each unit to its ~3x3 neighbourhood, so this is O(n) not O(n^2). Each pair
    // is handled once (by the lower index), so the result matches the old loop.
    {
        auto _g = std::chrono::steady_clock::now();
        rebuildGrid();   // units moved this tick; rebuild for accurate neighbours
        if (g_phase) g_gridMs += std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - _g).count();
    }
    tickAbilities(dt);   // reclaim / resurrect corpses (uses the fresh grid)
    tickAuras(dt);       // AdjustArmor/Attack stat auras (uses the fresh grid)
    // NO SEPARATION PASS. Retail has none, and with the occupancy rule above
    // ported faithfully it has nothing left to do: a body cannot step into a
    // parked one, nor into a mover that is slower or coming the other way, so
    // the overlaps this used to clean up no longer form. What it did form was
    // its own problem -- it enforced spacing at a different resolution from the
    // mover, and the two fighting is what read in play as units shoving each
    // other around.
    //
    // The known cost, accepted deliberately: a unit may still tuck in behind a
    // faster leader and end up overlapping it if that leader then stops, and
    // nothing now pushes the pair apart. Retail behaves the same way. Units
    // getting stuck the way retail's did is the intended outcome here, not a
    // regression to fix by reintroducing a push.
    // There was an "unstick" pass here, deleted 2026-09-12. It nudged any ground
    // unit standing on a blocked cell toward the nearest walkable one, to rescue
    // bodies "spawned by a building, shoved by a crowd, or clipped a corner".
    //
    // Retail has no such thing, and the three failure modes that comment lists
    // were ours: the crowd-shoving one was the separation pass, itself deleted
    // earlier today. What the nudge did do was move a unit every tick, which
    // reads as progress, which reset the wedged timer, which meant a unit that
    // could not reach its goal never gave up -- measured, one wandered 2426px in
    // 60 seconds and was still going. Take the nudge away and the same unit
    // settles after 3.9s having moved 30px.
    //
    // The risk taken knowingly: a unit that does end up on illegal ground now
    // stays there rather than being walked off it. The mover's own solidity is
    // what should keep it from happening in the first place.
    // Win/defeat is derived from unit state on every sim (clients + referee),
    // so all peers agree on the tick a team is eliminated / the game is won.
    updateOutcome();
    if (g_phase) {
        auto _end = std::chrono::steady_clock::now();
        double tsep = std::chrono::duration<double,std::milli>(_end-_sep0).count();
        double ttot = std::chrono::duration<double,std::milli>(_end-_tk0).count();
        static double thr = getenv("TAK_PHASE_MS") ? atof(getenv("TAK_PHASE_MS")) : 15.0;
        if (ttot > thr) {   // only report a stall (threshold tunable via TAK_PHASE_MS)
            int alive = 0; for (auto& u : units_) if (u.alive()) ++alive;
            // flow= and path= used to sit here; the flow fields and the inline
            // A* are both gone, so the counters were always zero.
            std::fprintf(stderr, "SIMPHASE tick=%.1fms combat=%.1f sep=%.1f vis=%.1f burn=%.1f grid=%.1f other=%.1f units=%d\n",
                         ttot, g_tcomb, tsep, g_visMs, g_burnMs, g_gridMs,
                         ttot - g_tcomb - tsep - g_visMs - g_burnMs - g_gridMs, alive);
        }
    }
    // Campaign mission runner: feed this tick's build/death events into the "god"
    // script, then advance it (VM + triggers + win/lose). Runs on every peer (build
    // & death events queue VM threads; the spawns/orders happen in step()), so the
    // mission stays in lockstep with no relayed actions.
    if (mission_) {
        for (auto& u : units_)
            if (u.justBuilt) mission_->unitBuilt(*this, u.justBuilt);
        for (int id : justDied_) mission_->unitDied(*this, id);
        mission_->step(*this, dt);
    }
    // Scenario (.crt) trigger runner: same lockstep contract as the mission
    // runner -- built identically on every peer, ticked here, hashed below.
    if (scenario_) {
        for (int id : justDied_) scenario_->unitDied(*this, id);
        scenario_->step(*this, dt);
    }
}

#ifndef NDEBUG
// See sim.h. Recomputes the same quantities stateHash() folds, but grouped, so a
// mismatch can be attributed to a component instead of a 64-bit number.
void World::hashTrace() const {
    static bool init = false, on = false;
    static uint32_t lo = 0, hi = 0;
    if (!init) {
        init = true;
        if (const char* e = std::getenv("TAK_HASHTRACE")) {
            unsigned a = 0, b = 0;
            if (std::sscanf(e, "%u:%u", &a, &b) == 2) { lo = a; hi = b; on = true; }
        }
    }
    if (!on || tickCounter_ < lo || tickCounter_ > hi) return;

    auto fnv = [](uint64_t h, uint64_t v) {
        for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xFF; h *= 1099511628211ULL; }
        return h;
    };
    auto bits = [](float f) { uint32_t b; std::memcpy(&b, &f, 4); return uint64_t(b); };
    const uint64_t seed = 1469598103934665603ULL;

    uint64_t hUnitPos = seed, hUnitHp = seed, hUnitOrd = seed, hUnitMisc = seed;
    uint64_t aliveN = 0;
    for (const auto& u : units_) {
        if (u.alive()) ++aliveN;
        hUnitPos  = fnv(fnv(fnv(hUnitPos, u.id), bits(u.x)), bits(u.z));
        hUnitPos  = fnv(hUnitPos, bits(u.heading));
        hUnitHp   = fnv(fnv(hUnitHp, u.id), bits(u.hp));
        hUnitOrd  = fnv(fnv(hUnitOrd, u.id), u.orders.size());
        if (!u.orders.empty()) {
            const Order& o = u.orders.front();
            hUnitOrd = fnv(fnv(fnv(hUnitOrd, uint32_t(o.targetId)), bits(o.x)), bits(o.z));
        }
        hUnitMisc = fnv(fnv(hUnitMisc, u.id), uint64_t(u.alive() ? 1 : 0));
        hUnitMisc = fnv(hUnitMisc, uint64_t(u.veteran));
        for (float rl : u.reloads) hUnitMisc = fnv(hUnitMisc, bits(rl));
        hUnitMisc = fnv(hUnitMisc, uint64_t(uint32_t(u.stance)));
        hUnitMisc = fnv(hUnitMisc, uint64_t(u.moveState) * 3 + uint64_t(u.fireState));
        hUnitMisc = fnv(hUnitMisc, uint64_t((u.cloakOn ? 1u : 0u) | (u.active ? 2u : 0u)));
        hUnitMisc = fnv(hUnitMisc, uint64_t(uint32_t(u.repairId)));
        hUnitMisc = fnv(hUnitMisc, uint64_t(uint32_t(int32_t(u.squad))));
    }
    uint64_t hProj = fnv(seed, projectiles_.size());
    for (const auto& p : projectiles_)
        hProj = fnv(fnv(fnv(hProj, uint32_t(p.fromPlayer)), bits(p.x)), bits(p.z));
    uint64_t hEff = fnv(seed, pendingEffects_.size());
    for (const auto& e : pendingEffects_)
        hEff = fnv(fnv(fnv(fnv(hEff, uint32_t(e.player)), bits(e.x)), bits(e.z)), bits(e.at));
    uint64_t hStorm = fnv(fnv(seed, uint32_t(stormSeq_)), storms_.size());
    for (const auto& s : storms_)
        hStorm = fnv(fnv(fnv(fnv(hStorm, uint32_t(s.player)), bits(s.x)), bits(s.z)), bits(s.left));
    uint64_t hPlayers = seed;
    for (const auto& t : players_)
        hPlayers = fnv(fnv(fnv(hPlayers, bits(t.mana)), bits(t.godFavor)), uint32_t(t.team));
    uint64_t fAlive = 0, fWork = 0;
    for (const auto& f : features_)
        if (f.alive) { ++fAlive; fWork ^= (bits(f.work) << 1) ^ uint64_t(uint32_t(f.id)); }
    uint64_t hFeat = fnv(fnv(seed, fAlive), fWork);
    // The nav overlay + grid cells drive losBetween, which gates target acquisition --
    // but NEITHER is folded into stateHash, so a divergence here is invisible to the
    // referee until it changes a decision seconds later.
    uint64_t hObst = seed;
    for (uint8_t o : obst_) hObst = fnv(hObst, o);
    // nav_ is built from (heights, seaLevel, limits); obst_'s occlusion pass reads
    // heights ONLY -- so heights-vs-seaLevel is exactly the split that would leave
    // obst_ matching while nav_ diverges.
    uint64_t hHeights = seed;
    for (uint8_t v : heights_) hHeights = fnv(hHeights, v);
    const uint64_t hNav = nav_.debugCellsHash();

    std::fprintf(stderr,
        "HASHTRACE t=%u units=%zu alive=%llu pos=%016llx hp=%016llx ord=%016llx misc=%016llx "
        "proj=%016llx(%zu) eff=%016llx storm=%016llx play=%016llx feat=%016llx burn=%llu fire=%u obst=%016llx nav=%016llx hgt=%016llx sea=%d hw=%d hh=%d\n",
        tickCounter_, units_.size(), (unsigned long long)aliveN,
        (unsigned long long)hUnitPos, (unsigned long long)hUnitHp,
        (unsigned long long)hUnitOrd, (unsigned long long)hUnitMisc,
        (unsigned long long)hProj, projectiles_.size(),
        (unsigned long long)hEff, (unsigned long long)hStorm,
        (unsigned long long)hPlayers, (unsigned long long)hFeat,
        (unsigned long long)burnRng_, fireRng_,
        (unsigned long long)hObst, (unsigned long long)hNav,
        (unsigned long long)hHeights, seaLevel_, hW_, hH_);
}
#endif

uint64_t World::stateHash() const {
    // FNV-1a over the quantities that must agree between lockstep peers.
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 1099511628211ULL;
        }
    };
    auto mixf = [&](float f) {
        uint32_t b;
        static_assert(sizeof b == sizeof f);
        std::memcpy(&b, &f, 4);
        mix(b);
    };
    for (const auto& u : units_) {
        mix(uint64_t(u.id));
        mix(uint64_t(u.player));
        mixf(u.x);
        mixf(u.z);
        mixf(u.hp);
        mixf(u.heading);
        mix(uint64_t(u.orders.size()));
        // Order target + attack-move flag: two sims can hold the same order
        // COUNT while chasing different targets -- fold the head order in so a
        // divergence shows up before damage lands (the referee must attribute
        // desyncs fast at 8 players).
        if (!u.orders.empty()) {
            const Order& o = u.orders.front();
            mix(uint64_t(uint32_t(o.targetId)));
            mix(uint64_t(o.attackMove ? 1 : 0));
            mixf(o.x);
            mixf(o.z);
        }
        mix(uint64_t(u.alive() ? 1 : 0));
        mix(uint64_t(u.veteran));
        // All three reload timers, not just the primary: they now decide WHICH
        // weapon the auto-selector fires, so a drift in any of them would change
        // behaviour.
        for (float rl : u.reloads) mixf(rl);
        mixf(u.selfDestructT);   // self-destruct countdown drives a deterministic death
        // Stance / cloak-intent / active gate auto-acquire, cloaking and firing, so a
        // divergence in them must fault directly rather than diffusing into positions.
        // Both standing orders drive behaviour, so both belong in the checksum --
        // the displayed stance alone would hide a peer whose move/fire fields had
        // drifted (they are set together, but only one of them is shown).
        mix(uint64_t(uint32_t(u.stance)));
        mix(uint64_t(u.moveState) * 3 + uint64_t(u.fireState));
        mix(uint64_t((u.cloakOn ? 1u : 0u) | (u.active ? 2u : 0u)));
        mix(uint64_t(uint32_t(u.repairId)));   // build-power target -> HP/mana divergence
        mix(uint64_t(uint32_t(int32_t(u.squad))));   // control squad: formation<0 drives movement
    }
    // Projectiles: count alone hides same-count divergence, so fold owner and
    // position of each in flight (mixf hashes the exact bits -- deterministic
    // across peers and NaN-safe, unlike an int cast).
    mix(uint64_t(projectiles_.size()));
    for (const auto& p : projectiles_) {
        mix(uint64_t(uint32_t(p.fromPlayer)));
        mixf(p.x);
        mixf(p.z);
    }
    // Remote Effect spells mid-channel and wandering storms mid-roam are live sim
    // state that outlives a tick, so a divergence in either must show up here.
    mix(uint64_t(pendingEffects_.size()));
    for (const auto& e : pendingEffects_) {
        mix(uint64_t(uint32_t(e.player)));
        mixf(e.x); mixf(e.z); mixf(e.at);
    }
    mix(uint64_t(uint32_t(stormSeq_)));
    mix(uint64_t(storms_.size()));
    for (const auto& s : storms_) {
        mix(uint64_t(uint32_t(s.player)));
        mixf(s.x); mixf(s.z); mixf(s.left); mixf(s.arm);
        // The wander offset and its timer decide the whole path, and each re-roll
        // advances the shared burn RNG -- fold them so a drift surfaces here rather
        // than as a mystery divergence seconds later.
        mixf(s.jitX); mixf(s.jitZ); mixf(s.nextVary);
    }
    for (const auto& t : players_) {
        mixf(t.mana);
        mixf(t.godFavor);
        // Team assignment drives sim behaviour (splash/acquire/auras) but is set
        // from setup -- fold it in so a lobby/config mismatch faults immediately
        // as a desync instead of diverging mysteriously.
        mix(uint64_t(uint32_t(t.team)));
    }
    // Reclaimable features: fold a cheap signature so a reclaim divergence faults as
    // a desync directly (mana/nav already reflect it indirectly). Order-independent.
    uint64_t fAlive = 0, fWork = 0;
    for (const auto& f : features_)
        if (f.alive) {
            ++fAlive;
            uint32_t w; std::memcpy(&w, &f.work, 4);
            uint32_t dm; std::memcpy(&dm, &f.dmg, 4);
            fWork ^= (uint64_t(w) << 1) ^ uint64_t(uint32_t(f.id)) ^
                     // burning/damage state: type swaps and timers are sim state
                     (uint64_t(uint32_t(f.type + 1)) << 17) ^
                     (uint64_t(f.burn) << 33) ^ (uint64_t(uint32_t(f.burnLeft)) << 40) ^
                     (uint64_t(dm) << 9);
        }
    mix(fAlive);
    mix(fWork);
    mix(uint64_t(burnRng_));   // burn-RNG stream position must agree
    if (mission_) mission_->foldHash(h);   // mission triggers/vars/outcome are lockstep state
    if (scenario_) scenario_->foldHash(h); // scenario flags/timers/outcome are lockstep state
    for (uint8_t d : forcedDefeat_) { h ^= (d + 1u); h *= 1099511628211ULL; }
    return h;
}

// Mission runner ownership -- ctor/dtor defined here where MissionScript is a complete
// type, so no other TU instantiates the unique_ptr<MissionScript> deleter.
World::World() = default;
World::~World() {
    if (visRunning_) visWorker_.join();   // the fog worker outlives nothing
}
void World::setMission(std::unique_ptr<MissionScript> m) {
    mission_ = std::move(m);
    if (mission_) mission_->start(*this);   // queue the Start script (runs on the first tick)
}
int World::missionOutcome() const { return mission_ ? mission_->outcome() : 0; }

void World::setScenario(std::unique_ptr<ScenarioScript> s) {
    scenario_ = std::move(s);
    if (scenario_) scenario_->start(*this);
}
void World::forceDefeat(int player) {
    if (player < 0) return;
    if (int(forcedDefeat_.size()) <= player) forcedDefeat_.resize(size_t(player) + 1, 0);
    forcedDefeat_[size_t(player)] = 1;
}

int World::updateOutcome() {
    // A player is defeated when it has no living units. Compute per-player
    // alive counts, then per-team. This runs on every sim (referee included),
    // so all peers conclude win/defeat on the same tick.
    std::vector<int> aliveByPlayer(players_.size(), 0);
    // Living Monarch (commander unit) per player, for the monarch-loss rule.
    std::vector<int> monarchByPlayer(players_.size(), 0);
    for (const auto& u : units_)
        if (u.alive() && u.type &&
            u.player >= 0 && u.player < int(players_.size())) {
            ++aliveByPlayer[size_t(u.player)];
            if (u.type->commander) ++monarchByPlayer[size_t(u.player)];
        }
    if (hadMonarch_.size() != players_.size()) hadMonarch_.assign(players_.size(), 0);
    for (int p = 0; p < int(players_.size()); ++p) {
        if (monarchByPlayer[size_t(p)] > 0) hadMonarch_[size_t(p)] = 1;
        // No living units OR -- when the Monarch is NOT expendable -- a player who
        // once fielded a Monarch has now lost it. Both are deterministic and computed
        // identically on every peer + the referee, so win/defeat agree in lockstep.
        bool monarchDead = !monarchExpendable_ && hadMonarch_[size_t(p)] &&
                           monarchByPlayer[size_t(p)] == 0;
        bool forced = p < int(forcedDefeat_.size()) && forcedDefeat_[size_t(p)];
        bool wasDefeated = players_[size_t(p)].defeated;
        players_[size_t(p)].defeated = (aliveByPlayer[size_t(p)] == 0) || monarchDead || forced;
        // Stamp the moment of elimination once, for the end-of-game "Time" column.
        if (!wasDefeated && players_[size_t(p)].defeated) players_[size_t(p)].defeatedAt = clock_;
        players_[size_t(p)].unitCount = aliveByPlayer[size_t(p)];   // re-sync the cap count
    }

    // Count DISTINCT teams that still have a living unit (robust to any team id,
    // not just 0..n-1): a surviving player counts its team once -- the first time
    // that team appears among survivors. If exactly one team remains it wins.
    // A player counts as surviving only if NOT defeated -- so a monarch-loss
    // elimination (units still alive but the Monarch is dead) removes them from the
    // running just like being wiped out.
    int survivingTeam = -1, survivingCount = 0;
    for (int p = 0; p < int(players_.size()); ++p) {
        if (players_[size_t(p)].defeated) continue;
        int tm = players_[size_t(p)].team;
        bool firstOfTeam = true;
        for (int q = 0; q < p; ++q)
            if (!players_[size_t(q)].defeated && players_[size_t(q)].team == tm) {
                firstOfTeam = false;
                break;
            }
        if (firstOfTeam) { ++survivingCount; survivingTeam = tm; }
    }
    winningTeam_ = (survivingCount == 1) ? survivingTeam : -1;
    return winningTeam_;
}

} // namespace tak::sim
