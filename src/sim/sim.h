#pragma once

#include <algorithm>
#include <atomic>
#include <thread>
#include "sim/pathsearch.h"
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace tak::hpi { class Vfs; }

namespace tak::sim {

// Cheat: when true, construction and production complete instantly and cost no
// mana (set via the takclient `--cheat` flag).
extern bool gInstantBuild;

// Unit stats loaded from .fbi files. Velocities are in map pixels per
// second (FBI values are per original 30Hz tick), headings in radians
// with 0 facing +z and the model's forward axis.

// Visual family for a weapon's projectile (drives how the viewer draws it).
enum class WeaponFx { Arrow, Lightning, Fire };

struct UnitType;   // for Weapon::damageVs

struct Weapon {
    std::string name;
    float range = 0;         // px
    float reload = 1;        // seconds
    float damage = 0;        // DAMAGE.default (base, used when no category matches)
    float projVel = 0;       // px/s; 0 = instant (melee)
    bool melee = false;
    float aoe = 0;           // areaofeffect radius (px); >0 = splash
    float edge = 1;          // edgeeffectiveness: damage fraction at the aoe edge
    float aimTol = 0.1f;     // aimtolerance in radians: how close to on-target to fire
    bool ballistic = false;  // FBI weapon type = Ballistic (lobbed arc, not flat)
    // FBI weapon type = "Line of Sight": a sustained hitscan beam (the drake's
    // Fire Breath), NOT a lobbed shot. Damage lands instantly along the sightline
    // and the flame stream is a client-side emitter driven by emitTime -- there is
    // no traveling projectile object.
    bool beam = false;
    float emitTime = 0;      // emittime (seconds): how long the flame/beam is emitted
    // The rest of the retail weapon-class model (FBI `type=`), beyond
    // melee/ballistic/line-of-sight:
    //   Guided       -- a homing projectile that steers at `turnRate` (20 weapons:
    //                   Tracking Arrow, Ball Lightning, dragon fireballs).
    //   Remote Effect-- the effect materialises AT THE TARGET POINT after
    //                   `buildUp`, then fades over `decay` (23: Earthquakes,
    //                   monarch waves, god spells, Area Mind Control).
    //   Wandering    -- a roaming storm entity that drifts for `duration`
    //                   (4: Tornado, Fire/Water Vortex, Hurricane).
    //   Dropped    -- a bomb RELEASED from a flyer: it falls to the ground under
    //                 the release point (retail looks up the terrain height right
    //                 there and drives it down under gravity), so the bomber has to
    //                 overfly its target instead of shooting from range.
    enum class Kind { Normal, Guided, Remote, Wandering, Dropped } kind = Kind::Normal;
    // Remote Effect splits further by subtype, and the split changes the damage
    // CADENCE completely (retail has a C++ subclass per variant):
    //   Plain      -- one area hit when the channel completes.
    //   Earthquake -- pulses every `shakeduration`, through buildup AND decay.
    //   Hailstorm  -- ignores buildup/decay; pulses `particlespersecond` times a
    //                 second for `duration` (the rain).
    //   MindCtl /  -- one conversion/freeze sweep at the end of the channel, and
    //   Freeze        the whole spell ABORTS if its caster dies mid-channel.
    enum class RemoteKind { Plain, Earthquake, Hailstorm, MindCtl, Freeze };
    RemoteKind remote = RemoteKind::Plain;
    float particlesPerSec = 0;   // particlespersecond: hailstorm pulse rate
    float turnRate = 0;      // guided: steering rate, radians/sec (FBI deg/s)
    // Retail splits a projectile's per-tick travel into sub-steps so no sub-step is
    // longer than one 16px cell, which is how its swept collision stays honest
    // (KINGDOMS.icd 0x531ccb: subSteps = ceil(unitsPerTick/16), threshold 480 px/s).
    // It matters here because a guided shot that SNAPS onto its target rebuilds its
    // velocity from the per-sub-step speed and then has that vector applied once per
    // sub-step -- so a correcting homer really does fly subSteps x nominal.
    int subSteps = 1;
    float buildUp = 0;       // builduptime: channel before the effect lands
    float decay = 0;         // decaytime: fade after it lands
    float duration = 0;      // wandering: seconds the storm roams
    float maxVariation = 0;  // wandering: heading wobble per variationtime (radians)
    float variationTime = 0; // wandering: seconds between heading changes
    bool  unitsOnly = false; // unitsonly: the effect skips features (trees/props)
    // subtype=mindcontrol: converts targets to the firer's side instead of damaging
    // them, on a veterancy-scaled probability roll. The [DAMAGE] table is the
    // ACQUISITION filter -- retail zeroes the categories you may not TARGET
    // (monarch/god/dragon/fort/factory/naval/lodestone) -- while what actually
    // protects a unit caught inside an AREA charm is the commander / cantbecaptured
    // / in-transport gate at the impact itself.
    bool  mindControl = false;
    float minRange = 0;      // minrange: can't hit targets closer than this
    bool noAir = false;
    // dontleadtargets: aim at the target's CURRENT position instead of extrapolating
    // where it will be. Retail leads a moving unit by default; this flag skips it.
    // All four shipped users are Dropped bombs, whose weaponvelocity is a fall
    // parameter rather than a flight speed -- leading on it would throw the aim
    // point hundreds of cells away.
    bool noLead = false;
    // BallisticWeapon's own two fields (KINGDOMS.icd 0x52bb70 parses exactly these).
    // gravityadjustment is a plain multiplier on world gravity for this weapon's
    // arc; lobpreferred picks the HIGH root of the ballistic quadratic instead of
    // the low one, which is what lets a mortar drop its shell behind a wall.
    // We have no projectile Y yet, so only lobPreferred changes behaviour today --
    // see the note at the line-of-sight gate in tickCombat.
    float gravityAdj = 1.0f;
    bool lobPreferred = false;      // noairweapon: cannot target flying units
    float manaCost = 0;      // manapershot: mana drained from the firer per shot
    // FBI damagetype: 1 normal, 2 fire, 3 explosion (gibs -- Killed deathType 3
    // EXPLODEs every piece and leaves no corpse), 4 paralyzer (retail icd
    // 0x531bbd; "monster" and friends map to non-gib codes).
    int dmgType = 1;
    // Status effect this weapon inflicts (freeze/petrify/paralyze), 0 = none.
    enum class Status { None, Frozen, Stoned, Paralyzed } status = Status::None;
    float statusDur = 0;     // seconds the inflicted status lasts
    std::string soundHit;    // soundhitclass: impact-sound class (arrow/sword/cannon..)
    // Projectile ART (display only -- never hashed, like explosionClass below).
    // Retail draws a shot as authored art: a GAF/TAF sprite (weaponart, 60
    // weapons), or a real 3DO mesh (model, 39 -- arrows, spears, boulders), with
    // an optional glow (nimbus) and a ground shadow. Without these every shot in
    // the game is the same hand-drawn streak.
    std::string weaponArt;    // weaponart: anims/<name>_4444.taf sprite sequence
    std::string shotModel;    // model: objects3d/<name>.3do projectile mesh
    bool  nimbus = false;     // nimbus: additive glow around the shot
    bool  hasBoltColor = false;
    uint8_t inner[3] = {255, 255, 255};    // innercolor: lightning bolt core
    uint8_t middle[3] = {200, 230, 255};   // middlecolor
    uint8_t outer[3] = {120, 170, 255};    // outercolor
    float spinRate = 0;       // spinheading: shot spins as it flies (rad/sec)
    std::string shadowArt;    // shadowart: sequence in shadowgaf (always "shadows")
    // shotart: a Remote Effect that is DELIVERED rather than conjured in place --
    // a visible shot flies to the aim point at weaponvelocity and only starts the
    // spell's channel when it lands. Exactly one shipped weapon (the Acolyte's
    // Earthquake) uses it, which is also the only reason its weaponvelocity is
    // read at all; for the other 22 Remote Effects it is dead data.
    std::string shotArt;
    int lightMap = 0;         // lightmap: ground light pool, 1 small 2 medium 3 large
    // A wandering storm's own three-part animation: the spin-up, the roaming loop,
    // and the dissipation. Without these a tornado is invisible.
    std::string wanderStart, wanderLoop, wanderEnd;
    std::string explosionClass;       // explosionclass: impact effect (gamedata/explosions)
    std::string waterExplosionClass;  // waterexplosionclass: impact effect over water
    // Area-effect shockwave rings emitted at this weapon's impact.
    std::string radiusArt[3];   // radiusart0..2: expanding-ring effect anims
    int   ringCount = 0;        // ringcount: how many rings
    float ringDelay = 0.2f;     // ringdelay: seconds between successive rings
    float ringDur = 1.0f;       // ringduration: seconds each ring lasts
    int   spriteCount = 24;     // spritecount: sprites arranged around each ring
    float shakeMag = 0;         // shakemagnitude: camera-shake intensity on impact
    float shakeDur = 0;         // shakeduration: seconds the shake lasts
    bool  fireStarter = false;  // firestarter: leaves ground fire at the impact
    // Per-target-category damage overrides (DAMAGE keys other than `default`),
    // keyed by lowercased category token (e.g. "monarch", "dragon", "fort").
    std::map<std::string, float> dmgVs;
    WeaponFx fx = WeaponFx::Arrow;
    // Damage this weapon deals to a unit of type `t` (category override, else base).
    float damageVs(const UnitType* t) const;
};

// An aura from an [AdjustArmor]/[AdjustAttack]/[AdjustJoy] block under [UNITINFO].
// Armor/Attack continuously scale nearby units' armour or attack; JOY IS NOT MORALE
// -- it is a passive repair aura (retail's tag for it routes straight into the shared
// build/repair routine), which is why it is the most common of the three.
// Friends (affectsenemy=0) or enemies (affectsenemy=1), never both.
struct Aura {
    enum class Kind { Armor, Attack, Joy } kind = Kind::Armor;
    float amount = 1;        // multiplier (Armor/Attack); build-power/sec (Joy)
    bool  affectsEnemy = false;
    float radius = 0;
    // edgeeffectiveness. The name is misleading and the retail opcodes are
    // unambiguous (KINGDOMS.icd 0x51f636/0x51f730/0x51f858): the strength factor is
    // `edge + (1 - edge) * dist/radius`, i.e. the aura is WEAKEST at the emitter and
    // reaches full strength at the rim -- not the other way round. Designers worked
    // around it by shipping edge=1 (flat) where they wanted no falloff at all.
    float edge = 1;
    // The retail falloff factor at `dist` from the emitter.
    float falloff(float dist) const {
        float t = radius > 0 ? dist / radius : 0.0f;
        return edge + (1.0f - edge) * t;
    }
};

struct UnitType {
    std::string id;        // lowercase objectname, e.g. "araarch"
    std::string name;      // display name, e.g. "Archer"
    std::string side;      // ARA/TAR/VER/ZON/CRE
    float maxVel = 30;     // px/s
    float accel = 15;      // px/s^2
    float brake = 15;      // px/s^2
    float turnRate = 6;    // rad/s
    // FBI turninplacerate: the pivot rate used when the unit is STOPPED and merely
    // turning to face something, which retail keeps separate from the turn rate it
    // uses while moving (KINGDOMS.icd 0x4d91b0 picks between the two on an in-place
    // flag). Retail's parse default is 0, which would leave the 39 ground movers that
    // omit the key unable to pivot at all -- so default to turnRate instead and let an
    // explicit 0 stand.
    float turnInPlaceRate = 6;
    float maxHp = 100;
    bool canMove = false;
    bool isBuilder = false;
    bool commander = false;   // FBI commander=1: the faction's Monarch (loss condition)
    // Buildings vs mobile units: the reliable test is maxVel. The FBI `canmove`
    // flag is set on some buildings too (e.g. the Keep, or the Taros Hell), so a
    // canMove building would otherwise be mistaken for a mobile builder.
    bool isStructure() const { return maxVel <= 0.0f; }
    float buildDist = 0;    // FBI builddistance: how far a builder reaches to build
    bool onMana = false;    // must be built on a mana deposit (yardmap 'S'), e.g. lodestones
    float buildCost = 0;    // mana
    float buildTime = 0;    // work units; seconds = buildTime / builder workerTime
    float workerTime = 1;
    float income = 0;       // mana/sec (mogriumincome)
    float storage = 0;      // mana cap contribution (mogriumstorage)
    int footX = 1, footZ = 1;
    std::string yardMap;      // footX*footZ chars; 'o' blocks, '.'/'c' passable
    // Lowercased target-category tokens (from FBI category/damagecategory/tedclass),
    // matched against a weapon's per-category damage overrides.
    std::vector<std::string> categories;
    float sight = 180;        // px (FBI sightdistance)
    bool canFly = false;
    // bankscale / pitchscale: how hard this flyer rolls into a turn and pitches
    // into a climb or dive. Display-only (53 and 33 units carry them).
    float bankScale = 0, pitchScale = 0;
    // defaultmissiontype = Standby_wander: cows, deer, wolves, boar, peasants and
    // villagers drift around instead of standing still. 16 types, and the missions
    // place hundreds of them.
    bool  wanders = false;
    float cruiseAlt = 0;      // world units above ground when flying
    enum class Domain { Ground, Water, Hover };
    Domain domain = Domain::Ground;   // from FBI movementclass prefix
    bool canTransport = false;
    int transportCap = 0;     // units carried (FBI transportcapacity)
    // FBI transportdistance: the px radius inside which this transport absorbs a
    // unit that is boarding it. Raw world units == our px (retail stores it as a
    // plain 16-bit with no scaling, the same family as sightdistance/builddistance),
    // so a barge reaches ~419px where we used one hardcoded 70 for every transport.
    float transportDist = 0;
    std::string soundClass;   // FBI soundcategory, keys gamedata/soundclasses
    std::string bodyType = "default";   // FBI bodytype (flesh/armor/wood/..) = hit-sound material
    std::string corpse;       // FBI corpse feature name
    std::string stoneFeat;    // FBI stone= statue feature (death while petrified)
    std::string frozenFeat;   // FBI frozen= statue feature (death while frozen)
    int corpseAdjX = 0, corpseAdjZ = 0;   // corpseadjustx/z: wreck offset in cells
    bool canAnimate = false;              // cananimate: raises animateType from corpses
    const UnitType* animateType = nullptr;   // animatetype=<unit>, resolved post-load
    std::string animName_;                   // raw animatetype value (loadDir fixup)
    std::string shadowArt;    // FBI shadowart: shadow sprite name in shadows.gaf
    // Display-only render flags (never hashed; the sim has no Y axis at all).
    bool noShadow = false;    // FBI noshadow: casts no ground shadow (walls, spectres)
    bool floater = false;     // FBI floater: rides the water surface. Retail skips the
                              // shadow for these too, independently of noshadow, which
                              // is why five ships carry a shadowart they never show.
    bool canHover = false;    // FBI canhover: wades rather than being blocked by water
    bool ghost = false;       // FBI ghost: a spectral body, drawn translucent. Three
                              // units carry it (Ghost of Garacaius, Ghost Ship, Risen
                              // Wolf) and all three also set noshadow.
    int  waterline = 0;       // FBI waterline: height units the model sits BELOW the
                              // water surface. Retail sets the object's Y to
                              // max(terrainHeight, waterLevel - waterline), so a god
                              // wades in up to its waist and a hull sits in the water.
    std::string veteranModel; // veteranmodel: 3DO the unit swaps to at max veterancy
    // --- extended FBI stats -------------------------------------------------
    float healTime = 0;       // healtime: seconds per HP regenerated (0 = no regen)
    float leash = 0;          // maneuverleashlength: max auto-chase distance (0 = unlimited)
    // Per-type standing orders, derived exactly as retail's [UNITINFO] parser does
    // (icd 0x4c005d). standingunitorder is a COMPOSITE front-end: 2 -> move 1 /
    // fire 2, 1 -> move 0 / fire 2, 0 -> move 0 / fire 0. When the key is absent
    // the parser's default is the sentinel 3, which falls through to
    // standingmoveorder (default 2) and standingfireorder (default 2) -- Roam and
    // Fire At Will. 147 of the 203 shipped units set standingunitorder; none sets
    // either of the other two, so the 56 that do not are the only ones that roam.
    uint8_t defaultMove = 2;
    uint8_t defaultFire = 2;
    // FBI defaultmissiontype = VTOL_standby: an idle flyer looks for somewhere to
    // put down rather than settling wherever it happens to stop. Every one of the
    // 27 flying types carries it (canfly and VTOL_standby are the same set), so it
    // is really just "is an aircraft" -- parsed anyway, because that is what the
    // behaviour is keyed on in retail (mission handler icd 0x417350).
    bool vtolStandby = false;
    // FBI unitstandorders (icd UnitDef+0x264 bit 2, default 1): whether this type
    // has a combat stance at all. Retail's command-panel query (0x4affac) reads the
    // stance only when the bit is set and greys the three buttons out otherwise.
    // 62 shipped types clear it -- the Keep, Mana Amplifier, Temple of Anu, cows,
    // peasants, beggars. Every one of them turns out to be unarmed, so gating on
    // "mobile and armed" happened to give the same answer; this is the flag retail
    // actually consults, so a unit that breaks that coincidence still behaves.
    bool canSetStance = true;
    float waterMult = 1;      // watermultiplier: speed factor in shallow water
    float roadMult = 1.2f;    // roadmultiplier: on-road speed factor. Retail's FBI
                              // parser defaults it to 16.16 0x13333 (~1.2) -- icd
                              // 0x4bfc5e -- so EVERY ground unit gains on roads.
    float maxWaterDepth = 0;  // deepest water a ground unit may wade into
    float maxWaterSlope = 255;   // MaxWaterSlope: the slope limit below the waterline
    float maxSlope = 255;     // steepest cell height-spread the unit may cross
    float minWaterDepth = 0;  // shallowest water a water unit needs (from MOVEINFO)
    float radar = 0;          // radardistance: fog-reveal radius (separate from sight)
    bool  noVeteran = false;  // noveteran: this unit can never gain veterancy
    float maxMana = 0;        // per-unit mana pool (casters); 0 = uses no personal mana
    float manaRegen = 0;      // manarechargerate: personal mana regained per second
    bool  canReclaim = false; // canreclaim: builder can reclaim corpses/features for mana
    bool  canResurrect = false;   // canresurrect: can revive nearby corpses
    bool  canCapture = false;     // cancapture: can convert an enemy unit to its player
    bool  canCloak = false;       // cancloak
    float cloakCost = 0;          // cloakcost: mana/sec while cloaked and idle
    float cloakCostMove = 0;      // cloakcostmoving: mana/sec while cloaked and moving
    float minCloakDist = 0;       // mincloakdistance: an enemy this close forces uncloak
    // FBI fireatwillrandom (icd: UnitDef+0x264 bit 24, parsed at 0x4c0a3a; its ONLY
    // reader is the auto-target scorer at 0x4129bc). Retail scores each candidate
    // n = dist^2 / damageVsTarget and keeps the lowest of rand(n)/2 + rand(n); this
    // flag swaps the numerator for INT_MAX, which dwarfs any real squared distance,
    // so the pick stops preferring what is NEAREST and becomes a random draw
    // weighted by damage. Set on the 16 missile troops (every faction's archers,
    // the Crossbowman, Musketeer and Cannoneer): a rank of them sprays a crowd
    // instead of every last one focusing the same closest body.
    bool fireAtWillRandom = false;
    bool  attractsGods = false;   // attractsgods: priest channels god favour
    // weaponswitching: this unit carries ONE active weapon at a time, picked by the
    // player. Without it, a multi-weapon unit fires every weapon independently.
    bool  weaponSwitching = false;
    bool  onOffable = false;      // onoffable: can be toggled active/inactive
    bool  activateWhenBuilt = true;   // activatewhenbuilt (default on)
    bool  cantBeStoned = false, cantBeFrozen = false;
    bool  cantBeCaptured = false, cantBeTransported = false;
    int   transportSize = 1;      // transportsize: transport slots this unit occupies
    // selfdestructcountdown: seconds between arming a self-destruct and the unit
    // leaving. Three bits in retail (icd 0x4c09e8 masks & 7 into UnitType+0x264
    // bits 21..23), so 0..7. Defaults to 2, which is what the parser leaves when
    // a type omits the key (0x4c0a1f) -- and no shipped type declares it.
    int   selfDestructCountdown = 2;
    uint8_t blood[3] = {150, 30, 10};   // bloodcolor1 (r,g,b) for hit/death spray
    std::vector<Aura> auras;   // stat auras projected onto nearby units
    Weapon weapon;            // primary (WEAPON1); damage 0 = unarmed
    std::vector<Weapon> weapons;   // all slots (WEAPON1..3)
    // [EXPLODEAS]: a weapon fired at the unit's OWN position the moment it dies
    // (Kamikaze Rat's 320-radius blast, Grenadier/Fire Demon/Balloon death pops).
    Weapon explodeAs;
    bool hasExplodeAs = false;
    // totalallowed: per-player cap on LIVE units of this type (dragons/gods/
    // juggernaut carry 1). 0 = unlimited.
    int totalAllowed = 0;
    float maxRange() const {
        float r = 0;
        for (const auto& w : weapons) r = std::max(r, w.range);
        return r;
    }
    // Does this unit lob? A lobbing weapon takes the high ballistic arc, which is
    // precisely how a mortar or catapult puts its shell behind a wall -- so a
    // lobber is exempt from the line-of-sight gate that makes other ranged units
    // refuse to fire through an obstacle. Seven units carry one: the Aramon
    // Grenadier and Catapult, the Creon Bomb Sprinkler, Sage and Submersible, and
    // the Veruna Mortar and Catapult.
    bool lobs() const {
        for (const auto& w : weapons) if (w.lobPreferred) return true;
        return false;
    }
};

// A pathfinding movement class from gamedata/MOVEINFO.tdf: per-class terrain
// limits that a unit inherits via its FBI `movementclass`.
struct MoveClass {
    // FootprintX/Z: the real source of a MOBILE unit's size. 125 of our 152 movers
    // carry no footprintx in their own FBI at all -- it comes from here, and not one
    // of them is 1x1 (2x2 through 5x5). Retail copies these into the unit def at
    // KINGDOMS.icd 0x4c0e54 and falls back to the FBI only for a unit with no class.
    int footX = 0, footZ = 0;
    float maxSlope = 255;
    float maxWaterSlope = 255;   // the limit used when the cell's low corner is wet
    float maxWaterDepth = 255;
    float minWaterDepth = 0;
};

class TypeRegistry {
public:
    // Parse gamedata/MOVEINFO.tdf (movement classes). Call BEFORE loadDir so
    // units can inherit their class's slope / water limits.
    void loadMoveInfo(const hpi::Vfs& vfs, const std::string& path);
    // Parse every .fbi under `prefix` in the VFS (e.g. "units"). All source
    // archives' units merge into one namespace, precedence already resolved.
    void loadDir(const hpi::Vfs& vfs, const std::string& prefix);
    // Parse <prefix>/<builder>/<buildable>.tdf into the build tree.
    void loadBuildTree(const hpi::Vfs& vfs, const std::string& prefix);
    const UnitType* find(const std::string& id) const;
    // Full type table in deterministic (name-sorted) order -- corpse interning
    // walks it so every peer builds identical FeatType indices.
    const std::map<std::string, UnitType>& types() const { return types_; }
    const std::vector<std::string>& buildable(const std::string& builderId) const;
    // Mobile, armed combat units of a faction `side` ("ARA".."CRE"), in a fixed
    // (name-sorted) order so every peer builds the same stress-test army. Excludes
    // structures, builders, the Monarch, and anything with no weapon.
    std::vector<const UnitType*> combatUnits(const std::string& side) const;
    // Largest build menu of any builder (drives the minimum window width so the
    // whole icon row always fits at full size -- some Crusades menus reach 13).
    std::size_t maxBuildMenu() const {
        std::size_t m = 0;
        for (const auto& [id, list] : buildTree_) m = std::max(m, list.size());
        return m;
    }

private:
    std::map<std::string, UnitType> types_;
    std::set<std::string> canonicalTypes_;   // ids whose defining .fbi filename == objectname
    std::map<std::string, std::vector<std::string>> buildTree_;
    std::map<std::string, MoveClass> moveClasses_;   // lowercased name -> limits
};

struct Order {
    float x = 0, z = 0;
    int targetId = 0;      // nonzero = attack (or board, if load) target
    bool load = false;     // board the friendly transport `targetId`
    bool unload = false;   // sail to (x,z) and disembark cargo
    bool attackMove = false;   // engage enemies encountered en route
    bool patrol = false;       // loop: completed orders re-queue at the back
    bool guard = false;        // follow friendly `targetId`, engage threats
    float wait = 0;            // >0: hold position, counting down (SetMission "w N")
    bool waitAttack = false;   // hold until an enemy is in sight, then release (SetMission "wa")
    // Last waypoint of the order the PLAYER actually gave. One order can expand
    // into a whole A* path, so the queue interleaves pathfinding waypoints with
    // real goals; this marks where each issued order ends. Display-only in the
    // sense that it changes no movement maths -- but the repath paths need it to
    // tell "the rest of this leg" from "everything queued behind it".
    bool goal = false;
    // This order is a BUILD: put `buildType` down here. Builds used to live in a
    // separate list, which meant they could never interleave with moves -- "go
    // here, build that, go there" was inexpressible -- and the order line could
    // not draw them. They are ordinary queue entries now, as they are in retail.
    // The order sits at the builder's working position; the site is footZ*8+24
    // north of it.
    const UnitType* buildType = nullptr;
    // This order is a RECLAIM: consume feature `reclaimFeat` (negative = a corpse
    // record). Reclaims used to live in Unit::reclaimQueue, a THIRD parallel list
    // with the same defect the build list had -- nothing drained it unless a
    // reclaim was already running, so an area reclaim queued behind anything else
    // sat there forever. One queue, like everything else.
    int reclaimFeat = 0;
    // This order is a REPAIR: mend unit `repairTarget`. repairId was a single int,
    // so a second queued repair simply overwrote the first and it was lost. Same
    // shape as the build and reclaim lists, same fix.
    int repairTarget = 0;
    // This target was picked by auto-acquisition, not asked for by the player.
    // The move standing order gates only AUTO engagement: a Defensive unit told
    // explicitly to attack something across the map still walks over and does it.
    bool autoTarget = false;
    // Tick this order was issued, for the order-line beads: retail phases the
    // trail by (now - orderCreationTick) so each segment's dots crawl toward the
    // destination independently (icd 0x4d5747 reading order+0x5e). Display only --
    // never read by the simulation, never hashed.
    uint32_t issuedTick = 0;
    // Where the player actually CLICKED, when that differs from x/z. order()
    // snaps a destination the unit cannot stand on to the nearest cell it fits
    // in, so x/z is where the unit will END UP -- but the on-map marker belongs
    // under the cursor the player aimed at, not on the spot the engine chose.
    // Display only: never folded into stateHash, never steered on.
    //
    // Declared LAST on purpose. Order is built with aggregate initialisers like
    // {x, z, targetId}; a field inserted after x/z silently becomes the third
    // one and every such call site starts setting the wrong member.
    float clickX = 0, clickZ = 0;
};

struct Unit {
    int id = 0;
    int player = 0;
    const UnitType* type = nullptr;
    float x = 0, z = 0;
    float heading = 0;     // radians, 0 = +z
    float speed = 0;       // px/s
    float hp = 100;
    float reloads[3] = {0, 0, 0};  // per weapon slot
    int   weaponSlot = 0;          // active weapon (0=primary); player-selectable
    // True until the player picks a weapon with Ctrl+W. While set, the sim chooses
    // the best usable weapon per target the way retail's fire-at-will scan does;
    // once the player has chosen, their pick is obeyed.
    bool  weaponAuto = true;
    float repathLeft = 0;   // chase steering repath countdown
    float stuckFor = 0;     // seconds wanting to move but making no progress
    float stuckX = 0, stuckZ = 0;   // position when the stuck timer last reset
    float goalStuckT = 0;           // seconds a point-destination move has not gotten closer
    float goalStuckD = 1e30f;       // best (closest) squared distance to that goal so far
    float buildStuckT = 0;          // seconds a builder has approached its site with no progress
    float buildStuckD = 1e30f;      // best (closest) squared dist to the build site so far
    float deadFor = -1;    // >= 0 once dead; counts up for death animation
    float corpseUntil = 4;   // deadFor when the body is gone (4 = right after the
                             // death anim; corpse types extend by decomposetime)
    float overkill = 0;      // damage past the killing blow (retail severity input)
    uint8_t deathType = 1;   // damagetype of the killing blow (3 = explosion/gib)
    // Retail's self-destruct damage type (icd: the SelfDestruct mission applies
    // 30000 of type 5, and 0x5126a9 gives 5 its own branch in the death
    // handler). Kept as a named constant because it is NOT an FBI damagetype a
    // weapon can carry -- nothing in the shipped data uses 5 -- it is the engine
    // marking "this unit quit" rather than "this unit was killed".
    static constexpr uint8_t kDeathSelfDestruct = 5;
    uint8_t severity = 0;    // retail Killed severity ((overkill% + 1s-ago HP%)/2)
    uint8_t hpPct1s = 100;   // HP% sampled every 30 ticks (previous sample -- the
    uint8_t hpPctCur = 100;  //  retail unit+0x111/+0x110 pair severity reads)
    int corpseStatue = -1;   // FeatType override chosen at death (stone/frozen), -1 = corpse=
    float corpseWork = 60;   // ordered-reclaim work left in the body (kReclaimRate/s)
    int reviveTarget = 0;    // priest: dead unit id being channelled back (0 = none)
    int8_t reviveMode = 0;   // 1 = resurrect (own corpse), 2 = animate (raise ghoul)
    float reviveLeft = 0;    // seconds of channel remaining
    float reviveTotal = 1;   // full channel length (mana drains proportionally)
    bool corpseBlocks = false;   // dead structure still occupies its nav footprint
                                 // (blocking wreck / neutral wall) until retired
    // --- extended runtime state --------------------------------------------
    float mana = 0;        // personal mana pool (casters), capped at type->maxMana
    int   xp = 0;          // accumulated experience from kills
    int   veteran = 0;     // veteran level (0..10); scales attack/armor/reload
    float atkBuff = 1;     // live attack multiplier from auras (decays to 1)
    float armBuff = 1;     // live armour multiplier from auras (decays to 1)
    float frozenFor = 0;   // >0 = frozen solid (can't act); counts down
    float stonedFor = 0;   // >0 = petrified (can't act, immune to damage while stone)
    float paralyzedFor = 0;// >0 = paralyzed (can't act, still takes damage)
    float selfDestructT = -1;// >=0 = self-destruct countdown (s) armed; -1 = not
    bool  cloaked = false; // currently invisible to enemies
    // OFF until ordered. Retail treats cloaking as a MISSION, not a spawn state:
    // translate/unitmissions.tdf carries CLOAK and DECLOAK as separate mission
    // codes, and the drain itself lives in the mission-handler block (icd
    // 0x4011b0, picking cloakcostmoving over cloakcost by speed) alongside Build
    // and SelfDestruct.
    //
    // Defaulting this ON quietly cost Taros its whole economy. tarnecro is the
    // Taros MONARCH -- a starting unit -- with cancloak=1 and cloakcost=25, so it
    // burned 25 mana/sec against an income of ~21 from the first second of every
    // match: income 21 vs everyone else's 43, one lodestone to their four, and a
    // treasury pinned at 0 that could never reach the ~121 needed to start
    // another. The AI looked broken; it was solvent code starving on a drain it
    // never asked for.
    bool  cloakOn = false;  // canCloak units: player wants to cloak (CLOAK order)
    bool  active = true;   // onoffable units: false = powered down
    // Retail keeps TWO independent standing orders, and the single "stance" the
    // HUD shows is only a front-end that writes both (icd setter 0x5198a0):
    //   Offensive -> move 1, fire 2      Defensive -> move 0, fire 2
    //   Passive   -> move 0, fire 0
    // moveState: 0 = never leave position to engage, 1 = engage within
    // maneuverleashlength of where the unit was posted, 2 = engage with no limit.
    //   NOTE 2 is unreachable from the stance buttons, exactly as in retail -- it
    //   is the spawn default for a type that sets no standingunitorder, and
    //   touching the buttons trades it away for good.
    // fireState: 0 = hold fire (no auto-acquire, no retaliation -- an explicit
    //   attack order still works), 1 = return fire (never self-acquires, but
    //   shoots what it is handed and does hit back), 2 = fire at will.
    uint16_t standbyTheta = 0;   // idle-flyer orbit phase (retail steps it ~-120 deg)
    uint8_t moveState = 2;
    uint8_t fireState = 2;
    int   stance = 1;      // combat stance: 0=offensive (chase freely), 1=defensive
                           // (leashed, the default/legacy behaviour), 2=passive
                           // (hold fire: no auto-acquire, only fights when ordered)
    int8_t squad = 0;      // control squad: 0=none, +N=group N, -N=formation N (N=1..10,
                           // the digit 0 key = 10). A unit is in exactly one squad. A
                           // formation (squad<0) moves at its slowest member's speed and
                           // its stragglers rejoin. Set via Cmd::SetSquad; folded in the hash.
    int   lastHitBy = 0;   // id of the unit that last damaged this one (for kill XP)
    float captureProg = 0; // canCapture units: seconds spent charming the current target
    float homeX = 0, homeZ = 0;   // leash anchor (idle position) for auto-chase
    bool  justFired = false;   // set for one tick when the weapon fires
    bool underConstruction = false;
    bool buildBegun = false;   // construction site: true once the builder arrived
    float conjureRate = 0;     // site: hp/sec the last builder added; drives decay
    bool  beingBuilt = false;  // site: transient -- a builder worked it this tick
    int buildSiteId = 0;   // builder: id of the building it is constructing
    // These queues hold at most a handful of entries and are edited only on order
    // completion (not in a hot inner loop), so std::vector -- which allocates NOTHING
    // when empty, unlike std::deque's eager ~576-byte control block -- is both the
    // memory-lean and the faster choice; front-pops become erase(begin()). Element
    // order (all that stateHash folds in) is preserved, so lockstep is byte-identical.
    int reclaimId = 0;                 // builder: feature being reclaimed (0 = none)
    int repairId = 0;                  // builder: damaged friendly being repaired (0 = none)
    int inTransport = 0;   // id of carrying transport, 0 = none
    std::vector<int> cargo;
    std::vector<Order> orders;
    // Production (buildings with a build tree).
    std::vector<const UnitType*> buildQueue;
    float buildProgress = 0;   // seconds of work done on queue front
    int justBuilt = 0;         // unit id produced this tick (viewer hook), else 0
    const UnitType* repeatType = nullptr;   // infinite production: re-queue when idle

    bool alive() const { return deadFor < 0; }
    bool embarked() const { return inTransport != 0; }
    // Frozen/petrified/paralyzed units can't move, turn, or fire.
    bool incapacitated() const { return frozenFor > 0 || stonedFor > 0 || paralyzedFor > 0; }
    // Veterancy stat multiplier (retail: base 1.0, +10% per level, level capped
    // at 10 => up to 2.0x). Scales attack up, armor up (less damage taken), and
    // reload down (faster). Verified against KINGDOMS.icd. See retail-engine-internals.
    float vetMul() const { return 1.0f + 0.10f * float(veteran); }
    bool moving() const { return alive() && (speed > 1.0f || !orders.empty()); }
    // Actually translating (for the walk animation), vs standing with an
    // attack/queued order.
    bool walking() const { return alive() && speed > 3.0f; }
};

// A reclaimable map feature (tree, rock, house, wreckage, …). Positions and stats
// are derived deterministically in setupMatch -- the same row-major walk runs on
// every peer -- so reclaim stays in lockstep. Not a Unit: no orders or combat,
// just reclaim "work" a builder chips away for mana. A `blocks` feature also
// occupies the nav grid, freed when it is reclaimed.
// Per-feature-type burn/footprint data the sim needs (built by setupMatch from
// the feature TDFs, identical on every peer -- indices are shared lockstep state).
struct FeatType {
    std::string name;        // lowercase TDF section name (client art lookup)
    bool flamable = false;   // TDF flamable=1: can be ignited / spread to
    bool hasBurnAnim = false;// TDF seqnameburn present (retail StartBurning requires it)
    int  spreadChance = 0;   // TDF spreadchance (percent)
    int  sparkTicks = 0;     // TDF sparktime * 30 (retail stores seconds*30)
    int  burntType = -1;     // TDF featureburnt -> index into the same table
    float energy = 0;        // reclaim yield of this stage
    int  fx = 1, fz = 1;
    bool blocking = false;
    // Corpse defs (features/corpses/*_dead.tdf): how long the body lies there
    // and whether a priest can raise it (retail decomposetime / resurrectable).
    int  decomposeTicks = 0; // TDF decomposetime * 30 (0 = never rots)
    bool resurrectable = false;
    bool reclaimable = false;
    bool isStone = false;    // TDF isstone=1 (statue: client tints it stone-gray)
    bool isFrozen = false;   // TDF isfrozen=1 (client tints it ice-blue)
    bool indestructible = false;
    float hp = 0;            // TDF damage= (weapon damage the feature absorbs)
    int  deadType = -1;      // TDF featuredead -> destroyed-replacement (placed neutral)
    std::string object;      // TDF object= (3D corpse mesh; client visual)
};

struct Feature {
    int   id = 0;          // cz*terrainWidth + cx: position-derived, peer-identical
    float x = 0, z = 0;
    int   fx = 1, fz = 1;  // footprint cells (for the nav unblock on removal)
    float manaYield = 0;   // total mana granted over a full reclaim (FBI `energy`)
    float work = 0;        // remaining reclaim work; consumed to 0
    float workFull = 1;    // initial work (for the proportional mana drip)
    bool  blocks = false;  // occupied the nav grid
    bool  alive = true;    // false once fully reclaimed (decal disappears)
    // Burning (retail mechanic, icd 0x494b40/0x495110/0x495300 -- ours runs
    // fully deterministic in the lockstep sim instead of retail's host-authority
    // event scheme). All hashed.
    int   type = -1;       // index into World's FeatType table (-1 = untyped)
    uint8_t burn = 0;      // 1 = burning
    float dmg = 0;         // accumulated weapon damage (dies at FeatType.hp)
    int   spreadIn = 0;    // ticks until the single spread event (sparktime-derived)
    int   burnLeft = 0;    // ticks until burn-out (swap to burntType / die)
};

struct Projectile {
    float x = 0, z = 0;
    float vx = 0, vz = 0;
    float damage = 0;
    int targetId = 0;
    int fromPlayer = 0;
    float life = 0;        // seconds left before it fizzles
    float age = 0;         // seconds since launch
    float flight = 1;      // expected seconds to target (for the render arc)
    // Set the moment a shot registers its direct hit. `life = -1` doubles as the
    // erase predicate, and `life -= dt` runs BEFORE the hit test, so on the final
    // tick a projectile can both connect and expire -- without this flag the
    // expiry-detonation below would crater on top of a hit that already landed.
    bool spent = false;
    int fromId = 0;        // firing unit id (for kill attribution / veterancy)
    const Weapon* wsrc = nullptr;   // source weapon (splash + per-category damage)
    WeaponFx fx = WeaponFx::Arrow;   // how the viewer draws it
};

// Walkability grid derived from TNT heights: a cell is blocked when the
// local height spread exceeds a cliff threshold.
class NavGrid {
public:
    NavGrid() = default;
    NavGrid(const std::vector<uint8_t>& heights, int w, int h, int cliff = 20);
    // Retail's passability rule (KINGDOMS.icd 0x508660 ClassifyCell). The TNT height
    // bytes are a CORNER LATTICE at 16-unit spacing, not per-cell centre values, so
    // cell (x,z)'s surface is the quad between corners (x,z) and (x+1,z+1). Retail
    // caches that quad's MAX and MIN (cell+5 / cell+6, written by 0x50ed60) and
    // blocks a cell when:
    //   MIN  <  sea - maxWaterDepth   (deepest point deeper than the class allows)
    //   MAX  >  sea - minWaterDepth   (shallowest point shallower than it needs)
    //   MAX - MIN > (MIN < sea ? maxWaterSlope : maxSlope)
    // This is a WITHIN-cell spread. Our old rule asked whether a NEIGHBOUR rises
    // above us, which blocks the cells at the FOOT of every slope and severs ramps
    // -- on Inner Circle that cost two thirds of the map's connectivity.
    struct Limits {
        int maxSlope = 255, maxWaterSlope = 255;
        int maxWaterDepth = 10000, minWaterDepth = -10000;   // retail's ctor defaults
    };
    NavGrid(const std::vector<uint8_t>& heights, int w, int h, int sea, const Limits& lim);

    // Terrain-only walkability: ignores the shared obstacle overlay. Used to tell a
    // cell blocked by a clearable doodad apart from one blocked by the ground itself.
    bool terrainWalkable(int cx, int cz) const {
        if (cx < 0 || cz < 0 || cx >= w_ || cz >= h_) return false;
        return cells_[size_t(cz) * size_t(w_) + size_t(cx)] != 0;
    }
    bool walkable(int cx, int cz) const {
        if (cx < 0 || cz < 0 || cx >= w_ || cz >= h_) return false;
        size_t i = size_t(cz) * size_t(w_) + size_t(cx);
        if (obst_ && (*obst_)[i]) return false;   // shared obstacle overlay
        return cells_[i] != 0;
    }
    // Obstacles -- buildings, blocking features, wrecks, the wall-occlusion pass --
    // block EVERY movement class equally, so they live in one overlay shared by all
    // the grids rather than being stamped into each. Before this they were stamped
    // into the ground grid alone, which is why hover units used to path straight
    // through buildings. `obst` must outlive the grid and match its dimensions.
    void setObstacles(const std::vector<uint8_t>* obst) {
        obst_ = (obst && obst->size() == size_t(w_) * size_t(h_)) ? obst : nullptr;
        clearDirty_ = true;
    }
    // Does a `foot`-cell-across unit fit CENTRED on (cx,cz)? (foot<=1 == walkable.)
    // Backed by a lazily-built clearance grid, so O(1). Footprint-aware pathing and
    // steering use this so a 4x4 unit never routes through a 1-cell gap and wedges.
    bool fits(int cx, int cz, int foot) const;
    // Force the lazy clearance grid up to date NOW. The parallel flow-field
    // prefetch calls this before handing the grid to worker threads, so they
    // only ever READ it (fits() would otherwise rebuild the mutable cache
    // concurrently from several threads).
    void ensureClearance() const { if (clearDirty_) rebuildClearance(); }
    bool empty() const { return cells_.empty(); }
    int width() const { return w_; }
    int height() const { return h_; }
    // Mark a rectangle of cells blocked (building footprint) or clear.
    void block(int cx, int cz, int w, int h, bool blocked);
    // Road preference (ground grid only): pathfinding rates non-road steps ~1.2x
    // costlier, so marches drift onto highways -- the retail steering area-rater
    // scores road cells 7 vs 6 (icd 0x508527). `roads` must outlive the grid and
    // match its dimensions (World::roads_; null = no preference).
    void markClearanceDirty() { clearDirty_ = true; ++version_; }
    // Bumped on every walkability edit (own cells or the shared overlay), so caches
    // derived from this grid can tell when they have gone stale.
    uint64_t version() const { return version_; }
    void setRoads(const std::vector<uint8_t>* roads) {
        roads_ = (roads && roads->size() == size_t(w_) * size_t(h_)) ? roads : nullptr;
    }
    bool roadAt(int cx, int cz) const {
        return roads_ && (*roads_)[size_t(cz) * w_ + cx] != 0;
    }
    bool hasRoads() const { return roads_ != nullptr; }

    // A* in cell space (16px cells), with waypoint simplification. `foot` = the unit's
    // footprint size in cells (1 = point). Returns world-space waypoints; empty if
    // unreachable for a unit that size.
    std::vector<Order> findPath(float x0, float z0, float x1, float z1, int foot = 1) const;

    // Line of sight: no blocked cell between the two world points, ignoring cells
    // within `skip0`/`skip1` cells of each endpoint — so a shooter or target's own
    // building footprint doesn't block the shot, but a wall between them does.
    bool losBetween(float wx0, float wz0, float wx1, float wz1,
                    int skip0 = 0, int skip1 = 0) const;

private:
    bool lineClear(int x0, int z0, int x1, int z1) const;
    void rebuildClearance() const;

    std::vector<uint8_t> cells_;
    // findPath scratch, reused across calls (a big map otherwise allocates + fills
    // ~1.1MB per search). A cell's g/from are valid only when its stamp matches
    // pathGen_, so "reset" is one counter bump. mutable like clear_ -- findPath is a
    // logical-const query and the sim is single-threaded per world.
    mutable std::vector<float> pathG_;
    mutable std::vector<int> pathFrom_;
    mutable std::vector<uint32_t> pathStamp_;
    mutable uint32_t pathGen_ = 0;
    // clear_[c] = side of the largest all-walkable square whose min corner is c.
    // Lazily rebuilt (dirtied by block()); a foot-cell unit fits at corner c iff
    // clear_[c] >= foot. mutable so fits()/pathfinding can build it on demand.
    // Values saturate at 15 (the max queryable footprint), which is what lets
    // block() refresh only a bounded rect instead of dirtying the whole grid.
    void updateClearanceRect(int cx, int cz, int w, int h) const;
    mutable std::vector<uint16_t> clear_;
    mutable bool clearDirty_ = true;
    uint64_t version_ = 1;
    const std::vector<uint8_t>* roads_ = nullptr;
    const std::vector<uint8_t>* obst_ = nullptr;   // shared obstacle overlay   // see setRoads
    int w_ = 0, h_ = 0;
};

// Block/unblock a building's yardmap-aware footprint on a nav grid.
void blockFootprint(NavGrid& nav, const UnitType& t, float x, float z, bool blocked);

struct Player {
    float mana = 500;
    float storage = 0;   // recomputed each tick from alive units
    float income = 0;
    // Income multiplier (1.0 = normal). Only ever != 1 for an Absurd-difficulty AI,
    // set once at match setup and applied to every mana source. Constant per game and
    // its effect lands in `mana` (which IS hashed), so it need not be hashed itself,
    // but every peer must set it identically or their mana diverges.
    float manaMult = 1.0f;
    // God economy: priests (attractsgods) channel mana into favour; once it fills
    // after the gods' appear time, the faction's god can manifest (once).
    float godFavor = 0;
    bool  godSummoned = false;
    int   kills = 0;     // enemy units this player has destroyed (F4 overlay)
    // End-of-game scoreboard counters (retail's victory/defeat screen columns).
    // Derived from hashed events and incremented in exactly one place each, so they
    // stay identical on every peer without being folded into stateHash -- same
    // treatment as `kills` above.
    int   built = 0;     // units this player has put into the field
    int   losses = 0;    // this player's units destroyed
    // Live unit count, refreshed every tick by updateOutcome (and bumped by spawn
    // within a tick). Derived from units_, so it is deterministic but NOT hashed;
    // drives the unit-cap check and could feed the F4 overlay.
    int   unitCount = 0;
    // Alliance layer above ownership: players sharing a team fight together and
    // share vision. Default (set at game setup) = the player's own index, i.e.
    // a free-for-all where everyone is on their own team. Immutable in v1 --
    // if diplomacy ever makes it mutable it must be a sequenced command AND
    // enter stateHash (see docs/multiplayer-design.md).
    int   team = 0;
    bool  defeated = false;   // no living units; set by the sim's win check
    float defeatedAt = -1;    // world clock when `defeated` first went true (-1 = still in)
    // Cosmetic "disco" emote (Shift+D): seconds this player's monarchs keep
    // dancing. Set by a lockstep Cmd::Disco so every peer agrees on the timing,
    // but it drives client-side eye-candy only and is NOT folded into stateHash
    // (like vis_).
    float discoLeft = 0;
    // Cosmetic "headbang" emote (Shift+H): seconds this player's monarchs headbang to
    // heavy metal. Same deal as discoLeft -- synced by Cmd::Headbang, not hashed.
    float headbangLeft = 0;
};

// Max simultaneous players/teams (the retail map ceiling is 8 start positions).
constexpr int kMaxPlayers = 8;

class MissionScript;   // src/sim/mission.h -- optional campaign "god" script + win/lose
class ScenarioScript;  // src/sim/scenario.h -- optional .crt per-player trigger runner

// Benchmark staged-spawn plan: a set of units to spawn at a scheduled tick. Built once in
// setupMatch (deterministically -- so the client sim and the referee build the identical
// plan) and executed by World::tick. See MatchConfig::benchmark.
struct BenchSpawn { const UnitType* type = nullptr; float x = 0, z = 0; int player = 0; };
struct BenchStage { uint32_t tick = 0; std::vector<BenchSpawn> units; };

class World {
public:
    int spawn(const UnitType* type, float x, float z, float heading = 0, int player = 0);
    // Benchmark: install the staged spawn plan (see BenchStage). endTick marks when the
    // benchmark run finishes; benchmarkMode() is true while a plan is installed.
    void setBenchmarkPlan(std::vector<BenchStage> plan, uint32_t endTick) {
        benchPlan_ = std::move(plan); benchCursor_ = 0; benchEndTick_ = endTick;
    }
    bool benchmarkMode() const { return benchEndTick_ > 0; }
    uint32_t benchmarkEndTick() const { return benchEndTick_; }
    uint32_t tickCount() const { return tickCounter_; }   // ticks elapsed (benchmark timing)
    // Build per-domain nav grids from heights + sea level. `features` is the
    // map's raw per-cell feature plane if available: retail stores per-cell
    // attributes in it as special values -- 0xFFFB marks ROAD cells (the
    // roadmultiplier speed bonus + the COB walk_road gait).
    void setTerrain(const std::vector<uint8_t>& heights, int w, int h, int seaLevel,
                    const std::vector<uint16_t>* features = nullptr);
    NavGrid& nav() { return nav_; }

    // Retail's per-cell query for one unit's movement class (icd 0x4139d0 ->
    // 0x413c80): impassable below the threshold, 4 when a parked body holds the
    // cell, 6 ordinary ground, 7 road. The pathfinder scores every candidate
    // through this, so it sees exactly what the mover will.
    int cellScore(const UnitType* t, int cx, int cz, int selfId) const;

    // Enable retail's background pathfinder for this world (default off).
    void setPathService(bool on) { pathService_ = on; if (!on) paths_.clear(); }
    bool pathService() const { return pathService_; }
    // Path search work units per tick, shared across all pending requests.
    void setPathBudget(int b) { paths_.setBudget(b); }

    // Passability is a function of the unit's MOVEMENT CLASS, not its domain:
    // retail bakes one grid per class at map load and its path search reads that
    // (KINGDOMS.icd 0x4e0940 builds them, 0x4139d0 queries them). A Catapult's
    // MaxSlope of 15 really is a different map from a Swordsman's 30. We bake one
    // grid per distinct LIMIT TUPLE rather than per class, which is the same thing
    // with the duplicates collapsed -- the shipped classes reduce to a handful.
    // `navClasses_` is filled by buildNavClasses() right after setTerrain.
    const NavGrid& navFor(const UnitType* t) const {
        if (t) {
            auto it = navIdx_.find(t);
            if (it != navIdx_.end()) return navClasses_[size_t(it->second)];
        }
        // No class table (a bare test world, or a type registered after the map):
        // fall back to the three legacy domain grids.
        if (t && t->domain == UnitType::Domain::Water) return navWater_;
        if (t && t->domain == UnitType::Domain::Hover) return navHover_;
        return nav_;
    }
    // Build one nav grid per distinct movement-limit tuple across the registry's
    // types. Call after setTerrain. Deterministic: grids are keyed and ordered by
    // the tuple itself, so every peer builds the same table in the same order.
    void buildNavClasses(const class TypeRegistry& reg);
    // Block/clear a rectangle for EVERY movement class at once (a building, a
    // blocking feature, a wreck). Writes the shared overlay and dirties the
    // clearance on every grid, so a new class grid built later inherits it.
    void blockCells(int cx, int cz, int w, int h, bool blocked);
    // Same, for a unit type's footprint (honours its yardmap like blockFootprint).
    void blockFoot(const UnitType& t, float x, float z, bool blocked);
    const std::vector<uint8_t>& obstacles() const { return obst_; }
    // Queue production of `typeId` at a builder building.
    void train(int builderId, const UnitType* type, int count = 1);   // queue `count`
    void dequeue(int builderId, const UnitType* type, int count);     // un-queue `count`
    void setRepeat(int builderId, const UnitType* type);   // toggle infinite build
    // How many of `type` are queued at a builder (for the build-icon count).
    int queuedCount(int builderId, const UnitType* type) const;
    // Mobile builder constructs a building at (x, z). Returns the new
    // building's id, or 0 if the site is invalid.
    // How startBuild gets the builder to the site:
    //   Replace -- the usual fresh order: walk there, dropping whatever it was doing.
    //   None    -- it is already standing in position (a queued build that has just
    //              come due), so issuing an approach would only disturb the queue.
    enum class Approach { Replace, None };
    int startBuild(int builderId, const UnitType* type, float x, float z,
                   Approach approach = Approach::Replace);
    // Build now if the builder is free, else queue it (shift-click). A
    // non-queued order replaces any pending queue.
    void queueBuild(int builderId, const UnitType* type, float x, float z, bool queue);
    // Abandon a builder's queued builds and drop any not-yet-started site.
    void cancelBuilds(int builderId);
    // Latch a mobile builder onto an existing construction site to resume/assist
    // conjuring it (e.g. reviving a decaying site). Resumes at THIS builder's
    // rate from the site's current HP. The caller checks the build tree.
    void assist(int builderId, int siteId, bool queue = false);
    // Cosmetic emote: make `player`'s monarchs dance for 10s (Cmd::Disco). Not
    // hashed -- purely for the viewer. discoActive() gates the client animation.
    void startDisco(int player);
    bool discoActive(int player) const;
    // Cosmetic emote: make `player`'s monarchs headbang for 10s (Cmd::Headbang).
    void startHeadbang(int player);
    bool headbangActive(int player) const;
    bool canPlace(const UnitType* type, float x, float z) const;
    // Would the site be placeable if the clearable doodads on it were gone? Fills
    // `out` with their feature ids, nearest-first is the caller's job. Returns false
    // when anything ELSE blocks -- terrain, a unit, a building, an unreclaimable
    // feature -- because those are refusals retail makes too and we keep. Read-only:
    // this is a query for the UI, and it changes no sim state.
    bool clearableForPlacement(const UnitType* type, float x, float z,
                               std::vector<int>& out) const;
    // Mana deposit ("Sacred Stone") spots, in world px. Lodestones (onMana)
    // can only be built on one, but only when the map actually has any.
    void setManaSpots(std::vector<std::pair<float, float>> spots) {
        manaSpots_ = std::move(spots);
    }
    bool hasManaSpots() const { return !manaSpots_.empty(); }
    const std::vector<std::pair<float, float>>& manaSpots() const { return manaSpots_; }
    // Reclaimable features (trees/rocks/houses). Populated only by setupMatch (the
    // one deterministic per-peer walk); never from the viewer. See struct Feature.
    void addFeature(int id, float x, float z, float manaYield, float work,
                    int fx, int fz, bool blocks, int type = -1);
    const std::vector<Feature>& features() const { return features_; }
    const Feature* feature(int id) const;                 // by id, nullptr if none
    const Feature* featureAt(float x, float z) const;     // by cell (viewer burn/art sync)
    void setFeatureTypes(std::vector<FeatType> t) { featTypes_ = std::move(t); }
    // Unit type -> its corpse feature def (index into featTypes_, -1 = none).
    void mapCorpse(const UnitType* t, int featType) { corpseType_[t] = featType; }
    void mapStatue(const UnitType* t, int stoneIdx, int frozenIdx) {
        if (stoneIdx >= 0) stoneType_[t] = stoneIdx;
        if (frozenIdx >= 0) frozenType_[t] = frozenIdx;
    }
    int statueTypeOf(const UnitType* t, bool frozen) const {
        const auto& m = frozen ? frozenType_ : stoneType_;
        auto it = m.find(t);
        return it == m.end() ? -1 : it->second;
    }
    int corpseTypeOf(const UnitType* t) const {
        auto it = corpseType_.find(t);
        return it == corpseType_.end() ? -1 : it->second;
    }
    // Drop all registered features (setupMatch rebuilds authoritatively -- the
    // client ctor may have pre-registered the launch map's via registerMapFeatures).
    void clearFeatures() { features_.clear(); featureIdx_.clear(); }
    const std::vector<FeatType>& featureTypes() const { return featTypes_; }
    bool featureAliveAt(float x, float z) const;          // viewer decal sync
    // Order a mobile builder to reclaim feature `featureId` (queue = append to its
    // reclaim queue, for an area drag). Grants the feature's mana as it consumes it.
    void reclaim(int builderId, int featureId, bool queue);
    // Order a mobile builder to repair damaged friendly `targetId` (restores HP at
    // the builder's work rate, draining mana proportionally). tickRepair runs it.
    void repair(int builderId, int targetId, bool queue);
    void tickRepair(Unit& b, float dt);
    // True if (x,z) lies over water (for choosing the water impact effect).
    // Retail (icd 0x509760): a unit is "on road" only when EVERY cell under its
    // footprint carries the road flag (ground units only; checked per tick into
    // a status bit that GET 34 and the speed formula read).
    bool onRoad(float x, float z, int footX = 1, int footZ = 1) const {
        if (roads_.empty()) return false;
        int cx = int(x) / 16, cz = int(z) / 16;
        int x0 = cx - footX / 2, z0 = cz - footZ / 2;
        if (x0 < 0 || z0 < 0 || x0 + footX > terW_ || z0 + footZ > terH_)
            return false;
        for (int dz = 0; dz < footZ; ++dz)
            for (int dx = 0; dx < footX; ++dx)
                if (!roads_[size_t(z0 + dz) * terW_ + size_t(x0 + dx)]) return false;
        return true;
    }
    // .ota waterdoesdamage/waterdamage: on two Iron Plague missions the water is
    // lethal, which is the whole point of their terrain. Damage is per second.
    void setWaterDamage(float perSec) { waterDamage_ = perSec; }
    bool isWater(float x, float z) const {
        if (depth_.empty()) return false;
        int cx = int(x) / 16, cz = int(z) / 16;
        if (cx < 0 || cz < 0 || cx >= terW_ || cz >= terH_) return false;
        return depth_[size_t(cz) * terW_ + cx] > 0;
    }
    Player& player(int i) { return players_[size_t(i)]; }
    const Player& player(int i) const { return players_[size_t(i)]; }
    int numPlayers() const { return int(players_.size()); }
    // Reset all simulation state so a match can be rebuilt (via setupMatch) and
    // replayed from tick 0 deterministically -- crucially nextId_ resets so the
    // replayed spawns get the SAME unit ids as the original run. setTerrain and
    // setPlayerCount (called by setupMatch afterwards) rebuild nav/vis/players.
    void resetForReplay() {
        units_.clear();
        projectiles_.clear();
        hits_.clear();
        features_.clear();
        featureIdx_.clear();
        // Every other kind of live cross-tick sim state has to go too, or a rejoin
        // replays tick 0 with leftovers from before the drop: a storm still roaming
        // (or a spell still channelling) would deal damage the referee never dealt
        // AND advance the shared RNG on this peer alone -- an instant desync. The
        // RNG itself is re-seeded for the same reason: replaying from a mid-match
        // stream position diverges from a referee that started fresh.
        pendingEffects_.clear();
        storms_.clear();
        stormSeq_ = 0;
        deathBlasts_.clear();
        burnRng_ = 0x54414B21;
        nextId_ = 1;
        tickCounter_ = 0;
        clock_ = 0;
        winningTeam_ = -1;
    }
    // Size the player table for a match (default 4 for single-player/scenarios).
    // Call BEFORE spawning any units -- shrinking it after would leave units with
    // an out-of-range owner index (dereferenced unchecked in the economy loop).
    void setPlayerCount(int n) {
        players_.assign(size_t(std::clamp(n, 1, kMaxPlayers)), Player{});
        for (int i = 0; i < numPlayers(); ++i) players_[size_t(i)].team = i;
    }
    // Assign a player's team, clamped to a valid team id [0, numPlayers). Teams
    // are 0..n-1 by construction; keeping them in range is the invariant that the
    // outcome check and the viewer's per-team arrays rely on -- so the sim owns
    // it rather than trusting setup (a lobby, or the TAK_FFA dev harness).
    void setTeam(int player, int team) {
        if (player < 0 || player >= numPlayers()) return;
        players_[size_t(player)].team = std::clamp(team, 0, numPlayers() - 1);
    }
    // Two players are allied if they share a team (a player is allied to itself).
    bool allied(int a, int b) const {
        if (a == b) return true;
        if (a < 0 || b < 0 || a >= numPlayers() || b >= numPlayers()) return false;
        return players_[size_t(a)].team == players_[size_t(b)].team;
    }

    // God economy (gamedata/Gods.tdf). Enable it, then a player whose god favour
    // fills after `appearSec` may manifest its god — the viewer polls godReady().
    void enableGods(float appearSec) { godsEnabled_ = true; godAppearTime_ = appearSec; }
    bool godsEnabled() const { return godsEnabled_; }

    // Per-player unit limit: production and new builds stall a player once it has
    // this many live units (0 = unlimited). Set at match start (from the lobby).
    // The count it tests (Player::unitCount) is deterministic, so all peers agree.
    void setUnitCap(int c) { unitCap_ = c; }
    int unitCap() const { return unitCap_; }
    bool atUnitCap(int player) const {
        return unitCap_ > 0 && player >= 0 && player < int(players_.size()) &&
               players_[size_t(player)].unitCount >= unitCap_;
    }
    // FBI `totalallowed`: a per-player cap on LIVE units of one type (the five
    // dragons, the five gods and the Aerial Juggernaut ship 1). Retail refuses to
    // finish a conjure that would exceed it, so a player fields only one. Counts
    // units under construction too, so queuing two dragons can't sneak both out.
    bool atTypeCap(int player, const UnitType* t) const {
        if (!t || t->totalAllowed <= 0) return false;
        int n = 0;
        for (const auto& u : units_)
            if (u.alive() && u.type == t && u.player == player &&
                ++n >= t->totalAllowed)
                return true;
        return false;
    }
    static constexpr float kGodFavorNeeded = 3000.0f;
    bool godReady(int t) const {
        return godsEnabled_ && !players_[size_t(t)].godSummoned &&
               clock_ >= godAppearTime_ && players_[size_t(t)].godFavor >= kGodFavorNeeded;
    }

    // Win/defeat, computed sim-side so every lockstep peer agrees on the same
    // tick. A player is defeated when it has no living units; a team is out when
    // all its players are. Returns the surviving team id once exactly one team
    // remains (the winner), or -1 while >1 team still has units. Updates each
    // Player::defeated as a side effect. Idempotent; call once per tick.
    int updateOutcome();
    int winningTeam() const { return winningTeam_; }

    // Which player the fog-of-war grid tracks (default 0 = local player).
    void setVisPlayer(int t) { visPlayer_ = t; }
    // Fog-of-war memory (client display only, never hashed): true = a seen cell stays
    // EXPLORED (dimmed) when it leaves sight; false = NOT EXPLORED -- it reverts to dark.
    void setFogExplored(bool e) { fogExplored_ = e; }
    // Monarch-expendable rule (net GameOptions): when FALSE, losing your Monarch
    // (a commander unit) loses you the game even if other units survive.
    void setMonarchExpendable(bool e) { monarchExpendable_ = e; }
    // Run flow-field prefetch on the calling thread only (no worker pool). The server
    // sets this on a referee World when it ticks several games IN PARALLEL: the
    // parallelism is already at the game level, so a per-game nested flow pool would
    // just oversubscribe. A lone game (SP, or the client's own sim) leaves it off and
    // keeps the intra-tick flow parallelism. Never affects results -- purely how the
    // (identical) flow builds are scheduled.
    // The server ticks several games at once and puts the parallelism at the GAME
    // level, so a room's sim must not spawn its own workers. Also honoured by the
    // fog/visibility passes (it was serialFlow_ when flow fields honoured it too).
    void setSerialThreads(bool s) { serialThreads_ = s; }
    // Deterministic digest of sim state, for lockstep sync checking.
    uint64_t stateHash() const;

    // Campaign mission: an optional in-sim "god" script + win/lose rules
    // (src/sim/mission.h). Ticked inside tick(), and folded into stateHash().
    World();                                                    // out-of-line (mission_ member)
    ~World();                                                   // out-of-line (mission_ dtor)
    void setMission(std::unique_ptr<MissionScript> m);
    MissionScript* mission() { return mission_.get(); }
    int missionOutcome() const;   // 0 none/running / +1 victory / -1 defeat

    // Scenario (.crt) trigger runner: an optional in-sim per-player rule engine
    // (src/sim/scenario.h). Ticked inside tick() and folded into stateHash().
    void setScenario(std::unique_ptr<ScenarioScript> s);
    ScenarioScript* scenario() { return scenario_.get(); }
    // Force a player to count as defeated regardless of its unit count (a
    // scenario Victory/Defeat action); respected by updateOutcome, hashed.
    void forceDefeat(int player);
    // Bumped whenever a feature's DISPLAY-RELEVANT state changes: added, ignited,
    // swapped to a burnt stage, or reclaimed away. The renderer's feature sync used to
    // take simMutex_ and rescan every feature every frame to discover changes that, in
    // a measured 45s run, happened on 0% of frames -- ~1ms of pure lock WAIT per frame
    // (the scan itself was 0.012ms). Reading this atomic costs nothing and needs no
    // lock, so the scan only runs when there is something to find. Display-only:
    // deliberately NOT part of stateHash, and nothing in the sim reads it.
    uint32_t featGeneration() const { return featGen_.load(std::memory_order_acquire); }

    // Fog of war for the local player over 16px cells: 0 hidden, 1 explored, 2 visible.
    const std::vector<uint8_t>& visibility() const { return vis_; }
    uint32_t visGeneration() const { return visGen_; }   // bumps on each fog recompute
    int visW() const { return visW_; }
    int visH() const { return visH_; }
    bool cellVisible(float x, float z) const {
        if (vis_.empty()) return true;
        int cx = int(x) / 16, cz = int(z) / 16;
        if (cx < 0 || cz < 0 || cx >= visW_ || cz >= visH_) return false;
        return vis_[size_t(cz) * visW_ + cx] == 2;
    }
    // Move order; queue appends. Ground units steer by a shared flow field
    // toward (x,z) (crowd-friendly); flyers and unreachable goals fall back to
    // A* waypoints.
    void order(int unitId, float x, float z, bool queue);

    // ---- order-queue helpers ------------------------------------------------
    // One issued order can expand into a whole A* path, so Unit::orders mixes
    // pathfinding waypoints with the goals the player actually asked for. These
    // two exist because the repath/unstick paths used to read orders.back() as
    // "the destination" -- which is the LAST QUEUED leg, not the current one --
    // and then clear the whole queue, silently deleting everything the player had
    // lined up behind it.

    // Index of the last waypoint of the leg being worked on right now. Falls back
    // to the whole queue for unmarked orders (mission scripts, older saves).
    static size_t currentLeg(const std::vector<Order>& o) {
        for (size_t i = 0; i < o.size(); ++i) if (o[i].goal) return i;
        return o.empty() ? 0 : o.size() - 1;
    }
    // Swap the current leg's waypoints for `path`, keeping the leg's own flags
    // and every order queued behind it.
    static void replaceLeg(Unit& u, const std::vector<Order>& path);
    // Drop the current leg entirely and move on to whatever was queued behind it.
    static void dropLeg(Unit& u);
    // Does this unit still have construction queued (anywhere in its orders)?
    static bool hasQueuedBuild(const Unit& u) {
        for (const Order& o : u.orders) if (o.buildType) return true;
        return false;
    }
    // Any construction OR reclaim still queued -- the "this builder is on a job"
    // test that several places need.
    static bool hasQueuedWork(const Unit& u) {
        for (const Order& o : u.orders)
            if (o.buildType || o.reclaimFeat || o.repairTarget) return true;
        return false;
    }
    void attackMove(int unitId, float x, float z, bool queue);
    // Can a unit of `type` at (fx,fz) actually reach goal (gx,gz)? (flow-field
    // connectivity). Lets the AI pick a REACHABLE target instead of one that's
    // merely nearest in a straight line but walled off (army would stall/pile).
    bool pathExists(const UnitType* type, float gx, float gz, float fx, float fz) const;
    void patrol(int unitId, float x, float z);
    // Queue a patrol waypoint (SetMission "p X Y"): like a move but the completed
    // order re-queues at the back, so a chain of these loops the unit through them.
    void patrolTo(int unitId, float x, float z, bool queue);
    // Queue a timed hold (SetMission "w N"): the unit stands still for `seconds`
    // before the next queued order runs.
    void orderWait(int unitId, float seconds, bool queue);
    // Queue an ambush hold (SetMission "wa"): the unit stands still until a non-allied
    // unit comes within its sight, then the next queued order runs.
    void orderWaitAttack(int unitId, bool queue);
    void guard(int unitId, int targetId, bool queue);
    void stop(int unitId);
    // Self-destruct a living unit (Ctrl+D via the command path; no kill credit).
    void destroy(int unitId);
    void setWeapon(int unitId, int slot);   // choose the active weapon (0=primary)
    void setStance(int unitId, int stance); // combat stance 0=offensive/1=defensive/2=passive
    void setCloak(int unitId, bool on);     // canCloak unit: enable/disable cloaking
    void setActive(int unitId, bool on);    // onOffable unit: power on/off
    void setSquad(int unitId, int squad);   // control squad: 0 none, +N group N, -N formation N
    // Attack order on an enemy unit.
    void attack(int unitId, int targetId, bool queue);
    // Board a friendly transport / sail to (x,z) and disembark.
    void loadInto(int unitId, int transportId);
    void unloadAt(int transportId, float x, float z);
    void tick(float dt);

    std::vector<Unit>& units() { return units_; }
    const std::vector<Unit>& units() const { return units_; }
    // A wandering storm (FBI type=wandering): a roaming hazard the VIEWER must
    // draw, so its state is public like a projectile's. `id` is stable for the
    // storm's life so the viewer can play its spin-up, loop and dissipation art.
    struct Storm {
        const Weapon* w = nullptr;
        float x = 0, z = 0;
        float dirX = 0, dirZ = 1;   // FIXED launch direction: a storm never re-aims
        float jitX = 0, jitZ = 0;   // current per-tick wander offset (px/tick)
        int player = 0, fromId = 0;
        int id = 0;
        float arm = 0;       // builduptime: it drifts but does not bite yet
        float left = 0;      // seconds of roaming left (duration)
        float nextVary = 0;  // seconds until the next wander re-roll
    };
    const std::vector<Storm>& storms() const { return storms_; }
    const std::vector<Projectile>& projectiles() const { return projectiles_; }
    Unit* unit(int id);
    const Unit* unit(int id) const {
        return const_cast<World*>(this)->unit(id);
    }

    // Weapon impacts this tick (view-side: impact sound + effect). Carries the
    // source weapon (soundhitclass / hweffect) and the unit struck (body material
    // for the material-specific hit sound). Cleared at the start of each tick.
    struct HitFx { float x = 0, z = 0; const Weapon* weapon = nullptr;
                   const UnitType* target = nullptr;
                   int victimId = 0;            // primary struck unit (0 = ground hit)
                   float fromX = 0, fromZ = 0;  // attacker pos (viewer flinch direction)
                   float damage = 0; };         // pre-armour damage vs the victim
    const std::vector<HitFx>& hits() const { return hits_; }
    // A mission script asking for a camera shake (the ScreenShake map command).
    // Viewer-only: the sequence number is what the client watches for an edge, and
    // it is deterministic because the script that bumps it runs in lockstep.
    struct ShakeReq { float mag = 0, dur = 0; uint32_t seq = 0; };
    const ShakeReq& shakeRequest() const { return shakeReq_; }
    // A mission script's PLAY_SOUND (scripted story VO). Viewer-only, same edge
    // protocol as the shake: the sim cannot play audio, so it records what was
    // asked for and the client acts on the sequence change.
    struct SoundReq { std::string name; uint32_t seq = 0; };
    const SoundReq& soundRequest() const { return soundReq_; }
    void requestSound(std::string n) { soundReq_.name = std::move(n); ++soundReq_.seq; }
    void requestShake(float mag, float dur) {
        shakeReq_.mag = mag; shakeReq_.dur = dur; ++shakeReq_.seq;
    }
    void clearHits() { hits_.clear(); }

private:
    void tickCombat(Unit& u, float dt);
    void fire(Unit& u, Unit& target, int slot);
    // Convert a unit to another player (contact charm + mind-control weapons).
    void captureUnit(Unit& t, int newPlayer);
    // Apply a weapon's damage at (hx,hz): the direct hit on `primary` plus, if
    // the weapon has areaofeffect, splash on other enemies of `fromPlayer` scaled
    // from full at the centre to `edge` at the rim. Per-category damage per victim.
    void applyHit(const Weapon& w, float hx, float hz, int fromPlayer, int fromId, Unit* primary);

    void tickProduction(Unit& u, float dt);
    void tickTransport(Unit& u, float dt);
    void tickConstruction(Unit& u, float dt);
    // Orphaned conjure (no builder worked it this tick): bleed HP at its build
    // rate, then vanish with no corpse.
    void decayConstruction(Unit& u, float dt);
    // A builder chips reclaim work off its target feature, drips mana, then removes
    // the feature and advances its reclaim queue.
    void tickReclaim(Unit& b, float dt);
    void tickAbilities(float dt);   // reclaim / resurrect on nearby corpses
    void tickAuras(float dt);       // AdjustArmor/Attack stat auras
    void tickHealAuras();           // AdjustJoy passive repair aura (1 Hz)
    void updateVisibility();

    // Return a flow field toward world (gx,gz) over `type`'s nav grid, built and
    // cached on first use (keyed by goal cell + domain). Cleared when the nav
    // grid changes (a building is placed or removed). nullptr if unbuildable.
    // const: the flow field is a PURE MEMO of (nav grid, goal, domain) -- content
    // never depends on when/whether it was built -- so building a cache entry the
    // AI needs (on the server, where clients don't) cannot perturb sim state. The
    // cache is therefore mutable and this is a logical-const query. Single-threaded
    // per world (not safe to call off the sim thread). See docs/multiplayer-design.md.
    struct CompGrid { uint64_t ver = 0; int w = 0, h = 0; std::vector<int32_t> label; };
    mutable std::map<std::pair<const NavGrid*, int>, CompGrid> compCache_;
    const CompGrid* components(const NavGrid& g, int foot) const;
    // A SEPARATE cache for non-sim queries (pathExists, which only the server-side
    // AI calls). Keeping it apart is not an optimisation, it is a correctness
    // requirement: the AI runs on ONE peer, so letting its questions insert into and
    // evict from the sim's memo made cache membership differ between peers. That was
    // invisible while the sim cache never reached its cap and desynced the referee
    // inside 60 ticks once it did.

    // Uniform spatial hash over mobile units, rebuilt each tick, so the
    // separation and combat-acquisition passes are O(n) instead of O(n^2).
    void rebuildGrid();
    // (rebuildOccupancy declared with occ_ below)
    // Call fn(int unitIndex) for every mobile unit whose cell lies within
    // `radius` of (x,z). Iterates cells in a fixed order, so it is deterministic.
    template <class F>
    void forEachNear(float x, float z, float radius, F&& fn) const {
        if (gW_ <= 0) return;
        int r = int(radius / gCell_) + 1;
        int cx = int((x - gOx_) / gCell_), cz = int((z - gOz_) / gCell_);
        for (int dz = -r; dz <= r; ++dz) {
            int gz = cz + dz;
            if (gz < 0 || gz >= gH_) continue;
            for (int dx = -r; dx <= r; ++dx) {
                int gx = cx + dx;
                if (gx < 0 || gx >= gW_) continue;
                for (int i = gHead_[size_t(gz) * gW_ + gx]; i >= 0; i = gNext_[size_t(i)])
                    fn(i);
            }
        }
    }
    // As forEachNear, but stops after `maxVisits` candidates. Deterministic (the grid
    // iteration order is fixed), so a lockstep-safe density cap: in an overcrowded cell
    // a unit only needs to interact with a bounded number of the nearest others (e.g.
    // the separation push is dominated by the closest neighbours). maxVisits <= 0 = all.
    template <class F>
    void forEachNearCapped(float x, float z, float radius, int maxVisits, F&& fn) const {
        if (gW_ <= 0) return;
        if (maxVisits <= 0) { forEachNear(x, z, radius, std::forward<F>(fn)); return; }
        int r = int(radius / gCell_) + 1;
        int cx = int((x - gOx_) / gCell_), cz = int((z - gOz_) / gCell_);
        int seen = 0;
        for (int dz = -r; dz <= r; ++dz) {
            int gz = cz + dz;
            if (gz < 0 || gz >= gH_) continue;
            for (int dx = -r; dx <= r; ++dx) {
                int gx = cx + dx;
                if (gx < 0 || gx >= gW_) continue;
                for (int i = gHead_[size_t(gz) * gW_ + gx]; i >= 0; i = gNext_[size_t(i)]) {
                    fn(i);
                    if (++seen >= maxVisits) return;
                }
            }
        }
    }
    // Unit solidity. Retail stamps every ground unit into an exclusive per-cell
    // occupancy layer and REFUSES a move whose destination footprint overlaps
    // another unit's cell (KINGDOMS.icd 0x507d10) -- units are hard-solid, and
    // body-blocking a bridge is a real tactic. Ours passed through each other
    // entirely; the only unit-vs-unit force was the separation relaxation, which
    // resolves overlap after the fact and cannot stop anyone.
    //
    // ONE DELIBERATE SIMPLIFICATION: we record only STATIONARY units. Retail's
    // hard test blocks on any occupant, but its steering layer (0x509020) rates a
    // parked unit impassable and a MOVING one merely expensive, so crowds flow
    // through each other's wake; it affords that with continuous short-hop
    // replanning, randomised repath delays and an age-weighted per-player path
    // budget that we do not have. Blocking only on parked units keeps what a
    // player notices -- a wall of bodies stops you -- while making a head-on
    // corridor lock between two marching units impossible by construction.
    // Rebuilt wholesale once a tick in unit-index order, so it is deterministic;
    // it is derived state and is not itself hashed.
    // Per-movement-class nav. navClasses_ holds one grid per distinct limit tuple;
    // navIdx_ maps a type to its grid. Derived from terrain + the registry, so it is
    // not hashed -- but it MUST be identical on every peer, which it is: the table
    // is built in a deterministic order from the same data.
    std::vector<uint8_t> obst_;   // shared obstacle overlay (see NavGrid::setObstacles)
    std::vector<NavGrid> navClasses_;
    std::unordered_map<const UnitType*, int> navIdx_;

    std::vector<int32_t> occ_;      // 16px cells -> occupying unit id (0 = free)
    int occW_ = 0, occH_ = 0;
    void requestPath(Unit& u, float x, float z);
    void rebuildOccupancy();
    // How deep would `u`'s body sit inside another mobile body if it stood at
    // (nx,nz)? Pixels of overlap along the shallower axis; <= 0 means clear.
    // cellFree() below answers at 16px cell resolution and so only changes its
    // mind when a unit CROSSES a boundary -- between crossings a body creeps
    // into its neighbour unopposed. This is the same question asked in pixel
    // space, which is the resolution the mover actually steps at.
    float bodyPenetration(const Unit& u, float nx, float nz) const;
    // Is (nx,nz) free of a parked body other than `selfId`? True when solidity is
    // off (no grid) or the cell is outside it.
    // Is the footprint rect at (nx,nz) free of a parked body other than `selfId`?
    // `foot` is the unit's footprint in cells; at foot==1 this is the single-cell
    // test it replaced, byte for byte.
    bool cellFree(float nx, float nz, int selfId, int foot = 1) const {
        if (occW_ <= 0) return true;
        int cx = int(nx) / 16, cz = int(nz) / 16;
        // Centre-anchored, matching NavGrid::fits and blockFootprint, so a unit's
        // nav footprint and its occupancy footprint are the same rect.
        cx -= foot / 2; cz -= foot / 2;
        for (int j = 0; j < foot; ++j)
            for (int i = 0; i < foot; ++i) {
                int x = cx + i, z = cz + j;
                if (x < 0 || z < 0 || x >= occW_ || z >= occH_) continue;
                int32_t o = occ_[size_t(z) * size_t(occW_) + size_t(x)];
                if (o != 0 && o != selfId) return false;
            }
        return true;
    }

    std::vector<int> gHead_, gNext_;
    int gW_ = 0, gH_ = 0;
    float gCell_ = 32.0f, gOx_ = 0, gOz_ = 0;

    std::atomic<uint32_t> featGen_{0};   // see featGeneration()
    void bumpFeatGen() { featGen_.fetch_add(1, std::memory_order_release); }

    std::vector<uint8_t> vis_;
    int visPlayer_ = 0;
    bool fogExplored_ = true;   // keep seen cells dimmed (vs. reverting to dark); display-only
    int visW_ = 0, visH_ = 0;
    // Client-local fog acceleration: the LoS-tested cell set for a (sight, radar,
    // cell) combination is a pure function of the IMMUTABLE heightmap, so it is
    // computed once and re-stamped while a unit stands on that cell -- the O(r^3)
    // ray-march that used to recompute every 0.25s made 300-600-unit armies cost
    // 11-66ms per fog pass. Display-only: never hashed, and the headless referee
    // (visPlayer_ < 0) skips the fog pass entirely.
    std::unordered_map<uint64_t, std::vector<uint32_t>> visMaskCache_;
    // The fog pass runs ASYNCHRONOUSLY on a worker. At 3800 units it costs ~86ms, and on
    // the 0.5s cadence below that was a stall you could feel twice a second -- the whole
    // of it, measured: "SIMPHASE tick=88.6ms ... vis=86.0". Parallelising inside the pass
    // (which it already does) cannot help, because the tick still waits for it.
    // Splitting it instead: visGather() is serial and cheap and reads units_ on the sim
    // thread; visCompute() is the expensive part and touches only the immutable heightmap
    // plus its own back buffer, so it runs off-thread while the game keeps rendering. The
    // result lands one pass late -- imperceptible against fog that already only moves at
    // 2-4Hz, and free of lockstep risk because NOTHING here is hashed (the referee at
    // visPlayer_ < 0 skips the pass, and every consumer of vis_ is client display code).
    struct Reveal { int cx, cz, r, rRadar2; bool los; uint64_t key; };
    struct Miss   { uint64_t key; int cx, cz, r, rRadar2; };
    std::vector<Reveal> visReveals_;
    std::vector<Miss>   visMisses_;
    std::vector<uint8_t> visBack_;      // worker's target; swapped into vis_ when it lands
    std::thread visWorker_;
    bool visRunning_ = false;           // sim thread only: is visWorker_ joinable?
    bool visHavePass_ = false;          // has a fog pass ever actually LANDED? (not: is
                                        // vis_ allocated -- setTerrain pre-fills it)
    std::atomic<bool> visDone_{false};  // worker -> sim thread: result is ready
    void visGather();                   // serial: reads units_, fills visReveals_/visMisses_
    void visCompute();                  // worker: ray-march, demote, stamp into visBack_
    void visPump();                     // sim thread: collect a finished pass, start the next
    // Bumped after every fog recompute so the renderer can skip re-uploading an
    // unchanged fog texture (vis_ changes at 4Hz; frames render far faster).
    uint32_t visGen_ = 0;
    float visTimer_ = 0;
    std::vector<uint8_t> heights_;   // raw TNT heightmap, for fog line-of-sight
public:
    // Terrain accessors for offline analysis tools (tools/footprobe): the raw
    // heightmap and its dimensions. Read-only; nothing in the sim uses these.
    const std::vector<uint8_t>& mapHeights() const { return heights_; }
    int mapW() const { return terW_; }
    int mapH() const { return terH_; }
    int mapSea() const { return seaLevel_; }
private:
    int hW_ = 0, hH_ = 0;
    // True if the ground at cell (tx,tz) is visible from a unit at cell (ux,uz):
    // no intervening terrain rises above the eye->target sight line. Local display
    // only (fog is not in stateHash), so plain float math is fine.
    bool sightClear(int ux, int uz, float eyeH, int tx, int tz) const;
    std::vector<Unit> units_;
    std::unique_ptr<MissionScript> mission_;   // optional campaign mission runner
    std::unique_ptr<ScenarioScript> scenario_; // optional .crt trigger runner
    std::vector<uint8_t> forcedDefeat_;        // scenario Victory/Defeat: forced-defeated slots
    std::vector<int> justDied_;                // unit ids that died this tick (mission/scenario hook)
    std::vector<std::pair<float, float>> manaSpots_;
    std::vector<Feature> features_;             // reclaimable map features
    std::vector<FeatType> featTypes_;           // per-type burn data (setup-time, static)
    std::unordered_map<const UnitType*, int> corpseType_;   // unit -> corpse FeatType
    std::unordered_map<const UnitType*, int> stoneType_, frozenType_;   // statue defs
    // Burn RNG: retail rolls spread on its game LCG (icd 0x535cc0, Lehmer 16807);
    // ours is identical on every peer -- draws happen only in deterministic sim
    // paths, and the state is folded into stateHash.
    uint32_t burnRng_ = 0x54414B21;
    // Target-scatter RNG for fireatwillrandom. Same Lehmer generator as burnRng_
    // and equally part of the lockstep contract: every peer runs the identical
    // acquisition in the identical order, so it advances in lockstep too.
    uint32_t fireRng_ = 0x4649524Eu;   // 'FIRN'
    // Builders whose queued build order has come due this tick. startBuild spawns
    // the site, which can REALLOCATE units_, so it must never be called while the
    // per-unit loop holds a Unit& -- that reference dangles the moment it returns.
    // Collected in unit order (deterministic) and drained after the loop.
    std::vector<int> buildDue_;
    uint32_t fireRand(uint32_t n) {    // retail's rand(n): 0 when n < 2
        fireRng_ = uint32_t((uint64_t(fireRng_) * 16807ULL) % 0x7FFFFFFFULL);
        return n < 2 ? 0u : fireRng_ % n;
    }
    int burnRand(int n) {
        burnRng_ = uint32_t((uint64_t(burnRng_) * 16807ULL) % 0x7FFFFFFFULL);
        return n > 0 ? int(burnRng_ % uint32_t(n)) : 0;
    }
    void igniteFeature(Feature& f);
    void swapFeature(Feature& f, int newType);   // chain stage swap (burnt/dead)
    void tickBurning();
    std::unordered_map<int, size_t> featureIdx_;   // feature id -> index in features_
    std::vector<Projectile> projectiles_;
    std::vector<HitFx> hits_;
    ShakeReq shakeReq_;
    float waterDamage_ = 0;   // .ota waterdamage when waterdoesdamage=1
    SoundReq soundReq_;
    // [EXPLODEAS] blasts queued during the death sweep and applied just after it
    // (applyHit mutates units_, which the sweep is walking). Transient within one
    // tick -- always empty at tick end, so it needs no hashing.
    struct DeathBlast { const Weapon* w; float x, z; int player, fromId; };
    std::vector<DeathBlast> deathBlasts_;
    // Remote Effect (FBI type=Remote Effect): the effect materialises at the AIMED
    // GROUND POINT after `builduptime`, then applies its damage/status/conversion
    // once over areaofeffect. Earthquakes, monarch waves, god spells, Area Mind
    // Control. Lives across ticks, so it IS hashed.
    struct PendingEffect {
        const Weapon* w = nullptr;
        float x = 0, z = 0;
        int player = 0, fromId = 0;
        float at = 0;        // seconds until the next pulse
        float endAt = 0;     // seconds until the effect expires
        float period = 0;    // seconds between pulses (<=0 = a single pulse)
        bool casterGated = false;   // dies with its caster (mind control / freeze)
    };
    std::vector<PendingEffect> pendingEffects_;
    // Wandering (FBI type=wandering): a storm entity that roams for `duration`,
    // drifting at weaponvelocity and wobbling its heading every `variationtime`.
    // Damages everything it passes on a fixed cadence. Hashed.
    std::vector<Storm> storms_;
    int stormSeq_ = 0;        // id source, so the viewer can track a storm's life
    std::vector<Player> players_ = []{
        std::vector<Player> v(4);
        for (int i = 0; i < 4; ++i) v[size_t(i)].team = i;
        return v;
    }();
    int winningTeam_ = -1;
    bool monarchExpendable_ = true;      // default: Monarch is just a unit (net option overrides)
    bool serialThreads_ = false;
    std::vector<uint8_t> hadMonarch_;   // per-player: ever fielded a Monarch (for the loss rule)
    bool godsEnabled_ = false;
    int unitCap_ = 0;                 // per-player live-unit limit (0 = unlimited)
    float godAppearTime_ = 1e9f, clock_ = 0;
    uint32_t tickCounter_ = 0;   // ticks elapsed; staggers per-unit auto-acquisition
    std::vector<BenchStage> benchPlan_;   // benchmark staged spawns (executed in tick)
    size_t benchCursor_ = 0;              // next unexecuted stage
    uint32_t benchEndTick_ = 0;           // 0 = not a benchmark run
    uint32_t acqStride_ = 4;     // auto-acquire re-scan period, widened with crowd size
                                 // (deterministic: derived from the live-unit count)
    PathService paths_;          // retail's request queue + budget scheduler
    // Enabled by setupMatch; a bare test World leaves it off.
    bool pathService_ = false;
    // How often a travelling unit re-asks for a route. Retail's figure, not a
    // guess: 0x4e545b re-requests only once the tick counter has passed the
    // stamp at navigator+0x110 by 0x78 -- 120 ticks -- and only when the path
    // did NOT fail (it tests the failed/detour bits first and does nothing at
    // all if either is set). We were re-asking every 30, four times retail's
    // rate, which is what kept units churning through fresh routes instead of
    // settling on one.
    static constexpr uint32_t kPathRetryTicks = 120;
    // A search that failed once from roughly here will fail again -- the terrain
    // has not changed. Sit out this many ticks before asking again, so a unit
    // stuck against a maze stops burning the whole budget on doomed searches and
    // leaves it for units that can actually be helped. Deterministic: keyed by
    // unit id off the tick counter.
    static constexpr uint32_t kPathFailBackoff = 150;   // 5s
    // How long a WEDGED unit keeps shoving at a goal it is not reaching before
    // it settles. Retail stops promptly -- ordered at an unreachable mountain it
    // walks as close as it can and comes to rest, with no long grind first --
    // so this is short. It is paired with a wedged test (stuckFor), which is
    // what keeps a unit that is merely crawling from being cut off.
    static constexpr float kGoalGiveUpSecs = 20.0f;
    std::map<int, uint32_t> pathRetryAt_;
    NavGrid nav_, navWater_, navHover_;
    // Per-cell terrain metrics (16px cells) for per-unit passability limits.
    std::vector<uint8_t> slope_;   // local height spread
    std::vector<uint8_t> depth_;   // water depth (sea level - height), 0 on land
    std::vector<uint8_t> roads_;   // 1 = road cell (feature-plane 0xFFFB); may be empty
    int terW_ = 0, terH_ = 0;
    int seaLevel_ = 0;   // heights below this are water (kept for tools/analysis)
    // A cell passable for `t`, honouring its maxSlope / maxWaterDepth on top of
    // the shared domain nav grid.
    bool passable(const UnitType* t, int cx, int cz) const {
        if (!navFor(t).walkable(cx, cz)) return false;
        if (!t || slope_.empty()) return true;
        if (cx < 0 || cz < 0 || cx >= terW_ || cz >= terH_) return false;
        size_t i = size_t(cz) * terW_ + cx;
        if (t->domain == UnitType::Domain::Ground) {
            if (slope_[i] > t->maxSlope) return false;                 // too steep
            if (t->maxWaterDepth > 0 && depth_[i] > t->maxWaterDepth)  // too deep
                return false;
        } else if (t->domain == UnitType::Domain::Water) {
            if (t->minWaterDepth > 0 && depth_[i] < t->minWaterDepth)  // too shallow
                return false;
        }
        return true;
    }
    int nextId_ = 1;
};

} // namespace tak::sim
