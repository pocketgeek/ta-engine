#pragma once
#include "sim/retailreclaimarea.h"
#include "sim/retailreclaim.h"
#include "sim/heapoptional.h"

#include "sim/fixed.h"
#include "sim/convoy.h"
#include "cob/emissionpose.h"
#include "sim/footprint.h"
#include "sim/retailrng.h"
#include "sim/retailmotion.h"
#include "sim/retailpiecepose.h"
#include "sim/retailprojectile.h"
#include "sim/retailairgrid.h"
#include "sim/retailflame.h"
#include "sim/retailhweffect.h"
#include "sim/retaileffectclock.h"
#include "sim/retailheight.h"
#include "sim/retailfeaturesink.h"
#include "sim/retailexploration.h"
#include "sim/retailweaponswitch.h"
#include "sim/retailflight.h"
#include "sim/retailairpad.h"
#include "sim/retailairseek.h"
#include "sim/retailairtoair.h"
#include "sim/retailballistic.h"
#include "sim/retailwind.h"
#include "sim/taeconomy.h"
#include "sim/retailweapon.h"
#include "sim/retailmission.h"
#include "sim/retailpark.h"
#include "sim/retailgroup.h"
#include "sim/retailplayer.h"
#include "sim/retailconstruction.h"
#include "sim/retaildeath.h"
#include "sim/retailveterancy.h"
#include "sim/retailconstructionparticles.h"
#include "sim/retailai.h"
#include "sim/retailgrade.h"
#include "sim/retailmap.h"
#include "cob/retailstate.h"
#include "sim/simprobe.h"
#include "sim/simpool.h"
#include "sim/powercodes.h"

#include <array>
#include <bitset>
#include <cstddef>
#include <span>
#include <algorithm>
#include <atomic>
#include <thread>
#include "sim/copymember.h"
#include "sim/pathsearch.h"
#include "sim/pathmode.h"
#include "sim/legion.h"
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <functional>
#include <map>
#include <tuple>
#include <memory>
#include <set>
#include <source_location>
#include <string>
#include <vector>

namespace ta { struct RetailLightningDefinition; }
namespace ta::hpi { class Vfs; }

namespace ta::sim {

// Cheat: when true, construction and production complete instantly and cost no
// mana (set via the taclient `--cheat` flag).
extern bool gInstantBuild;

// Unit stats loaded from .fbi files. Velocities are in map pixels per
// second (FBI values are per original 30Hz tick), headings in radians
// with 0 facing +z and the model's forward axis.

// Visual family for a weapon's projectile (drives how the viewer draws it).
enum class WeaponFx { Arrow, Lightning, Fire };

struct UnitType;   // for Weapon::damageVs

struct Weapon {
    std::string name;
    int32_t range = 0;       // px (readInt)
    float reload = 1;        // seconds
    uint16_t switchReloadTicks = 30; // native switchreloadtime, defaulting to reloadtime
    int32_t damage = 0;      // DAMAGE.default (readInt; the dump prints it %i) (base, used when no category matches)
    float projVel = 0;       // px/s; 0 = instant (melee)
    bool melee = false;
    bool cursorAirstrike = false; // FBI `dropped=` -> WeaponType+0xc8 bit 0x20
    int32_t aoe = 0;         // areaofeffect radius px (readInt); >0 = splash
    float edge = 1;          // edgeeffectiveness: damage fraction at the aoe edge
    // RAW COB ANGLE UNITS, which is both what retail stores (AimTolerance is
    // readInt) and what the comparison needs: the mover's `diff` is a bamDiff, so
    // holding this in radians meant comparing an integer 0..32768 against ~0.1 and
    // firing only when the heading matched EXACTLY.
    int32_t aimTol = 1024;   // aimtolerance: how close to on-target to fire
    bool ballistic = false;  // FBI weapon type = Ballistic (lobbed arc, not flat)
    // Line-of-Sight is a trajectory family; its subtypes have distinct updates.
    bool beam = false;
    bool straight = false; // Ordinary LOS class: any subtype the classes below do not claim.
    bool lightning = false;
    // The Line-of-Sight status classes (subtype mindcontrol, turntostone,
    // turntofrozen: LineOfSightMindControl / TurnToStone / TurnToFrozen,
    // vtables 5f3a90 / 5f3a5c / 5f3a28). They launch and draw as the ordinary
    // straight shot (52c540 / 52c8c0) but fly with their own update (52d8d0 /
    // 52db40 / 52dda0), and the unit a shot strikes goes to the class's impact
    // callback (52da10 / 52dca0 / 52df00) instead of the damage. Retail's
    // parser (530935..530b16) gives every other subtype the plain class.
    enum class LosStatus : uint8_t { None, MindControl, Stone, Frozen };
    LosStatus losStatus = LosStatus::None;
    std::string hwEffectName;
    std::shared_ptr<const RetailLightningDefinition> lightningEffect;
    std::array<uint16_t,3> shotSpin{}; // roll, heading, pitch per collision substep.
    int8_t flameKind = -1; // -1: other class; 0 fire, 1 bluefire, 2 dieselflame.
    bool groundBounce = false,waterWeapon = false;
    int32_t emitTime = 0;    // emittime (readInt): ticks the flame/beam is emitted
    // The rest of the retail weapon-class model (FBI `type=`), beyond
    // melee/ballistic/line-of-sight:
    //   Guided       -- a homing projectile that steers at `turnRate` (20 weapons:
    //                   Tracking Arrow, Ball Lightning, dragon fireballs).
    //   Remote Effect-- the effect materialises AT THE TARGET POINT after
    //                   `buildUp`, then fades over `decay` (23: Earthquakes,
    //                   monarch waves, god spells, Area Mind Control).
    //   Wandering    -- a roaming storm entity that drifts for `duration`
    //                   (4: Tornado, Fire/Water Vortex, Hurricane).
    //   Dropped    -- a bomb RELEASED from a flyer: retail starts at its scripted
    //                 QueryWeapon muzzle with zero vertical speed, then divides the
    //                 aim-point X/Z delta by the gravity-derived fall ticks.
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
    // Retail's BallisticWeapon class: a plain Ballistic shell (not Dropped, a
    // flame or a beam). Its inRange (52bc60) solves the launch arc first.
    bool ballisticClass() const {
        return ballistic && kind == Kind::Normal && !melee && !beam && flameKind < 0;
    }
    // Does a shot from this weapon need the engine's 2D clear-line test? That
    // test is a deviation (retail has none: its weapon update admits a shot on
    // range alone, World::slotClearShot), kept for a ranged weapon whose shot
    // flies the line. A plain Ballistic shell is exempt whatever its root: its
    // 3D arc and the projectile's own collision (52a4d0) decide what it clears,
    // as in retail, and World::noteShotImpact catches a shell that keeps
    // landing short. Decided per weapon -- the firing slot's, or the approach
    // slot's for the unit as a whole -- never by "any weapon of the unit".
    bool needsClearLine() const { return !melee && range > 64 && !ballisticClass(); }
    // Is this weapon's reach to a structure measured centre to centre, with no
    // allowance for the structure's size? Retail's unit-target range test is a
    // virtual of the weapon class (vtable +0xc), and 16 of its 17 classes
    // answer centre to centre (530580, inclusive): WeaponType, every Line-of-
    // Sight subclass (plain, lightning, fire, mind control, stone, freeze),
    // Guided, every Remote Effect subclass (plain, hailstorm, earthquake, both
    // area spells), Wandering, DroppedBallistic (532290, a bare call of
    // 530580) and Ballistic (52bc60: its arc, then 530580). Only MeleeWeapon
    // (52b980) does not, testing footprint boxes instead (meleeInRange).
    // tools/re/check_inrange_classes.py runs all 17 natively against a 7x12
    // building. With our old pad a Crusades Death Totem (range 550) took a
    // lodestone 584 px off and its bolts died ~540 px out, and a Dropped
    // bomb, a Remote or Wandering spell or a freeze, stone or mind-control
    // beam was cast at a building up to 8*foot+24 px past its range, where
    // retail's never is.
    bool centreReach() const { return !melee; }
    // The engine's range allowance for a structure target of `foot` cells
    // (its larger side): the half extent plus a margin for where navigation
    // halts at its edge. It stands in for melee's box contact when an attack
    // approaches; every other class gets none (centreReach).
    int32_t structurePad(int32_t foot) const { return centreReach() ? 0 : 8*foot+24; }
    int32_t particlesPerSec = 0; // particlespersecond (readInt): hailstorm pulse rate
    float turnRate = 0;      // guided: steering rate, radians/sec (FBI deg/s)
    // Retail splits a projectile's per-tick travel into sub-steps so no sub-step is
    // longer than one 16px cell, which is how its swept collision stays honest
    // (KINGDOMS.icd 0x531ccb: subSteps = ceil(unitsPerTick/16), threshold 480 px/s).
    // It matters here because a guided shot that SNAPS onto its target rebuilds its
    // velocity from the per-sub-step speed and then has that vector applied once per
    // sub-step -- so a correcting homer really does fly subSteps x nominal.
    int subSteps = 1;
    float buildUp = 0;       // builduptime: channel before the effect lands
    int32_t buildUpTicks = 0; // native truncation of seconds * 30
    float decay = 0;         // decaytime: fade after it lands
    int32_t decayTicks = 0;  // native truncation of seconds * 30 (52e780)
    float duration = 0;      // wandering: seconds the storm roams
    int32_t durationTicks = 0, variationTicks = 0;
    int32_t maxVariation = 0;// maxvariation (readInt): wander half-width, px/tick
    float variationTime = 0; // wandering: seconds between heading changes
    bool  unitsOnly = false; // unitsonly: the effect skips features (trees/props)
    // subtype=mindcontrol: converts targets to the firer's side instead of damaging
    // them, on a veterancy-scaled probability roll. The [DAMAGE] table is the
    // ACQUISITION filter -- retail zeroes the categories you may not TARGET
    // (monarch/god/dragon/fort/factory/naval/lodestone) -- while what actually
    // protects a unit caught inside an AREA charm is the commander / cantbecaptured
    // / in-transport gate at the impact itself.
    bool  mindControl = false;
    int32_t minRange = 0;    // minrange (readInt): can't hit targets closer than this
    bool noAir = false;
    // WeaponType+0xc8 flags the fire-at-will sweep (icd 40f330/412620) reads:
    // toairweapon 0x4000 (only flyers are candidates) and commandfire 0x20000
    // (never swept for a human owner; fires only on an explicit order).
    bool toAir = false;
    bool commandFire = false;
    bool hoverAttack = false;
    int32_t hoverAttackDistance = 0, hoverAttackAltitude = 0;
    // airtoairdistance (WeaponType +0xc6, a word, 0 when absent): AirToAir's
    // reach for this weapon when it is the selected one; 0 takes the unit's.
    uint16_t airToAirDistance = 0;
    // dontleadtargets: aim at the target's CURRENT position instead of extrapolating
    // where it will be. Retail leads a moving unit by default; this flag skips it.
    // Shipped dropped bombs use this to aim at the current point; their
    // weaponvelocity is a fall parameter rather than a flight speed.
    bool noLead = false;
    // BallisticWeapon's own two fields (KINGDOMS.icd 0x52bb70 parses exactly these).
    // gravityadjustment is a plain multiplier on world gravity for this weapon's
    // 3D arc; lobpreferred picks the HIGH root of the ballistic quadratic instead
    // of the low one, which is what lets a mortar drop its shell behind a wall.
    float gravityAdj = 1.0f;
    bool lobPreferred = false;      // noairweapon: cannot target flying units
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
    // an optional ground shadow. Without these every shot in
    // the game is the same hand-drawn streak.
    std::string weaponArt;    // weaponart: anims/<name>_4444.taf sprite sequence
    std::string shotModel;    // model: objects3d/<name>.3do projectile mesh
    std::string veteranShotModel; // veteranmodel: alternate projectile mesh at veteranlevel
    int32_t veteranLevel = 0; // veteranlevel: source rank that selects veteranShotModel
    bool usesVeteranShotModel(int sourceVeteran) const {
        return !veteranShotModel.empty() && sourceVeteran >= veteranLevel;
    }
    bool  nimbus = false;     // nimbus: faction caster effect and conditional launch delay
    // nimbustime (WeaponType+0x7c, seconds * 30 truncated): the launch delay a
    // nimbus caster with nimbus art adds to the shot's start tick. No shipped
    // weapon defines it, so retail's delay is always 0 -- builduptime is a
    // Remote Effect timer only (pushed once, by 52e780).
    int32_t nimbusTicks = 0;
    bool  hasBoltColor = false;
    uint8_t inner[3] = {255, 255, 255};    // innercolor: lightning bolt core
    uint8_t middle[3] = {200, 230, 255};   // middlecolor
    uint8_t outer[3] = {120, 170, 255};    // outercolor
    int32_t spinRate = 0;     // spinheading (readInt): shot spins as it flies
    std::string shadowArt;    // shadowart: sequence in shadowgaf (always "shadows")
    // shotart: a Remote Effect that is DELIVERED rather than conjured in place --
    // a visible shot flies to the aim point at weaponvelocity and only starts the
    // spell's channel when it lands. Exactly one shipped weapon (the Acolyte's
    // Earthquake) uses it, which is also the only reason its weaponvelocity is
    // read at all; for the other 22 Remote Effects it is dead data.
    std::string shotArt;
    int lightMap = 0;         // lightmap: ground light pool, 1 small 2 medium 3 large
    std::string explosionClass;       // explosionclass: impact effect (gamedata/explosions)
    std::string waterExplosionClass;  // waterexplosionclass: impact effect over water
    // Area-effect shockwave rings emitted at this weapon's impact.
    std::string radiusArt[3];   // radiusart0..2: expanding-ring effect anims
    int   ringCount = 0;        // ringcount: how many rings
    float ringDelay = 0.2f;     // ringdelay: seconds between successive rings
    float ringDur = 1.0f;       // ringduration: seconds each ring lasts
    int   spriteCount = 24;     // spritecount: sprites arranged around each ring
    int32_t shakeMag = 0;       // shakemagnitude (readInt): camera-shake intensity
    float shakeDur = 0;         // shakeduration: seconds the shake lasts
    int32_t shakeTicks = 0;     // the same, ticks truncated (WeaponType+0x84): Earthquake pulse period
    bool  fireStarter = false;  // firestarter: leaves ground fire at the impact
    // Precomputed integer damage (default * multiplier, truncated at load),
    // keyed by lowercased category token (e.g. "monarch", "dragon", "fort").
    std::map<std::string, int32_t> dmgVs;
    // The same overrides with the category token INTERNED to an int, in the string
    // map's order. damageVs() is the hottest lookup in the
    // sim -- measured 29k-245k of them a tick -- and walking a string-keyed red-black
    // tree per candidate is a poor way to answer it. Built once, at load, and never
    // mutated afterwards: the server ticks several rooms in parallel over ONE shared
    // TypeRegistry, so a lazily-filled cache here would be a data race, and a race in
    // hashed state is a desync rather than merely a bug.
    std::vector<std::pair<int, int32_t>> dmgVsIds;
    WeaponFx fx = WeaponFx::Arrow;
    // ---- Total Annihilation weapon record (weapons/*.tdf; TotalA.exe 0x42e440,
    // docs/retail-data.md section 3). Values are stored the way retail stores them.
    int taId = -1;                  // ID= slot 0..255 (0 = NOWEAPON)
    std::string section;            // [SECTION] name: what FBI weapon1.. refers to
    uint32_t taFlags = 0;           // TaWeaponFlag bits (+0x111)
    float energyPerShot = 0, metalPerShot = 0;
    int32_t velocityFx = 0, startVelocityFx = 0, accelerationFx = 0;   // fx16 per tick / tick^2
    uint16_t reloadTicks = 0, weaponTimerTicks = 0, burst = 0, burstRateTicks = 0;
    uint16_t sprayAngle = 0, durationTicks16 = 0, flightTimeTicks = 0, holdTimeTicks = 0;
    uint16_t smokeDelayTicks = 0, randomDecayTicks = 0, turnRatePerTick = 0;
    uint16_t accuracy = 0, tolerance = 0, pitchTolerance = 0;
    int32_t coverage = 0;
    float minBarrelAngle = -0.19634954f;   // radians; retail default -11.25 degrees
    uint8_t renderType = 0, color = 0, color2 = 0;
    std::string explosionGaf, explosionArt, waterExplosionGaf, waterExplosionArt,
                lavaExplosionGaf, lavaExplosionArt;
    std::string soundStart, soundWater;
    // Aim handler retail installs (0x49e010): which of turret / vlaunch /
    // lineofsight-or-selfprop / dropped claimed it. None = cannot fire.
    enum class TaAim : uint8_t { None, Turret, VLaunch, Direct, Dropped } taAim = TaAim::None;
    bool hasTaFlag(uint32_t bit) const { return (taFlags >> bit) & 1u; }
    // Damage this weapon deals to a unit of type `t` (category override, else base).
    float damageVs(const UnitType* t) const;
};

struct UnitType {
    std::string id;        // lowercase objectname, e.g. "araarch"
    uint16_t orientation=0,buildAngle=0; // original spawn-heading range in BAM
    std::string name;      // display name, e.g. "Archer"
    std::string side;      // ARA/TAR/VER/ZON/CRE
    bool hasNimbusArt = false;
    Fixed maxVel = Fixed::fromInt(1);                 // px/tick  (= 30 px/s)
    Fixed animationMoveRate1=Fixed::fromInt(2),animationMoveRate2=Fixed::fromInt(2);
    Fixed accel = Fixed::fromFloat(15.0f / 900.0f);   // px/tick^2 (= 15 px/s^2)
    Fixed brake = Fixed::fromFloat(15.0f / 900.0f);   // px/tick^2 (= 15 px/s^2)
    // BAM PER TICK -- the raw FBI `turnrate`, which retail stores as an int and which
    // is already in COB angle units, i.e. the same 65536-to-a-circle unit as Bam. We
    // used to scale it to rad/s at load and call bamFromRadians(turnRate * dt) in the
    // mover, which is the identity round trip: tr * kCobAngle * kTick * (1/30) back
    // through bamFromRadians lands on tr again.
    int32_t turnRate = 1092;   // ~6 rad/s
    // FBI turninplacerate: the pivot rate used when the unit is STOPPED and merely
    // turning to face something, which retail keeps separate from the turn rate it
    // uses while moving (KINGDOMS.icd 0x4d91b0 picks between the two on an in-place
    // flag). An omitted key has rate zero, including when a facing target is set.
    int32_t turnInPlaceRate = 0;   // unsigned 16-bit BAM/tick in retail
    int32_t experiencePoints = 666; // native Type+1c2, awarded to player score on a kill
    int32_t maxHp = 100;        // maxdamage: readInt at +0x1be in retail
    bool canMove = false;
    bool canGuard = false, canPatrol = false; // authored command capability flags
    uint8_t buildMovementCode=0; // Authored bmcode byte (retail default 0), used by construction visuals.
    bool isBuilder = false;
    bool builderLimited = false; // native assist is restricted to the build menu only when set
    // The type's canbuild/<id>/ menu (TypeRegistry::loadBuildTree), so the sim
    // can apply HelpBuild's builderlimited test (0x519e60) without a registry.
    std::vector<std::string> buildMenu;
    // Can this type train units (and therefore hold a rally)? Set for any builder
    // structure; used to decide whether a move/attack/patrol order on a BUILDING
    // means "set the rally" rather than "do nothing".
    bool producesUnits() const { return isBuilder && isStructure(); }
    std::shared_ptr<const cob::File> productionScript;
    std::shared_ptr<const cob::File> simulationScript;
    const cob::File* script() const { return simulationScript ? simulationScript.get() : productionScript.get(); }
    std::vector<RetailModelPiece> productionModel; // shared script-piece hierarchy, also used for combat queries
    // Each script piece's authored vertices, X/Z mirrored and deduplicated: the
    // geometry 4dd2a0 bounds for SweetSpot once the draw path has posed it.
    std::vector<std::vector<std::array<int32_t,3>>> scriptPieceVertices;
    bool commander = false;   // FBI commander=1: the faction's Monarch (loss condition)
    // Buildings vs mobile units: the reliable test is maxVel. The FBI `canmove`
    // flag is set on some buildings too (e.g. the Keep, or the Taros Hell), so a
    // canMove building would otherwise be mistaken for a mobile builder.
    bool isStructure() const { return maxVel <= Fixed(); }
    int32_t buildDist = 0;  // FBI builddistance (readInt): how far a builder reaches to build
    bool onMana = false;    // must be built on a mana deposit (yardmap 'S'), e.g. lodestones
    bool sacredIncome = false; // retail yardmap S: income requires sacred-site coverage
    bool gate = false; // FBI gate low bit: closed passage cells use search grade 3
    float buildCost = 0;    // legacy single-resource cost (= buildCostMetal for TA data)
    // Total Annihilation economy (FBI; TotalA.exe 0x42bf40, docs/retail-data.md 2.2).
    // buildcost* are read as INTEGERS then stored as float (12.9 -> 12).
    float buildCostMetal = 0, buildCostEnergy = 0;
    float energyMake = 0, energyUse = 0, metalMake = 0, extractsMetal = 0;
    uint8_t makesMetal = 0;          // read as an integer byte
    float windGenerator = 0, tidalGenerator = 0;
    float energyStorage = 0, metalStorage = 0;
    std::string objectName;          // 3DO name (defaults to unitname)
    std::string description;
    int16_t sonar = 0, radarJam = 0, sonarJam = 0;
    bool stealth = false, canDgun = false, showPlayerName = false, downloadable = false;
    bool isAirBase = false, antiWeapons = false, targetingUpgrade = false;
    float buildTime = 0;    // work units; seconds = buildTime / builder workerTime
    float workerTime = 1;
    float income = 0;       // mana/sec (mogriumincome)
    float storage = 0;      // mana cap contribution (mogriumstorage)
    // mogriumbounty (UnitDef +0x222): on a credited kill (512c6b) the killer's
    // player gains it in its pool and lifetime total. No shipped type sets it.
    float bounty = 0;
    int footX = 1, footZ = 1;
    std::string yardMap;      // footX*footZ chars; 'o' blocks, '.'/'c' passable
    // Lowercased general category tokens (FBI category/damagecategory/tedclass).
    // Weapon overrides use the separate single damageCategory below.
    std::vector<std::string> categories;
    // Native combat and cursor lookup use only FBI damagecategory (UnitDef+0x9e).
    std::string damageCategory;
    int damageCategoryId = -1; // only DamageCategory participates in weapon overrides
    std::vector<int> catIds;    // broader category list, interned in the shared token table
    int32_t sight = 180;      // px (FBI sightdistance, readInt)
    bool canFly = false;
    bool receivesWind = false; // wind OR windgenerator: retail WindChange callback gate
    // bankscale / pitchscale also scale surface support-plane angles. Surface
    // pitch participates in the mover's ramp speed limit.
    // 16.16 in retail: bankscale/pitchscale go through the fixed-point reader.
    Fixed bankScale = Fixed(), pitchScale = Fixed();
    // defaultmissiontype = Standby_wander: cows, deer, wolves, boar, peasants and
    // villagers drift around instead of standing still. 16 types, and the missions
    // place hundreds of them.
    bool  wanders = false;
    int32_t cruiseAlt = 0;    // world units above ground when flying (readInt)
    bool hoverAttack = false;
    int32_t hoverAttackDistance = 0, hoverAttackAltitude = 0;
    // airtoairdistance (UnitType +0x25a, a word, 4c0548): how far from its
    // target's led SweetSpot a flyer fights another flyer (AirToAir). Absent:
    // sqrt(hoverattackdistance^2 + hoverattackaltitude^2) with a
    // hoverattackdistance (212 for a Tarbeak), else 100.
    uint16_t airToAirDistance = 100;
    // attackrunlength (UnitType +0x234, a word, 4c082e): how far past its
    // target a flyer without hoverattack carries an attack run (AirToGround).
    uint16_t attackRunLength = 0;
    enum class Domain { Ground, Water, Hover };
    Domain domain = Domain::Ground;   // from the final water limits / canhover (sim.cpp loadDir)
    bool canTransport = false;
    int transportCap = 0;     // passenger count (FBI transportcapacity)
    int transportSizeCap = 0; // sum of passenger costs (transportsizecapacity)
    int maxTransportSize = 0; // largest individual passenger (transportsize)
    bool transportLandEligible = true; // native MinWaterDepth < 0, without changing nav defaults
    // FBI transportdistance: the px radius inside which this transport absorbs a
    // unit that is boarding it. Raw world units == our px (retail stores it as a
    // plain 16-bit with no scaling, the same family as sightdistance/builddistance),
    // so a barge reaches ~419px where we used one hardcoded 70 for every transport.
    int32_t transportDist = 0;   // readInt
    // An air carrier's attach piece: the script piece its QueryTransport names
    // (cob::queryTransportPiece; TotalA.exe VTOL_PICKUP 0x41138a). -1 for none.
    int transportPiece = -1;
    std::string soundClass;   // FBI soundcategory, keys gamedata/soundclasses
    std::string bodyType = "default";   // FBI bodytype (flesh/armor/wood/..) = hit-sound material
    bool instantCorpse = false; // proven constant Killed + no Dying callback
    bool explosionCorpse = false; // Killed explicitly requests a corpse for death type 3
    std::string corpse;       // FBI corpse feature name
    std::string stoneFeat;    // FBI stone= statue feature (death while petrified)
    std::string frozenFeat;   // FBI frozen= statue feature (death while frozen)
    int corpseAdjX = 0, corpseAdjZ = 0;   // corpseadjustx/z: wreck offset in cells
    bool canAnimate = false;              // cananimate: raises animateType from corpses
    const UnitType* animateType = nullptr;   // animatetype=<unit>, resolved post-load
    std::string animName_;                   // raw animatetype value (loadDir fixup)
    std::string shadowArt;    // FBI shadowart: shadow sprite name in shadows.gaf
    bool upright=false;
    std::optional<RetailGroundSupport> groundSupport;
    std::optional<RetailCollisionQuad> projectileQuad;
    bool groundModelLoaded=false;
    int32_t modelTop=0; // native fixed-point model top, type +0x14a
    uint8_t sightHeight=0; // low byte of the bind-pose model top's integer height
    bool noShadow = false;    // FBI noshadow: casts no ground shadow (walls, spectres)
    bool floater = false;     // FBI floater: rides the water surface. Retail skips the
                              // shadow for these too, independently of noshadow, which
                              // is why five ships carry a shadowart they never show.
    bool canHover = false;    // FBI canhover: wades rather than being blocked by water
    bool ghost = false;       // FBI ghost: a spectral body, drawn translucent. Three
                              // units carry it (Ghost of Garacaius, Ghost Ship, Risen
                              // Wolf) and all three also set noshadow.
    int  waterline = 0;       // FBI waterline: height units the model sits BELOW the
                              // water surface; upright hoverers clamp to terrain,
                              // floaters use the waterline without a terrain clamp.
    std::string veteranModel; // veteranmodel: 3DO the unit swaps to at max veterancy
    // --- extended FBI stats -------------------------------------------------
    float healTime = 0;       // healtime: worker time the unit repairs ITSELF with (0 = none)
    int32_t leash = 0;        // maneuverleashlength (readInt): max auto-chase distance (0 = unlimited)
    // Per-type standing orders, derived exactly as retail's [UNITINFO] parser does
    // (icd 0x4c005d). standingunitorder is a COMPOSITE front-end: 2 -> move 1 /
    // fire 2, 1 -> move 0 / fire 2, 0 -> move 0 / fire 0. When the key is absent
    // the parser's default is the sentinel 3, which falls through to
    // standingmoveorder (default 2) and standingfireorder (default 2) -- Roam and
    // Fire At Will -- and stores the composite as 2 (Offensive), so it is never 3.
    // 147 of the 203 shipped units set standingunitorder; only the animals set
    // standingmoveorder (2), so the 56 that do not are the only ones that roam.
    uint8_t defaultStandingOrder = 2; // native composite (+0x130 bits 20-21), COB GET 46
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
    Fixed waterMult = Fixed::fromInt(1);      // watermultiplier: speed factor in shallow water
    // Ticks to cross HALF A CELL (8px) at this type's best speed -- retail's per-type
    // movement time scale, UnitType+0x249, computed at FBI load (icd 0x4bfc5e-0x4bfd34):
    //   clamp( int( 8.0 / (max(watermultiplier, roadmultiplier, 1.0) * maxvelocity) ),
    //          1, 255 ),  and 255 outright when maxvelocity <= 0.
    // Everything retail times about movement scales by it: the randomised repath
    // deadlines are dice * this (0x4e5226/0x4e5284/0x4e535d), and the goal-crowding
    // tolerance is 50 / this cells (0x414563). A standard walker (1.1 px/tick, road
    // x1.2) lands on 6, making the tolerance 8 cells = 128px.
    int32_t halfCellTicks = 6;
    Fixed roadMult = Fixed::raw(0x13333);     // roadmultiplier: on-road speed factor. Retail's FBI
                              // parser defaults it to 16.16 0x13333 (~1.2) -- icd
                              // 0x4bfc5e -- so EVERY ground unit gains on roads.
    int32_t maxWaterDepth = 0;   // deepest water a ground unit may wade into (readInt)
    int32_t maxWaterSlope = 255; // MaxWaterSlope (readInt): the slope limit below the waterline
    int32_t maxSlope = 255;   // steepest cell height-spread the unit may cross (readInt)
    int32_t minWaterDepth = 0;   // shallowest water a water unit needs (readInt)
    int32_t badSlope=-1,badWaterSlope=-1;
    int32_t badMaxWaterDepth=32768,badMinWaterDepth=32768;
    // Retail type words read by the landing-site predicate 509400: the movement
    // class's values, else the FBI keys with retail's defaults (4dfb10/4dfc40).
    int16_t landMaxWaterDepth=10000,landMinWaterDepth=-10000;
    uint8_t landMaxSlope=255;
    int32_t radar = 0;        // radardistance (readInt): fog-reveal radius (separate from sight)
    bool  noVeteran = false;  // noveteran: this unit can never gain veterancy
    bool  canReclaim = false; // canreclaim: builder can reclaim corpses/features for mana
    bool  canResurrect = false;   // canresurrect: can revive nearby corpses
    // cancapture (flagsB +0x245 bit 12): TotalA.exe's hit response reads it
    // (0x406fb4) for a computer's unit under fire.
    bool  canCapture = false;
    bool  canCloak = false;       // cancloak
    bool  initCloaked = false;    // init_cloaked: a new unit's cloak order starts on
    float cloakCost = 0;          // cloakcost: energy per second while cloaked and idle (taSettle)
    float cloakCostMove = 0;      // cloakcostmoving: the same while moving (default cloakcost/30)
    int32_t minCloakDist = 0;     // mincloakdistance (readInt): retail only draws it round a cloaked unit
    // FBI fireatwillrandom (icd: UnitDef+0x264 bit 24, parsed at 0x4c0a3a; its ONLY
    // reader is the auto-target scorer at 0x4129bc). Retail scores each candidate
    // n = dist^2 / damageVsTarget and keeps the lowest of rand(n)/2 + rand(n); this
    // flag swaps the numerator for INT_MAX, which dwarfs any real squared distance,
    // so the pick stops preferring what is NEAREST and becomes a random draw
    // weighted by damage. Set on the 16 missile troops (every faction's archers,
    // the Crossbowman, Musketeer and Cannoneer): a rank of them sprays a crowd
    // instead of every last one focusing the same closest body.
    bool fireAtWillRandom = false;
    // kamikaze (UnitDef+0x260 bit 28): the sweep's selector skips the in-range
    // test for these (icd 412789).
    bool kamikaze = false;
    // kamikazedistance (UnitDef+0x236, 0x4c0876). Load-time data nothing in the
    // simulation reads: only the client's Shift range circle (0x4d6530) draws it.
    int32_t kamikazeDist = 0;
    // shootme (UnitDef+0x260 bit 15): the sweep's selector lets a HUMAN-owned
    // shooter auto-pick only types that set it (icd 412620). FBIs that omit it
    // read 0 (aranull, cregate, npcflag); synthetic types default to shootable.
    bool shootMe = true;
    // notargetcategory (UnitDef+0x9a) and w{pri,sec,spe}_badtargetcategory
    // (+0x8e/+0x92/+0x96), lowercased tokens matched against a target's
    // `categories`. The sweep's selector never picks a noTarget candidate and
    // takes a badTarget one only when nothing else qualifies (icd 412620).
    std::vector<std::string> noTargetCats;
    std::vector<std::string> noChaseCats;   // nochasecategory, TotalA.exe type +0x23d
    std::array<std::vector<std::string>,3> badTargetCats;
    // weaponswitching: this unit carries ONE active weapon at a time, picked by the
    // player. Without it, a multi-weapon unit fires every weapon independently.
    bool  weaponSwitching = false;
    bool  onOffable = false;      // onoffable: can be toggled active/inactive
    // activatewhenbuilt (the parser's default is retail's, off; a type built
    // in code keeps this on)
    bool  activateWhenBuilt = true;
    // damagemodifier (UnitDef +0x17e, 16.16 from the float key, default 1.0;
    // 4bfd6f): 51a16e scales a hit on an ACTIVATED unit (+0x114 bit 1) by it,
    // heals and hits of 30000 or more excepted. No shipped type sets it.
    int32_t damageModifier = 0x10000;
    bool  cantBeStoned = false, cantBeFrozen = false;
    bool  cantBeCaptured = false, cantBeTransported = false;
    // immunetoparalyzer (UnitDef+0x260 bit 26): paralyzer weapons (damage
    // type 4) are useless against it; read by AI weapon switching.
    bool  immuneToParalyzer = false;
    int   transportSize = 1;      // transportedsize, default footprint area
    // selfdestructcountdown: seconds between arming a self-destruct and the unit
    // leaving. Three bits in retail (icd 0x4c09e8 masks & 7 into UnitType+0x264
    // bits 21..23), so 0..7. Defaults to 2, which is what the parser leaves when
    // a type omits the key (0x4c0a1f) -- and no shipped type declares it.
    int   selfDestructCountdown = 2;
    uint8_t blood[3] = {150, 30, 10};   // bloodcolor1 (r,g,b) for hit/death spray
    std::array<uint32_t,3> bloodColors={0xff961e0a,0xff961e0a,0xff961e0a}; // display ARGB
    Weapon weapon;            // primary (WEAPON1); damage 0 = unarmed
    std::vector<Weapon> weapons;   // all slots (WEAPON1..3)
    int32_t maxWeaponReloadMs=0; // SetMaxReloadTime: max of all native weapon-slot reload ticks
    bool hasPrimaryWeaponBlock=false; // native UnitDef+264 bit 0x20 is set by WEAPON1 presence
    std::array<bool,3> weaponAirstrikeCursor{}; // FBI `dropped=` bit on native WeaponType slots
    std::array<uint8_t,3> weaponNativeSlotForLocal{}; // compressed local damage vector -> FBI slot
    // The FBI slot (0..2: weapon1..3) a local weapon slot came from; a
    // synthetic type without the map keeps its local slots.
    int nativeWeaponSlot(int local) const {
        if (local<0 || local>=3) return local;
        return std::max(local,int(weaponNativeSlotForLocal[size_t(local)]));
    }
    // [EXPLODEAS]: a weapon fired at the unit's OWN position the moment it dies
    // (Kamikaze Rat's 320-radius blast, Grenadier/Fire Demon/Balloon death pops).
    Weapon explodeAs;
    bool hasExplodeAs = false;
    // Self-destruction selects its own authored block, with no normal-death
    // fallback when [SELFDESTRUCTAS] is absent (native UnitDef+1ba).
    Weapon selfDestructAs;
    bool hasSelfDestructAs = false;
    // totalallowed: per-player cap on LIVE units of this type (dragons/gods/
    // juggernaut carry 1). 0 = unlimited.
    int totalAllowed = 0;
    // Raw FBI words only the Classic AI reads (src/ai/classictypes.cpp derives
    // retail's own type flags from them, which differ in detail from the fields
    // above: retail's default bmcode is 0, its yardmap is read only for bmcode 0,
    // and "armed" is any [WEAPONn] block, damage or not). Load-time data that
    // nothing in the simulation reads.
    struct ClassicFbi {
        std::string unitName;    // `unitname=` lowercased (else the file stem): retail's table key
        std::string category;    // the `category=` value alone
        std::string yardMap;     // the `yardmap=` value as written
        int32_t bmcode = 0;      // `bmcode=`, retail's default 0
        bool isFeature = false;  // `isfeature=` low bit
        uint8_t weaponBlocks = 0;// bit n-1: a [WEAPONn] block is present
        std::string aiWeight;    // TA `ai_weight=` profile line (applied by the retail AI)
    } classicFbi;
    float maxRange() const {
        float r = 0;
        for (const auto& w : weapons) r = std::max(r, float(w.range));
        return r;
    }
};

// A pathfinding movement class from gamedata/MOVEINFO.tdf: per-class terrain
// limits that a unit inherits via its FBI `movementclass`.
struct MoveClass {
    bool transportLandEligible = true;
    // FootprintX/Z: the real source of a MOBILE unit's size. 125 of our 152 movers
    // carry no footprintx in their own FBI at all -- it comes from here, and not one
    // of them is 1x1 (2x2 through 5x5). Retail copies these into the unit def at
    // KINGDOMS.icd 0x4c0e54 and falls back to the FBI only for a unit with no class.
    int footX = 0, footZ = 0;
    int32_t maxSlope = 255;
    int32_t maxWaterSlope = 255;   // the limit used when the cell's low corner is wet
    int32_t maxWaterDepth = 255;
    int32_t minWaterDepth = 0;
    int32_t badSlope=-1,badWaterSlope=-1;
    int32_t badMaxWaterDepth=32768,badMinWaterDepth=32768;
    // Raw class words as retail stores them in the type (+0x192/+0x194/+0x23c),
    // with the class constructor's defaults (4dfb10).
    int16_t retailMaxWaterDepth=10000,retailMinWaterDepth=-10000;
    uint8_t retailMaxSlope=255;
};

class TypeRegistry {
public:
    // Parse gamedata/MOVEINFO.tdf (movement classes). Call BEFORE loadDir so
    // units can inherit their class's slope / water limits.
    void loadMoveInfo(const hpi::Vfs& vfs, const std::string& path);
    // Parse every .fbi under `prefix` in the VFS (e.g. "units"). All source
    // archives' units merge into one namespace, precedence already resolved.
    void loadDir(const hpi::Vfs& vfs, const std::string& prefix);
    // Parse <prefix>/<builder>/<buildable>.tdf into the build tree, as retail
    // 0x4c1950 does: only for loaded builder types, only entries naming a loaded
    // type, ascending [Menu] Priority with stable ties. Call AFTER loadDir.
    void loadBuildTree(const hpi::Vfs& vfs, const std::string& prefix);
    const UnitType* find(const std::string& id) const;
    // Full type table in deterministic (name-sorted) order -- corpse interning
    // walks it so every peer builds identical FeatType indices.
    const std::map<std::string, UnitType>& types() const { return types_; }
    // FBIs that lost their objectname to another file (the Iron Plague's
    // tarnecr2.fbi, objectname TARNECRO): retail keeps each as its own unit-type
    // entry under its unitname, so the Classic AI's catalogue lists them too.
    // Keyed by lowercased unitname. Nothing in the simulation reads them.
    const std::map<std::string, UnitType>& classicVariants() const { return classicVariants_; }
    // Category token -> small int. Assigned while walking types_ in its name-sorted
    // order, so every peer interns the same tokens to the same ids.
    int categoryId(const std::string& tok) const {
        auto it = catIds_.find(tok);
        return it == catIds_.end() ? -1 : it->second;
    }
    const std::vector<std::string>& buildable(const std::string& builderId) const;
    // TA build-menu placement: GUI page (1-based, after the orders page) and slot 0..5.
    struct BuildSlot { std::string unit; int page = 1; int slot = 0; };
    const std::vector<BuildSlot>& buildPages(const std::string& builderId) const;
    // Mobile, armed combat units of a faction `side` ("ARA".."CRE"), in a fixed
    // (name-sorted) order so every peer builds the same stress-test army. Excludes
    // structures, builders, the Monarch, and anything with no weapon.
    std::vector<const UnitType*> combatUnits(const std::string& side) const;
    // Intern every category token and resolve the per-weapon overrides against it.
    // Called once after loading; idempotent.
    void internCategories();
    // Largest build menu of any builder (drives the minimum window width so the
    // whole icon row always fits at full size -- some menus exceed 10).
    std::size_t maxBuildMenu() const {
        std::size_t m = 0;
        for (const auto& [id, list] : buildTree_) m = std::max(m, list.size());
        return m;
    }

private:
    bool effectsLoaded_=false;
    std::map<std::string,std::shared_ptr<const RetailLightningDefinition>> lightningEffects_;
    std::map<std::string, int> catIds_;
    std::map<std::string, UnitType> types_;
    std::set<std::string> canonicalTypes_;   // ids whose defining .fbi filename == objectname
    std::map<std::string, UnitType> classicVariants_;
    std::map<std::string, std::vector<std::string>> buildTree_;
    std::map<std::string, std::vector<BuildSlot>> buildPages_;
    std::map<std::string, MoveClass> moveClasses_;   // lowercased name -> limits
    std::shared_ptr<class TaWeaponTable> taWeapons_;  // weapons/*.tdf by ID and section name
public:
    const class TaWeaponTable* taWeapons() const { return taWeapons_.get(); }
private:
};


// A campaign mission's order (SetMission / InitialMission, sim/mission.cpp)
// whose state does not fit in an Order: the World keeps it in a table and
// the Order holds its 1-based index (Order::scripted). Retail runs these as
// missions with their own parameters (icd 0x513fb0 builds them):
//  - AttackType  `a TYPE`, AttackUType (401580): attack the nearest of a
//    type, again and again, until none is left. `type`.
//  - Train       `b TYPE N` on a factory, BuildingBuild (401c20): build
//    `count` of `type`, then go on. `count` counts down as they finish.
//  - WaitAttack  `wa [NAME]`, WaitForAttack (4017a0): hold until `watched`
//    is hit (event 0x10) or gone (0x08). `heard` is its attackedTick when
//    the order was given: retail's listener collects hits from then on.
//  - WaitNear    `w F N`, Wait (401480) with a radius: hold `count` ticks,
//    looking every rand(7)+7 ticks for a visible enemy within `radius` px.
//  - SelfDestruct `d`, SelfDestructFG (4017e0, +0x4e 1): 30000 damage of
//    type 5 to itself when the order comes up.
// An index stays valid while an order refers to it (addScriptedOrder
// compacts a full table and renumbers the orders that refer to it).
// VTOL_PATROL's registry flags (row 0x5ebd50) as its ctor keeps them with a
// point and no target (4d6c40): a flyer's patrol leg. Its stage 0 (4d6b80)
// adds 0x4000 to the leg that has appended (or found) the loop's anchor.
// tools/re/check_air_point_mission.py.
inline constexpr uint32_t kVtolPatrolFlags = 0x1000412u;
// VTOL_Move's (row 0x5ebcd3) with a point and no target: a flyer's move,
// fight-move or rally step, and its return move after an attack (4de530
// action 2 gives a flyer VTOL_Move: not Move_Ground's 0x3000400, so no
// 0x2000000 navigator formation bit). tools/re/check_air_point_mission.py.
inline constexpr uint32_t kVtolMoveFlags = 0x1000402u;

// What another unit's reference to a unit becomes when that unit changes
// owner (World::retireReferences): the id of no unit. Retail's change of
// owner kills the old object, whose death lets go of every mission and
// controller that named it (519a60), and makes the new owner a new one; World
// keeps the id for the new object, so the references to the old one are
// pointed here instead. World::unit answers nothing for it, so each holder
// loses its target as it loses one that has died and gone.
inline constexpr int kGoneUnit = -1;

struct ScriptedOrder {
    enum class Kind : uint8_t { AttackType, Train, WaitAttack, WaitNear, SelfDestruct, MakeSelectable };
    Kind kind = Kind::AttackType;
    const UnitType* type = nullptr;
    int32_t count = 0;
    int32_t watched = 0;
    uint32_t heard = 0;
    int32_t radius = 0;
};

// Field placement is part of the design (WE D2), checked by the static_asserts
// after the struct: a new field must say which cache line it belongs on.
//  - Line 0 (bytes 0-63) holds everything the per-tick order predicates read:
//    groundMissionOrder's `plain`, currentLeg, hasQueuedWork/hasQueuedBuild,
//    tickCombat's front-order tests. A queue scan then touches
//    one line per Order. Add a field here only if such a predicate reads it.
//  - Line 1 (64-127): the per-order mission state the movers read once they have
//    the order (controller, the presence of park/buildRectangle/flightGoal,
//    convoyTick, missionTarget).
//  - Lines 2+ : everything else (transport, display, flight stations).
// Large, rarely engaged payloads are HeapOptional (one pointer here), so the
// record is 5 lines instead of 7 (432 B, unaligned).
struct alignas(64) Order {
    // ---- line 0: the predicate line --------------------------------------
    // x, z, targetId stay FIRST: Order is built with aggregate initialisers like
    // {x, z, targetId}. A positional initialiser naming more than these three
    // would silently pick up whatever follows, so none does (and none may).
    Fixed x, z;   // goal, in the same fixed-point domain as Unit::x/z
    int targetId = 0;      // nonzero = attack (or board, if load) target
    bool load = false;     // board the friendly transport `targetId`
    bool unload = false;   // sail to (x,z) and disembark cargo
    bool attackMove = false;   // engage enemies encountered en route
    bool patrol = false;       // loop: completed orders re-queue at the back
    bool guard = false;        // follow friendly `targetId`, engage threats
    bool waitAttack = false;   // hold until an enemy is in sight, then release (SetMission "wa")
    // Last waypoint of the order the PLAYER actually gave. One order can expand
    // into a whole route, so the queue interleaves pathfinding waypoints with
    // real goals; this marks where each issued order ends. Display-only in the
    // sense that it changes no movement maths -- but the repath paths need it to
    // tell "the rest of this leg" from "everything queued behind it".
    bool goal = false;
    // Owned by the issued ground-move goal, never its intermediate waypoints.
    // Other mission kinds retain their existing handlers until ported.
    bool groundMission = false;
    int32_t wait = 0;          // >0: hold position, counting down in TICKS (SetMission "w N")
    // This order is a BUILD: put `buildType` down here. Builds used to live in a
    // separate list, which meant they could never interleave with moves -- "go
    // here, build that, go there" was inexpressible -- and the order line could
    // not draw them. They are ordinary queue entries now, as they are in retail.
    // The order sits at the builder's working position; the site is footZ*8+24
    // north of it.
    const UnitType* buildType = nullptr;
    HeapOptional<RetailReclaimArea> reclaimArea;
    // Layout pad: Order's cache-line plan (TA_ORDER_LINE below) was laid out with an
    // 8-byte slot here (once Kingdoms' area-build order); keep the plan intact.
    // A D-gun order (TotalA.exe AttackSpecial 0x403190): fires only the
    // commandfire slot and ends after its shot (World::dgun).
    bool dgun = false;
    uint8_t layoutPad0_[7] = {};
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
    bool transportUnloadApproach = false; // native surface unload circle/poll mission
    bool navigationConsumed = false; // only the final stored point remains
    // This target was picked by auto-acquisition, not asked for by the player.
    // The move standing order gates only AUTO engagement: a Defensive unit told
    // explicitly to attack something across the map still walks over and does it.
    bool autoTarget = false;
    bool flightMoveMission = false;
    bool landing = false; // controller owned by the VTOL landing mission
    bool transportPickup = false; // carrier-owned air/surface pickup mission
    // Inactive navigator: either consumed or disabled with stored points intact.
    bool navigationExhausted = false;
    bool hasSegment = false;

    // ---- line 1: mission state ---------------------------------------------
    // Tick this order was issued, for the order-line beads: retail phases the
    // trail by (now - orderCreationTick) so each segment's dots crawl toward the
    // destination independently (icd 0x4d5747 reading order+0x5e).
    uint32_t issuedTick = 0;
    // Legion only (sim/convoy.h): the tick this order's convoy opened, one
    // value for every order one click gave a selection, even across the
    // client's 64-per-tick uplink split. issuedTick stays the identity of the
    // order's own 64-unit part. A patrol's return leg copies its outbound
    // leg's value. kNone outside Legion and on orders no click made (formation
    // anchors, rallies, waypoints spliced in by a route). Retail never sets
    // it; hashed with a tag when set.
    // The value is the order's ARRIVAL AREA key
    // (ConvoyTable::Convoy::key): an explicit selection order (Cmd::Order)
    // joins the table once for all its members, and an area it opens is keyed
    // kExplicitArea | its serial, unique per area; per-unit commands (the AI,
    // scripts) still open areas keyed by their first tick.
    uint32_t convoyTick = ConvoyTable::kNone;
    uint64_t controller = 0;
    HeapOptional<RetailParkState> park;
    // Restored MobileBuild approach; production after arrival still uses the
    // World's construction host. Bounds use retail footprint-origin cells.
    // A RECLAIM leg (reclaimFeat) carries the same controller: 406250 stage 0
    // installs it through the same 4d4e80 (prepareReclaimApproach), around the
    // target feature's footprint instead of the site's.
    HeapOptional<RetailRectGoal> buildRectangle;
    HeapOptional<RetailFlightGoal> flightGoal;
    // Mission destination can change while the navigator retains its old route.
    std::optional<std::pair<Fixed,Fixed>> missionTarget;
    bool patrolRepair = false; // automatic patrol detour; clickX/Z retain its local anchor
    // A Shift-queued ground patrol point whose loop has no return point yet.
    // Retail's patrol mission owns that point: Initialize (40388d) appends
    // one where the unit stands when the patrol STARTS, unless another
    // patrol leg in the queue already belongs to a loop. So "move to A,
    // Shift-patrol B" laps A<->B, and Shift-patrols B, C lap start, B, C.
    // Hashed when set; ordinary patrol() loops never set it.
    bool patrolOpen = false;
    // A RepairUnit a guarding builder put ahead of its guard (tickGuardHelp).
    // Like retail's RepairUnit, it follows the target: while out of reach
    // its approach is re-aimed at where the target is now (tickRepair).
    // Hashed when set.
    bool guardRepair = false;
    bool transportUnloadReleasePending = false; // unload's one-tick empty mission tail
    bool transportUnloadTransferDeferred = false; // approach arrival wakes unload next tick
    // Retail Move_Ground_Formation (402880) pushed ahead of a Move_Ground:
    // its mission+0x56 level. Negative = move to the moving members' centre
    // until within that level; positive = stop and wait for the group.
    int8_t formationLevel = 0;
    // Legion flight stations (W6 FL-04, a flyer's last order): the release
    // from its station once the ground stops advancing (1, release A) or its
    // centroid has stood within 16 px of stationRef since stationRefTick for
    // 900 ticks (2, release B), or its order's point moved to its landing
    // lane in front of the arriving ground (3); sticky for the order.
    // stationRefTick 0: no reference yet. Hashed with a tag when set.
    uint8_t stationFree = 0;
    // A ground RepairUnit's give-up count (0x407190, mission +0x52): 1 for
    // each look at a moving target, 3 when it is in reach or starts moving
    // while worked on; past 10 the repair ends. Never reset while the job
    // lasts. Cold, but in line 1's one-byte pad before missionRadius (the
    // cold tail's pads hold the mission, revive and seek fields); hashed on
    // repair legs.
    uint8_t repairLooks = 0;
    uint32_t missionRadius = 0;

    // ---- lines 2+: the rest ------------------------------------------------
    RetailMissionState mission;
    RetailGroundResponse groundResponse;
    // Navigator segment start retained when preceding route nodes are consumed.
    // This belongs to this route leg, never to the next queued player command.
    Fixed segmentX, segmentZ;
    // Where the player actually CLICKED, when that differs from x/z. order()
    // snaps a destination the unit cannot stand on to the nearest cell it fits
    // in, so x/z is where the unit will END UP -- but the on-map marker belongs
    // under the cursor the player aimed at, not on the spot the engine chose.
    // Display only: never folded into stateHash, never steered on.
    Fixed clickX = Fixed(), clickZ = Fixed();
    // Automatic Legion production exit. A blocked generated parking point
    // may retire once the child has cleared this original birthplace.
    std::optional<std::pair<Fixed,Fixed>> productionExit;
    uint32_t transportProductionAhead = 0; // finite outputs preceding a queued boarding order
    int transportTicks = 0; // native load/unload effect waits fifteen ticks
    int transportPassenger = 0;
    RetailMissionState transportMission{2};
    uint32_t transportApproachAttempts = 0;
    Fixed transportX, transportY, transportZ;
    // A build leg's site centre. A ground HelpBuild leg (helpSite) keeps its
    // site's centre for the failed approach's reach test; a ground RepairUnit
    // leg keeps where its target stood when the approach was installed
    // (mission +0x22), for the look that re-aims it once that cell changes.
    Fixed buildX, buildZ;
    // A repair-pad mission a flyer put ahead of its own (TotalA.exe pushes
    // "VTOL_LANDING" with the pad as its target, 0x43acb0, from VTOL_Patrol
    // 0x410f3e and its kin under 3/4 health): kAirPadLanding is VTOL_Landing
    // (0x4118e0, retailVtolLanding), kAirPadRepair SelfRepair (0x402430,
    // retailPadRepair), which the landing pushes once the flyer sits on the
    // pad (0x411ecf, "SELFREPAIR"). Both run on `mission` (World::tickAirPad);
    // the pad is airPadId (mission +0x16), the landing's circle angle and then
    // its pad piece (both mission +0x36) guardOrbitAngle and airRunCount, its
    // controller flightGoal (followed when transportTicks is set). 0 on every
    // other order. In the byte the Kingdoms step-out kept. Hashed with a tag
    // when set.
    static constexpr uint8_t kAirPadLanding = 1, kAirPadRepair = 2;
    uint8_t airPad = 0;
    // VTOL_SeekGuard (TotalA.exe 0x410850, tickSeekGuard): what a flyer's guard
    // becomes when its guarded unit is lost and the guard was its last order
    // (VTOL_Follow's 0x410341, seekLostWard). It circles x/z, the guarded unit's
    // position at the guard's last look (mission +0x22, kept in attackPointX/Z
    // on the guard), on `mission`, its orbit angle in guardOrbitAngle (+0x36), and
    // guards the first allied ground unit it finds. targetId stays 0. In the
    // pad after airPad. Hashed with a tag.
    bool seekGuard = false;
    // AirStrike's stage-4 controller (4e3f70, tickAirStrike) follows the
    // attack's target: flightGoal's point is the target's, every tick. In the
    // same pad. Hashed when set.
    bool strikeFollow = false;
    // The attack mission's point (mission +0x22) on a flyer's attack order:
    // the target's position or the selected slot's led SweetSpot as
    // AirToGroundHover last read it (stages 0-3), as AirToGround last read it
    // (41c13c, each wake) or AirToAir's led SweetSpot (51aa50, each stage-2
    // wake). A lost target's VTOL_SEEKATTACK circles it
    // (retailAirLostTarget). Zero until first read.
    Fixed attackPointX, attackPointZ;
    // The attack mission's +0x4e (retailAirToGround, retailAirToGroundHover,
    // which run on `mission`): AirToGround's is 1 once a run has brought the
    // flyer within half its range of the target, and its stage 7 counts
    // wakes off the map on it; AirToGroundHover's counts its step-outs. A
    // step-out order's D. On a ground walker's automatic melee attack
    // (Attack_Melee 404690, which a flyer never runs) it is the tick of the
    // next stage-2 timer wake instead (stage 1's 4 + rand(5) + rand(5)
    // sleep), where the walker may take another target (4049a0); 0 until
    // the approach starts (retailMeleeRetarget). Hashed with a tag then.
    int32_t airRunCount = 0;
    // A ground attack (Attack_Chase, 403bb0; tickCombat): whether the unit
    // has walked toward its target under this order (`attackChased`, beside
    // `chaseLeashed` below so the cold tail stays five lines), and the tick
    // its weapons first aim at it (kAttackUnaimed until it first stands in
    // reach). Hashed with a tag when set.
    static constexpr uint32_t kAttackUnaimed = UINT32_MAX;
    uint32_t attackAimTick = kAttackUnaimed;
    // AirToAir (retailAirToAir, tickAirToAir), which runs on `mission` too:
    // its mission +0x4e, the moving controller and a pushed VTOL_Evade.
    // Engaged once a flyer attacks a flyer; hashed with a tag.
    HeapOptional<RetailAirToAirState> airToAir;
    int32_t stationRefX = 0, stationRefZ = 0;
    uint32_t stationRefTick = 0;
    // MobileBuild placement at the site (405824..405911): failures while only
    // mobile units stand on it, and the tick of the next 10-tick retry. Zero
    // until the first such failure. Hashed with a tag when set.
    // Legion only: the serial of the explicit selection order (Cmd::Order)
    // whose member got this leg, 0 for every other order (and
    // always in Retail). Stamped and copied with convoyTick; the pair is the
    // leg's OrderIdentity. Hashed with a tag when set.
    uint32_t orderSerial = 0;   // cold: read once per Legion registration and by the hash
    OrderIdentity identity() const { return {convoyTick, orderSerial}; }
    void setIdentity(const OrderIdentity& id) { convoyTick = id.convoyTick; orderSerial = id.serial; }
    uint32_t placementRetryTick = 0;
    uint8_t placementFailures = 0;
    // RECLAIMAREA's own approach (406670 stage 1): a radius-4 circle on the
    // builder's position clamped into the area, pushed ahead of the area order.
    // It ends on arrival OR failure (wait mask 0x700), never widening like a
    // Move_Ground. Hashed with a tag when set.
    bool reclaimAreaApproach = false;
    // An auto attack issued with a return (4d7de0's leash path: the idle
    // retaliation mission, Maneuver or a patrol hit): mission +0x56 is the
    // leash (maneuverleashlength) and +0x2e/+0x30 the origin; the attack
    // retires once the unit itself is `chaseLeash` from the origin (0 =
    // unlimited), and the return move queued behind it walks it back. Hashed
    // with a tag when set.
    bool chaseLeashed = false;
    bool attackChased = false; // Attack_Chase: walked under this order (above)
    uint16_t chaseLeash = 0;
    RetailGroundPoint chaseOrigin{};
    // A campaign mission's order with state of its own: the 1-based index of
    // its World::scriptedOrders_ entry (ScriptedOrder), 0 for every other
    // order. In the pad before reclaimCountdown, so the cold tail stays five
    // lines. Hashed with its entry when set.
    uint16_t scripted = 0;
    // RECLAIM's work countdown (406250 mission +0x4e): trunc(energy + 15) at
    // arrival, less 2 every two-tick wake (retailReclaimWork). Its stages run
    // on `mission`. Hashed on reclaim legs.
    int32_t reclaimCountdown = 0;
    // The facing the stage-1 turn child (FacingNoFormation) turns the builder
    // to before the stance wait: retail heading from the target to the builder,
    // as MobileBuild's turn. Hashed on reclaim legs.
    uint16_t reclaimHeading = 0;
    // AirToGround's and AirToAir's stage 1 made the unselected slots
    // automatic (Unit::slotAuto) for as long as this order's mission runs
    // (in the pad beside reclaimHeading so the cold tail stays five lines).
    // Hashed when set.
    bool mixedSlots = false;
    // A RESURRECT leg (retail 0x4068b0, VTOL_Resurrect 0x41fe70): the leg's
    // reclaimFeat is the corpse (negative id), and this is the kind the order
    // was issued as -- 1 resurrect, 2 animate (retail mission +0x56 = 0 / 1);
    // 0 = an ordinary RECLAIM. Its walk, turn and stance wait are RECLAIM's
    // (tickReclaim), then the revive channel (tickAbilities). In the pad
    // beside mixedSlots. Hashed on reclaim legs when set.
    uint8_t reviveKind = 0;
    // A HelpBuild (World::assist, or one a guarding builder put ahead of its
    // guard): the unfinished unit it helps. A ground builder's leg is the job
    // itself, run on `mission` with the rectangle controller (tickGroundHelp);
    // a flyer's is only its approach. The leg goes when that job ends, however
    // it ends. Hashed with a tag when set.
    int helpSite = 0;
    // A flyer's guard (VTOL_Follow, TotalA.exe 0x40fbe0, World::tickFlyingGuard):
    // its orbit angle around the guarded unit (mission +0x36, read as a word).
    // Drawn once (rand(0x10000)) when the guard starts, stepped back by
    // 0x4000 + rand(0x2000) each time the orbit point is reached. The guard
    // runs on `mission` (stage 2 once started; its deadline is the next look).
    // Hashed with a tag once the mission runs. VTOL_SeekAttack's orbit angle
    // (seekAttack) lives here too: a seek is never a guard.
    uint16_t guardOrbitAngle = 0;
    // VTOL_SeekAttack (4192f0, tickSeekAttack): what a flyer's attack order
    // becomes when its target is lost (retailAirLostTarget). It circles x/z,
    // the attack's last point, on `mission`, its orbit angle in
    // guardOrbitAngle (mission +0x4e). targetId stays 0. In the
    // tail pad after guardOrbitAngle. Hashed with a tag.
    bool seekAttack = false;
    // A flyer's auto attack put ahead of its default mission (VTOL_Standby,
    // which World does not keep in the queue): acquired idle by the standby's
    // scan or the hit response, or AirToAir's retarget of such an attack. In
    // retail the attack takes that mission's run-time flag 0x2000 (4d7709)
    // and is not the last mission (+0x66), so a lost target ends it without
    // VTOL_SEEKATTACK. Hashed with a tag when set.
    bool aheadOfDefault = false;

    // ---- line 5: SUPPRESS ----------------------------------------------------
    // Retail SUPPRESS (UNITMISSIONCODE_SUPPRESS, 0x404d70; World::tickSuppress):
    // fire at a point. Armed ATTACK on the ground (Cmd::AttackGround) gives it
    // to a ground unit or a structure whose selected weapon is not melee, and
    // to a melee one only over a blocking, clearable feature (World::suppress).
    // Its point is the mission's +0x22, kept in attackPointX/Z (the melee
    // stage 2 walks it toward the unit); x/z are where the order line points
    // and, on a melee approach, the rectangle's steering cell. targetId stays
    // 0. Its stages run on `mission`. Hashed with a tag when set.
    bool suppress = false;
    bool suppressFeature = false;   // +0x56: a clearable blocking feature is the target
    uint8_t suppressSlot = 0;       // +0x4e: the slot it fires (519ac0 at the order)
    int32_t suppressReach = 0;      // +0x52: reach left for the approach rings (range - minrange at stage 0)
    // The approach stage 4 puts ahead of the order (4d4e10's ring with a
    // minimum range, else 4d4da0's circle; wait 0x700): a ground move to the
    // point that ends on arrival or failure, like RECLAIMAREA's. Its outer
    // radius is missionRadius + 4 (the circle's), suppressInner the ring's
    // inner one (0: the circle). Hashed with a tag when set.
    bool suppressApproach = false;
    int32_t suppressInner = 0;
    // A flyer's attack at a point (the same 4de530 kind 3 resolver, by the
    // selected slot's weapon): AIRSTRIKE (`dropped`), AIRTOGROUNDHOVER (the
    // type's hoverattack) or AIRTOGROUND, each with no target unit (mission
    // +0x16 is 0). Its point (+0x22) is attackPointX/Z, its mission runs on
    // `mission`, its +0x4e in airRunCount and its controller in flightGoal,
    // as on a flyer's unit attack (World::tickAirPoint). targetId stays 0.
    // Hashed with a tag when set.
    static constexpr uint8_t kAirStrike = 1, kAirHover = 2, kAirRun = 3;
    uint8_t airPoint = 0;
    // A formation leg an attack's group check put ahead of the attack
    // (SUPPRESS's Move_Ground_Formation, a flyer attack's
    // VTOL_Move_Formation; World::formationAttack), not one a Move's group
    // check made. Hashed with a tag when set.
    bool attackFormation = false;

    // A build leg Ctrl interrupted after its site went down (insertOrderFront):
    // that site. When the leg runs again it helps the site if it still stands
    // unfinished, and places it again if it has gone (World::resumeBuild);
    // negated while that help runs ahead of the leg.
    // Cold: read once, when the leg comes back. Hashed with a tag when set.
    int resumeSite = 0;

    // GROUND_UNLOAD's TransportDrop call: the carrier's retail heading when
    // BoomCalc set the boom. transportX/Z then hold the drop point as an
    // offset from the carrier at that call; the link carries it with the hull
    // (retailHullPoint). Hashed with the transport fields.
    uint16_t transportHeading = 0;
    // A repair-pad mission's pad (Order::airPad; VTOL_Landing / SelfRepair
    // mission +0x16). Hashed with airPad.
    int airPadId = 0;
};
// Which line each field sits on (WE D2). Moving a field across a line, or
// growing line 0/1 past 64 bytes, is a compile error here. (offsetof on a type
// with non-public members of members is conditionally supported; GCC, Clang
// and MSVC all give the real offset, Clang just warns.)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
#define TA_ORDER_LINE(field, line) \
    static_assert(offsetof(Order, field) / 64 == (line) && \
                  (offsetof(Order, field) + sizeof(Order::field) - 1) / 64 == (line), \
                  "Order::" #field " must sit on cache line " #line)
TA_ORDER_LINE(x, 0); TA_ORDER_LINE(z, 0); TA_ORDER_LINE(targetId, 0);
TA_ORDER_LINE(load, 0); TA_ORDER_LINE(unload, 0); TA_ORDER_LINE(attackMove, 0);
TA_ORDER_LINE(patrol, 0); TA_ORDER_LINE(guard, 0); TA_ORDER_LINE(waitAttack, 0);
TA_ORDER_LINE(goal, 0); TA_ORDER_LINE(groundMission, 0); TA_ORDER_LINE(wait, 0);
TA_ORDER_LINE(buildType, 0); TA_ORDER_LINE(reclaimArea, 0);
TA_ORDER_LINE(reclaimFeat, 0); TA_ORDER_LINE(repairTarget, 0);
TA_ORDER_LINE(transportUnloadApproach, 0); TA_ORDER_LINE(navigationConsumed, 0);
TA_ORDER_LINE(autoTarget, 0); TA_ORDER_LINE(flightMoveMission, 0); TA_ORDER_LINE(landing, 0);
TA_ORDER_LINE(transportPickup, 0); TA_ORDER_LINE(navigationExhausted, 0);
TA_ORDER_LINE(hasSegment, 0);
TA_ORDER_LINE(issuedTick, 1); TA_ORDER_LINE(convoyTick, 1); TA_ORDER_LINE(controller, 1);
TA_ORDER_LINE(park, 1); TA_ORDER_LINE(buildRectangle, 1); TA_ORDER_LINE(flightGoal, 1);
TA_ORDER_LINE(missionTarget, 1); TA_ORDER_LINE(patrolRepair, 1); TA_ORDER_LINE(patrolOpen, 1);
TA_ORDER_LINE(guardRepair, 1);
TA_ORDER_LINE(transportUnloadReleasePending, 1); TA_ORDER_LINE(transportUnloadTransferDeferred, 1);
TA_ORDER_LINE(formationLevel, 1); TA_ORDER_LINE(stationFree, 1); TA_ORDER_LINE(missionRadius, 1);
TA_ORDER_LINE(orderSerial, 4);
static_assert(offsetof(Order, mission) == 128, "line 1 ends at byte 127; line 2 starts with mission");
TA_ORDER_LINE(suppress, 5); TA_ORDER_LINE(suppressFeature, 5); TA_ORDER_LINE(suppressSlot, 5);
TA_ORDER_LINE(suppressReach, 5); TA_ORDER_LINE(suppressApproach, 5); TA_ORDER_LINE(suppressInner, 5);
TA_ORDER_LINE(airPoint, 5); TA_ORDER_LINE(attackFormation, 5); TA_ORDER_LINE(resumeSite, 5);
TA_ORDER_LINE(transportHeading, 5); TA_ORDER_LINE(airPadId, 5);
static_assert(offsetof(Order, suppress) == 320, "lines 0-4 are full; line 5 starts with SUPPRESS");
static_assert(sizeof(Order) == 384, "Order is six cache lines (line 5 has room left); a new field names its line above");
#undef TA_ORDER_LINE
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// Legion's headway window (World::updateLegionStill): one Legion ground
// body's tallies since its window began, and one tick of the rule as a pure
// function of the tick's inputs (tools/legion_still_test drives it with
// made-up motion). (x, z): where the window began (raw px); turn: the signed
// turn since (BAM); path: the distance walked (raw px); turned: the turns'
// total (BAM); all valid while the body's window ticks are above 0. (lockX,
// lockZ): where its livelock began (raw px), and lockTicks its age, valid
// while it lasts (at most kLock ticks).
struct LegionHeadway {
    static constexpr uint8_t kWindow=8;
    static constexpr int32_t kLock=64;
    int32_t x=0,z=0,turn=0,path=0,turned=0,lockX=0,lockZ=0,lockTicks=0;
    // One tick: the body's raw position at its start (beforeX, beforeZ) and
    // end (x, z), its heading change (BAM), base speed (raw px a tick), turn
    // rate this tick (BAM) and larger footprint side (cells). Updates the
    // window's ticks and the still and livelock (shuffle) flags.
    void tick(uint8_t& ticks,bool& still,bool& shuffle,int32_t beforeX,int32_t beforeZ,int32_t x,int32_t z,
              int16_t turn,int32_t speed,int32_t rate,int foot);
};

// Laid out by cache line (WE D1), alignas(64), with offset static_asserts after
// the struct (TA_UNIT_LINE): a new field has to name its line.
// - line 0: what a neighbour probe reads (forEachNear callbacks, rebuildGrid/
//   rebuildOccupancy, the economy and live-count passes, unit(id) probes):
//   identity, position, liveness, hp, speed, heading and the hot flag bytes.
// - lines 1-3: per-tick mutable state of the unit itself (incapacitation timers,
//   the presence of the rarely engaged construction/landing payloads -- each a
//   HeapOptional, one pointer -- the order and build queues, the builder ids,
//   reloads, fire bookkeeping, route stamps); line 4 the per-tick weapon
//   animation clear (line 5 holds the rest of the animation packet).
// - line 6: the mover's own state and what exploration reads.
// - after that: warm per-unit state, then the cold tail (display-only and
//   rarely set blobs).
struct alignas(64) Unit {
    int id = 0;
    int player = 0;
    // Keep broad-phase eligibility and position together for nearby-unit scans.
    const UnitType* type = nullptr;
    Fixed x, z;   // FIXED-POINT: 16.16, 65536/px. See fixed.h.
    int32_t deadFor = -1;  // >= 0 once dead; counts up for death animation
    int inTransport = 0;   // id of carrying transport, 0 = none
    // Fixed, not float: retail keeps no float in its unit state (docs/retail-engine.md).
    //
    // RANGE. Fixed is 16.16 in an int32, so it saturates at 32768 -- and the largest
    // `maxdamage` in the 155 shipped FBIs is 29999, which fits with about 9% to spare.
    // (That the shipped ceiling sits just under a 16-bit boundary is itself a hint at
    // what retail stored.) A mod declaring more than 32767 hp would wrap, so this is a
    // real limit, not a theoretical one.
    //
    // The economy pools are NOT Fixed for exactly this reason: `mana` accumulates
    // without a cap and routinely passes 32768 (the tests alone set 200000 and 1e9,
    // which wrap to 3392 and -13824). Converting it broke production outright. Doing
    // mana in fixed point needs a 64-bit Fixed, which is a separate job.
    Fixed hp = Fixed::fromInt(100);
    // FIXED-POINT px/s. Retail's is too: its occupancy test compares two units' speeds
    // with an integer cmp/jl (0x4db79e), and the scaling either side runs through a
    // 64-bit shift-by-16 helper (0x5d3dc0) -- a 16.16 multiply, the same operation as
    // Fixed::operator*. Speed feeds the step length, so leaving it float would have kept
    // a float multiply in the middle of an otherwise integer displacement.
    Fixed speed;
    // BINARY ANGLE, 65536 == 360 degrees, 0 = +z. Retail's navigator +0x7e
    // differs by a half turn; retailmotion.h converts at observation boundaries.
    // Wrapping is a mask, two angles added
    // cannot drift, and there is no rounding for two libms to disagree about.
    Bam heading;
    int   weaponSlot = 0;          // active weapon (0=primary); player-selectable
    bool underConstruction = false;
    bool  cloaked = false; // currently invisible to enemies
    bool  beingBuilt = false;  // site: transient -- a work event at its GetBuilt dispatch
    int8_t squad = 0;      // control squad: 0=none, +N=group N, -N=formation N (N=1..10,
                           // the digit 0 key = 10). A unit is in exactly one squad. A
                           // formation (squad<0) moves at its slowest member's speed and
                           // its stragglers rejoin. Set via Cmd::SetSquad; folded in the hash.
    uint8_t flightGroundMode=1; // retail unit +130 low bits: 1 landed, 2 airborne
    bool legionLift=false; // see the lift block below
    bool legionStill = false; // see legionWindow below
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
    uint8_t standingOrder = 2; // 0 passive, 1 defensive, 2 offensive (retail never holds 3)
    uint8_t moveState = 2;
    uint8_t fireState = 2;
    bool  active = true;   // onoffable units: false = powered down
    bool standbyActive = false; // see standbyState below
    bool guardNoMoveActive = false; // see guardNoMoveState below
    bool constructionHolding = false; // recomputed each tick before movement; not persistent state
    bool  justFired = false;   // set for one tick when the weapon fires
    // True until the player picks a weapon with Ctrl+W. While set, the sim chooses
    // the best usable weapon per target the way retail's fire-at-will scan does;
    // once the player has chosen, their pick is obeyed.
    bool  weaponAuto = true;
    HeapOptional<RetailConstructionSite> retailSite;
    HeapOptional<RetailConstructionJob> retailBuild;
    HeapOptional<RetailConstructionEmitter> constructionEmitter;
    // Ordinary visual effects run on sim ticks but never affect gameplay/hash.
    HeapOptional<RetailConstructionEmitter> cosmeticConstructionEmitter;
    // The landing mission (see the lift and flight blocks below).
    HeapOptional<RetailLandingState> landing;
    // TICKS, all four -- see the note on `reloads`. Retail counts a timer in
    // 30Hz ticks and scales by 1/30 only to display it (tools/re/emufields.py).
    int32_t frozenFor = 0;   // >0 = frozen solid (can't act); counts down
    int32_t stonedFor = 0;   // >0 = petrified (can't act, immune to damage while stone)
    int32_t paralyzedFor = 0;// >0 = paralyzed (can't act, still takes damage)
    // Self-destruct (the SelfDestruct mission 4017e0): -1 not armed; >= 0 the
    // countdown's ticks left, its last run drawing rand(15); <= -2 that
    // draw's wait, -2 - ticks left (World::tickSelfDestruct).
    int32_t selfDestructT = -1;
    // The tick's REQUESTED turn (want - heading, unclamped BAM). Not hashed
    // itself, but World::notifyTurnDirection reports it to the unit's own
    // (hashed) script VM, so it must stay deterministic; the client animation
    // reads it too. Retail's TurnDirection callin (icd 0x4d9593) converts the
    // requested arg, NOT the clamped rotation applied to heading, so both need
    // this rather than a heading-delta (which would truncate to zero for slow
    // turners). Reset each tick; set where the mover turns.
    int32_t turnReqBam = 0;
    // GET 33 reads the mover's signed applied turn from its previous update.
    // COB runs before this tick's mover, so publish the completed turn at tick end.
    int16_t animationTurnBam = 0;
    // Scratch captured at tick start; intentionally excluded from lockstep state.
    uint16_t tickStartHeadingBam = 0;
    // These queues hold at most a handful of entries and are edited only on order
    // completion (not in a hot inner loop), so std::vector -- which allocates NOTHING
    // when empty, unlike std::deque's eager ~576-byte control block -- is both the
    // memory-lean and the faster choice; front-pops become erase(begin()). Element
    // order (all that stateHash folds in) is preserved, so lockstep is byte-identical.
    std::vector<Order> orders;
    // Production (buildings with a build tree).
    std::vector<const UnitType*> buildQueue;
    const UnitType* repeatType = nullptr;   // infinite production: re-queue when idle
    int buildSiteId = 0;   // builder: id of the building it is constructing
    int reclaimId = 0;                 // builder: feature being reclaimed (0 = none)
    int repairId = 0;                  // builder: damaged friendly being repaired (0 = none)
    int productionSiteId = 0; // attached unfinished factory output
    // PLAIN TICKS, and deliberately not 16.16.
    //
    // Retail's build routine computes buildtime / (workertime/30) * 65536 and stores
    // an integer (0x406d11..0x406d2a, emulated), which reads as a fixed-point tick
    // count -- and this field was that. It overflows. The shipped VERMAGE has
    // buildtime 100000 against a workertime of 10, i.e. 300,000 ticks, and 300000 <<
    // 16 does not fit an int32: `total` came out NEGATIVE, `buildProgress < total`
    // was false on the first tick, and the most expensive unit in the game finished
    // instantly and free because the mana branch never ran. ZONHURT/ZONHUNT/TARNECRO
    // at buildtime 22500 wrapped to about a 34th of their real cost.
    //
    // Nothing is lost by dropping the fraction: progress advances by exactly one tick
    // per tick, so there is no partial tick to keep. It does mean my reading of that
    // retail routine is incomplete -- whatever it stores at +0x52, it cannot be a
    // 16.16 count of a 300,000-tick build either.
    int32_t buildProgress = 0;
    int justBuilt = 0;         // unit id produced this tick (viewer hook), else 0
    int conjureHoverTarget = 0;
    // TICKS, not seconds -- retail stores a reload as an integer count and only
    // multiplies by 1/30 to PRINT it. Confirmed by emulating the display path
    // (0x4fbfb2: mov 0x9c(%esi),%cx; fildl; fmull 0x5f25d8 where 0x5f25d8 == 1/30):
    // a planted 90 renders as "3.000000" seconds. See docs/retail-engine.md.
    int32_t reloads[3] = {0, 0, 0};  // per weapon slot, in ticks
    uint32_t firedWeapons = 0; // display event mask; each bit identifies an actual shot
    uint32_t fireAnimations = 0; // callback delivery, before script-triggered projectile creation
    Fixed homeX = Fixed(), homeZ = Fixed();   // leash anchor (idle position) for auto-chase
    // The tick the navigator admitted its last search (-1 = none) plus the search's
    // outcome flags -- retail's navigator stamp (+0x110) and outcome bits (+0x134
    // bits 0/1; bits 2|3 = failed). The cadence ladder in the mover reads these
    // EVERY FRAME, rolls fresh dice, and re-requests when the elapsed time beats
    // them; nothing stores a deadline. Stamps and outcomes are hashed.
    int32_t routeStamp = -1;
    bool routeCrowded = false;   // bit 0: same-way traffic within goal tolerance
    bool routeTraffic = false;   // bit 1: same-way traffic elsewhere on the route
    bool routeFailed = false;    // original outcome bit 2: partial route
    bool routeDetour = false;    // bit 3: reconstruction exceeded Manhattan distance
    uint16_t groundTerrainFlags = 0; // mover +36 bits 11/12, refreshed by the mover
    uint8_t groundMovementMode = 0; // retail mover +36 bits 5..7; mode transitions remain partial
    uint8_t groundSpeedMode=0; // mover +36 bits 8..10
    int scriptAimTarget = 0;
    std::vector<ta::RetailWeaponAnimation> pendingWeaponAnimations; // command callbacks for next display tick
    ta::RetailWeaponAnimations weaponAnimations;
    // Line 6: the mover's own state and what exploration reads (flightY/groundY,
    // sightFootprint), so updateNavigationExploration touches lines 0 and 6.
    Fixed flightY;     // absolute simulation height, not a render animation
    Fixed groundY;
    RetailSightFootprint sightFootprint;
    Fixed baseSpeed;   // entity +0x12b: individual speed before terrain modifiers
    uint32_t groundMoveTick=0; // mover +2c: last proposed position/mode change
    uint32_t groundScanTick = 0;    // mover +30: next local navigation scan
    uint32_t groundGradeTick = 0;   // mover +28: last occupancy-grid refresh
    // TICKS and 16.16, like the rest of the sim -- these steer repaths and give-ups,
    // so they decide hashed outcomes even though they are not themselves folded in.
    int32_t repathLeft = 0; // chase steering repath countdown, in ticks
    int32_t stuckFor = 0;   // ticks wanting to move but making no progress
    Fixed stuckX = Fixed(), stuckZ = Fixed();   // position when the stuck timer last reset
    // Asymmetric yield (see the yield block in the mover): >0 while this unit is standing
    // aside to let an opposing one through, counting down.
    // ...and a cooldown after one, during which it cannot be asked to yield again.
    // Without it, "the lower id yields" starves the low ids outright: a unit facing a
    // stream of higher-id units is re-yielded the moment each hold expires and never
    // moves again. Measured -- opposing columns fell from 32/32 arriving to 20/32, with
    // the survivors travelling an almost perfect straight line, which is the shape of a
    // rule that works beautifully for whoever wins it.
    // Consecutive refused steps, saturated at two: mover +36 sets 0x8 on
    // first refusal, then 0x4 on repetition. Caps are base/2 then base/5.
    int32_t bodyBlockStreak = 0;
    uint16_t groundPitch=0,groundRoll=0;
    // Legion (deliberate, not retail): a ground body that makes no real
    // headway stands in its scripts (GET 29 and MoveRate report it stopped)
    // instead of walking on the spot. The ticks of the current headway
    // window (0: the next tick starts one; its start and tallies live in
    // World::legionWindow_), and whether the body stands although it went on
    // stepping or turning (a livelock: its navigator then holds it). See
    // World::updateLegionStill (legionStill has a window of its own there,
    // World::legionBodyWindow_). Hashed in Legion.
    uint8_t legionWindow = 0;
    bool legionShuffle = false;
    uint16_t variationPhase=0; // original per-unit random phase at +82
    // --- extended runtime state --------------------------------------------
    // DOUBLE, like Player::mana and for the same reason: retail's pools are 64-bit
    // floating point (the affordability check at 0x46e85f subtracts one with `fsubl`).
    double layoutPadMana_ = 0;   // was Kingdoms' per-unit mana; kept for the Unit layout asserts
    uint32_t missionEvents = 0; // read by every unit's mission dispatch each tick
    // Experience (+0xf0): the experiencepoints of every unit it killed (512ce0),
    // or an authored level's worth (setLevel 519370). `veteran` is its level.
    int32_t xp = 0;
    float scenarioArmor = 1.0f, scenarioWeapon = 1.0f; // permanent authored multipliers, separate from auras
    int   veteran = 0;     // veteranLevel(): up to 10; scales attack/armor/mana per shot
    float atkBuff = 1;     // live attack multiplier from auras (decays to 1)
    float armBuff = 1;     // live armour multiplier from auras (decays to 1)
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
    int   stance = 1;      // combat stance: 0=offensive (chase freely), 1=defensive
                           // (leashed, the default/legacy behaviour), 2=passive
                           // (hold fire: no auto-acquire, only fights when ordered)
    // Engine addition (owner, 2026-11-12): a factory's, or a mobile builder's on
    // an infinite queue, stance for its output (World::setStance); -1 = none,
    // the output keeps its own default. Hashed when set.
    int8_t outputStance = -1;
    int lastHitPlayer = -1; // score attribution captured at impact; mission script input
    int   lastHitBy = 0;   // id of the unit that last damaged this one (for kill XP)
    // Blocked shots (engine deviation, see World::noteShotImpact): the target
    // whose last shots from here struck terrain or scenery this unit cannot
    // destroy, short of the target; how many in a row; the whole-pixel spot
    // they were fired from and the one the target stood on; how many firing
    // spots it has walked to against that target since it last hit it; and
    // whether the last spot search from here found nothing. Hashed with a
    // tag when blockedTarget is set.
    int32_t blockedTarget = 0;
    uint8_t blockedShots = 0;
    uint8_t repositions = 0;
    uint8_t searchFailed = 0;
    int16_t blockedX = 0, blockedZ = 0;
    int16_t blockedTX = 0, blockedTZ = 0;
    bool workScriptWorking = false; // repair/reclaim requested StartBuilding
    bool buildBegun = false;   // construction site: true once the builder arrived
    RetailMissionState standbyState;
    bool standbyAllowed = true; // partial replay can disable unsupported idle handlers
    bool guardNoMoveAllowed = true;
    RetailMissionState guardNoMoveState;
    int32_t buildStuckT = 0;        // ticks a builder has approached its site with no progress
    // SQUARED px, which is why this one is NOT Fixed: 20px squared is 400, but a map
    // diagonal squared is ~1.3e8 -- far past 16.16's 32768 ceiling. Exact int instead.
    int32_t buildStuckD = INT32_MAX;// best (closest) squared dist to the build site so far
    // INT32_MAX while an authored Dying callback still owns the body. SET26/31
    // resolve the handoff; no Dying callback retires immediately.
    int32_t deathAnimationTicks = 0;
    bool deathHasCorpse = false;
    int corpseAnimationTicks() const { return corpseStatue>=0 ? 0 : deathAnimationTicks; }
    int32_t corpseUntil = 0;  // handoff plus authored decomposition/sinking lifetime
    // Fixed: this is hp that went past zero, in the same units hp is now kept in.
    Fixed overkill = Fixed();  // damage past the killing blow (retail severity input)
    uint8_t deathType = 1;   // damagetype of the killing blow (3 = explosion/gib)
    // Retail's self-destruct damage type (icd: the SelfDestruct mission applies
    // 30000 of type 5, and 0x5126a9 gives 5 its own branch in the death
    // handler). Kept as a named constant because it is NOT an FBI damagetype a
    // weapon can carry -- nothing in the shipped data uses 5 -- it is the engine
    // marking "this unit quit" rather than "this unit was killed".
    static constexpr uint8_t kDeathSelfDestruct = 5;
    static constexpr uint8_t kDeathPassenger = 8;    // a dying carrier's cargo
    // World's own number (no Kingdoms kind): a unit reclaimed to nothing,
    // TotalA.exe's damage kind 5 (ReclaimUnit 0x404981).
    static constexpr uint8_t kDeathReclaim = 16;
    static constexpr uint8_t kDeathBecomeFeature = 9;   // a finished wall (isfeature)
    uint8_t severity = 0;    // retail Killed severity ((overkill% + 1s-ago HP%)/2)
    // Dead but not yet retired (512ae0): its Killed/Dying still runs and its
    // object still lives (+0x130 & 0x1000000), so a shot of its in flight
    // still names it the hitter. Retirement credits the kill and fires the
    // death weapon (World::retireDeath).
    bool deathPending = false;
    uint8_t hpPct1s = 100;   // HP% sampled every 30 ticks (previous sample -- the
    uint8_t hpPctCur = 100;  //  retail unit+0x111/+0x110 pair severity reads)
    int corpseStatue = -1;   // FeatType override chosen at death (stone/frozen), -1 = corpse=
    // A reclaim finished this body: it fades out (corpseUntil is the end of the
    // fade) and can no longer be reclaimed or raised. Hashed.
    bool corpseFading = false;
    // The revive channel of the current RESURRECT leg (Order::reviveKind):
    // the dead unit being raised (0 = none) and its kind, 1 resurrect (the
    // body's own unit) or 2 animate (the caster's animatetype).
    int reviveTarget = 0;
    int8_t reviveMode = 0;
    // Retail mission +0x52: the channel's work left, 16.16. Each tick takes the
    // owner's allocation x 65536 (icd 0x406d3b); the revive completes at <= 0.
    // Hashed while channelling.
    int32_t reviveWork = 0;
    bool corpseBlocks = false;   // installed corpse feature blocks its own footprint
    // Retail unit+0x10e: the 1/256-HP remainder self-repair carries between calls
    // (retailSelfRepairPoints). Zero at creation, like retail's (0x511ae2).
    uint8_t healCarry = 0;
    RetailAirOccupant airOccupant; // secondary occupancy list/origin (+0x117..+0x128)
    RetailFlightVector flightVelocity;
    std::array<int32_t,3> flightAcceleration{}; // retained native banking input
    RetailFlightNavigation flightNavigation;
    // A native flight-point controller belongs to the mover and can outlive
    // the mission that installed it (VTOL_UNLOAD's post-transfer step-out).
    HeapOptional<RetailFlightGoal> retainedFlightGoal;
    bool retainedFlightControllerActive=false;
    int flightSectorX=0,flightSectorZ=0; // center sector at the last footprint relocation
    uint32_t scriptOccupancy=0; // last setSFXoccupy notification (unit +100)
    uint32_t flightBeginCallbackSerial=0; // display-only BeginFlight call-in edge
    uint32_t flightLandingCallbackSerial=0; // display-only edge; excluded from lockstep hash
    // Legion (deliberate, not retail): an idle landed flyer lifted to let an
    // allied ground group pass under it (World::requestLegionLift). It hovers
    // over the spot it left until no group has asked for `legionLiftQuiet`
    // ticks, then lands again through the retail landing mission. Hashed.
    uint32_t legionLiftUntil=0;
    uint32_t legionLiftRest=0;   // after landing again, no new lift before this tick
    Fixed legionLiftX,legionLiftZ;
    // W6 FL-01: the tick this lift episode began (hashed with legionLift).
    // An episode is capped at kLegionLiftCap ticks: after that only a member
    // whose next planned cells the flyer covers keeps it up, and once it has
    // landed it rests kLegionLiftCapRest ticks.
    uint32_t legionLiftSince=0;
    // Observation only (never hashed, never read by a decision): the tick the
    // flyer handed its last episode over to landing (the relift counter).
    uint32_t legionLiftEnded=0;
    // Engine deviation (owner, 2026-11-12): a landed flyer on Hold Position
    // whose auto slot has a target takes off and hovers over the spot it
    // left, facing the target, without chasing (World::tickHoldLift); it lands
    // again holdLiftQuiet ticks after its last target in range. Some flyers'
    // scripts (the Thirsha's) release a shot only in flight. Hashed.
    bool holdLift=false;
    uint32_t holdLiftUntil=0;
    Fixed holdLiftX,holdLiftZ;
    // A flyer's FireWeapon starts that its script never released (no SET 23
    // before the next start): the Thirsha's on the ground. Counted for flyers
    // only; hashed when set.
    bool fireStartPending=false;
    uint8_t unreleasedFires=0;
    uint32_t fireStartTick=0;   // the tick of the pending start
    std::array<ta::TaAimState,3> weaponAim{};   // TotalA.exe slot aim record (tickScriptWeapon)
    std::array<std::array<int32_t,3>,3> weaponMuzzlePoints{}; // display-only captured QueryWeapon geometry
    std::array<std::array<int32_t,3>,3> weaponAimPoints{}; // display-only captured aim geometry
    // Retail's per-slot weapon record layer under the missions (icd 52aae0,
    // 519b10/519b70, 40f330). slotTarget is record +0x10 (0 = none): the target
    // the fire-at-will sweep or the hit response handed an AUTO slot (and
    // the air missions' 51a700 their manual selected slot). slotAuto is
    // record +0x1a bit 4 per slot (set at spawn); a mission that owns the
    // weapons (attack, the work stage of build/repair/reclaim, a status) makes
    // the slots manual and slotManual records why, so the slots return to auto
    // when that mission retires (the 4d6da0 destructor rule). AirToGround and
    // AirToAir alone leave an attack order's slots mixed (Order::mixedSlots):
    // the selected one manual, the others auto. faceRequest is the mover's
    // facing request (+0x34 with +0x36 bit 0x10, -1 = none) that an auto slot
    // posts when it starts aiming; only a stopped mover honours it. All
    // hashed.
    std::array<int32_t,3> slotTarget{};
    uint8_t slotAuto = 7;
    uint8_t slotManual = 0;
    // Formerly Legion's Kingdoms aim-stall count; TA has no SET 22 handshake,
    // so it stays 0 (kept for the Unit layout). Hashed with a tag when set.
    std::array<uint8_t,3> aimWait{};
    int32_t faceRequest = -1;
    // A slot's point target (record +4/+6 with +6 not 0x8000, set by 51a770:
    // SUPPRESS's stage 3): the point in whole px, and a bit per slot that
    // holds one. A slot holds a unit target or a point, never both. Its
    // weapon update (52ae90 -> 52fe30) aims and fires at the point
    // (World::tickPointWeapons) whatever mission leads, until the slot is
    // retargeted or changes mode (51a7f0, TargetCleared). Hashed when set.
    uint8_t slotPoint = 0;
    std::array<RetailGroundPoint,3> slotPointAt{};
    // 4c6800's current-view footprint (the sight object at unit+0x84 as the
    // per-player view counters see it) and the plane it is counted in (-1 =
    // none). Kept beside sightFootprint, which feeds the persistent exploration
    // plane and skips removals; this one always takes its old cells out of the
    // counts when it moves (only those it no longer admits, see
    // retailSightMove), so World::viewCounts_ is exactly the sum of the active
    // footprints. Recording the counted player makes deaths and ownership
    // changes self-correcting. Hashed.
    RetailSightFootprint viewSight;
    int8_t viewPlayer = -1;
    // The planes viewSight is counted in (bit per player): the owner's, plus
    // each ally's, human or computer (World::viewShareMask). 0 = none. Hashed.
    uint16_t viewPlanes = 0;
    float constructionFraction() const {
        if (!underConstruction) return 1.0f;
        // TA: retail's remaining fraction (sim/taeconomy.cpp) once building began.
        if (buildStarted) return std::clamp(1.0f-buildRemaining,0.0f,1.0f);
        return std::clamp(retailSite ? 1.0f-retailSite->progress.remaining :
            (hp.toFloat()/std::max(1,type->maxHp)-0.05f)/0.95f,0.0f,1.0f);
    }
    uint32_t constructionVisualRandom=1;
    // Cumulative cosmetic work events (falling/rising), excluded from stateHash.
    // Counters preserve completion events when a published snapshot skips ticks.
    std::array<uint32_t,2> constructionEmissions{};
    HeapOptional<RetailFlightGoal> conjureHoverGoal;
    std::vector<int> cargo;
    // BUILD stage 2's refusal sleep (401df4): the tick a factory whose output
    // site was refused may test it again; also a rallied mobile producer's next
    // room search (clearProductionRoom). 0 = not waiting. Hashed when set.
    uint32_t productionRetryTick = 0;
    // Factory/conjure progress past buildProgress, in 1/65536 of a tick of work.
    // Nonzero only when the player's construction share (Player::constructionShare)
    // bought part of a tick. Hashed when set.
    uint16_t buildFraction = 0;
    // RALLY orders for a production building (or a mobile builder while it repeats
    // production): what the units it makes should do, from the moment they are
    // finished. Kept OUT of `orders` on purpose -- a
    // structure must never enter the mover (it has no velocity, but the mover still
    // turns a unit's heading toward its goal, which is how right-clicking used to spin
    // a keep). Nothing reads this but tickProduction, which copies it onto each new
    // unit. Move, attack, fight-move and patrol all land here, and they queue;
    // Stop clears them. Hashed when set.
    std::vector<Order> rally;

    // Retail scenario rename: cosmetic label, independent of FBI type identity.
    std::string scenarioName;
    // The body's place in retail's ground word +0 (World::groundOwner_): the
    // origin and size it was last stamped with (+0x74/+0x78), its sector list
    // (+0xa4) and when it joined it, and the two overlap bits of +0x130.
    struct GroundStamp {
        enum : uint8_t { kLinked=1, kOffMap=2, kRefusing=4 /* 0x400000 */, kRefused=8 /* 0x800000 */ };
        int16_t x=0,z=0,sectorX=0,sectorZ=0;
        uint8_t fx=0,fz=0,flags=0;
        uint32_t link=0;   // sector lists are newest first (506630 pushes at the head)
    } ground;
    // Retail unit +0xcc: the tick before which the unit may not cloak. The cloak
    // step (icd 0x401160) decloaks while it lies ahead, and sets it to now + 90
    // when the unit's own mana cannot pay a tick of cloaking. 0 when never set;
    // hashed when set. Cold: only cloakers read it, so it sits past the
    // prefetched lines.
    uint32_t cloakBlockedUntil = 0;
    // A mobile builder's guard (Follow_Ground 0x407f90 stage 1): the tick its
    // next look for work to help with or repair is due. Re-armed 15 + rand(15)
    // ticks out when it finds nothing. (A flyer's guard, VTOL_Follow, keeps
    // its look deadline on the guard order's mission instead.) Hashed with a
    // tag when set.
    uint32_t guardHelpTick = 0;
    // Retail's attacked alarm: every hit that runs the hit response (409a50,
    // which first sends event 0x10 to the unit's listeners, a guard's Follow
    // mission among them) stamps tick + 1 here; the attacker is lastHitBy
    // (retail +0xf8). Read only by guards (guardHeard), so not hashed.
    uint32_t attackedTick = 0;
    // On a guard: the guarded unit's attackedTick it has already answered
    // (Follow_Ground / VTOL_Follow consume event 0x10 when a look runs with
    // it). A larger attackedTick is an alarm still to hear. Hashed with a tag
    // when set.
    uint32_t guardHeard = 0;
    // On a factory/conjurer output: the producer whose buildProgress is this
    // unit's one shared progress (retail +0x108). A hint only -- read through
    // World::productionOwnerOf, which checks the producer still holds it, so a
    // stale value is harmless and it is not hashed.
    int productionOwner = 0;
    // On a building's finished output: the factory that made it (set at
    // completion, never cleared). World::clearYardDoor takes one with orders in
    // front of its factory as the door's traffic, and moves a parked one only
    // off the pad or off a later output it holds up. Set once, from hashed state
    // (the completion), and read only there, so not hashed.
    int builtBy = 0;
    // On a guard: the factory whose door it last stepped out of
    // (World::clearYardDoor) and that factory's doorSerial then. It steps aside
    // there again only once the serial has moved on (a newer output or a new
    // refusal), not each time its guard walks it back in. Cleared by a fresh
    // guard order. Hashed with a tag when set.
    int doorYieldFactory = 0;
    uint32_t doorYieldSerial = 0;
    // On a factory: grows by one for each refusal episode of its next output's
    // site (odd) and to the next even number for each finished output, so a
    // guard's step aside is keyed on this factory alone and never goes back.
    // Hashed with a tag when set.
    uint32_t doorSerial = 0;
    // On a factory: its pad's offset from its centre (world px), from the last QueryBuildInfo point its BUILD
    // stage asked for (World::productionPosition); padKnown once one was asked for. A guard of the factory keeps
    // out of the lane in front of it (World::guardDoorLane). Hashed with a tag when set.
    int32_t padDx = 0, padDz = 0;
    bool padKnown = false;
    // A factory's refused output site (BUILD stage 2's point, world px), while
    // productionRetryTick lies ahead: clearYardDoor clears it with the pad (a
    // pad point on the yard's edge puts part of the site outside the yard).
    // Set at each refusal from hashed state (the script's point) and read only
    // while that refusal lasts, so not hashed (as builtBy).
    int32_t refusedSiteX = 0, refusedSiteZ = 0;
    // On a computer player's unit: its retail AI squad (unit+0xc8, written by
    // the AI's set-squad 0x50b4f0) when that squad is a strike or a VTOL squad,
    // the only kinds whose record the AI makes a formation (World::aiFormation,
    // 50b700); else 0, as the other squads' plain records pace nothing. Set by
    // Cmd::AiGroup (World::setAiGroup); hashed with a tag when set.
    int8_t aiGroup = 0;
    // Its kill count (+0xfe, a word): 512ccd counts a kill under the same gate
    // as the experience, a placement's Kills sets it (514cfb), and a new object
    // starts at 0 (511c16). Shown on the panel's KillCount for the local
    // player's unit (4b28e8..4b2909). Display only, so not hashed, like
    // Player::kills.
    uint16_t kills = 0;
    // A campaign mission's fixed veterancy multiplier (SetMission/InitialMission
    // `v F`, unit +0xf4, set by icd 0x5148e1). Not 0: getMult (0x5193c0) answers
    // it instead of 1 + level/10 and getLevel (0x519310) answers 10, so the unit
    // shows full rank and kills no longer change either. A noveteran type
    // ignores it, as both helpers test that bit first. Hashed when set.
    float vetOverride = 0.0f;
    // Retail's selectable bit (unit +0x130 & 0x20). Every new unit has it
    // (511990, 511b21), a change of owner included; only a mission's order
    // parse clears it (513fb0's end, 514a7d: when a clause pushed a mission,
    // InitialMission and SetMission alike), and MakeSelectable (401450) sets it
    // again when it comes up. Without it the owner cannot select the unit, its
    // move answers no attacker (Move_Ground 402ece, VTOL_Move 418250), the
    // retail AI leaves it out of its bases, and a computer's revive is never
    // abandoned (406ebb). Hashed when clear.
    bool selectable = true;
    // An unfinished site's own GetBuilt (402220), every game: the dispatcher
    // state World::tick's decay pass runs it with (retailSiteTimer). A work
    // event from a worker in a later unit slot reaches the site's dispatch one
    // tick later (siteWorkLate; beingBuilt is the event at dispatch time).
    // Hashed while the site is unfinished.
    RetailMissionState siteTimer;
    bool siteWorkLate = false;
    // Whether the unit carries that GetBuilt. A site placed by a builder or a
    // producer does (pushed at 401f95 / 405a2b); the new object of a change of
    // owner never does: 514da0 creates it finished (512060's sixth argument 1,
    // 514ec6) and only then copies the old object's remaining work and HP, so
    // an unfinished site that changes sides never decays (E: no mission is
    // pushed on it, check_change_owner.py unfinished). Hashed when clear on an
    // unfinished site.
    bool siteGetBuilt = true;
    // TotalA.exe's +0xf5: the damage kind (sim/retailveterancy.h, tadeath) of
    // the last damage record that reached the unit, written by every record but
    // a repair whether or not it names an attacker (0x489dac); a new unit starts
    // at 0. The under-attack request reads it, before the hit updates it
    // (0x4071ca). Hashed when set.
    uint8_t lastHitKind = 0;
    // TotalA.exe's +0xf9: the carrier's script piece a carried unit rides
    // (0x48ab70 stores ATTACH_UNIT's piece; -1, the carrier's own point, for
    // a drop and for VTOL_PICKUP's engine attach here). The per-tick commit
    // (4dad30 -> 4dd250) places the cargo there. Hashed while aboard.
    int8_t attachPiece = -1;
    const std::string& displayName() const { return scenarioName.empty() ? type->name : scenarioName; }
    bool alive() const { return deadFor < 0; }
    int32_t maximumHp() const { return retailSite ? int32_t(retailSite->type.maxHp) : type->maxHp; }
    bool embarked() const { return inTransport != 0; }
    // Frozen/petrified/paralyzed units can't move, turn, or fire.
    bool incapacitated() const { return frozenFor > 0 || stonedFor > 0 || paralyzedFor > 0; }
    // Veterancy stat multiplier (retail: base 1.0, +10% per level, level capped
    // at 10 => up to 2.0x). Scales attack up, armor up (less damage taken) and
    // divides mana per shot; not the reload (530140 reads the weapon's own),
    // except in World's script-less fallback (fire). Verified against
    // KINGDOMS.icd. See retail-engine-internals.
    // A mission's `v F` (vetOverride) replaces it, as getMult 0x5193c0 does.
    float vetMul() const {
        if (vetOverride != 0.0f && type && !type->noVeteran) return vetOverride;
        // getMult's fild/fmul 0.1f/fadd 1.0f run at the x87's 53 bits: one
        // rounding to float, not two (tools/re/check_veterancy.py).
        return float(1.0 + double(0.10f) * double(veteran));
    }
    // getLevel (0x519310), from xp, the unit's own experiencepoints and `v F`.
    // Retail works it out on every read; World keeps `veteran` equal to it.
    int veteranLevel() const {
        return type ? retailVeteranLevel(xp, type->experiencePoints, type->noVeteran, vetOverride) : 0;
    }
    bool moving() const { return alive() && (speed > Fixed::fromFloat(1.0f / 30.0f) || !orders.empty()); }
    // Actually translating (for the walk animation), vs standing with an
    // attack/queued order.
    bool walking() const { return alive() && speed > Fixed::fromFloat(3.0f / 30.0f); }
    // Total Annihilation economy books (sim/taeconomy.h) and construction state:
    // buildRemaining is retail's remaining fraction (1 at placement, 0 complete);
    // it, not HP, decides completion. Hashed.
    econ::UnitBooks books;
    float buildRemaining = 0;
    bool buildStarted = false;
    // TotalA.exe VTOL_Standby's post (mission +0x2e / +0x30, set at stage 0,
    // 0x40f9ed..0x40f9fc): the integer position a passenger-carrying flyer
    // loiters round (0x40f839..0x40f853). Hashed while it carries one.
    int16_t standbyPostX = 0, standbyPostZ = 0;
    // The pad piece a flyer sits on (unit +0xf9, 0x48ab70 writes it on every
    // attach): set while it rides an air base (inTransport names one, its
    // type isairbase), -1 otherwise. VTOL_Landing's free-pad test (0x47e570)
    // reads it on the pad's other passengers. Hashed while it rides one.
    int8_t padPiece = -1;
};
// Which line each hot field sits on (WE D1); see the layout note above Unit.
// Moving a field across a line, or growing lines 0-4 past their bytes, is a
// compile error here.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
#define TA_UNIT_LINE(field, line) \
    static_assert(offsetof(Unit, field) / 64 == (line) && \
                  (offsetof(Unit, field) + sizeof(Unit::field) - 1) / 64 == (line), \
                  "Unit::" #field " must sit on cache line " #line)
TA_UNIT_LINE(id, 0); TA_UNIT_LINE(player, 0); TA_UNIT_LINE(type, 0);
TA_UNIT_LINE(x, 0); TA_UNIT_LINE(z, 0); TA_UNIT_LINE(deadFor, 0); TA_UNIT_LINE(inTransport, 0);
TA_UNIT_LINE(hp, 0); TA_UNIT_LINE(speed, 0); TA_UNIT_LINE(heading, 0); TA_UNIT_LINE(weaponSlot, 0);
TA_UNIT_LINE(underConstruction, 0); TA_UNIT_LINE(cloaked, 0); TA_UNIT_LINE(beingBuilt, 0);
TA_UNIT_LINE(squad, 0); TA_UNIT_LINE(flightGroundMode, 0); TA_UNIT_LINE(legionLift, 0);
TA_UNIT_LINE(legionStill, 0); TA_UNIT_LINE(standingOrder, 0); TA_UNIT_LINE(moveState, 0);
TA_UNIT_LINE(fireState, 0); TA_UNIT_LINE(active, 0); TA_UNIT_LINE(standbyActive, 0);
TA_UNIT_LINE(guardNoMoveActive, 0); TA_UNIT_LINE(constructionHolding, 0); TA_UNIT_LINE(justFired, 0);
TA_UNIT_LINE(weaponAuto, 0);
TA_UNIT_LINE(retailSite, 1); TA_UNIT_LINE(retailBuild, 1); TA_UNIT_LINE(constructionEmitter, 1);
TA_UNIT_LINE(cosmeticConstructionEmitter, 1); TA_UNIT_LINE(landing, 1);
TA_UNIT_LINE(frozenFor, 1); TA_UNIT_LINE(stonedFor, 1); TA_UNIT_LINE(paralyzedFor, 1);
TA_UNIT_LINE(selfDestructT, 1); TA_UNIT_LINE(turnReqBam, 1); TA_UNIT_LINE(animationTurnBam, 1);
TA_UNIT_LINE(tickStartHeadingBam, 1);
TA_UNIT_LINE(orders, 2); TA_UNIT_LINE(buildQueue, 2); TA_UNIT_LINE(repeatType, 2);
TA_UNIT_LINE(buildSiteId, 2); TA_UNIT_LINE(reclaimId, 2);
TA_UNIT_LINE(repairId, 3); TA_UNIT_LINE(productionSiteId, 3); TA_UNIT_LINE(buildProgress, 3);
TA_UNIT_LINE(justBuilt, 3); TA_UNIT_LINE(conjureHoverTarget, 3); TA_UNIT_LINE(reloads, 3);
TA_UNIT_LINE(firedWeapons, 3); TA_UNIT_LINE(fireAnimations, 3); TA_UNIT_LINE(homeX, 3);
TA_UNIT_LINE(homeZ, 3); TA_UNIT_LINE(routeStamp, 3); TA_UNIT_LINE(routeCrowded, 3);
TA_UNIT_LINE(routeTraffic, 3); TA_UNIT_LINE(routeFailed, 3); TA_UNIT_LINE(routeDetour, 3);
TA_UNIT_LINE(groundTerrainFlags, 3); TA_UNIT_LINE(groundMovementMode, 3);
TA_UNIT_LINE(groundSpeedMode, 3); TA_UNIT_LINE(scriptAimTarget, 3);
TA_UNIT_LINE(pendingWeaponAnimations, 4);
TA_UNIT_LINE(flightY, 6); TA_UNIT_LINE(groundY, 6); TA_UNIT_LINE(sightFootprint, 6);
TA_UNIT_LINE(baseSpeed, 6); TA_UNIT_LINE(groundMoveTick, 6); TA_UNIT_LINE(groundScanTick, 6);
TA_UNIT_LINE(groundGradeTick, 6); TA_UNIT_LINE(repathLeft, 6); TA_UNIT_LINE(stuckFor, 6);
TA_UNIT_LINE(stuckX, 6); TA_UNIT_LINE(stuckZ, 6); TA_UNIT_LINE(bodyBlockStreak, 6);
TA_UNIT_LINE(groundPitch, 6); TA_UNIT_LINE(groundRoll, 6); TA_UNIT_LINE(legionWindow, 6);
TA_UNIT_LINE(legionShuffle, 6); TA_UNIT_LINE(variationPhase, 6);
static_assert(offsetof(Unit, weaponAnimations) / 64 == 4 &&
              (offsetof(Unit, weaponAnimations) + offsetof(ta::RetailWeaponAnimations, events) - 1) / 64 == 4,
              "Unit::weaponAnimations' count and overflow (what the per-tick clear touches) must sit on line 4");
// World::tick's unit loop prefetches lines 0-8 of every unit ahead and lines
// 9-11 only for flyers and the dead: those must hold what they read.
inline constexpr size_t kUnitPrefetchHot=9*64;
#define TA_UNIT_LINES(field, first, last) \
    static_assert(offsetof(Unit, field) / 64 >= (first) && \
                  (offsetof(Unit, field) + sizeof(Unit::field) - 1) / 64 <= (last), \
                  "Unit::" #field " must sit on cache lines " #first "-" #last)
TA_UNIT_LINE(standbyState, 8); TA_UNIT_LINE(guardNoMoveState, 8); TA_UNIT_LINE(standbyAllowed, 8);
TA_UNIT_LINES(deathAnimationTicks, 8, 11); TA_UNIT_LINES(corpseUntil, 8, 11); TA_UNIT_LINES(corpseStatue, 8, 11);
TA_UNIT_LINES(corpseFading, 8, 11); TA_UNIT_LINES(airOccupant, 9, 11);
TA_UNIT_LINES(flightVelocity, 9, 11);
TA_UNIT_LINES(flightAcceleration, 9, 11); TA_UNIT_LINES(flightNavigation, 9, 11);
TA_UNIT_LINES(retainedFlightGoal, 9, 11); TA_UNIT_LINES(flightSectorZ, 9, 11);
TA_UNIT_LINES(legionLiftUntil, 9, 11); TA_UNIT_LINES(legionLiftEnded, 9, 11);
#undef TA_UNIT_LINES
#undef TA_UNIT_LINE

// The ids of the units of a few rare classes, each in units_ order, so a
// pass that wants only those walks them alone, in the order the all-unit
// walk visited them (Legion W1 A5; World's prologue and aura passes, WE
// E2.5). A unit's type never changes; spawn only appends to units_, and
// compaction (the only removal) keeps the order, so the lists follow units_
// by reading the units appended since the last sync, and are rebuilt
// whenever the entry they ended at moved (a compaction) or the caller says
// units_ was reordered. They keep dead units until then: every walk tests
// alive() as before. Never hashed.
struct UnitClassLists {
    std::vector<int> flyers, structures, gates, emitters;   // canFly, isStructure, gate, auras
    size_t listed = 0; int tail = 0;
    void sync(const std::vector<Unit>& units, bool reordered = false) {
        size_t from = listed;
        if (reordered || from > units.size() || (from && units[from-1].id != tail)) {
            from = 0; flyers.clear(); structures.clear(); gates.clear(); emitters.clear();
        }
        for (size_t i = from; i < units.size(); ++i) {
            const Unit& u = units[i];
            if (!u.type) continue;
            if (u.type->canFly) flyers.push_back(u.id);
            if (u.type->isStructure()) structures.push_back(u.id);
            if (u.type->gate) gates.push_back(u.id);
        }
        listed = units.size(); tail = units.empty() ? 0 : units.back().id;
    }
    bool operator==(const UnitClassLists& o) const {
        return flyers == o.flyers && structures == o.structures && gates == o.gates && emitters == o.emitters;
    }
};
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// A reclaimable map feature (tree, rock, house, wreckage, …). Positions and stats
// are derived deterministically in setupMatch -- the same row-major walk runs on
// every peer -- so reclaim stays in lockstep. Not a Unit: no orders or combat.
// A builder reclaims it in a fixed time set by its `energy` (Reclaim 406250,
// see sim/retailreclaim.h) and gains no mana for it. A `blocks` feature also
// occupies the nav grid, freed when it is reclaimed.
// Per-feature-type burn/footprint data the sim needs (built by setupMatch from
// the feature TDFs, identical on every peer -- indices are shared lockstep state).
struct FeatType {
    std::string name;        // lowercase TDF section name (client art lookup)
    bool flamable = false;   // TDF flamable=1: can be ignited / spread to
    bool hasBurnAnim = false;// TDF seqnameburn present (retail StartBurning requires it)
    uint32_t burnTicks = 1; // full-quality authored burn/overlay lifetime
    // TDF seqnamedie's length in ticks (each frame max(1, delay), 5373d0): how
    // long a feature destroyed by damage stays before featuredead replaces it.
    // 0 = no seqnamedie, replaced at once (494ce0 -> 494ee3).
    uint32_t dieTicks = 0;
    int  spreadChance = 0;   // TDF spreadchance (percent)
    int  sparkTicks = 0;     // TDF sparktime * 30 (retail stores seconds*30)
    int  burntType = -1;     // TDF featureburnt -> index into the same table
    float energy = 0;        // TDF energy: the reclaim countdown (readFloat in retail too)
    float metal = 0;         // TDF metal (int & 0xffff): metal paid on reclaim; metal patches
    int  fx = 1, fz = 1;
    bool blocking = false;
    // Corpse defs (features/corpses/*_dead.tdf): how long the body lies there
    // and whether a priest can raise it (retail decomposetime / resurrectable).
    int  decomposeTicks = 0; // TDF decomposetime * 30 (0 = never rots)
    bool resurrectable = false;
    // TDF animatable=1: a cananimate caster may raise it as its animatetype.
    // Retail's feature flag 0x1000 (loader 0x494223; tested by 0x496fd0), apart
    // from resurrectable's 0x800 (0x497100). Every shipped animatable corpse is
    // also resurrectable, not the other way round.
    bool animatable = false;
    bool reclaimable = false;
    // TDF autoreclaimable (feature flags +0xfe bit 8, default 1; 0x422baf):
    // VTOL_RepairPatrol's feature look (0x47ea40) takes only these.
    bool autoReclaimable = true;
    bool isStone = false;    // TDF isstone=1 (statue: client tints it stone-gray)
    bool isFrozen = false;   // TDF isfrozen=1 (client tints it ice-blue)
    bool indestructible = false;
    int32_t hp = 0;          // TDF damage= -- readInt in retail, so an int here too
    int  deadType = -1;      // TDF featuredead -> destroyed-replacement (placed neutral)
    uint8_t projectileHeight = 0; // Native feature collision height.
    std::string object;      // TDF object= (3D corpse mesh; client visual)
    bool treeOrRock = false; // see RetailMapFeatureType::treeOrRock
    // The unit a RESURRECT raises from this feature: TotalA.exe 0x404f36 cuts
    // the feature's name at its first '_' and looks that up as a unit type
    // (0x488b10; "cornecro_dead" and "cornecro_heap" both raise cornecro).
    // Null when no unit has that name. Set by setupMatch / registerMapFeatures.
    const UnitType* raises = nullptr;
};

// What a caster does with a body, retail's raise resolver (0x4de530 modes 1
// and 12, and 0x4068b0 stage 0): resurrect when its type has canresurrect and
// the body is resurrectable (0x497100), else animate when it has cananimate
// (and an animatetype) and the body is animatable (0x496fd0). 0 = neither, and
// a reclaimer reclaims it instead. There is no owner test: a caster raises a
// body of any side, as its own unit.
// TA (TotalA.exe): the RESURRECT order takes any reclaimable feature
// (0x440043: feature flag 0x80, `reclaimable`), and the mission raises the
// unit its name names (FeatType::raises; none: "Ressurection failed",
// 0x404f6f) -- there is no resurrectable key in TA's data.
inline uint8_t reviveKindFor(const UnitType& caster, const FeatType& corpse) {
    if (caster.canResurrect && (corpse.resurrectable || (corpse.reclaimable && corpse.raises))) return 1;
    if (caster.canAnimate && caster.animateType && corpse.animatable) return 2;
    return 0;
}


struct Feature {
    int   id = 0;          // cz*terrainWidth + cx: position-derived, peer-identical
    Fixed x = Fixed(), z = Fixed();   // 16.16, the same world units as everything else
    int   fx = 1, fz = 1;  // footprint cells (for the nav unblock on removal)
    // TDF energy (type+0x128), which alone sets how long a reclaim takes
    // (retailReclaimCountdown). Retail pays no mana for it.
    float energy = 0;
    bool  blocks = false;  // occupied the nav grid
    bool  alive = true;    // false once fully reclaimed (decal disappears)
    // Reclaimed, fading out (494ce0's object: 34 ticks). It stays installed and
    // blocking until this reaches 0, but can no longer be reclaimed. Hashed.
    uint8_t fadeLeft = 0;
    // Burning (retail mechanic, icd 0x494b40/0x495110/0x495300 -- ours runs
    // fully deterministic in the lockstep sim instead of retail's host-authority
    // event scheme). Active/timer state is hashed; display timestamp/order is not.
    int   type = -1;       // index into World's FeatType table (-1 = untyped)
    uint8_t burn = 0;      // 1 = burning
    uint32_t burnStarted = 0; // cosmetic ignition timestamp; excluded from stateHash
    uint64_t burnSequence = 0; // cosmetic activation order; excluded from stateHash
    // INT, because both sides of the comparison are: weapon damage is readInt in
    // retail and so is a feature's `damage` (its hit points).
    int32_t dmg = 0;       // accumulated weapon damage (dies at FeatType.hp)
    int   spreadIn = 0;    // ticks until the single spread event (sparktime-derived)
    uint32_t burnLeft = 0;    // ticks until burn-out (swap to burntType / die)
    // Destroyed by damage, playing seqnamedie (494ce0 mode 0): ticks until its
    // featuredead stage replaces it. Meanwhile it keeps its cells and blocking
    // but takes no damage, cannot catch fire and is not a reclaim target.
    // Hashed.
    uint32_t dieLeft = 0;
};

struct Projectile {
    // Ordinary LOS shots retain their actual 3D fixed-point trajectory.
    std::array<int32_t,3> position{},velocity{};
    std::array<uint16_t,3> angles{};
    uint32_t start=0,end=0,substeps=1;
    int slot=0;
    bool straight=false;
    bool guided3d=false;    // GuidedWeapon's simulation-owned XYZ state.
    bool ballistic3d=false; // BallisticWeapon XYZ motion and collision, also used to pose the authored shot.
    bool projectileUsesVeteranModel=false; // model variant selected once at launch by source rank.
    // TotalA.exe selfprop/vlaunch motion (0x49b9c2..0x49bb5b, retailtashot.h):
    // also guided3d for drawing. `end` is the expiry tick (+0x46): powered
    // while end > now, then burnblow detonates or gravity takes over.
    bool taSelfProp=false;
    uint8_t taPhase=0;          // +0x69 bits 4..5: twophase stage
    uint16_t taHeading=0,taPitch=0;   // +0x36 / +0x38, mover convention
    int32_t taSpeed=0;          // +0x3a
    std::array<int32_t,3> taTargetPoint{}; // +0x28
    std::array<int32_t,3> muzzle{};
    std::optional<RetailLightningEffect> lightningEffect;
    // Position in the same 16.16 as a unit's: retail's world is 0x100000 per 16px
    // cell, i.e. 65536 per pixel (docs/retail-engine.md), and a projectile is bounded
    // by the map exactly as a unit is, so the range that rules mana out does not bite.
    // (On a 2048-cell map the east and south edges ARE the end of that range: x and z
    // stop there, as every Fixed sum does; the 3D `position` of a shot that flies on
    // past them wraps as retail's does, which reads as off the map and retires it.)
    Fixed x = Fixed(), z = Fixed();
    // PER TICK, 16.16 -- retail stores weapon velocity as a fixed-point integer and
    // renders it in px/s by scaling 30/65536 (the dump at 0x4fbf2a..0x4fbf4f:
    // imul of the two int components, fildll, then * 0x5f2d68 == 30/65536).
    Fixed vx = Fixed(), vz = Fixed();
    int targetId = 0;
    int fromPlayer = 0;
    // TICKS, like every other retail timer.
    int32_t life = 0;      // ticks left before it fizzles
    int32_t age = 0;       // ticks since launch
    int32_t flight = 1;    // expected ticks to target (for the render arc)
    // Set the moment a shot registers its direct hit. `life = -1` doubles as the
    // erase predicate, and `life -= dt` runs BEFORE the hit test, so on the final
    // tick a projectile can both connect and expire -- without this flag the
    // expiry-detonation below would crater on top of a hit that already landed.
    bool spent = false;
    int fromId = 0;        // firing unit id (for kill attribution / veterancy)
    const Weapon* wsrc = nullptr;   // source weapon (splash + per-category damage)
    WeaponFx fx = WeaponFx::Arrow;   // how the viewer draws it
};

struct FlameShot {
    const Weapon* weapon=nullptr;
    int owner=0,fromId=0,slot=0;
    int targetId=0;        // the unit it was fired at, for the blocked-shot count
    std::array<int32_t,3> position{},velocity{},endpoint{};
    std::array<int32_t,3> muzzle{},aimPoint{}; // display-only native admission endpoints
    uint32_t start=0,end=0,lifetime=1,speed=0,substeps=1;
    bool impacted=false,expired=false;
    std::vector<ta::RetailFlameParticle> particles;
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
        int badSlope=-1,badWaterSlope=-1;
        int badMaxWaterDepth=32768,badMinWaterDepth=32768;
    };
    NavGrid(const std::vector<uint8_t>& heights, int w, int h, int sea, const Limits& lim);

    // Terrain-only walkability: ignores the shared obstacle overlay. Used to tell a
    // cell blocked by a clearable doodad apart from one blocked by the ground itself.
    bool terrainWalkable(int cx, int cz) const {
        if (cx < 0 || cz < 0 || cx >= w_ || cz >= h_) return false;
        return cells_[size_t(cz) * size_t(w_) + size_t(cx)] != 0;
    }
#ifndef NDEBUG
    // Debug-only: checksum of this grid's own cells, for locating a nav divergence.
    uint64_t debugCellsHash() const {
        uint64_t h = 1469598103934665603ULL;
        for (uint8_t c : cells_) { h ^= c; h *= 1099511628211ULL; }
        return h;
    }
#endif
    bool walkable(int cx, int cz) const {
        if (cx < 0 || cz < 0 || cx >= w_ || cz >= h_) return false;
        size_t i = size_t(cz) * size_t(w_) + size_t(cx);
        if (obst_ && (*obst_)[i]) return false;   // shared obstacle overlay
        return cells_[i] != 0;
    }
    int terrainGrade(int cx,int cz,bool includeObstacles=true) const {
        if (!(includeObstacles ? walkable(cx,cz) : terrainWalkable(cx,cz))) return 0;
        const unsigned penalty=terrainPenalty_.empty() ? 0 : terrainPenalty_[size_t(cz)*w_+cx];
        if (penalty&2) return 4;
        return roadAt(cx,cz) ? 7 : (penalty&1) ? 4 : 6;
    }
    int liveTerrainGrade(int cx,int cz,bool includeObstacles=true) const {
        if (!(includeObstacles ? walkable(cx,cz) : terrainWalkable(cx,cz))) return -1;
        const unsigned penalty=terrainPenalty_.empty() ? 0 : terrainPenalty_[size_t(cz)*w_+cx];
        // 508232..5082bd: live placement keeps the slope penalty on roads.
        if (penalty&3) return 4;
        return roadAt(cx,cz) ? 7 : 6;
    }
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
    // Force the lazy clearance grid up to date NOW, so a caller that will only READ
    // the grid afterwards cannot trip fits() into rebuilding the mutable cache
    // underneath it. components() calls this before its flood fill; it was added for
    // the flow-field prefetch, which handed the grid to worker threads and is gone.
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
    // A cell in the SHARED obstacle overlay changed. Every grid reads that overlay
    // through walkable(), so each one's clearance needs refreshing -- but only over
    // the band the edit can reach, exactly as block() does for a grid's own cells.
    // World::blockCells used to markClearanceDirty() here instead, which made every
    // building placement re-run a full w*h clearance DP on EVERY grid. The win is
    // asymptotic (bounded rect vs whole map) rather than one you can see in a
    // profile today -- the sim is far from the frame budget at -O2.
    void overlayChanged(int cx, int cz, int w, int h);
    // Bumped on every walkability edit (own cells or the shared overlay), so caches
    // derived from this grid can tell when they have gone stale.
    uint64_t version() const { return version_; }
    void setRoads(const std::vector<uint8_t>* roads) {
        roads_ = (roads && roads->size() == size_t(w_) * size_t(h_)) ? roads : nullptr;
    }
    // After a World copy: the same overlays, at the new world's addresses. Unlike
    // setObstacles this keeps the clearance grid, which a copy holds up to date.
    void rebindOverlays(const std::vector<uint8_t>* obst, const std::vector<uint8_t>* roads) {
        if (obst_) obst_ = obst;
        if (roads_) roads_ = roads;
    }
    bool roadAt(int cx, int cz) const {
        return roads_ && (*roads_)[size_t(cz) * w_ + cx] != 0;
    }
    // What a copy of this grid may still have to copy for itself: the clearances it
    // shares, which the next walkability edit rewrites (World::copyOnWriteBytes).
    uint64_t copyOnWriteBytes() const { return clear_.sharedBytes(); }

    // Line of sight: no blocked cell between the two world points, ignoring cells
    // within `skip0`/`skip1` cells of each endpoint — so a shooter or target's own
    // building footprint doesn't block the shot, but a wall between them does.
    bool losBetween(float wx0, float wz0, float wx1, float wz1,
                    int skip0 = 0, int skip1 = 0) const;

    // Can a `foot`-cell body travel the straight line between two WORLD points?
    //
    // Unlike losBetween, which converts its world endpoints to cells immediately
    // and then walks cell centres, this keeps the sub-cell position of both endpoints
    // and visits EVERY cell the real segment passes through. A cell-centred walk
    // follows a different line from the one the unit actually travels, and it can
    // skip a cell the body will cross.
    // (15,1) -> (24,56) passes through cell (1,0); the cell-to-cell Bresenham between the
    // same endpoints' cells never visits it, so an obstacle sitting there was invisible to
    // validation and the unit was handed a shortcut straight into it.
    //
    // Exact and integer: a supercover DDA in world units with cross-multiplied
    // comparisons, so it makes the same decisions bit-for-bit on every peer.
    bool segmentFits(float wx0, float wz0, float wx1, float wz1, int foot) const;

private:
    bool lineClear(int x0, int z0, int x1, int z1) const;
    void rebuildClearance() const;

    // The terrain's own cells: written by the constructor and by block() alone, so a
    // world and its copies (World::clone) share them, as they do the clearances until
    // a structure or a feature changes one (SharedArray, sim/copymember.h). A keyframe
    // of the world holds the clearances as runs of its own instead.
    SharedArray<uint8_t> cells_;
    SharedArray<uint8_t> terrainPenalty_;
    // clear_[c] = side of the largest all-walkable square whose min corner is c.
    // Lazily rebuilt (dirtied by block()); a foot-cell unit fits at corner c iff
    // clear_[c] >= foot. mutable so fits()/pathfinding can build it on demand.
    // Values saturate at 15 (the max queryable footprint), which is what lets
    // block() refresh only a bounded rect instead of dirtying the whole grid.
    void updateClearanceRect(int cx, int cz, int w, int h) const;
    mutable SharedPackedArray<uint16_t> clear_;
    mutable bool clearDirty_ = true;
    uint64_t version_ = 1;
    const std::vector<uint8_t>* roads_ = nullptr;
    const std::vector<uint8_t>* obst_ = nullptr;   // shared obstacle overlay   // see setRoads
    int w_ = 0, h_ = 0;
};

// Block/unblock a building's yardmap-aware footprint on a nav grid.
void blockFootprint(NavGrid& nav, const UnitType& t, float x, float z, bool blocked);

struct Player {
    // Accounting for the HUD and the server-side skirmish AI's mana-pressure test
    // (src/ai): never read by the simulation itself or folded into stateHash().
    // Only the server's AI reads it, from its own World, and it is built from
    // hashed sim arithmetic alone, so it is the same in every run of a seed.
    RetailResourceHistory displayResources;
    bool automaticGates=false; // normal AI capability; human gates remain manually controlled
    bool defensiveAi = false; // Passive AI defends in place from the moment units spawn
    // A campaign mission's brainless Player%i (villagers, wildlife, props: the
    // .ota's "passive" control word, or no "strategic"), whom missions put on the
    // human's team. Shares no sight and no explored cells with anyone, even when
    // allied (World::viewShareMask): retail makes every mission computer a kind-2
    // owner (4d298f -> 4f6290 -> 4f5940 stores lobby +0x84 = 2), which 4f6770
    // pairs with nobody; outside campaigns the owner's rule is the same. Hashed.
    bool passiveNeutral = false;
    RetailPlayerCacheClock cacheClock;
    std::optional<RetailBuildCache> buildCache;
    std::optional<RetailConstructionResources> retailResources;
    void creditMana(double amount) {
        displayResources.income=float(double(displayResources.income)+amount);
        mana+=amount;
        if (retailResources) {
            auto& r=*retailResources;
            mana=r.stored=float(mana);
            r.produced=float(double(r.produced)+amount);r.totalProduced+=amount;
        }
        if (buildCache) buildCache->resources.income=float(double(buildCache->resources.income)+amount);
    }
    // Records a consumer's full-rate request: retail's resource object +0x10,
    // which the HUD's -N and the AIs' shortfall test both average (401350).
    // debitMana requests exactly what it pays; construction work requests
    // through constructionShare and pays its share with payMana.
    void requestMana(double amount) {
        displayResources.usage=float(double(displayResources.usage)+amount);
        if (retailResources) retailResources->requested=float(double(retailResources->requested)+amount);
        if (buildCache) buildCache->resources.usage=float(double(buildCache->resources.usage)+amount);
    }
    void debitMana(double amount) {
        requestMana(amount);
        payMana(amount);
    }
    void payMana(double amount) {
        mana-=amount;
        if (retailResources) { auto& r=*retailResources; mana=r.stored=float(mana); }
    }
    // Restored retail AI is separate from the command-emitting skirmish AI.
    // Unsupported planner branches stop diagnostic replay explicitly.
    std::optional<RetailAiState> retailAi;
    // Legacy World economy uses a double pool. Retail's resource object stores
    // the current pool as float; only produced/excess lifetime totals are double.
    // RetailConstructionResources preserves that arithmetic for the pending
    // economy/construction integration.
    double mana = 500;
    // Construction work shares the pool, as retail does (51d837 sets allocation =
    // stored / demand; 429af0 scales every worker's work by it and pro-rates the
    // last one against what is left; see RetailConstructionResources). Without it
    // each consumer paid its whole per-tick cost or did nothing, in unit order, so
    // cheap low-id producers took every trickle and a costly later one could sit at
    // zero progress for minutes while mana income was short.
    //   constructionDemand: full-rate mana asked for by construction, repair and
    //     production work this tick (retail records the request before scaling).
    //     It is the demand the next tick's allocation divides by. Hashed when set.
    //   constructionAllocation: this tick's share, recomputed after income.
    // Unused for a player with retailResources, which runs the native object.
    double constructionDemand = 0;
    double constructionAllocation = 1;
    // Retail's resource object +8: the fraction of full-rate work this tick's
    // pool affords (stored / last demand, capped at 1). A float, as retail
    // stores it.
    float allocation() const {
        return retailResources ? retailResources->allocation : float(constructionAllocation);
    }
    // The fraction of `cost` (one tick of full-rate work) this consumer may do
    // now. Records the request; the caller pays cost * share, capped at mana,
    // with payMana (the request is already counted).
    double constructionShare(double cost) {
        if (cost <= 0) return mana >= cost ? 1.0 : 0.0;
        requestMana(cost);
        if (!retailResources) constructionDemand += cost;
        if (mana <= 0) return 0.0;
        return std::min(constructionAllocation, mana / cost);
    }
    float scenarioResourceLimit = 0; // map scenario action 16: caps both TA stores (taSettle)
    float storage = 0;   // recomputed each tick from alive units
    float income = 0;
    float manaCapacity() const {
        return retailResources ? retailResources->capacity :
            (scenarioResourceLimit > 0 ? scenarioResourceLimit : std::max(storage,100.0f));
    }
    // Income multiplier (1.0 = normal). Only ever != 1 for an Absurd-difficulty AI,
    // set once at match setup and applied to every mana source. Constant per game and
    // its effect lands in `mana` (which IS hashed), so it need not be hashed itself,
    // but every peer must set it identically or their mana diverges.
    float manaMult = 1.0f;
    // Total Annihilation metal/energy (sim/taeconomy.h). Hashed.
    econ::PlayerBooks books;
    int32_t score = 0;   // authored kill points; GET40 reads this in campaign scripts
    int   kills = 0;     // enemy units this player has destroyed (F4 overlay)
    // Damage events this player took part in, as attacker or victim (display only,
    // not hashed): the battle-music monitor's +1 input (TotalA.exe 0x489de6).
    uint32_t combatEvents = 0;
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
    uint8_t manaShareMask = 0xff; // outgoing overflow recipients; alliance still required
    // Fire-at-will sweep cursor (controller +0x1a1, icd 40f330): the unit slot
    // the last budget-limited pass stopped on. INT32_MAX is the constructor's
    // "last slot of the pool" (40fed0). Hashed.
    int32_t sweepCursor = INT32_MAX;
    // 409a8a: an AI Monarch hit sets the controller's threat deadline (ctrl
    // +0xd = now+30+3 x rand(300)) that the unported AI planner 40c890 reads.
    // Hashed.
    uint32_t aiThreatUntil = 0;
    // 411700's per-player unit lists, rebuilt on the 7-tick bookkeeping clock
    // (cacheClock; visibleRefresh stands in when that clock is disabled):
    // list +5, the hostile units this player saw (4f7210) at the last rebuild,
    // and list +0x15, the visible allied units, own units included. Unit ids in
    // units_ order. Callers re-check liveness and distance, never visibility,
    // so a target stays a candidate up to 6 ticks after it leaves sight.
    // Hashed.
    std::vector<int> visibleEnemies, visibleAllies;
    uint32_t visibleRefresh = 0;
    bool  defeated = false;   // no living units; set by the sim's win check
    int32_t defeatedAt = -1;  // TICK when `defeated` first went true (-1 = still in)
    // Cosmetic emotes: sequenced in lockstep, excluded from stateHash.
    // Targets are captured at command time; subsequent selection changes do not
    // move an active emote to other units.
    uint64_t emoteSequence = 0;
    bool emoteDisco = false;
    std::unordered_set<int> emoteUnits;

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
    // Offline diagnostic importer; not a supported game-save load interface.
    friend struct RetailReplayProbe;
    friend class LegionNavigator;
public:
    // `finished` is 512060's sixth argument: a placement, a game start, Create,
    // the CRT and a god are made finished; production and a builder's site are
    // not (they finish through activateBuilt at completion).
    int spawn(const UnitType* type, float x, float z, std::optional<float> heading = {}, int player = 0,
              bool finished = true);
    // 512301 (a finished creation) and 429a8d (a completion): an
    // activatewhenbuilt type is activated, 51e4d0(1, 1), whose rising edge
    // starts the script's Activate. A new unit's activation byte is 0
    // (511990), so this is the only thing that turns one on unasked.
    // `hold` is off for a finished creation, whose Create has not run yet and
    // runs first in Activate's own zero-elapsed pass (switchScript).
    void activateBuilt(int unitId,bool hold=true);
    // An activation edge (51e4d0): sets the script's activation flag and starts
    // Activate or Deactivate. Owner decision (0.9.20, not retail-proven): an
    // Activate waits while the unit's Create still runs (cob::createHold). A
    // factory's Create sleeps until its build is done and only then runs
    // InitState; production queued on the tick the Navy Yard finished started
    // Activate first, its RequestState found the yard "already on" and it never
    // opened. The held Activate starts on the tick Create returns
    // (tickUnitScript); a Deactivate meanwhile cancels it and starts nothing.
    // `hold` false starts it at once (a finished creation).
    void switchScript(Unit& u,bool on,bool hold=true);
    // The activation byte's bit 1 (+0x114 & 2): the script's activation,
    // or the unit's own flag when it has no script.
    bool retailActivated(const Unit& u) const {
        const auto* script=unitScript(u.id);
        return script ? script->activated : u.active;
    }
    // Whether the unit's script has been activated (its +0x114 bit 1); tests.
    bool unitScriptActivated(int unitId) const {
        const auto* script=unitScript(unitId);
        return script && script->activated;
    }
    // A creation that credits its storage (512060's last argument): the unit's
    // mogriumstorage joins its owner's pool and capacity at once.
    void creditCreationStorage(int unitId);
    // A credited kill's bounty (512c6b..512c99): the victim type's
    // mogriumbounty to the killer player's pool and lifetime total, uncapped.
    void creditKillBounty(int player, float bounty);
    // A finished creation or completion of an isfeature type (the walls):
    // it dies with kind 9 and leaves its feature.
    void finishAsFeature(int unitId);
    // A finished unit stands here, or a finished wall became its feature here
    // (harness checks of what a build order left behind).
    bool finishedHere(const Unit& u) const {
        return u.alive() ? !u.underConstruction :
            u.deathType==Unit::kDeathBecomeFeature && corpseDamage(u.id)>=0;
    }
    // CRT ownership transfer: preserves HP/progress, clears the former owner's commands.
    bool scenarioTransfer(int unitId, int newPlayer);
    // The same, telling `notify` (the running mission, or none) of the capture.
    bool scenarioTransfer(int unitId, int newPlayer, MissionScript* notify);
    // Dev harness and tools: give a unit an authored veteran level as placement
    // does (setLevel 519370: experience for the level, then the level from it).
    void setVeteranLevel(Unit& u, int level);
    int scenarioCreate(const UnitType* type,int player,int lx,int lz,int hx,int hz);
    void scenarioDestroy(int unitId,uint8_t deathType=0);
    // The mission Kill verb (513130): every unit dies, its shots gone.
    void missionKillAll();
#ifndef NDEBUG
    // Dev harness: hit `targetId` with `w` through the combat damage path
    // (applyHit), so the death edge sees authored overkill and damage type.
    void debugApplyHit(const Weapon& w,int targetId,int fromPlayer) {
        if (auto* target=unit(targetId))
            applyHit(w,target->x.toFloat(),target->z.toFloat(),fromPlayer,0,target);
    }
#endif
    // The CRT's Heal / Damage actions (4cb131 / 4cb1e3): 51a140(0, unit,
    // amount, 12 / 13, 0) for each of the player's units in the region.
    void scenarioAdjustHealth(int unitId,int32_t amount,bool heal);
    // Benchmark: install the spawn ramp (see BenchStage; a stage's tick is nominal only).
    // Its stages fire in order as Cmd::BenchClock says they are due, and the same command
    // ends the run; benchmarkMode() is true once a plan is installed.
    void setBenchmarkPlan(std::vector<BenchStage> plan) {
        benchStages_ = std::move(plan); benchDue_ = 0; benchFired_ = 0; benchmark_ = true; benchEnded_ = false;
    }
    // Cmd::BenchClock: `due` stages are due by the server's wall clock (never goes back);
    // `ended` = the run's 60 real seconds are over.
    void benchmarkClock(uint32_t due, bool ended) {
        if (!benchmark_) return;
        benchDue_ = std::max(benchDue_, std::min<uint32_t>(due, uint32_t(benchStages_.size())));
        if (ended) benchEnded_ = true;
    }
    // Stress fill: a staged executor by tick, without making this a benchmark run
    // (Monarchs stay mortal).
    void setStagedSpawns(std::vector<BenchStage> plan) {
        size_t n = 0;
        for (const auto& stage : plan) n += stage.units.size();
        // No reallocation of the 1.3 KB records during the fill: copying ~15k of them is a
        // 6 ms tick. The slack covers what the players produce meanwhile, which otherwise
        // overflows an exact reservation on the last stage. Capacity only, never hashed.
        units_.reserve(units_.size() + n + n / 4);
        benchPlan_ = std::move(plan); benchCursor_ = 0;
    }
    bool benchmarkMode() const { return benchmark_; }
    bool benchmarkEnded() const { return benchEnded_; }
    uint32_t tickCount() const { return tickCounter_; }   // ticks elapsed (benchmark timing)
    // Situation tools only (client/situation.h, tools/legion_scn.h): the game RNG state, and a way to start a rebuilt
    // world on the clocks of the recording it was cut from. Per-tick cadences key on the tick counter and every dice
    // roll on the RNG, so a situation that starts at tick 0 with a fresh seed walks a different dice stream. Never
    // called by the game itself.
    uint32_t gameRngState() const { return gameRng_; }
    void resumeClocks(uint32_t tick, uint32_t gameRng);
    // The id the next spawn takes. A rebuilt situation gives every body its recorded id: schedulers stagger by id, so
    // ids that only keep the recording's order (1, 2, 3 ...) run their rechecks on other ticks.
    void setNextUnitId(int id) { nextId_ = id; }
    // Build per-domain nav grids from heights + sea level. `features` is the
    // map's raw per-cell feature plane if available: retail stores per-cell
    // attributes in it as special values -- 0xFFFB marks ROAD cells (the
    // roadmultiplier speed bonus + the COB walk_road gait).
    void setTerrain(const std::vector<uint8_t>& heights, int w, int h, int seaLevel,
                    const std::vector<uint16_t>* features = nullptr);
    NavGrid& nav() { return nav_; }
    // Observational pathfinder counters (never hashed) -- for benchmarks.
    const PathService& pathStats() const { return paths_; }
    void setPathfindingMode(PathfindingMode mode);
    PathfindingMode pathfindingMode() const { return pathfindingMode_; }
    // Retail mode's native group record for a unit (null outside Retail or ungrouped).
    const RetailGroupRecord* retailGroupRecord(int unitId) const {
        const Unit* u=unit(unitId);return u?retailGroupOf(*u):nullptr;
    }
    // Whether the last group tick paced the unit (its mission's 0x2000000).
    bool retailGroupPaced(int unitId) const {
        const Unit* u=unit(unitId);if(!u)return false;
        const size_t index=size_t(u-units_.data());
        return index<retailGroupPaced_.size() && retailGroupPaced_[index];
    }
    // Legion mode's formation slot for a member of a formation -- an Alt+N
    // formation or a classic AI squad flagged as one (legionFormationGroup) --
    // for the standby re-form (legionStandbyCheck): null when it has none.
    struct LegionFormationSlot {
        int8_t group=0;         // the formation (retailGroupSlot's group, 1..99) the slot was taken in
        uint8_t state=0;        // kSlotHeld, kSlotReturning, kSlotGivenUp
        int32_t x=0,z=0;        // the slot footprint's centre (raw Fixed)
        uint32_t returnTick=0;  // the tick the walk back was ordered (kSlotReturning)
        int32_t arrivalX=0,arrivalZ=0;   // the arrival's goal centre (anchorGoal) the slot was taken at
        int32_t destX=0,destZ=0;         // that move's requested point (arrivalPoint), shared by the formation
    };
    static constexpr uint8_t kSlotHeld=0,kSlotReturning=1,kSlotGivenUp=2;
    const LegionFormationSlot* legionFormationSlot(int unitId) const {
        const auto found=legionFormationSlots_.find(unitId);
        return found==legionFormationSlots_.end()?nullptr:&found->second;
    }
    LegionNavigator::Stats legionStats() const {return legion_?legion_->stats():LegionNavigator::Stats{};}
    LegionNavigator* legionNavigator() {return legion_.get();}
    uint32_t legionSwapTick(int unitId) const {return legion_?legion_->swapTick(unitId):UINT32_MAX;}
    // The unit's own Legion spot on its current move-like leg (LegionNavigator::destinationOf);
    // false in Retail mode or when it has none. Display only, read with the frame capture.
    bool legionDestination(int unitId,Fixed& x,Fixed& z) const {return legion_&&legion_->destinationOf(unitId,x,z);}
    // Legion's open convoys (sim/convoy.h) and their lookup counters. The
    // counters are observation only (never hashed); tools export them beside
    // the navigator's Stats as legion_convoy_*.
    const ConvoyTable& convoys() const {return convoys_;}
    ConvoyTable::Stats convoyStats() const {return convoys_.stats();}


    // Footprint route score: terrain/parked bodies block, qualifying same-way
    // traffic costs extra, ordinary ground is 6 and roads are 7. Step placement
    // separately reserves the entire footprint, even in moving traffic.
    bool unitHoldsCell(const Unit& u) const;
    int cellScore(const UnitType* t, int cx, int cz, int selfId) const;

    // Enable retail's background pathfinder for this world (default off).
    void setPathService(bool on) { pathService_ = on; if (!on) {paths_.clear();legion_.reset();} }
    // Path search work units per tick, shared across all pending requests.
    void setPathBudget(int b) { paths_.setBudget(b); }
    // Bit p: player p is in retail's 5x path-budget class (see pathBudgetClassMask_).
    void setPathBudgetClasses(uint32_t mask) { pathBudgetClassMask_ = mask; }
    void setPathProfiling(bool enabled) { paths_.setProfiling(enabled); }
    void resetPathDiagnostics() { paths_.resetDiagnostics();searchGradeBodyRebuilds_=0; }
    PathService::Diagnostics pathDiagnostics() const {
        auto result=paths_.diagnostics();
        result.bodySnapshotRebuilds=searchGradeBodyRebuilds_;
        result.bodySnapshotBytes=searchGradeBodies_.capacity()*sizeof(RetailGradeBody);
        for (const auto& plane:searchGrades_) result.gradePlaneBytes+=plane.cells.capacity();
        return result;
    }

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
            // A type outside the registry (a modified copy) shares the grid of
            // its movement limits: the mover's cell-entry test (0x43d7ff) reads
            // the type's own limits, and a search on looser legacy limits sent
            // it into cells it is refused, re-asking the same route for ever.
            if (const int index=navClassIndex(*t);index>=0) return navClasses_[size_t(index)];
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
    // Evaluate the same current scripted/fallback output site used by production.
    // Runs QueryBuildInfo, so editor diagnostics must use a private World.
    bool productionPosition(int builderId,const UnitType* type,Fixed& x,Fixed& y,Fixed& z);
    // Queue production of `typeId` at a builder building.
    void train(int builderId, const UnitType* type, int count = 1);   // queue `count`
    void dequeue(int builderId, const UnitType* type, int count);     // un-queue `count`
    void setRepeat(int builderId, const UnitType* type);   // toggle infinite build
    // Mobile builder constructs a building at (x, z). Returns the new
    // building's id, or 0 if the site is invalid.
    // How startBuild gets the builder to the site:
    //   Replace -- the usual fresh order: walk there, dropping whatever it was doing.
    //   None    -- it is already standing in position (a queued build that has just
    //              come due), so issuing an approach would only disturb the queue.
    enum class Approach { Replace, None };
    // `mobileBlocked`, when given, is set when the site is refused only by
    // mobile units standing on it (retail re-runs 507d10 ignoring them).
    int startBuild(int builderId, const UnitType* type, float x, float z,
                   Approach approach = Approach::Replace, bool* mobileBlocked = nullptr);
    // Build now if the builder is free, else queue it (shift-click). A
    // non-queued order replaces any pending queue.
    void queueBuild(int builderId, const UnitType* type, float x, float z, bool queue);
    // Persistent area job: explore and try each mana deposit, without allocating
    // a potentially map-sized list of commands or changing either pathfinder.
    // Abandon a builder's queued builds and drop any not-yet-started site.
    void cancelBuilds(int builderId);
    // Latch a mobile builder onto an existing construction site to resume/assist
    // conjuring it (e.g. reviving a decaying site). Resumes at THIS builder's
    // rate from the site's current HP. The caller checks the build tree.
    void assist(int builderId, int siteId, bool queue = false);
    // Retail HelpBuild eligibility (0x519e60): `site` is alive and unfinished,
    // not `b`, has b's OWNER (allies do not qualify), and b is a builder with a
    // mover whose build menu holds the site's type when it is builderlimited.
    bool canHelpBuild(const Unit& b, const Unit& site) const;
    // A player's production order (Train, RepeatTrain, Build, BuildManaArea:
    // applyCommand asks before anything is queued or cancelled): `b` is a live
    // builder, `type` is on b's own build menu -- the menu the HUD offers, the
    // same whoever owns b; retail's MobileBuild tests it too, 0x519850, and
    // builderlimited narrows only HelpBuild -- and no game restriction forbids
    // it (buildAllowed). An output that is not `placed` (Train, RepeatTrain:
    // tickProduction makes it beside the producer, with no placement) is a
    // mobile type; a structure goes through placement (Build). No shipped factory has
    // a structure on its menu. Mission scripts (orderTrain) and the retail AI's
    // base planner call World::train directly and are not held to the menu;
    // the server's AIs send commands and are.
    bool canProduce(const Unit& b, const UnitType* type, bool placed) const;
    // Retail repair eligibility (0x519d40): a damaged, finished, allied unit
    // other than b, which is a mobile builder; a flying b also needs the
    // target's top above the sea (repairTopAboveSea).
    bool canRepairUnit(const Unit& b, const Unit& target) const;
    // 0x519d40's flying branch: the target's model top (UnitDef+0x14c) plus
    // its world Y (+0x6e) is at or above the sea level.
    bool repairTopAboveSea(const Unit& target) const;
    // The producer whose progress `site` shares, or null for an ordinary site.
    Unit* productionOwnerOf(const Unit& site);
    // Cosmetic emotes. A positive ID targets one owned living unit; callers
    // resolve empty-selection monarch fallback. ID 0 queries the latest trigger kind. The viewer owns real-time duration.
    void startDisco(int player, int unitId, bool append = false);
    bool discoTarget(int player, int unitId = 0) const;
    void startHeadbang(int player, int unitId, bool append = false);
    bool headbangTarget(int player, int unitId = 0) const;
    bool canPlace(const UnitType* type, float x, float z, int player = -1) const;
    // Every cell of a `foot`x`foot` footprint at origin (ox,oz) passable for `t` itself: its movement class's grid
    // and its own maxSlope / maxWaterDepth (passable). Match setup snaps spawns with it (snapSpawn).
    bool footprintPassable(const UnitType* t, int ox, int oz, int foot) const {
        for (int j=0;j<foot;++j) for (int i=0;i<foot;++i) if (!passable(t,ox+i,oz+j)) return false;
        return true;
    }
    // A builder's placement test for a site of `type` at (x,z), the one
    // startBuild runs. A building's is canPlace. A mobile type's, a conjured
    // unit, is retail's footprint test (405824 -> 507d10(type, 0, cell, 1, 0)):
    // any body on a footprint cell refuses the site, the conjurer's own too.
    // canPlace's centre radius for a mobile type is not that test.
    bool canPlaceSite(const UnitType* type, float x, float z, int player = -1) const;
    // Placement for a player's build ORDER (queueBuild and the build cursor):
    // canPlaceSite at the site the order stores (makeBuildOrder snaps the
    // point to the footprint grid), except that the player's own units that
    // would step aside for the builder (yieldsToSite) do not refuse the site,
    // nor does the conjurer `builderId` itself on a mobile type's site: it
    // has left the site before it places it. Engine deviation: retail's
    // cursor test (429787 -> 507400) refuses any visible body.
    bool canPlaceOrder(const UnitType* type, float x, float z, int player, int builderId = 0) const;
    // Observational whole-map query at cell centers. Returns empty on cancellation.
    // Reuses placementCheck with spatially bounded occupant candidates.
    std::vector<uint8_t> placementCells(const UnitType* type,const std::atomic_bool* cancel=nullptr);
    const Unit* lodestoneUpgradeSource(const UnitType* type, float x, float z, int player) const;
    // The plain lodestone `type` upgrades in place (retail's explicit faction
    // pairs, "tarmana" -> "tarlode"), else empty: `type` is no upgrade.
    static std::string lodestoneUpgradeBase(const UnitType& type);
    // Would the site be placeable if the clearable doodads on it were gone? Fills
    // `out` with their feature ids, nearest-first is the caller's job. Returns false
    // when anything ELSE blocks -- terrain, a unit, a building, an unreclaimable
    // feature -- because those are refusals retail makes too and we keep (the
    // player's own units that step aside excepted, as canPlaceOrder). Read-only:
    // this is a query for the UI, and it changes no sim state.
    bool clearableForPlacement(const UnitType* type, float x, float z,
                               std::vector<int>& out, int player = -1) const;
    // Mana deposit ("Sacred Stone") spots, in world px. Lodestones (onMana)
    // can only be built on one, but only when the map actually has any.
    void setManaSpots(std::vector<std::pair<float, float>> spots) {
        manaSpots_ = std::move(spots);
    }
    bool hasManaSpots() const { return !manaSpots_.empty(); }
    const std::vector<std::pair<float, float>>& manaSpots() const { return manaSpots_; }
    // The map's sacred-site features (each anchor, in map scan order); read-only,
    // for the server-side Classic AI's deposit search.
    // Reclaimable features (trees/rocks/houses). Populated only by setupMatch (the
    // one deterministic per-peer walk); never from the viewer. See struct Feature.
    void addFeature(int id, float x, float z, float energy,
                    int fx, int fz, bool blocks, int type = -1, bool placeOnMap = true);
    const std::vector<Feature>& features() const { return features_; }
    void setMapPlacementFeatures(const std::vector<uint16_t>& raw,
                                 std::vector<RetailMapFeatureType> types);
    const std::vector<RetailMapFeatureCell>& mapPlacementCells() const { return mapPlacementCells_; }
    // The raw TNT feature plane after map install (retailSurvivingMapFeatures):
    // what retail keeps once the map has loaded. Every consumer of the map's
    // features (matchsetup's registration, the viewer's feature art) reads this,
    // never map.features. A load-time snapshot: runtime reclaim, burning and
    // corpses do not change it. Empty until setMapPlacementFeatures.
    const std::vector<uint16_t>& installedMapFeatures() const { return installedMapFeatures_.values(); }
    const std::vector<RetailMapFeatureType>& mapPlacementTypes() const { return mapPlacementTypes_; }
    // A map cell's feature grade as the search planes rate it (0 blocked, 1 a
    // clearable blocker, 7 free); needs the feature plane (mapPlacementCells).
    int mapFeatureGrade(int,int) const;
    void updateNavigationExploration();
    // 4f6a40/4c6a70/4c6800 for the per-player current-view counters: one pass
    // after each tick, beside updateNavigationExploration.
    void updateViewCounts();
    // The players whose current-view plane a unit of `owner` counts in: the
    // owner and every allied player, human or computer (owner decision D2,
    // beyond retail). Retail's game start (4f6770) sets ShareMapAndLOS
    // (+0xf7[]/+0x101[]) both ways for each pair of players on one lobby team
    // unless either is a computer, and a unit's sight object (4c66b0) then
    // stamps into the sharing player's plane; World shares between computers
    // too. A passive neutral (Player::passiveNeutral) shares with nobody and
    // nobody with it, allied or not: its mask is its own bit, and it is in no
    // other player's. Bit per player; 0 for a neutral or out-of-range owner.
    uint16_t viewShareMask(int owner) const;
    // Test reference: move counted footprints by retail's full removal and
    // re-add instead of retailSightMove (the counts and hashes must not differ).
    void setViewCountsByRebuildForTest(bool on) { viewCountsByRebuild_=on; }
    void updateExplorationPlane();
    // Per-player current-view count of one projected 32px cell (0 = unseen).
    int viewCount(int player,int cx,int cz) const {
        const int width=hW_/2,height=hH_/2;
        const size_t cells=size_t(width)*height;
        if (player<0 || uint32_t(cx)>=uint32_t(width) || uint32_t(cz)>=uint32_t(height) ||
            (size_t(player)+1)*cells>viewCounts_.size()) return 0;
        return viewCounts_[size_t(player)*cells+size_t(cz)*width+cx];
    }
    const std::vector<uint16_t>& viewCounts() const { return viewCounts_; }
    // The projected 32px grid both planes use (cells per row / rows).
    int viewPlaneW() const { return hW_/2; }
    int viewPlaneH() const { return hH_/2; }
    // Display bookkeeping for the client, never hashed and never read by the
    // simulation: one byte per (1<<kViewDirtyShift)-cell square of the two
    // planes, set where a view or exploration footprint was added, moved or
    // removed (every square when a plane is replaced or rewritten whole) since
    // the client last cleared it. The client rebuilds only those squares of its
    // display plane, which on the largest maps is a million cells.
    static constexpr int kViewDirtyShift = 4;
    const std::vector<uint8_t>& viewDirty() const { return viewDirty_; }
    int viewDirtyW() const { return viewDirtyW_; }
    void clearViewDirty() { std::fill(viewDirty_.begin(),viewDirty_.end(),uint8_t(0)); }
    const std::vector<uint16_t>& navigationExploration() const { return navigationExplored_; }
    // Write access for the harnesses and tests that seed or restore the plane
    // (situations, "everything explored" fixtures); nothing else may write it.
    // The block summaries derived from the plane (explorationBlocks_) are kept by
    // its own writers, so this drops them and the next exploration pass rebuilds
    // them from what the caller wrote: finish writing before that pass.
    std::vector<uint16_t>& editNavigationExploration() { explorationBlocks_.clear(); return navigationExplored_; }
    // Tests and TA_VERIFY_LISTS: the block summaries are present and equal a
    // fresh rebuild from the plane.
    bool explorationBlocksMatchPlane() const;
    // `player`'s Mapping-plane bit at the 32 px cell under (x,z), as tickManaBuildArea
    // reads it: true without a plane (no terrain), false off the map.
    bool exploredAt(int player,float x,float z) const {
        const int width=hW_/2,height=hH_/2;
        if (navigationExplored_.empty() || navigationExplored_.size()!=size_t(width)*height) return true;
        if (player<0 || player>=16 || !(x>=0) || !(z>=0)) return false;
        const int cx=int(x)/32,cz=int(z)/32;
        return cx<width && cz<height && (navigationExplored_[size_t(cz)*width+cx]>>player & 1)!=0;
    }
    bool mobilePlacement(const Unit& subject,int x,int z,bool allowMoving) const {
        return mobilePlacement(*subject.type,uint32_t(subject.id),x,z,allowMoving);
    }
    // 507d10's mobile footprint loop for a body of `type` that is not on the map
    // yet: `self` 0 excludes no occupant (BUILD stage 2's factory output query).
    bool mobilePlacement(const UnitType& type,uint32_t self,int x,int z,bool allowMoving) const;
    Fixed surfaceHeight(const Unit& subject,uint32_t clock,uint16_t* pitch=nullptr,uint16_t* roll=nullptr) const;
    void commitGroundStep(Unit& subject,Fixed dx,Fixed dz,bool strictDiagonal=false);
    void updateGroundTerrainFlags(Unit& subject) const;
    Fixed groundTerrainMultiplier(const Unit& subject) const;
    Fixed retailUnderwaterCap(const Unit& u,Fixed cap) const;   // TA: speed cap halved under water
    void brakeGround(Unit& subject,std::optional<Bam> facing=std::nullopt);
    void tickNavigationMovement(Unit& subject,Fixed maximum);
    SinCos steerGround(Unit& subject,RetailSteeringPoint start,RetailSteeringPoint end,
                       RetailSteeringPoint next,Fixed maximum);
    bool featureReclaimable(const Feature& f) const {
        return f.alive && !f.fadeLeft && !f.dieLeft && (f.type<0 ||
            (size_t(f.type)<featTypes_.size() && featTypes_[size_t(f.type)].reclaimable));
    }
    const Feature* feature(int id) const;                 // by id, nullptr if none
    const Feature* featureAt(float x, float z) const;     // by cell (viewer burn/art sync)
    void setFeatureTypes(std::vector<FeatType> t) { featTypes_ = std::move(t); }
    void clearFeatureTypes() {
        featTypes_.clear();
        // Registry rebuilds can reuse an old UnitType address for a different
        // unit. Types without a corpse/statue must not inherit that old mapping.
        corpseType_.clear(); stoneType_.clear(); frozenType_.clear();
    }
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
    void clearFeatures() {
        features_.clear(); featureIdx_.clear(); clearFeatureTypes(); burnSequence_=0; fadingFeatures_=0;
        clearFeatureMarks();
    }
    const std::vector<FeatType>& featureTypes() const { return featTypes_; }
    // A water grave's sinking body (4959c0, retailfeaturesink.h), or null for
    // a body that does not sink. Its y is where the body is drawn.
    const RetailFeatureSink* corpseSink(int unitId) const {
        const auto found=corpseSinks_.find(unitId);
        return found==corpseSinks_.end() ? nullptr : &found->second.motion;
    }
    // The weapon damage a dead unit's installed corpse has taken (-1: none installed).
    int32_t corpseDamage(int unitId) const {
        const auto found=corpseFootprints_.find(unitId);
        return found==corpseFootprints_.end() ? -1 : found->second.damage;
    }
    bool featureAliveAt(float x, float z) const;          // viewer decal sync
    // Order a mobile builder to reclaim feature `featureId` (negative = a corpse;
    // queue = append to its orders, for an area drag). Retail Reclaim (406250).
    void reclaim(int builderId, int featureId, bool queue);
    void reclaimArea(int builderId, float x0, float z0, float x1, float z1, bool queue);
    // Order `casterId` to raise the body of dead unit `corpseId` (retail
    // RESURRECT 0x4068b0, or VTOL_RESURRECT 0x41fe70 for a flyer): walk to it,
    // then channel a resurrect or an animate (reviveKind). A no-op when the
    // caster cannot raise that body (reviveKind is 0).
    void resurrect(int casterId, int corpseId, bool queue);
    // The kind a resurrect order from `casterId` on dead unit `corpseId` would
    // be (reviveKindFor): 0 when either is not one, the body is gone or fading,
    // or the caster cannot move.
    uint8_t reviveKind(int casterId, int corpseId) const;
    // Order a mobile builder to repair damaged friendly `targetId` (restores HP at
    // the builder's work rate, draining mana proportionally). tickRepair runs it.
    void repair(int builderId, int targetId, bool queue);
    void tickRepair(Unit& b, float dt);
    void tickPatrolRepair(Unit& b);
    void tickFlyingPatrolLook(Unit& b);
    void tickGroundPatrolLook(Unit& b);
    void setPatrolRepairs(bool enabled) { patrolRepairs_=enabled; }
    // True if (x,z) lies over water (for choosing the water impact effect).
    // Retail (icd 0x509760): a unit is "on road" only when EVERY cell under its
    // footprint carries the road flag (ground units only; checked per tick into
    // a status bit that GET 34 and the speed formula read).
    bool onRoad(Fixed x, Fixed z, int footX = 1, int footZ = 1) const {
        if (roads_.empty()) return false;
        int x0 = footprintOrigin(x, footX), z0 = footprintOrigin(z, footZ);
        if (x0 < 0 || z0 < 0 || x0 + footX > terW_ || z0 + footZ > terH_)
            return false;
        for (int dz = 0; dz < footZ; ++dz)
            for (int dx = 0; dx < footX; ++dx)
                if (!roads_[size_t(z0 + dz) * terW_ + size_t(x0 + dx)]) return false;
        return true;
    }
    bool onRoad(float x, float z, int footX = 1, int footZ = 1) const {
        return onRoad(Fixed::fromFloat(x), Fixed::fromFloat(z), footX, footZ);
    }
    // .ota waterdoesdamage/waterdamage: on two Iron Plague missions the water is
    // lethal, which is the whole point of their terrain. Whole hit points, once
    // a second (retailDeadlyWaterDamage).
    void setWaterDamage(int32_t perSecond) { waterDamage_ = perSecond; }
    void setNoSeaLevelTrigger(bool value) { noSeaLevelTrigger_ = value; }
    bool isWater(float x, float z) const {
        if (depth_.empty()) return false;
        int cx = int(x) / 16, cz = int(z) / 16;
        if (cx < 0 || cz < 0 || cx >= terW_ || cz >= terH_) return false;
        return depth_[size_t(cz) * terW_ + cx] > 0;
    }
    Player& player(int i) { return players_[size_t(i)]; }
    const Player& player(int i) const { return players_[size_t(i)]; }
    int numPlayers() const { return scenarioParticipantCount_ >= 0 ? scenarioParticipantCount_ : int(players_.size()); }
    bool isNeutralPlayer(int p) const { return scenarioParticipantCount_ >= 0 && p == 8; }
    bool validUnitOwner(int p) const { return (p >= 0 && p < numPlayers()) || isNeutralPlayer(p); }
    void enableScenarioNeutralPlayer() {
        if (scenarioParticipantCount_ >= 0) return;
        scenarioParticipantCount_ = int(players_.size());
        players_.resize(9);
        for (int p = scenarioParticipantCount_; p < 9; ++p) players_[size_t(p)].team = -1;
    }
    // Retail CRT type defaults apply to every subsequent spawn; health is not consumed.
    void setScenarioTypeStats(std::string type, int armor, int weapon, int veteran) {
        scenarioTypeStats_[std::move(type)] = {armor, weapon, veteran};
    }
    // Retail mission SET40 replaces player0 score and disables all automatic scoring.
    void setScriptScore(int32_t value) { players_[0].score=value; scoreAutomaticDisabled_=true; }
    bool scoreAutomaticDisabled() const { return scoreAutomaticDisabled_; }
    // Initialize before spawning units. Match/replay peers supply the same seed.
    void setGameSeed(uint32_t seed) {
        gameRng_ = initialGameRng_ = retailSeed(seed);
        windRng_ = initialWindRng_ = seed;
        damageCrtUsed_ = false;
    }
    void setWindRange(int minimum, int maximum) {
        minimum = std::clamp(minimum, 0, 32767);
        maximum = std::clamp(maximum, 0, 32767);
        if (maximum < minimum) std::swap(minimum, maximum);
        wind_ = {}; wind_.minimum = minimum; wind_.maximum = maximum;
        windEnabled_ = true;
    }
    void setBallisticGravityRaw(int32_t gravity) { ballisticGravityRaw_=gravity; }
    // The OTA's gravity as TotalA.exe keeps it (GetInt, 0x43654d -> map info
    // +0xd3c): AirStrike's release ring reads this integer (0x4123c0).
    void setMapGravity(int32_t gravity) { mapGravity_=gravity; }
    int32_t ballisticGravityRaw() const { return ballisticGravityRaw_; }
    const RetailWind& wind() const { return wind_; }
    float windFactor() const { return taWindFactor(wind_); }
    // TA map metal (docs/retail-economy.md section 6): SurfaceMetal everywhere, then
    // each indestructible feature with metal overwrites the cells under it.
    void setSurfaceMetal(int metal) {   // keeps stamped ore; unstamped cells take the new value
        const uint8_t old = surfaceMetal_;
        surfaceMetal_ = uint8_t(std::clamp(metal, 0, 255));
        for (auto& c : cellMetal_) if (c == old) c = surfaceMetal_;
    }
    void stampFeatureMetal(int cx, int cz, int fx, int fz, int metal, int mapW, int mapH);
    int cellMetal(int cx, int cz) const;
    int surfaceMetal() const { return surfaceMetal_; }
    void setTidalStrength(float t) { wind_tidal_ = t; }
    // The map's [GlobalHeader] killmul / timemul (TotalA.exe 0x4365e6 / 0x4365fe,
    // default 0): the result plate's Score weights (econ::plateScore). Set at
    // setup, the same on every peer; display only.
    void setScoreWeights(float killMul, float timeMul) { scoreKillMul_ = killMul; scoreTimeMul_ = timeMul; }
    float scoreKillMul() const { return scoreKillMul_; }
    float scoreTimeMul() const { return scoreTimeMul_; }
    // Start resources (lobby value x100 in MP/skirmish): stored = start, base
    // storage = max(start, 200).
    void setStartResources(int player, float energy, float metal);
    // Construction/repair/weapon economy hooks (sim/taeconomy.h semantics).
    bool econBuildStep(Unit& builder, Unit& site);
    float econBuildFraction(Unit& builder, Unit& site);   // < 0: stalled by debt
    bool taProductionWork(Unit& worker, Unit& producer, Unit& site);
    bool econRepairStep(Unit& builder, Unit& target);
    bool econPayShot(Unit& u, const Weapon& w);
    // A reclaim target's yield (feature or corpse type): metal and energy paid in full
    // when the reclaim ends (docs/retail-economy.md section 8).
    bool reclaimYield(int target, float& energy, float& metal) const;
    // Credit a player's own accumulator (reclaim, gifts), scaled by its multiplier.
    void econCredit(int player, float energy, float metal);
    const econ::PlayerBooks& books(int player) const { return players_[size_t(player)].books; }
    struct RngObservation {
        uint32_t tick;
        int32_t bound;
        uint32_t seedBefore, seedAfter, result;
        std::source_location caller;
        uint32_t retailReturnAddress=0; // diagnostic call-site identity, never simulation input
    };
    // Diagnostic observer only: it must not modify the world. Observation is
    // excluded from lockstep state and survives resetForReplay for replay tools.
    void setRngObserver(std::function<void(const RngObservation&)> observer) {
        rngObserver_ = std::move(observer);
    }
    void setCrtRngObserver(std::function<void(const RngObservation&)> observer) {
        crtRngObserver_=std::move(observer);
    }
    // Reset all simulation state so a match can be rebuilt (via setupMatch) and
    // replayed from tick 0 deterministically -- crucially nextId_ resets so the
    // replayed spawns get the SAME unit ids as the original run. setTerrain and
    // setPlayerCount (called by setupMatch afterwards) rebuild nav/vis/players.
    void resetForReplay() {
        clearScenarioState();
        scoreAutomaticDisabled_=false;
        unitScripts_.clear();unitScriptById_.clear();scriptYardById_.clear();
        paths_.clear();legion_.reset();convoys_.clear();
        searchGrades_.clear(); activeSearchGrade_=-1;
        units_.clear();unitSlotById_.clear();retiredOwners_.clear();retiredHash_=0;
        replacedObjects_.clear();justDied_.clear();
        // The retail group records and the AI squads' formation flags
        // (hashed) live across ticks: a replay from tick 0 starts with none.
        retailGroups_={};retailGroupPaced_.clear();aiFormation_={};
        // Legion's formation slots (hashed, by unit id) likewise.
        legionFormationSlots_.clear();legionFormationClasses_={};legionSlotDests_.clear();
        groundOwner_.clear();groundLinkSerial_=0;
        projectiles_.clear();flames_.clear();bursts_.clear();
        hits_.clear();
        pendingSounds_.clear();soundReq_={};
        transportEffects_.clear();
        scriptEmissions_.clear();
        scriptSounds_.clear();
        features_.clear();
        burnSequence_ = 0; fadingFeatures_ = 0;
        clearFeatureMarks();
        mapPlacementCells_.clear();mapPlacementTypes_.clear();corpseFootprints_.clear();corpseAnchors_.clear();
        corpseSinks_.clear();
        installedMapFeatures_.clear();
        explorationHeights_.clear();navigationExplored_.clear();explorationBlocks_.clear();
        viewCounts_.clear();deathSights_.clear();
        restoredNavigationViewer_=-1;
        featureIdx_.clear();
        clearFeatureTypes();
        // Every other kind of live cross-tick sim state has to go too, or a rejoin
        // replays tick 0 with leftovers from before the drop: a storm still roaming
        // (or a spell still channelling) would deal damage the referee never dealt
        // AND advance the shared RNG on this peer alone -- an instant desync. The
        // RNG itself is re-seeded for the same reason: replaying from a mid-match
        // stream position diverges from a referee that started fresh.
        pendingEffects_.clear();
        deathBlasts_.clear();
        gameRng_ = initialGameRng_;
        windRng_ = initialWindRng_;
        damageCrtUsed_ = false;
        const int windMin = wind_.minimum, windMax = wind_.maximum;
        wind_ = {}; wind_.minimum = windMin; wind_.maximum = windMax;
        nextId_ = 1;
        legionBodyWindow_.clear();   // its ticks live here, not in the Unit: ids start again at 1
        retailAllocation_=false;retailEntityPools_.reset();spawnGeneration_=0;
        entitySlots_.unitAt={};entitySlots_.slotOf.clear();
        nextMovementController_ = 0;
        tickCounter_ = 0;
        winningTeam_ = -1;
    }
    // Size the player table for a match (default 4 for single-player/scenarios).
    // Call BEFORE spawning any units -- shrinking it after would leave units with
    // an out-of-range owner index (dereferenced unchecked in the economy loop).
    void setPlayerCount(int n) {
        clearScenarioState();
        scenarioParticipantCount_ = -1;
        scenarioTypeStats_.clear();
        players_.assign(size_t(std::clamp(n, 1, kMaxPlayers)), Player{});
        for (int i = 0; i < numPlayers(); ++i) players_[size_t(i)].team = i;
    }
    // Assign a player's team, clamped to a valid team id [0, numPlayers). Teams
    // are 0..n-1 by construction; keeping them in range is the invariant that the
    // outcome check and the viewer's per-team arrays rely on -- so the sim owns
    // it rather than trusting setup (a lobby, or the TA_FFA dev harness).
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


    // Per-player unit limit: production and new builds stall a player once it has
    // this many live units (0 = unlimited). Set at match start (from the lobby).
    // The count it tests (Player::unitCount) is deterministic, so all peers agree.
    void setManaSharing(int fromPlayer, int toPlayer, bool enabled);
    bool canGiveUnit(int unitId, int fromPlayer, int toPlayer) const;
    bool giveUnit(int unitId, int fromPlayer, int toPlayer);
    // Authored scenario construction whitelist. Script-created and preplaced units
    // are not construction requests and deliberately bypass this restriction.
    void setBuildRestrictions(std::vector<std::string> types, bool enabled = true);
    bool buildAllowed(const UnitType* type) const;
    // The cap also sizes each player's entity pool (retail maxunits, 511570):
    // see entityPoolCapacity. Pools already allocated grow to a raised size.
    void setUnitCap(int c) {
        unitCap_ = c;
        const size_t size=size_t(entityPoolCapacity(c));
        for (auto& pool:entitySlots_.unitAt)
            if (!pool.empty() && pool.size()<size) pool.resize(size,-1);
    }
    // The pool a player starts with under unit cap `cap`: what retail's
    // 511570 allocates for that maxunits. 499590 clamps maxunits to 20..500,
    // so every lobby cap retail allows (20..500) is its own size, and the
    // engine's larger caps (1,000, 2,000) start where retail's setter would
    // put them, at 500. Uncapped worlds (tests, tools) start at the options
    // default 250 (4c7c7a). A pool grows a slot only when a unit needs one.
    static int entityPoolCapacity(int cap) {
        return cap>0 ? std::clamp(cap,kMinEntityCapacity,kMaxEntityCapacity) : kDefaultEntityCapacity;
    }
    int unitCap() const { return unitCap_; }
    // A unit's offset in its owner's entity pool (the order 416430 admits
    // path searches in), or -1 for a restored-pool fixture or no unit.
    int entitySlot(int unitId) const { return entitySlots_.slot(unitId); }
    // An ownership change gives the unit a new record in the new owner's pool,
    // as retail's transfer allocates a new object. Call before setting player.
    void transferEntitySlot(Unit& u,int newPlayer);
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

    // Win/defeat, computed sim-side so every lockstep peer agrees on the same
    // tick. A player is defeated when it has no living units; a team is out when
    // all its players are. Returns the surviving team id once exactly one team
    // remains (the winner), or -1 while >1 team still has units. Updates each
    // Player::defeated as a side effect. Idempotent; call once per tick.
    int updateOutcome();
    int winningTeam() const { return winningTeam_; }
    void applyScenarioResourceAction(int player, int opcode, int32_t value);
    bool scenarioRandomPercent(int threshold) {
        damageCrtUsed_=true; // Scenario draws also require the shared CRT state in the hash.
        return double(threshold)>double(crtRand(0x4cac22))*0.0030517578125;
    }
    bool hasScenarioOutcomes() const;
    // Explicit CRT result for one participant; -1 asks for a completed spectator
    // result (all participants finished, victory if any won). Not a campaign.
    int scenarioOutcome(int player) const;

    void setDoubleSight(bool enabled) { doubleSight_ = enabled; }
    // The room's (or campaign .ota's) starting vision, as retail's 4c6540 seeds it:
    // Map Revealed fills the Mapping plane (every cell explored), and FULL VISION
    // also turns LosOn off. Area clear reads these (its 509cc0 fog test), and our
    // AI levels' fog rule (ai::Controller::knowsGround / sees);
    // the exploration plane itself still starts unexplored. Hashed when set.
    void setRoomVision(bool mapRevealed, bool losOff) { roomMapRevealed_ = mapRevealed; roomLosOff_ = losOff; }
    bool roomMapRevealed() const { return roomMapRevealed_; }
    bool roomLosOff() const { return roomLosOff_; }
    // `player`'s whole map counts as explored: the room revealed it (no power codes
    // then, which take the reveal over), or its +Mapping code -- area clear's rule.
    bool mapRevealedFor(int player) const { return (roomMapRevealed_ && !powerCodes_) || powerMappingOff(player); }
    // Power codes (sim/powercodes.h; the room's POWER CODES option). setPowerCodes
    // also starts every code at its off state: no damage mode, line of sight on for
    // every player, and the shroud on unless the room starts with the map revealed
    // (FOG OF WAR: EXPLORED or FULL VISION). Retail seeds MappingOn from the room's
    // Map Revealed box the same way, so the first +Mapping there brings the shroud
    // back. applyPowerCode is Cmd::PowerCode's effect, a no-op while the option is
    // off. All of it is hashed once the option is on.
    void setPowerCodes(bool enabled, bool mapRevealed = false) {
        powerCodes_ = enabled; damageMode_ = DamageMode::Normal; powerLosOff_ = 0;
        powerMappingOff_ = enabled && mapRevealed ? uint16_t(0xffff) : uint16_t(0);
    }
    bool powerCodes() const { return powerCodes_; }
    void applyPowerCode(int player, PowerCode code, PowerArg arg, int unitId);
    DamageMode damageMode() const { return damageMode_; }
    // +LOS Off / +NowISee: `player` sees every unit on a cell it has explored
    // (playerSees), and the client's display fog shows explored cells in view.
    bool powerLosOff(int player) const { return player >= 0 && player < 16 && (powerLosOff_ >> player & 1); }
    // +Mapping / +NowISee: `player`'s whole map is explored (sim and display).
    bool powerMappingOff(int player) const { return player >= 0 && player < 16 && (powerMappingOff_ >> player & 1); }
    int32_t sightDistance(const UnitType& type) const { return type.sight * (doubleSight_ ? 2 : 1); }
    int32_t radarDistance(const UnitType& type) const { return type.radar * (doubleSight_ ? 2 : 1); }
    // Monarch-expendable rule (net GameOptions): when FALSE, losing your Monarch
    // (a commander unit) loses you the game even if other units survive.
    void setMonarchExpendable(bool e) { monarchExpendable_ = e; }
    bool monarchExpendable() const { return monarchExpendable_; }
    // Keep this World's intra-tick work on the calling thread (no worker pool): the
    // sim's SimPool then runs its identical fixed chunks inline. For tests and tools;
    // the game and taserver choose a width instead (setSimWorkers).
    // Never affects results -- purely scheduling.
    void setSerialThreads(bool s) { serialThreads_ = s; }
    // The SimPool's worker count (-1: SimPool::defaultWorkers(), AUTO), at most
    // SimPool::kMaxWorkers: the client's SIMULATION THREADS setting
    // (SimPool::sessionThreads), a game's share of taserver --sim-threads
    // (SimPool::serverShare, set again before each tick), and tests.
    // setSerialThreads(true) still means 0. Scheduling only, like the above.
    // AUTO is read once, at the first simPool() call after this, and kept as a
    // chosen count is: the width never follows a CPU mask that changes mid-game.
    // Call it between ticks, on the thread that ticks this World (or before
    // that thread starts): the pool is rebuilt at the next simPool() call.
    void setSimWorkers(int n) { simWorkers_ = std::min(n, int(SimPool::kMaxWorkers)); autoWorkers_ = -1; }
    // Pin the SimPool's workers (scheduling only, like the above): worker k to
    // cpus[k], a worker past the end to no CPU. cpus[0] is for the thread that
    // ticks this World, which whoever owns that thread pins (sim/simcpus.h): the
    // client's sim worker and taserver's per-game worker do, a main thread never
    // is. From SimPool::handOut, or a game's part of taserver --sim-cpus; call it
    // as setSimWorkers is called. The pool is rebuilt at the next simPool() call
    // if the CPUs changed. Never called: no thread's CPUs are touched.
    void setSimCpus(std::vector<int> cpus) { simCpus_ = std::move(cpus); simPin_ = true; }
    // Deterministic digest of sim state, for lockstep sync checking.
    struct WeaponAimSolution {
        uint16_t heading=0,pitch=0;
        std::array<int32_t,3> target{}, source{};
        // 52fe30's facing request, a native heading (retailSlotFacing).
        uint16_t facing=0;
        // A ballistic slot whose arc (52bd10) cannot reach the aim point: the
        // aim keeps native pitch 0 (52bdf0), but no shell is launched.
        bool unreachable=false;
    };
    std::optional<WeaponAimSolution> queryWeaponAim(int unitId,int targetId,int slot);
    // A weapon slot's target: a unit, or (unit null) a ground point -- retail's
    // slot record holds either (+4/+6, +6 == 0x8000 for a unit). `point` is
    // the slot's aim point for a point target (pointAimPoint, 51aa50).
    struct WeaponTarget {
        Unit* unit=nullptr;
        std::array<int32_t,3> point{};
    };
    std::optional<WeaponAimSolution> queryWeaponAim(Unit& u,const WeaponTarget& t,int slot);
    // 51aa50 for a point target: the point in whole px, its height the
    // terrain's there or the sea level, whichever is greater.
    std::array<int32_t,3> pointAimPoint(RetailGroundPoint p) const;
    // 51aa50 for a unit target: its SweetSpot plus the weapon's lead.
    std::array<int32_t,3> retailLeadPoint(const Unit& u,const Unit& target,const Weapon* weapon);
    std::array<int32_t,3> queryUnitScriptPoint(int unitId,bool sweetSpot,int slot=0);
    // A script piece's world point (0x43def0; a SweetSpot's drawn centre);
    // a piece out of range is the unit origin.
    std::array<int32_t,3> unitPiecePoint(Unit& u,int32_t piece,bool sweetSpot);
    bool projectilePointInUnit(int unitId,const std::array<int32_t,3>& point) const;
    // Native collision result: 0 clear/bounce, 1 outside map, 2 environmental hit.
    int projectileEnvironment(const std::array<int32_t,3>& point,int32_t& verticalSpeed,
                              uint32_t weaponFlags) const;
    // Retail's persistent secondary (airborne) occupant grid, map word +2.
    // Rebuilt once per tick in player/entity order (51da76/51db36); read by
    // projectiles and by the landing-site predicate 509400.
    RetailAirOccupancy airOccupancy_;
    int airRetiring_=0;
    struct AirOccupancyHost;
    std::span<const int> updateAirOccupancy();
    void retireAirOccupant(Unit& u);
    struct ProjectileCollisionResult { int code=0,unitId=0; };
    ProjectileCollisionResult projectileCollision(const std::array<int32_t,3>& point,
        int32_t& verticalSpeed,uint32_t weaponFlags,int owner,std::span<const int> airGrid,
        bool bypassPrimaryGeometry=false,std::optional<std::array<int32_t,3>> targetShot={},
        uint16_t proximityRadius=0) const;
    uint64_t stateHash() const;
    // A coarse, layout-independent digest for replay verification: FNV over (id, type,
    // owner, x, z, hp, front order kind and target) in unit-id order and nothing else --
    // no Legion bookkeeping, scripts, nav or RNG. Never folded into stateHash and never
    // read by the sim. A replay whose stateHash diverges but whose posDigest still
    // matches changed a hash LAYOUT (or hidden state); one whose posDigest diverges too
    // changed behaviour.
    uint64_t posDigest() const;
    // Read-only invariant check for the incrementally maintained grade hashes.
    bool searchGradeChecksumsValid() const;
    // Test hook: build every existing plane's shape afresh both ways (the
    // box-minimum build and the per-cell reference) and compare. Derived only.
    bool searchGradeBuildMatchesReference();
    // Test hook: refresh one window of every plane both ways (box minima and
    // the per-cell reference) on copies and compare. Derived only.
    bool searchGradeRefreshMatchesReference(int x,int z,int w,int h);
#ifndef NDEBUG
    // Debug-only divergence locator, and the first thing to reach for when the referee
    // reports a desync. TA_HASHTRACE="lo:hi" dumps a PER-COMPONENT checksum every tick
    // in [lo,hi]: units (position / hp / orders / misc), projectiles, effects, storms,
    // players, features, the shared game RNG stream, and -- deliberately -- the nav
    // overlay and grid cells, which stateHash does NOT fold.
    //
    // stateHash() collapses the world to one number, which says THAT two peers disagree
    // but not about what. Run both peers with this set, diff the two logs, and the first
    // differing column names the component and the tick. That is how the viewer-writes-
    // to-nav_ desync was found: every column matched except `ord`, and `nav` had differed
    // since tick 1 while `obst` never did -- which pointed straight at the grid rather
    // than at anything the checksum covers.
    //
    // Costs a full-world fold per traced tick, so it early-outs before doing any work
    // when unset. Never read by the sim -- pure observation.
    void hashTrace() const;
#endif

    // Campaign mission: an optional in-sim "god" script + win/lose rules
    // (src/sim/mission.h). Ticked inside tick(), and folded into stateHash().
    World();                                                    // out-of-line (mission_ member)
    ~World();                                                   // out-of-line (mission_ dtor)
    // A separate, exact copy of this world between ticks: from here it runs as this
    // one would, byte for byte, sharing nothing it can change (the type registry and
    // the map data it reads are the caller's, as for any World). Its thread pool is its
    // own and comes on first use; observers are left behind. It shares with this world
    // the per-cell maps neither has changed since (SharedArray, sim/copymember.h).
    std::unique_ptr<World> clone() const;
    // A copy to keep and restore from, not to run: as clone(), but its sparse per-cell
    // maps (occupancy, ownership, sight counts, obstacles, Legion's soft stamps...) are
    // held as runs, megabytes each in a world that runs. restoreFrom() and clone() give
    // them back exactly; nothing else may be asked of a keyframe (tick() and stateHash()
    // throw). The replay viewer's rewind points are these (client/replaybar.h).
    std::unique_ptr<World> keyframe() const;
    bool isKeyframe() const { return kept_; }
    // What a copy just made shares with this world that either may yet have to copy for
    // itself: the per-cell maps a structure or a feature rewrites (the grids' clearances,
    // Legion's planes; SharedArray), not the terrain nothing writes after setup. A copy
    // holds at most what it allocated plus this. (A keyframe packs those maps instead
    // of sharing them, so for one this is only what did not pack.)
    uint64_t copyOnWriteBytes() const;
    // Become an exact copy of `from` (a clone, or another world) without moving: anything
    // holding this World still holds it. Keeps this world's thread pool. A throw (out of
    // memory) leaves this world unusable.
    void restoreFrom(const World& from);
private:
    World(const World& from);
    World& operator=(const World& from);
    void bindPathService();
    // After a defaulted copy: everything that points into a World (or back at it)
    // points into this one, and the members a copy leaves empty are cloned.
    void adoptCopy(const World& from);
public:
    void setMission(std::unique_ptr<MissionScript> m);
    MissionScript* mission() { return mission_.get(); }
    int missionOutcome() const;   // 0 none/running / +1 victory / -1 defeat

    // Scenario (.crt) trigger runner: an optional in-sim per-player rule engine
    // (src/sim/scenario.h). Ticked inside tick() and folded into stateHash().
    void setScenario(std::unique_ptr<ScenarioScript> s);
    void clearScenarioState();
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

    // Move order; queue appends. A unit steers STRAIGHT at the front order's point --
    // there is no local obstacle avoidance in the steering -- and the background path
    // search (retail's boundary tracer, sim/pathsearch.h, NOT an A*) routes around
    // terrain by splicing its waypoints in as further order legs (see replaceLeg).
    // This used to describe a shared flow field; that system is gone.
    // Under Legion every order joins a convoy of its class (Move here, Fight
    // from attackMove, Patrol from patrolTo); `identityCopy`, when set, is stamped
    // instead of joining (a patrol's return leg). Inside an explicit order
    // (beginOrder) the order's area and serial are stamped instead.
    void order(int unitId, float x, float z, bool queue, ConvoyClass cls = ConvoyClass::Move,
               std::optional<OrderIdentity> identityCopy = std::nullopt);
    // Explicit selection orders (Cmd::Order; applySelectionOrder in
    // sim/matchsetup.cpp): between beginOrder and endOrder the members of ONE order
    // are applied, each named by orderMember before its command, and under Legion
    // every leg the member gets for itself carries the order's identity: its
    // serial, and -- for an order with a click point (`point`: Move, AttackMove,
    // Patrol) -- the arrival area the order joins once per class at that point
    // (`x`, `z`), never per member. Legs of other verbs (an Assist's approach
    // move) and of any unit but the member keep the per-unit identity. Not state:
    // it lasts one applyCommand call.
    void beginOrder(uint32_t serial, int player, bool point, Fixed x, Fixed z);
    void orderMember(int unitId) { if (activeOrder_) activeOrder_->member = unitId; }
    void endOrder() { activeOrder_.reset(); }
    // A unit's orders changed (a leg gained or retired, a mission step, its
    // death): Legion drops the settled-arrival and parting records the unit
    // no longer qualifies for. The order helpers, dropLeg, retailIssueAutoAttack, the
    // ground/flight/construction mission dispatchers and the death edge call
    // it. Touches Legion state only (a no-op without Legion).
    void noteOrders(int unitId);

    // ---- order-queue helpers ------------------------------------------------
    // One issued order can expand into a whole route, so Unit::orders mixes
    // pathfinding waypoints with the goals the player actually asked for. These
    // two exist because the repath/unstick paths used to read orders.back() as
    // "the destination" -- which is the LAST QUEUED leg, not the current one --
    // and then clear the whole queue, silently deleting everything the player had
    // lined up behind it.

    // A mission script's hold at the head (a wait, a WaitForAttack, or an
    // order with a mission of its own, Order::scripted): no goal of its own,
    // and the leg behind it must not start (route, approach) until it ends --
    // a route installed for that leg would replace the hold with waypoints.
    static bool scriptHold(const Unit& u) {
        if (u.orders.empty()) return false;
        const Order& o = u.orders.front();
        return o.wait > 0 || o.waitAttack || o.scripted;
    }
    // Index of the last waypoint of the leg being worked on right now. Falls back
    // to the whole queue for unmarked orders (mission scripts).
    static size_t currentLeg(const std::vector<Order>& o) {
        for (size_t i = 0; i < o.size(); ++i) if (o[i].goal) return i;
        return o.empty() ? 0 : o.size() - 1;
    }
    // Swap the current leg's waypoints for `path`, keeping the leg's own flags
    // and every order queued behind it.
    static void replaceLeg(Unit& u, const std::vector<Order>& path);
    void deliverSearchRoute(int,const std::vector<PathCell>&,Fixed,Fixed,bool,bool,bool,bool);
    void refreshSearchRequest(int,PathCell&,int&,RetailCostSearch::Costs&) const;
    bool admitSearchRequest(int,uint32_t);
    // Drop the current leg entirely and move on to whatever was queued behind it.
    void dropLeg(Unit& u);
    // Does this unit still have construction queued (anywhere in its orders)?
    static bool hasQueuedBuild(const Unit& u) {
        for (const Order& o : u.orders) if (o.buildType) return true;
        return false;
    }
    // Any construction OR reclaim still queued -- the "this builder is on a job"
    // test that several places need.
    static bool hasQueuedWork(const Unit& u) {
        for (const Order& o : u.orders)
            if (o.buildType || o.reclaimFeat || o.reclaimArea || o.repairTarget) return true;
        return false;
    }
    void attackMove(int unitId, float x, float z, bool queue);
    // Armed ATTACK on the ground (Cmd::AttackGround; 4de530 kind 3 with a
    // point and no target): SUPPRESS for a ground unit or a structure whose
    // selected weapon is ranged, or a melee one over a clearable blocking
    // feature; nothing for a toairweapon, or a melee weapon elsewhere. A
    // flyer gets AIRSTRIKE, AIRTOGROUNDHOVER or AIRTOGROUND at the point
    // (Order::airPoint). Whether an order was given.
    bool suppress(int unitId, float x, float z, bool queue);
    bool suppressAccepts(int unitId, float x, float z) const;
    // The resolver's rule by the type and the selected slot alone (519ac0: a
    // switcher's selected slot, else the first), shared with the client's
    // click (clickrules.h): Fire gives the order anywhere, OverBlocker (a melee
    // weapon) only where clearableBlockerAtPoint holds.
    enum class SuppressRule : uint8_t { None, Fire, OverBlocker };
    static SuppressRule suppressRule(const UnitType& t, int weaponSlot);
    // 4c18d7..4c18ff (and the reload path 4c1f6c..4c1f7c): a type with a
    // weapon, bmcode 0 and a canbuild menu loses canattack (+0x264 0x20) at
    // load, and the resolver's attack mode (4de530 kind 3, 4de62d) then gives
    // it no mission: no fire at a point, no attack on a unit, ordered or
    // automatic (4d7de0). No shipped type is one; a mod's armed factory is.
    // The weapons still fire through the sweep (40f330 sets slot targets).
    static bool attackStrippedAtLoad(const UnitType& t) {
        return !t.weapons.empty() && t.buildMovementCode == 0 && !t.buildMenu.empty();
    }
    bool clearableBlockerAtPoint(float x, float z) const {
        return clearableBlockerAt(Fixed::fromFloat(x), Fixed::fromFloat(z));
    }
    // Can a unit of `type` at (fx,fz) actually reach goal (gx,gz)? Answered from
    // footprint-aware COMPONENT labelling (one flood fill per grid+footprint, see
    // components()), not from a distance field -- the flow-field version cost ~40ms a
    // call and was the per-second AI hitch. Lets the AI pick a REACHABLE target
    // instead of one merely nearest in a straight line but walled off.
    bool pathExists(const UnitType* type, float gx, float gz, float fx, float fz) const;
    // Can this mobile builder get close enough to build `site` at (x,z)? True when some
    // cell its body can reach from where it stands (same component of its own nav grid)
    // passes the MobileBuild reach test against the site (retailBuildWithinReach).
    // A placement-preview question for the client, not an order rule: retail accepts the
    // site and the builder walks, fails and says CANNOTREACHCONSTRUCTION, so the sim
    // still admits such an order. Flyers, structures, missing or embarked builders
    // answer true (nothing to decide). Not hashed; same answers whatever was queried before.
    bool buildSiteReachable(int builderId, const UnitType* site, float x, float z) const;
    // How the component labelling behind pathExists() kept up with walkability edits:
    // full relabels versus edits carried across incrementally. Lets navincr_test tell a
    // fast path that fires from one that silently never does. Not hashed.
    struct CompStats {
        uint64_t relabels = 0;         // whole-grid labellings built by components()
        uint64_t blockCarried = 0;     // blocks proven harmless inside their window
        uint64_t splitJoined = 0;      // blocks proven harmless by resolveSplit()
        uint64_t splitCut = 0;         // blocks that really cut a component, relabelled locally
        uint64_t splitFallback = 0;    // resolveSplit() over budget -> full relabel
        uint64_t unblockCarried = 0;
    };
    const CompStats& compStats() const { return compStats_; }
    // Build the labelling behind pathExists() now for every movement grid and
    // footprint these types walk with (flyers and structures have none). The first
    // query of each used to flood-fill the whole map inside a tick -- a factory's
    // first output of a class (parkProductionOutput), 82-110 ms on a 2048-cell map --
    // so setupMatch builds them before tick 0. A cache: no answer changes, and
    // nothing here is hashed.
    void prebuildComponents(const std::vector<const UnitType*>& types) const;
    void patrol(int unitId, float x, float z);
    // Queue a patrol waypoint (SetMission "p X Y"): like a move but the completed
    // order re-queues at the back, so a chain of these loops the unit through them.
    void patrolTo(int unitId, float x, float z, bool queue, std::optional<OrderIdentity> identityCopy = std::nullopt);
    // The player's Shift-patrol (Cmd::Patrol, queue=1): appends point (x,z)
    // after every queued order. If it starts with no loop in the queue, the
    // patrol mission adds the return point where the unit then stands (see
    // Order::patrolOpen); otherwise it joins the existing loop.
    void queuePatrol(int unitId, float x, float z);
    // Queue a timed hold (SetMission "w F"): the unit stands still for `seconds`
    // before the next queued order runs. With a radius ("w F N", N cells times
    // 16 px) it also ends once a visible enemy is that close (retail Wait 401480
    // with +0x52 set: it looks every rand(7)+7 ticks, see ScriptedOrder).
    void orderWait(int unitId, float seconds, bool queue, int32_t radius = 0);
    // Queue a WaitForAttack hold (SetMission "wa [NAME]", retail 4017a0): the unit
    // stands still until `watched` (0 = itself) is hit or gone, counting hits
    // from the moment the order is given; then the next queued order runs.
    void orderWaitAttack(int unitId, bool queue, int watched = 0);
    // Queue an attack on every unit of `type` not owned by this unit's player,
    // nearest (with retail's random spread) first, until none is left
    // (SetMission "a TYPE", AttackUType 401580).
    void orderAttackType(int unitId, const UnitType* type, bool queue);
    // Queue production on a factory (SetMission "b TYPE N", BuildingBuild
    // 401c20): when it comes up, `count` of `type` join the build queue, and the
    // order holds the queue behind it until they are built. 0 builds nothing.
    void orderTrain(int factoryId, const UnitType* type, int count, bool queue);
    // Queue a self-destruct (SetMission "d", SelfDestructFG): when it comes up
    // the unit dies as a Ctrl+D countdown ends, with no wreck and no credit.
    void orderSelfDestruct(int unitId, bool queue);
    // A mission's MakeSelectable (401450, descriptor flags 4: kept through a
    // clear): when it comes up the unit is selectable again and the next
    // order runs.
    void orderMakeSelectable(int unitId);
    // Put `passengerId` straight into `transportId`'s hold (InitialMission
    // "i NAME", retail 51b4f0): no pickup, and its later orders wait aboard.
    bool embarkInto(int passengerId, int transportId);
    // Drop a unit's queued route search (orders given to a unit still aboard
    // ask for their routes when they come up after the unload).
    void cancelRoute(int unitId) { if (Unit* u = unit(unitId)) cancelPath(*u); }
    // SetMission "o A" (Standing_UnitOrder 0x5198a0, KINGDOMS.icd: 2 offensive,
    // 1 defensive, 0 passive, both fields at once; no TA clause uses it).
    // scriptStandingFields: InitialMission "o A B" (TotalA.exe 0x487c9d): move
    // state A (unit+0x110 bits 18-19) and fire state B (bits 20-21), each 0..3,
    // the shown order unchanged; only that the unit lives is checked.
    void scriptStandingOrder(int unitId, int order);
    void scriptStandingFields(int unitId, int move, int fire);
    // SetMission "v F": the fixed veterancy multiplier (Unit::vetOverride); 0 ends it.
    void setVeterancyOverride(int unitId, float multiplier);
    // The state of a mission order (Order::scripted), or nullptr.
    const ScriptedOrder* scriptedOrder(const Order& o) const {
        return o.scripted && o.scripted <= scriptedOrders_.size() ? &scriptedOrders_[o.scripted - 1u] : nullptr;
    }
    void guard(int unitId, int targetId, bool queue);
    void stop(int unitId);
    // Self-destruct a living unit (Ctrl+D via the command path; no kill credit).
    void destroy(int unitId);
    void setWeapon(int unitId, int slot);   // choose the active weapon (0=primary)
    void setStance(int unitId, int stance); // combat stance 0=offensive/1=defensive/2=passive
    static constexpr int kStanceNone=3;     // setStance on a producer: no stance for its output
    void setCloak(int unitId, bool on);     // canCloak unit: enable/disable cloaking
    void setActive(int unitId, bool on);    // onOffable unit: power on/off
    void setSquad(int unitId, int squad);   // control squad: 0 none, +N group N, -N formation N
    // A computer player's retail AI squads (Cmd::AiGroup, from the server's
    // classic AI): unitId != 0 puts that unit in squad `slot` (0 none,
    // Unit::aiGroup); unitId == 0 sets squad `slot`'s formation flag (record
    // +8, 50b700). Only for a computer-owned player (automaticGates).
    void setAiGroup(int player, int unitId, int slot, bool formation);
    bool aiFormation(int player, int slot) const {
        return player>=0 && player<kMaxPlayers && slot>0 && slot<100 && aiFormation_[size_t(player)].test(size_t(slot));
    }
    // Attack order on an enemy unit.
    void attack(int unitId, int targetId, bool queue);
    // D-gun (candgun units): AttackSpecial on a unit, one commandfire shot.
    void dgun(int unitId, int targetId, bool queue);
    static bool isDgunSlot(const UnitType& t, int slot);
    // Board/unload through a friendly transport; queue=true appends the paired
    // pickup orders or unload route after current orders instead of replacing them.
    bool canLoadInto(int unitId, int transportId) const;
    bool scriptYardOpen(int unitId) const;
    void loadInto(int unitId, int transportId, bool queue = false);
    void unloadAt(int transportId, float x, float z, Fixed destinationY = {},
                  bool queue = false);
    // Retail's queue modifiers on a player's order (4d78a0's place, 4d7a30's
    // toggle; applyCommand reads them from Command::queue). The kind a queued
    // order is matched by, from its own fields (queuedKind).
    enum class QueuedKind : uint8_t { None, Move, AttackMove, Patrol, Attack, Guard, Load, Unload,
                                      Build, Reclaim, Repair, Assist, AttackGround };
    struct QueuedMatch {
        QueuedKind kind = QueuedKind::None;
        Fixed x, z;                         // the point kinds' click (Build: the site)
        int target = 0;                     // the unit kinds' target (Reclaim: the feature)
        const UnitType* type = nullptr;     // Build
    };
    static QueuedKind queuedKind(const Order& o);
    // Ctrl (4d7a09 -> 4d7750): `issue` appends its order to an emptied list --
    // the unit's own, or its rally list when `rally` (a producer, or a mobile
    // builder repeating production) -- and everything that was queued goes
    // back behind it. The interrupted leg starts over when it comes round.
    void insertOrderFront(int unitId, bool rally, const std::function<void()>& issue);
    // Shift or Ctrl on an order of the same kind as a queued one, on its target
    // or within 16 px of its point (4d7a5b..4d7ab2): that order goes instead.
    // The first match from the head; the rally list when `rally`. True when
    // one went.
    bool removeQueuedMatch(int unitId, bool rally, const QueuedMatch& m);
    void tick(float dt);

    std::vector<Unit>& units() { return units_; }
    const std::vector<Unit>& units() const { return units_; }
    const std::vector<Projectile>& projectiles() const { return projectiles_; }
    const std::vector<FlameShot>& flames() const { return flames_; }
    Unit* unit(int id);
    const Unit* unit(int id) const {
        return const_cast<World*>(this)->unit(id);
    }
    // Whether a unit's retail object still lives (+0x130 & 0x1000000): alive,
    // or dead with its retirement still to come.
    static bool retailObjectLives(const Unit& u) {
        return u.type && (u.alive() || (u.deathPending && u.deadFor<kRetiredTicks));
    }
    // unit(id) without its lookup-table repair, so it never writes: safe on
    // SimPool workers, with the same result.
    Unit* unitNoRepair(int id);
    const Unit* unitNoRepair(int id) const { return const_cast<World*>(this)->unitNoRepair(id); }

    // Weapon impacts this tick (view-side: impact sound + effect). Carries the
    // source weapon (soundhitclass / hweffect) and the unit struck (body material
    // for the material-specific hit sound). Cleared at the start of each tick.
    struct HitFx { float x = 0, z = 0; const Weapon* weapon = nullptr;
                   const UnitType* target = nullptr;
                   // Every unit this impact damaged, splash recipients included: the
                   // under-attack alert's input (TotalA.exe 0x489da2 -> 0x406f80 per
                   // damage record). Cosmetic, not hashed.
                   std::vector<int> hurtUnits;
                   int victimId = 0;            // primary struck unit (0 = ground hit)
                   float fromX = 0, fromZ = 0;  // attacker pos for hitscan presentation
                   int fromPlayer = 0;         // display colour for hitscan projectile models
                   std::optional<std::array<int32_t,3>> position, fromPosition;
                   // A status weapon's impact (529c10 with its effects flag clear)
                   // plays the hit sound but no explosion; a spell that lands adds
                   // a silent event with the explosion on the victim (492c80).
                   bool sound = true, explosion = true; };
    const std::vector<HitFx>& hits() const { return hits_; }
    const std::vector<int>& recordHurt() const { return recordHurt_; }
    // Cosmetic events captured at the mission callback, before mover updates.
    struct TransportFx {
        uint32_t tick = 0;
        std::array<int32_t,3> passenger{}, carrier{};
    };
    const std::vector<TransportFx>& transportEffects() const { return transportEffects_; }
    // Extended COB effects capture their origin at the emitting instruction.
    // Display events do not participate in the lockstep hash or consume RNG.
    struct ScriptEmission {
        uint32_t tick=0;
        int unitId=0,player=0,piece=0;
        int32_t code=0;
        std::array<int32_t,3> position{};
        std::vector<cob::EmissionPose> pose;
        std::array<std::array<int32_t,3>,2> vertices{};
        uint16_t heading=0,pitch=0,roll=0;
    };
    const std::vector<ScriptEmission>& scriptEmissions() const { return scriptEmissions_; }
    // PLAY_SOUND from a dying unit's VM: Killed, Dying and any thread still
    // running in it. Its RAND draws the shared game stream, so this is the cry
    // every peer, replay and spectator hears; the client plays these instead of
    // its display VM's own death sounds. Presentation only, like the emissions:
    // not hashed, no RNG, cleared every tick. `name` indexes type->script()->names.
    struct ScriptSound {
        uint32_t tick=0;
        int unitId=0,player=0;
        Fixed x,z;
        const UnitType* type=nullptr;
        int32_t name=0,flags=0;
        // The native host's visibility inputs at the call: the unit position
        // +0x68 (ground or flight height), and whether LosOn hides it from
        // other owners: cloaked (+0x114&4) or carried (+0x130&3).
        Fixed y;
        bool hidden=false;
    };
    const std::vector<ScriptSound>& scriptSounds() const { return scriptSounds_; }


    // A mission script asking for a camera shake (the ScreenShake map command).
    // Viewer-only: the sequence number is what the client watches for an edge, and
    // it is deterministic because the script that bumps it runs in lockstep.
    struct ShakeReq { float mag = 0, dur = 0; uint32_t seq = 0; };
    const ShakeReq& shakeRequest() const { return shakeReq_; }
    // Mission audio is an ordered presentation event stream, not latest-only
    // snapshot state: two lines in one tick and skipped render frames must not
    // discard requests. Bounded storage also serves headless worlds with no viewer.
    struct SoundReq { std::string name; uint32_t seq = 0, tick = 0; int32_t flags = 7; };
    const SoundReq& soundRequest() const { return soundReq_; }
    void requestSound(std::string n, int32_t flags = 7) {
        soundReq_.name=std::move(n);++soundReq_.seq;
        soundReq_.tick=tickCounter_;soundReq_.flags=flags;
        if(pendingSounds_.size()>=256)pendingSounds_.erase(pendingSounds_.begin());
        pendingSounds_.push_back(soundReq_);
    }
    std::vector<SoundReq> takeSoundRequests() {
        std::vector<SoundReq> out;out.swap(pendingSounds_);return out;
    }
    void requestShake(float mag, float dur) {
        shakeReq_.mag = mag; shakeReq_.dur = dur; ++shakeReq_.seq;
    }
    void clearHits() { hits_.clear(); }

private:
    bool canPlaceProduction(int builderId,const UnitType* type,float x,float z) const;
    // `site`: a builder's site, where a mobile type is tested on its footprint
    // cells like a building (canPlaceSite) instead of canPlace's centre radius.
    bool placementCheck(const UnitType* type, float x, float z, int player,
                        std::vector<int>* clearFeatures,const std::vector<int>* candidates=nullptr,
                        int ignoreId=0,bool ignoreMobile=false,int yieldPlayer=-1,bool site=false) const;
    // The player's own units in a build site step aside for its builder
    // (engine deviation, user-requested; retail's MobileBuild only waits for
    // them, 405824..405911). A unit yields when it is the player's, mobile,
    // able to move and idle or guarding (its guard resumes afterwards).
    bool yieldsToSite(const Unit& k, int player) const;
    void clearBuildSite(const Unit& builder, const UnitType* type, Fixed x, Fixed z);
    // A conjurer standing on its own site walks to the site's edge first:
    // the player's idle units on the perimeter cell it can step to, beside
    // the site and so not clearBuildSite's, make room the same way.
    void clearSiteExit(const Unit& builder, const RetailRectGoal& rectangle);
    // A mobile producer with a rally has no room for its next output: one idle
    // unit walks out of the area the output may be made in. (x,z) is a scripted
    // producer's pad; the unscripted search has no point of its own.
    void clearProductionRoom(Unit& producer, const UnitType* type, Fixed x, Fixed z);
    // A factory's output waits in or just out of its yard (or its next site is
    // refused): the player's own idle units in front of the yard, on the
    // output's plane, move aside, out of the door's way (engine deviation,
    // user-requested; retail moves no idle body, see clearYardDoor).
    void clearYardDoor(const Unit& factory);
    bool doorGuard(const Unit& k) const;
    bool pointOutside(const Unit& k, int left, int right, int top, int bottom, int rx, int rz,
                      const std::array<bool,4>& avoid, const std::vector<std::array<int,4>>& taken,
                      int& x, int& z) const;
    void leaveFactoryYard(int helperId);
    void tickCombat(Unit& u, float dt, bool& groundMovementHandled);
    // False when no shot left (a charm it cannot take, or a non-script shot
    // it cannot pay for).
    // A scripted TA shot's adjustments (tickScriptWeapon): the accuracy
    // spread added to the launch heading and pitch (0x49d6bc..0x49d73e), and
    // a burst clone, which charges nothing (docs/retail-weapon-scripts.md 5).
    // free: a burst clone (already paid with its emitter, 0x49b75e).
    struct ShotTweak { uint16_t heading=0,pitch=0; bool free=false; };
    bool fire(Unit& u, Unit& target, int slot,bool scriptTriggered=false) { return fire(u,WeaponTarget{&target},slot,scriptTriggered); }
    bool fire(Unit& u, const WeaponTarget& t, int slot,bool scriptTriggered=false) { return fire(u,t,slot,scriptTriggered,ShotTweak{}); }
    bool fire(Unit& u, const WeaponTarget& t, int slot,bool scriptTriggered,ShotTweak tweak);
    // TotalA.exe 0x49e1a0 for one slot of a unit with a COB script: AimX
    // start, the fire gate, the shot, FireX, RockUnit and the reload
    // (docs/retail-weapon-scripts.md 7.4). True when the unit has a script
    // (the caller's unscripted fallback is then skipped). `given` is unused.
    bool tickScriptWeapon(Unit& u,Unit& target,int slot,bool swept=false,const WeaponAimSolution* given=nullptr) {
        return tickScriptWeapon(u,WeaponTarget{&target},slot,swept,given);
    }
    bool tickScriptWeapon(Unit& u,const WeaponTarget& t,int slot,bool swept=false,const WeaponAimSolution* given=nullptr);
    // 0x43e2e0: the AimFromX piece's world point (-1: QueryX's instead).
    std::array<int32_t,3> aimFromPoint(int unitId,int slot);
    // 0x48a1e0's point for a slot target: a unit's SweetSpot (led by a
    // veteran of more than 5 kills, unless cruise), or the point as it stands.
    std::array<int32_t,3> taTargetPoint(Unit& u,const WeaponTarget& t,const Weapon& w);
    // 0x49e264..0x49e2cc: AimX's heading (relative to the body) and pitch
    // from `from` to `to`; none when the weapon has no solution.
    std::optional<std::array<uint16_t,2>> taAimAngles(const Unit& u,const Weapon& w,
        const std::array<int32_t,3>& from,const std::array<int32_t,3>& to) const;
    // Start a TA weapon call-in for a local weapon slot: AimX / FireX /
    // QueryX / AimFromX by the FBI slot the local one came from.
    enum class TaWeaponCall : uint8_t { Aim, Fire, Query, AimFrom };
    int taWeaponScript(const UnitType& t,TaWeaponCall call,int slot) const;
    // Burst emitters (0x49b75e..0x49b9a9): a burst weapon's shot clones
    // itself every burstrate ticks until its count runs out. Hashed.
    struct BurstEmitter {
        int fromId=0,slot=0,targetId=0;
        std::array<int32_t,3> point{};
        uint16_t left=0,rate=1;
        uint32_t next=0;
    };
    void tickWeaponBursts();
    // SUPPRESS (0x404d70): the order's five stages on the current leg, run
    // ahead of the unit's combat; true while it owns the body this tick.
    bool tickSuppress(Unit& u,bool& groundMovementHandled);
    // 52ae90 for the slots that hold a point (Unit::slotPoint): aim, fire,
    // and 52ffd3's refusal (event 0x100000) when the point is out of reach
    // or inside the minimum range.
    void tickPointWeapons(Unit& u);
    // 52ae90 for the manual slots of a flyer's unit attack while the
    // formation its group check put ahead of it leads: they keep the target,
    // aim and fire per reload on the way back.
    void tickAttackWalkWeapons(Unit& u,Unit& target);
    // The walk back an attack's group check put ahead of it (formationAttack),
    // after the weapons: SUPPRESS's on the ground, a flyer attack's in the
    // air, then the turn an idle navigator makes to its facing request
    // (4d9b6a). False once the formation has retired (the attack runs on).
    bool tickAttackWalk(Unit& u);
    void setSlotPoint(Unit& u,int slot,RetailGroundPoint p);   // 51a770
    void dropSlotPoint(Unit& u,int slot);                      // 51a7f0 on a point
    // 530630 / vtable +0x10 (530500, BallisticWeapon 52bbb0, MeleeWeapon
    // 52b8a0) for a point target's aim point.
    bool slotPointInsideMinRange(const Unit& u,const std::array<int32_t,3>& aim,int slot) const;
    bool slotReachesPoint(const Unit& u,const std::array<int32_t,3>& aim,int slot) const;
    // 50e660 + feature +0x13c: a blocking (0x20), clearable (0x20000) feature
    // in the point's cell (a tail followed back to its anchor).
    // `footprint`: the feature's anchor cell and size (4930d0).
    bool clearableBlockerAt(Fixed x,Fixed z,std::array<int,4>* footprint=nullptr) const;
    int suppressStage(Unit& u,Order& o,RetailMissionState& m,uint32_t events);
    void pushSuppressApproach(Unit& u,Fixed x,Fixed z,int32_t outer,int32_t inner);
    // The approach ring of a SUPPRESS approach leg (Order::suppressInner).
    std::optional<RetailRingGoal> suppressRing(const Unit& u,const Order& goal) const;
    void clearScriptWeaponTarget(Unit& u,bool fromCommand=false);
    // Convert a unit to another player (contact charm + mind-control weapons).
    void captureUnit(Unit& t, int newPlayer);
    // Retail's change of owner 514da0, under ShareUnits, the CRT transfer, the
    // mission Capture and the charm: the old object retires with death kind 6
    // and a new one is made for the new owner (World keeps the id). `notify`
    // hears the capture first (523570). False when nothing changed.
    bool changeOwner(Unit& u, int newPlayer, MissionScript* notify);
    // The old object of a change of owner is gone for every other unit that
    // named it (512860 -> 519a60): their order legs, rally steps and weapon
    // slots on it are pointed at kGoneUnit, whoever owns them.
    void retireReferences(int unitId);
    // A new object's standing orders: its type's (511990 +0x130 bits 30-31,
    // +0x134 cleared), or Defensive under a defensive AI.
    void startObjectOrders(Unit& u);
    // The experience of the new object retail makes for a unit (512060): none
    // (511990 zeroes +0xf0 and +0xf4), then its type's scenario level (4cd5f4).
    void startObjectStats(Unit& u);
    // TotalA.exe's death accounting (the death packet's switch 0x486892, at the
    // death): the owner's loss, the hitter player's kill and the killer unit's
    // kill count, by TA death kind (retailTaDeathCountsAsLoss / retailTaKillCounts).
    void countTaDeath(Unit& victim, uint8_t taKind);
    // Retail's retirement 512ae0 for a unit whose death has started.
    void retireDeath(Unit& u);
    // 51a1c0 / 51a281: the hitter a hit names, only while its object lives. A
    // unit that changed sides is a new object to retail (514da0), so a shot
    // fired for its old player names nobody, though World keeps its id.
    const Unit* retailHitter(int id,int player) const {
        const Unit* u=id ? unit(id) : nullptr;
        return u && retailObjectLives(*u) && u->player==player ? u : nullptr;
    }
    // Apply a weapon's damage at (hx,hz), as retail's impact dispatch 529c10
    // does: a weapon with areaofeffect <= 16 that struck `primary` hits it
    // alone; any other impact damages every unit near the 3D `impact` point
    // (default `position`, else hx/hz at the primary's or the ground's height)
    // by its distance to their bounding boxes. `position` is display geometry.
    // `slot`: the shooter's weapon slot when the hit resolves a shot it fired
    // (4099e0 posts the shot's outcome to it); -1 for everything else.
    void applyHit(const Weapon& w, float hx, float hz, int fromPlayer, int fromId, Unit* primary,
                  std::optional<std::array<int32_t,3>> position = {},
                  std::optional<std::array<int32_t,3>> impact = {}, int slot = -1);

    void tickProduction(Unit& u, float dt);
    // One product's shared progress (see the definitions): the producer's own
    // work and every helper's advance the same counter; finishing releases the
    // output and frees the producer, once.
    int32_t productionTotal(const Unit& producer, const UnitType* t) const;
    bool workProduction(Unit& producer, Unit& site, const UnitType* t, uint32_t step);
    void finishProduction(int producerId, Unit& site);
    // BuildingBuild's abort (0x401c20, flag 2): a dying factory takes its
    // unfinished output with it, and so does cancelling that output. Mobile
    // producers and builders do not. `now` retires it at once (a cancel or a
    // scripted kill); otherwise the death sweep takes it this pass.
    void killProductionOutput(Unit& producer, bool now = false);
    void tickGuardHelp(Unit& u);
    void tickFlyingGuard(Unit& u);
    void guardHelpPick(Unit& u, const Unit& g, int& help, int& mend);
    void guardInsertJob(Unit& u, int help, int mend);
    void pushGuardFormation(Unit& u);
    RetailRingGoal guardRing(const Unit& u, const Unit& g) const;
    bool guardInDoorLane(const Unit& u, const Unit& g, int64_t x, int64_t z) const;
    void guardDoorPoint(const Unit& u, const Unit& g, Fixed& x, Fixed& z) const;
    Order& guardStandAside(Unit& u);
    bool guardAlarm(const Unit& u, const Unit& g) const {
        return g.attackedTick>u.guardHeard;
    }
    bool guardDefend(Unit& u, const Unit& g);
    void helpApproach(Unit& b, const Unit& site, bool queue);
    void initializeRetailSite(Unit& site, int builderId);
    void tickRetailAiBase(int owner,unsigned index);
    void tickRetailAiStrike(int owner,unsigned index);
    // Where a newly produced unit is sent: the nearest free, occupiable spot to the
    // factory's exit. See the definition -- the fixed five-point fan it replaced is
    // what made units walk at each other for ever.
    bool exitSpot(const UnitType* t, float fx, float fz, float& outX, float& outZ, int departingId = 0) const;
    // GetBuilt's PARK release of a factory's finished output. See the definition.
    void parkProductionOutput(int id,int factoryId);
    // Legion's spread PARK: a free spot for a factory output (see sim.cpp).
    static constexpr int kLegionParkLayers=16;
    // Half the width of a factory's door lane, px: about its narrow side.
    static int legionParkLaneHalf(const UnitType& factory) {
        return std::max(48,8*std::min(factory.footX,factory.footZ));
    }
    static bool legionParkInLane(const RetailParkState& park,const UnitType& factory,
                                 int64_t x,int64_t z,int fx,int fz,int margin);
    std::optional<std::pair<int,int>> legionParkSpot(const Unit& u,const RetailParkState& park,
                                                     const Unit& factory) const;
    // A flyer's PARK, and its point once its move first runs. See the definitions.
    void queueFlyerPark(int id,int factoryId,uint32_t padding);
    void resolveFlyerPark(Unit& u,Order& leg);
    // Route a move/attack/patrol order aimed at a production BUILDING into its rally.
    // True when it was consumed that way. See the definition.
    static bool setRally(Unit& u, const Order& o, bool queue);
    bool tickTransport(Unit& u, float dt);
    // The carrier's transport missions (TotalA.exe GROUND_PICKUP 0x406780,
    // VTOL_PICKUP 0x4111b0, VTOL_UNLOAD 0x411560) on the order at `leg`.
    bool tickGroundPickup(Unit& u, size_t leg);
    bool tickVtolPickup(Unit& u, size_t leg);
    bool tickVtolUnload(Unit& u, size_t leg);
    // ATTACH_UNIT (0x481340 -> 0x48aac0 -> 0x48ab70) of `passenger` to
    // `carrier`, and DROP_UNIT's release (0x4813b0 -> 0x48aac0 with no
    // carrier) at (x, z).
    bool canAttachCargo(const Unit& carrier, const Unit& passenger) const;
    void attachCargo(Unit& carrier, Unit& passenger, int piece = -1);
    // A surface carrier's TransportPickup / TransportDrop (the call-in runs
    // at once, TotalA.exe 0x4b0a70 with its immediate flag); false without
    // such a script.
    bool scriptedTransport(const Unit& carrier, bool drop) const;
    void startTransportScript(Unit& carrier, bool drop, uint32_t passenger, uint32_t point);
    bool scriptBusy(int unitId) const;
    // ATTACH_UNIT / DROP_UNIT (0x481340 / 0x4813b0) from the carrier's script.
    void scriptAttach(Unit& carrier, uint32_t passenger, int32_t piece);
    void scriptDrop(Unit& carrier, uint32_t passenger);
    // Where a carried unit rides: its carrier's attach piece (4dd250).
    std::array<int32_t,3> cargoAttachPoint(const Unit& carrier, const Unit& cargo) const;
    void releaseCargo(Unit& carrier, Unit& passenger, Fixed x, Fixed z);
    // An air carrier's transport controller, flown this tick: arrival posts
    // 0x100 to `mission` (and, for a point controller, 0x400 as it lets go).
    void flyTransportGoal(Unit& u, Order& o, RetailMissionState& mission);
    // 0x44e6c0: a point controller's height, `altitude` px over the ground or
    // the sea at (x, z) (0x485070), at most 511.
    int32_t transportGoalHeight(Fixed x, Fixed z, int altitude) const;
    // Drop a queued path search whose orders have been replaced. See the definition.
    void cancelPath(Unit& u);
    void tickConstruction(Unit& u, float dt);
    void tickConjureHover(Unit& builder, const Unit& site, bool repair = false);
    void tickFlyingRepair(Unit& b, Unit& target, float dt);
    bool payRepair(Unit& b, Unit& target, float dt);
    void tickRetailConstruction(Unit& u);
    void emitConstruction(Unit& u,bool rising);
    void tickRetailGetBuilt(Unit& u);
    void completeRetailConstruction(Unit& builder,Unit& site);
    void notifyUnitScript(Unit& u,const char* name);
    void notifyUnitScript(Unit& u,const char* name,uint32_t argument);
    void startWorkAnimation(Unit& u,Fixed targetX,Fixed targetZ);
    void stopWorkAnimation(Unit& u);
    void notifyMovementRate(Unit& u);
    // Retail's writers of unit +0xcc (shots, construction, repair, reclaim and
    // revive work): max(+0xcc, now + ticks), so the cloak step keeps the unit
    // visible until then. Written for cloakers only, the step's only reader.
    void blockCloak(Unit& u, uint32_t ticks) {
        if (!u.type || !u.type->canCloak) return;
        u.cloakBlockedUntil = std::max(u.cloakBlockedUntil, tickCounter_ + ticks);
        // A writer after this tick's cloak step stands for one before it in
        // retail's unit update (missions and weapons precede 0x401160).
        u.cloaked = false;
    }
    void updateLegionStill(Unit& u, Fixed beforeX, Fixed beforeZ);
    // Each unit's position at the start of the tick, by id (Legion only),
    // before Legion's own start-of-tick steps (yields). Derived each tick,
    // never hashed. A unit spawned during the tick has none (generation).
    std::vector<std::array<int32_t,2>> legionTickStart_;
    std::vector<uint32_t> legionTickStartAt_;
    // Each Legion ground unit's headway window (see updateLegionStill), by
    // id. Hashed while valid.
    std::vector<LegionHeadway> legionWindow_;
    // Each Legion ground unit's second window, judged by the turn its body
    // shows (updateLegionStill): it alone sets Unit::legionStill. Its ticks
    // (0: the next tick starts one) live here; its livelock flag is unused.
    // Hashed while valid.
    struct LegionBodyWindow {LegionHeadway w;uint8_t ticks=0;bool shuffle=false;};
    std::vector<LegionBodyWindow> legionBodyWindow_;
    void notifyFlightOccupancy(Unit& u);
    bool prepareBuildApproach(Unit& u);
    bool resumeBuild(Unit& u);   // a Ctrl-interrupted build leg back on its site (Order::resumeSite)
    Order makeBuildOrder(const Unit& builder,const UnitType* type,Fixed x,Fixed z) const;
    // One tick of an abandoned site's decay (429d70): it gives back
    // 0.5 / buildtime of its build and that much of its cost to its owner, and
    // dies (kind 11, no corpse) once no work is left to give back.
    void decayConstruction(Unit& u);
    // `worker` worked `site` this tick (429af0's 0x10000000 event).
    void noteSiteWork(const Unit& worker, Unit& site);
    // Retail Reclaim (406250) once its leg is current: the approach events,
    // the stance wait and the two-tick work countdown, then the removal.
    void tickReclaim(Unit& b);
    // 406250 stage 0: install the reclaim leg's rectangle controller.
    void prepareReclaimApproach(Unit& u);
    // Ground HelpBuild (405cf0) and RepairUnit (407190) on their legs: stage 0
    // installs the rectangle controller round the target's footprint (the
    // dispatch, like prepareReclaimApproach); tickGroundHelp/tickGroundRepair
    // answer the approach events and do the work, measuring no distance once
    // it has started. A leg that is not current waits from its start.
    void prepareHelpApproach(Unit& u);
    void prepareRepairApproach(Unit& u);
    void installRepairApproach(Unit& u, Order& leg, const Unit& target);
    // Attack_Melee (404690) stage 0 for a target with no mover: the same
    // rectangle controller round the target's footprint (4d4e80).
    void installMeleeApproach(Unit& u, const Unit& target);
    void tickGroundHelp(Unit& b, Unit& site, size_t leg, float dt);
    void tickGroundRepair(Unit& b, Unit& target, float dt);
    bool constructionWork(Unit& b, Unit& site, float dt);
    // The target's MoveRate tier (+0x130 & 0xc, 4db350) is not stopped: it
    // walks, flies or turns. A unit without a script: its speed.
    bool moveRateMoving(const Unit& t) const;
    void finishReclaim(Unit& b,int target);
    void tickFeatureFades();
    void tickReclaimArea(Unit& b);
    void tickAbilities(float dt);   // reclaim / resurrect on nearby corpses

    // Connected-component labelling of a nav grid for one footprint, built on first
    // use and reused until the grid's walkability version moves.
    //
    // const with a mutable cache, and that is a correctness argument rather than a
    // convenience: the labelling is a PURE MEMO of (nav grid, footprint) -- its content
    // never depends on when or whether it was built -- so the server-side AI populating
    // an entry that clients never ask for cannot perturb sim state. Single-threaded per
    // world. See docs/multiplayer-design.md.
    //
    // (This paragraph documented flowField(), which is gone; the const-memo reasoning is
    // what survived it, and it applies to this cache for the same reason.)
    // `alias` is a union-find over label ids, and it is what lets an UNBLOCK edit skip
    // the whole-map relabel. Unblocking can only ever MERGE components, and merging by
    // rewriting one component's cells is O(map); merging two ids is O(1). Cells keep
    // whatever id they were first given and root() resolves it, so two cells are in the
    // same component iff root(a) == root(b) -- which is the ONLY question either caller
    // asks. Never compare raw labels.
    struct CompGrid {
        uint64_t ver = 0; int w = 0, h = 0;
        uint64_t checkedVer = ~uint64_t(0);   // TA_COMPCHECK: last version verified

        // ASK THROUGH THESE. The stored ids are NOT comparable: an unblock merges two
        // components by uniting their ids rather than rewriting a map of cells, so two
        // cells can share a component while still carrying the different ids they were
        // first given. A raw `==` is right until the first merge and silently wrong
        // afterwards -- reachability would start depending on whether a passage had ever
        // been open, which is history, not geometry.
        //
        // That is not hypothetical: the comment alone did not prevent it. The incremental
        // split check compared raw ids and shipped, and a reopened-then-reclosed passage
        // stayed "open" until review caught it (navincr_test covers it now). Hence the
        // raw vector is private and the id never leaves this struct resolved.
        int32_t componentAt(int x, int z) const {          // -1 where nothing fits
            if (x < 0 || z < 0 || x >= w || z >= h) return -1;
            return root(raw_[size_t(z) * size_t(w) + size_t(x)]);
        }
        bool empty() const { return raw_.empty(); }

    private:
        // Only components() and the two incremental updaters assign ids; everything else
        // goes through componentAt(). friend rather than public so the unresolved id has
        // no route out of this struct.
        friend class World;
        PackedWhenKept<int32_t> raw_;   // megabytes a grid as an array, kilobytes as the runs a keyframe holds
        std::vector<int32_t> alias;   // id -> parent id; identity after a full build
        int32_t root(int32_t id) const {
            if (id < 0) return id;
            while (id < int32_t(alias.size()) && alias[size_t(id)] != id) id = alias[size_t(id)];
            return id;
        }
        void unite(int32_t a, int32_t b) {
            a = root(a); b = root(b);
            if (a < 0 || b < 0 || a == b) return;
            // Lower id wins, so the outcome does not depend on argument order.
            if (a < b) alias[size_t(b)] = a; else alias[size_t(a)] = b;
        }
    };
    mutable std::map<std::pair<const NavGrid*, int>, CompGrid> compCache_;
    const CompGrid* components(const NavGrid& g, int foot) const;
    // Fast path for a walkability edit that only BLOCKS cells. Blocking can only ever
    // SPLIT a component, never merge one, and a split is provable locally: if every
    // still-passable cell around the edit that shared a label still reaches the others
    // without crossing the edit, then any route that used those cells can be rerouted
    // around them, so every label stays correct and only the cells that stopped fitting
    // need clearing to -1. When the window cannot show it, resolveSplit() settles it
    // over the whole grid; false only when that runs over budget, leaving the entry
    // stale for a full relabel.
    bool tryIncrementalBlock(const NavGrid& g, int foot, CompGrid& cg,
                             int x0, int z0, int x1, int z1) const;
    // The pieces a block may have cut one component into, settled without a whole-map
    // relabel: one breadth-first flood per piece, run in turns, so pieces that meet are
    // proven joined and a piece that runs out of cells is proven cut off. Stops as soon
    // as at most one piece is still growing, so the cost is the SMALLER side, not the
    // map. fits() is read from `okw` inside the edit's window [wx0,wx1]x[wz0,wz1] and
    // from the grid outside it. Appends the cells of every cut-off piece but one to
    // `cut` (each needs a fresh id); false when the floods exceed their budget.
    bool resolveSplit(const NavGrid& g, int foot, const std::vector<std::vector<int>>& seeds,
                      int wx0, int wz0, int wx1, int wz1, const std::vector<uint8_t>& okw,
                      std::vector<std::vector<int>>& cut) const;
    // The whole-grid flood fill behind components(): `cg` relabelled from scratch.
    static void labelComponents(const NavGrid& g, int foot, CompGrid& cg);
    // TA_COMPCHECK (debug): recompute the whole labelling from scratch and abort unless
    // it partitions the cells exactly as `cg`'s roots do.
    void verifyComponents(const NavGrid& g, int foot, const CompGrid& cg) const;
    // (Sized on use, so a copy of the world does without them.)
    mutable LeftEmptyOnCopy<std::vector<uint32_t>> compMark_;   // resolveSplit's flood marks, by epoch
    mutable uint32_t compMarkTop_ = 0;
    mutable CompStats compStats_;
    // The mirror of the above for an edit that only CLEARS cells. Unblocking can only
    // merge components, never split one, so there is nothing to prove: give each newly
    // passable cell a label (an adjacent component's, or a fresh id if it stands alone)
    // and unite whatever components it now bridges. Always succeeds.
    bool tryIncrementalUnblock(const NavGrid& g, int foot, CompGrid& cg,
                               int x0, int z0, int x1, int z1) const;
    // A SEPARATE cache for non-sim queries (pathExists, which only the server-side
    // AI calls). Keeping it apart is not an optimisation, it is a correctness
    // requirement: the AI runs on ONE peer, so letting its questions insert into and
    // evict from the sim's memo made cache membership differ between peers. That was
    // invisible while the sim cache never reached its cap and desynced the referee
    // inside 60 ticks once it did.

    // Uniform spatial hash over mobile units, rebuilt each tick, so the
    // separation and combat-acquisition passes are O(n) instead of O(n^2).
    void rebuildGrid();
    // (rebuildOccupancy declared with occ_ below.) Both in one walk; returns the
    // count of alive units with a type (embarked ones included).
    uint32_t rebuildGridAndOccupancy();
    uint32_t rebuildSpatial(bool grid, bool occupancy);
    // rebuildSpatial on the SimPool (WE E4.3, P6): the chunks read the units
    // into spatialRec_, then serial walks over those records stamp and link in
    // unit order, so every chain, stamp and list is the serial one.
    uint32_t rebuildSpatialParallel(bool grid, bool occupancy);
    uint32_t rebuildSpatialSerial(bool grid, bool occupancy);
    static constexpr size_t kSpatialMinParallel = 2048;
    struct SpatialRec {
        float x, z;                 // u.x/u.z as float (grid units only)
        int32_t id;
        int16_t ox, oz;             // footprint origin (occupancy units only)
        uint8_t fx, fz;
        uint8_t player;             // 0xff: not representable in the 64-bit owner masks
        uint8_t kind;               // bit 0: in the grid; bits 1-2: 1 moving body, 2 parked body
    };
    std::vector<SpatialRec> spatialRec_;
    struct SpatialChunk { float minx, minz, maxx, maxz; uint32_t live; bool any; };
    std::vector<SpatialChunk> spatialChunks_;
    // Call fn(int unitIndex) for every mobile unit whose cell lies within
    // `radius` of (x,z). Iterates cells in a fixed order, so it is deterministic.
    //
    // Cells are visited row-major (dz outer, dx inner) and a cell whose owner mask has
    // none of `players` is passed over. When the masks are valid, a whole 8x8-cell block
    // whose mask (the OR of its cells' masks) has none of `players` is jumped in one step
    // (WE A2b): every cell in it would fail the per-cell test anyway, so the cells
    // visited -- and the units, in the same order -- are exactly those of the plain scan.
    // gPlayersValid_ is re-read at every cell, as before: a capture inside fn turns the
    // masks off for the rest of the scan. TA_VERIFY_NEAR (debug) checks the sequence
    // against the plain scan on every call.
    // There is deliberately no geometric cull (A2c: drop cells whose box lies outside a
    // search disk): units stay in the cell rebuildGrid bucketed them in while they move
    // during the tick's unit loop, so a cell's box does not bound its units' positions.
    // R-2h and L-2h each have a findTarget candidate in range whose cell was more than
    // 2 px outside the disk.
    template <class F>
    void forEachNear(float x, float z, float radius, F&& fn, uint64_t players=~uint64_t(0)) const {
        if (gW_ <= 0) return;
#ifndef NDEBUG
        if (probe::kStats) probeNear(x, z, radius, players, false);
        static const bool verify = probe::envOn("TA_VERIFY_NEAR");
        if (verify) verifyNear(x, z, radius, players);
        if (probe::kStats) { scanNear<true, true>(x, z, radius, players, fn); return; }
#endif
        scanNear<true, false>(x, z, radius, players, fn);
    }
    // forEachNear's cell walk; kBlockSkip=false is the plain square scan (TA_VERIFY_NEAR's
    // reference), kCount counts the cells tested (TA_SIMSTATS near_cells_tested).
    template <bool kBlockSkip, bool kCount, class F>
    void scanNear(float x, float z, float radius, uint64_t players, F&& fn) const {
        int r = int(radius / gCell_) + 1;
        int cx = int((x - gOx_) / gCell_), cz = int((z - gOz_) / gCell_);
        for (int dz = -r; dz <= r; ++dz) {
            int gz = cz + dz;
            if (gz < 0 || gz >= gH_) continue;
            for (int dx = -r; dx <= r; ++dx) {
                int gx = cx + dx;
                if (gx < 0 || gx >= gW_) continue;
                if (kBlockSkip && gPlayersValid_ &&
                    !(gBlockPlayers_[size_t(gz >> 3) * size_t(gBlockW_) + size_t(gx >> 3)] & players)) {
                    dx += 7 - (gx & 7);   // the loop's ++dx lands on the next block's first column
                    continue;
                }
                const size_t cell=size_t(gz)*gW_+gx;
#ifndef NDEBUG
                if (kCount) ++probe::tl.nearCellsTested;
#endif
                if (gPlayersValid_ && !(gPlayers_[cell]&players)) continue;
                for (int i = gHead_[cell]; i >= 0; i = gNext_[size_t(i)])
                    fn(i);
            }
        }
    }
    // Per-movement-class nav. navClasses_ holds one grid per distinct limit tuple;
    // navIdx_ maps a type to its grid. Derived from terrain + the registry, so it is
    // not hashed -- but it MUST be identical on every peer, which it is: the table
    // is built in a deterministic order from the same data.
    // (PackedWhenKept, here and on the other per-cell state maps -- occupancy, ground owners, sight counts, the
    // placement cells, the search grades: arrays as ever in a world that runs, runs in a keyframe; World::keyframe.)
    PackedWhenKept<uint8_t> obst_;   // shared obstacle overlay (see NavGrid::setObstacles)
    // A sacred yard (S) may stand on fixed obstacles (placementCheck): footprint
    // origin cell -> the overlay cells already blocked when it was stamped, so its
    // removal (blockFoot) leaves those obstacles blocked. Not hashed: derived from
    // the same placements on every peer, and only the overlay reads it.
    // A blocker the sim can remove later (a registered feature, or a wreck).
    bool removableBlockerAt(int gx,int gz) const;
    // The map placement cell still holds a blocking feature or a 0xFFFC cell.
    bool fixedBlockerAt(int gx,int gz) const;
    std::vector<NavGrid> navClasses_;
    std::unordered_map<const UnitType*, int> navIdx_;
    // The same grids by limit tuple (World::navLimits): navFor's fallback.
    std::map<std::tuple<int,int,int,int,int,int,int,int>,int> navKeyIdx_;
    int navClassIndex(const UnitType& t) const;
    struct SearchGradePlane {
        const NavGrid* nav=nullptr;
        int footX=1,footZ=1;
        uint64_t navVersion=0;
        RetailGradePreparation preparation;
        PackedWhenKept<uint8_t> cells;
        uint64_t checksum=0;
    };
    std::vector<SearchGradePlane> searchGrades_;
    int activeSearchGrade_=-1;
    // Eligibility/footprints/stamps cannot change during the synchronous path
    // scheduler. Share one sorted snapshot only within that explicit scope;
    // replay/probe calls outside it still sample current bodies each time.
    std::vector<RetailGradeBody> searchGradeBodies_;
    uint64_t searchGradeBodyRebuilds_=0; // profiling only, never hashed
    bool searchGradeBatch_=false,searchGradeBodiesValid_=false;
    struct SearchGradeBatch {
        World& world;
        explicit SearchGradeBatch(World& w):world(w) {
            world.searchGradeBatch_=true; world.searchGradeBodiesValid_=false;
        }
        ~SearchGradeBatch() {
            world.searchGradeBatch_=false; world.searchGradeBodiesValid_=false;
        }
        SearchGradeBatch(const SearchGradeBatch&)=delete;
        SearchGradeBatch& operator=(const SearchGradeBatch&)=delete;
    };
    // Occupant per rectangle cell. Footprint-sized rectangles (the placement
    // and grade queries made per step) stay inline instead of allocating.
    class BodyCells {
    public:
        static constexpr size_t kInline=64;
        BodyCells()=default;
        explicit BodyCells(size_t count):size_(count) {
            if (count>kInline) heap_.assign(count,nullptr);
            else std::fill_n(inline_.begin(),count,nullptr);
        }
        BodyCells(const BodyCells& other):size_(other.size_),heap_(other.heap_) {
            std::copy_n(other.inline_.begin(),other.size_>kInline ? 0 : other.size_,inline_.begin());
        }
        BodyCells& operator=(const BodyCells& other) {
            size_=other.size_;heap_=other.heap_;
            std::copy_n(other.inline_.begin(),other.size_>kInline ? 0 : other.size_,inline_.begin());
            return *this;
        }
        size_t size() const { return size_; }
        bool empty() const { return !size_; }
        const Unit** begin() { return size_>kInline ? heap_.data() : inline_.data(); }
        const Unit** end() { return begin()+size_; }
        const Unit* const* begin() const { return size_>kInline ? heap_.data() : inline_.data(); }
        const Unit* const* end() const { return begin()+size_; }
        const Unit*& operator[](size_t i) { return begin()[i]; }
        const Unit* operator[](size_t i) const { return begin()[i]; }
        bool operator==(const BodyCells& other) const {
            return std::equal(begin(),end(),other.begin(),other.end());
        }
    private:
        size_t size_=0;
        std::array<const Unit*,kInline> inline_;
        std::vector<const Unit*> heap_;
    };
    struct SearchBodyRect {
        int x=0,z=0,width=0,height=0;
        BodyCells cells;
    };
    // Inside World::tick (set for its whole extent). Between ticks the lockstep
    // driver applies commands: Legion queues the navigator events those raise and
    // replays them at the start of its next tick (Legion 10,000 stage 4). Derived
    // from the call context, never hashed.
    bool inTick_=false;
    bool kept_=false;   // a keyframe(): its packed maps are not arrays until restored from
    // Tick-local broad phase. Buckets keep conservative candidates, including
    // dead/flying bodies; exact current eligibility and yards are checked below.
    bool bodyIndexEnabled_=false;
    mutable bool bodyIndexValid_=false;
    mutable int bodyTilesW_=0,bodyTilesH_=0;
    mutable std::vector<std::vector<int>> bodyTiles_;
    mutable std::vector<size_t> bodyUsedTiles_;
    mutable std::vector<std::array<int,4>> bodyTileBounds_;
    mutable std::vector<std::array<int,4>> bodyFootprints_;
    void rebuildBodyIndex() const;
    void updateBodyIndex(const Unit&) const;
    SearchBodyRect searchBodyRect(int,int,int,int,int ignoreId=0) const;
    // Retail's ground word +0, kept by events as retail keeps it: the id of
    // the body that holds each cell (0 = none). A body placed over a live
    // holder leaves it the cell (5066f0), and a cell is handed on only when
    // its holder leaves (506aa0 restamps the bodies it refused). Consumers
    // take the holder among the bodies that cover a cell (searchBodyRect).
    PackedWhenKept<int32_t> groundOwner_;
    uint32_t groundLinkSerial_=0;
    bool groundHolderLive(int id) const;
    bool groundStampsCell(const Unit&,int dx,int dz) const;
    bool groundClaimCell(Unit&,size_t cell);
    void groundStamp(Unit&);                 // 5066f0
    void groundUnstamp(Unit&);               // 506aa0
    void groundRemove(Unit&);                // 5066a0
    void groundRestamp(Unit&);               // 5062d0
    void groundRestampRect(int,int,int,int); // 5065b0 -> 5097f0
    // 51b3b0's cell or mode change: unstamp, then stamp at the current spot.
    void groundMove(Unit& u) { groundUnstamp(u); groundStamp(u); }
    void groundRebuild();
    int cellScoreWithBodies(const UnitType*,int,int,int,const SearchBodyRect*) const;
    bool gatePassageAt(int,int,const std::vector<int>* gates=nullptr) const;
    bool gateWantsOpen(const Unit&,const std::vector<int>& gates) const;
    void tickAutomaticGates();
    int rawSearchGrade(const SearchGradePlane&,int,int,int,int,const SearchBodyRect* = nullptr) const;
    static void setSearchCell(SearchGradePlane&,size_t,uint8_t);
    void refreshSearchRect(SearchGradePlane&,int,int,int,int);
    void refreshSearchRectPerCell(SearchGradePlane&,int,int,int,int);
    int searchCellGrade(const NavGrid&,int,int,bool) const;
    void buildSearchPlane(SearchGradePlane&);
    void prepareSearchGrade(int,bool);
    void finishSearchGrade(int);
    int searchGrade(int,int,int,int,PathCell) const;
    void refreshMovingSearchBody(Unit&);

    // Derived occupancy: rebuilt each tick, then updated after every cell change.
    // Both moving and parked bodies reserve their whole footprint (0 = free).
    PackedWhenKept<int32_t> occ_;
    std::vector<size_t> occTouched_;
    std::vector<uint32_t> occParked_;   // rebuildOccupancy's scratch: parked bodies, unit order
    void setOccupant(size_t cell,int id) {
        if (id && !occ_[cell]) occTouched_.push_back(cell);
        occ_[cell]=id;
    }
    int occW_ = 0, occH_ = 0;
    RetailCostSearch::Costs searchCosts(const Unit& u) const;
    bool requestPath(Unit& u, float x, float z);   // true iff a search was queued
    static Order* groundMissionOrder(Unit& u, bool includeCombatMoves = false);
    static bool groundMissionAccepts(const Unit&,const Order&);
    // attackWalk: the call tickCombat makes, after the weapons, for a
    // formation leg SUPPRESS put ahead of itself (the main loop's skips it).
    void tickGroundMission(Unit& u,bool attackWalk=false);
    // 4d856a: an empty mission list gets the FBI default mission (Standby /
    // VTOL_Standby), even while VTOL_UNLOAD's step-out controller still flies.
    bool standbyEligible(const Unit& u) const;
    void beginStandby(Unit& u);
    Order* navigationMissionOrder(Unit& u);
    void tickGuardNoMove(Unit& u);
    void tickFlightMovement(Unit& u, bool persistent = false);
    void tickRetainedFlightMovement(Unit& u);
    void tickFlightBody(Unit& u);
    // The flyer attack missions take a null target for an attack at a
    // point (Order::airPoint, tickAirPoint): mission +0x16 is 0.
    void tickAirToGroundHover(Unit& u, const Unit* target, const Weapon* weapon, int slot);
    void flyHoverGoal(Unit& u, Order& order, const Unit* target);
    void tickAirToGround(Unit& u, const Unit* target, const Weapon* weapon);
    void tickAirToAir(Unit& u, const Unit& target, const Weapon* weapon);
    void tickAirStrike(Unit& u, const Unit* target);
    // A flyer's attack at a point at the front (Order::airPoint): its
    // mission, then the mover.
    void tickAirPoint(Unit& u);
    // The lost-target exit (41c8f6 / 41e28f / 41be1b) of an attack at a
    // point: VTOL_SEEKATTACK at the point, with no target, when the flyer is
    // alive and not dying, the attack is its last mission and not on Hold
    // Fire. Called once the attack has gone, when those held.
    void seekLostPoint(Unit& u, Fixed x, Fixed z);
    bool seekLostTarget(Unit& u, const Unit* target, bool targetLive);
    void tickSeekAttack(Unit& u, bool carriesTarget = false);
    // VTOL_Follow's lost ward (TotalA.exe 0x410341) and VTOL_SeekGuard (0x410850).
    bool seekLostWard(Unit& u);
    void tickSeekGuard(Unit& u);
    int seekGuardFind(const Unit& u) const;
    void restartFlyingHelp(Unit& b, const Unit& site);
    void flyMissionGoal(Unit& u, Order& order);
    bool flyerTakeOff(Unit& u, Order& o, bool wait = true);
    // The repair pads (TotalA.exe VTOL_Landing 0x4118e0, SelfRepair 0x402430):
    // a pad mission at the head (Order::airPad), the detour that pushes one
    // (0x40b530's pads under 3/4 health), and the pad's attach list.
    static bool airPadFront(const Unit& u) { return !u.orders.empty() && u.orders.front().airPad; }
    bool airPadDamaged(const Unit& u) const;
    int airPadPick(Unit& u);
    int airPadDetour(Unit& u);
    void pushAirPadLanding(Unit& u, int padId);
    void tickAirPad(Unit& u);
    void flyAirPadGoal(Unit& u, Order& order);
    std::array<int32_t,4> queryLandingPads(Unit& pad);
    bool padPieceFree(const Unit& pad, int piece) const;
    void attachToPad(Unit& pad, Unit& u, int piece);
    void detachFromPad(Unit& u);
    bool onAirPad(const Unit& u) const;
    void tickFlightPatrol(Unit& u);
    int flightGround(const Unit& u) const;
    bool flightLandingFree(const Unit& u, Fixed x, Fixed z) const;
    bool flyerLandingOccupied(const Unit& self, int x0, int z0, int fx, int fz) const;
    // A flyer standing on the ground for Legion's purposes (W6, PLAN 3.6):
    // landed (mode 1), or descending in the landing mission's stage 2 (TotalA.exe
    // VTOL_LandIfCan 0x40f3e4).
    static bool flyerGrounded(const Unit& f) {
        return f.flightGroundMode==1 || (f.landing && f.landing->mission.stage==2);
    }
    std::pair<int,int> cellHeightRange(size_t cell) const;
    // A fight-move leg: attackMove, and off a patrol Move response mode -1
    // with the leg's point as its goal (retail's MoveFight order, 5214b0).
    static void markFightLeg(Order& o);
    bool combatLineOfSight(const Unit& from, const Unit& to) const;
    bool canCaptureTarget(int player, const Unit& target) const;
    bool canAttackTarget(const Unit& from, const Unit& target) const;
    // --- retail weapon-slot layer (fire-at-will sweep, icd 40f330) ----------
    void setSlotsAuto(Unit& u);                    // 519b10(3)
    void setSlotsManual(Unit& u,uint8_t cause);    // 519b70(3)
    void setSlotMode(Unit& u,int slot,bool automatic);   // 519b10/519b70 on one slot
    void dropSlotTarget(Unit& u,int slot);         // 51a7f0
    void setSlotTarget(Unit& u,int slot,int id);   // 51a700
    void syncSlotMode(Unit& u);
    bool slotInRange(const Unit& u,const Unit& t,int slot) const;
    // 52bc60 (BallisticWeapon::inRange) for a shooter standing at (x,y,z).
    bool ballisticInRange(Fixed x,Fixed y,Fixed z,const Unit& t,const Weapon& w) const;
    bool ballisticInRange(const Unit& u,const Unit& t,const Weapon& w) const;
    int approachSlot(const Unit& u) const;
    // A ground mover's attack (Attack_Chase) approaches on its order slot alone.
    static bool chasesOnOrderSlot(const Unit& u);
    // tickCombat's reach to a target at centre distance `dist`: the weapon it
    // approaches on (approachSlot), and whether the target is within it.
    // `pad` is the structure footprint pad the non-ballistic reach adds.
    struct CombatReach { int slot; const Weapon* weapon; bool arc; float best; float pad; bool in; };
    CombatReach combatReach(const Unit& u,const Unit& t,float dist) const;
    // Does `u` fighting `t` as a whole (approach, acquisition, the drop of a
    // target it may not chase) need the 2D clear-line test? Its approach
    // slot's weapon decides (Weapon::needsClearLine); a flyer on either side
    // never does.
    bool approachNeedsClearLine(const Unit& u,const Unit& t) const;
    bool slotInsideMinRange(const Unit& u,const Unit& t,int slot) const;
    bool slotBadTarget(const Unit& u,int slot,const Unit& t) const;
    bool slotClearShot(const Unit& u,const Unit& t,int slot) const;
    // Blocked shots (engine deviation, see noteShotImpact in sim.cpp).
    // Called after the shot's applyHit, with the target's hit points from before it.
    void noteShotImpact(int fromId,int targetId,const Weapon& w,const std::array<int32_t,3>& impact,int hitUnit,Fixed targetHp,
                        bool refused=false);
    bool statusShotRefused(const Weapon& w,int player,int targetId,int struckId) const;
    void noteNoArc(Unit& u,const Unit& t);
    bool impactIndestructible(const Weapon& w,int cx,int cz) const;
    bool shotBlockedHere(const Unit& u,const Unit& t) const;
    // Engine deviation: a shot of `w` would strike the water before `t`'s aim
    // point (its SweetSpot is below the surface: a wading unit swimming).
    bool aimPointSubmerged(const Weapon& w,const Unit& t) const;
    bool statusCanTake(const Weapon& w,const Unit& t) const;
    bool weaponServes(const Unit& from,const Weapon& w,const Unit& target) const;
    bool aiSlotServes(const Unit& u,const Weapon& w,const Unit& t,int slot,bool fromHere) const;
    bool shotChordClear(const Unit& u,Fixed fromX,Fixed fromZ,const Unit& t,int slot);
    bool chordClear(const std::array<int64_t,3>& a,const std::array<int64_t,3>& b,const Weapon& w,
                    int64_t skipFrom,int64_t skipTo,const Unit& to) const;
    bool repositionForShot(Unit& u,const Unit& t,int slot);
    bool hoverForShot(Unit& u,const Unit& t,int slot,const std::array<int32_t,3>& centre,int distance,int altitude);
    bool sweepKeepsTarget(const Unit& u,int slot,const Unit& t) const;
    void tickFireSweep(int player);
    void tickSlotWeapons(Unit& u);
    void slotHitResponse(Unit& victim,const Unit& attacker);
    // A guard's slot retarget at its guarded unit's attacker (Follow_Ground
    // 0x4081fa, VTOL_Follow 0x418c60): 409a50 part (b) without the attacker's
    // reach test; a flyer's keeps a current target inside its minimum range.
    void guardSlotRetarget(Unit& u,const Unit& attacker);
    // 51a8b0 (SwitchWeapon): the command and AI paths share it. fromCommand
    // routes the clear/switch animations through pendingWeaponAnimations.
    bool switchWeaponSlot(Unit& u,int slot,bool fromCommand);
    // 531ea0: a slot's manapershot over the veteran multiplier (5193c0).
    // 52af72..52af9a: after a release, a switcher that cannot pay for its
    // weapon again drops to a cheaper slot.
    // 40f411..40fb92: a computer-owned switcher's per-visit weapon choice.
    void retailAiWeaponSwitch(Unit& u);
    // 409a50: the victim's response to a hit -- the AI Monarch block, the
    // idle retaliation mission (part a) and, failing that, the per-slot
    // retarget (part b).
    void retailHitResponse(Unit& victim,const Unit* attacker,int dmgType);
    // The SelfDestruct mission's hit (40188a) and its countdown runs; each
    // answers whether the unit is now dying.
    bool selfDestructHit(Unit& u);
    bool tickSelfDestruct(Unit& u);
    // The status callbacks' roll (52da10 charm, 52dca0 stone, 52df00 freeze):
    // chance min(99, (veteran level + 16) * 5), so a green unit falls 80 times
    // in 100 and a level-4 veteran 99; one CRT rand() scaled to 0..99
    // (rand()*100/32768: _allmul 5d3c60, _alldiv 5d4470) must fall below the
    // chance times the area falloff, truncated (x87 product, _ftol). `origin`
    // is the rand() call site, for the RNG observer.
    bool retailStatusRoll(const Unit& victim,float scale,uint32_t origin);
    // 4d7de0 with explicit=0, auto=1: insert an auto attack, leashed with a
    // return move on the Maneuver/patrol path. Whether orders were inserted.
    // forced (explicit=1, auto=0, the guards' call): neither the Hold
    // Position nor the Hold Fire gate, no leash or return move, and the
    // attack is not an auto target.
    bool retailIssueAutoAttack(Unit& u,int targetId,bool forceReturn,bool forced=false);
    std::vector<std::vector<int>> sweepIds_;   // per-player id-ordered scratch; not state
    std::vector<int> pickCandidates_;          // retailSelectTarget's candidate list; scratch, not state
public:
    // Why a unit's slots are manual (Unit::slotManual).
    enum : uint8_t { kSlotsAuto=0, kSlotsAttack=1, kSlotsWork=2, kSlotsStatus=3 };
    // Port of 412620: one weapon slot's target, by retail's 50-draw sampled
    // score. sweep=true is the fire-at-will radius (the weapon's range).
    int retailSelectTarget(Unit& u,int slot,bool sweep);
    // 519ac0: the slot a mission scan picks for -- a switcher's selected
    // slot when it is below 3, else the first (World packs the present
    // slots, so the first is 0). -1 for an unarmed unit.
    int retailScanSlot(const Unit& u) const;
    // Port of 4d8370, the mission scan: an armed unit on Fire At Will runs
    // the picker (412620) on its scanning slot with the mission radius,
    // max(sightdistance, range); anything else gets 0. Whether the unit's
    // current mission is one that scans stays the host's test.
    int retailMissionScan(Unit& u);
    // How a mission turns its scan's hit into an attack: the arguments its
    // call site passes to 4d7de0 (unit, target, explicit, point, auto,
    // forceLeash) (docs/retail-engine.md, "Mission target scan").
    enum class MissionAttack : uint8_t {
        Standby,     // (0, &pos, 1, 0): Standby 40784e (the hit response 409e4b too)
        Patrol,      // (0, &pos, 1, 1): Patrol 403972: a forced leash and return move
        Vtol,        // (0, 0, 1, 0): VTOL_Standby 4176cc and the air missions' retargets
        VtolPatrol,  // (0, 0, 1, 1): VTOL_Patrol 41a25e
        Forced,      // (1, ., 0, 0): an explicit attack, the guards' answer
    };
    // A reject applied to the single pick before the gate (Standby_Wander's
    // post, a Move's goal, a formation point, a melee origin): a target at or
    // beyond `leash` from `point` (retailGroundDistance) is dropped, and no
    // rescan follows.
    struct MissionLeash { RetailGroundPoint point; int32_t leash=0; };
    // The mission's tail on a pick: the leash filter, then 4d7de0 as `how`
    // says (retailIssueAutoAttack). An idle flyer's VTOL attack goes in ahead
    // of its default mission. Whether orders were inserted.
    bool retailMissionAttack(Unit& u,int targetId,MissionAttack how,
                             const std::optional<MissionLeash>& leash={});
    // The host's stand-in for "the unit's current mission is one that scans"
    // (a powered-down switchable, a neutral owner, a builder at or holding
    // work are in another mission in retail), then 4d8370.
    int retailHostScan(Unit& u);
    // 4d7de0's refusals, without inserting anything: whether
    // retailIssueAutoAttack(u, target, forceReturn, forced) would issue.
    bool retailIssueAutoAttackAllowed(const Unit& u,int targetId,bool forceReturn,bool forced=false) const;
    // The stored-attacker answer shared by the hit response (409ba8..409e4b)
    // and the response of Move_Ground (402f..403) and VTOL_Move (4181c1..
    // 4184a5): the attacker passes the scanning slot's category masks, is in
    // the slot's reach (52ad60) or, for a mover, nearer than 9/10 of
    // maneuverleashlength plus the slot's range (a melee slot adds none), and
    // the slot can hurt it on its current layer.
    bool retailAnswersAttacker(const Unit& e,const Unit& a,int slot,bool mobile) const;
    // Move_Ground's and VTOL_Move's fallback when the scan finds nothing: the
    // stored attacker (+0xf8, lastHitBy) alive, finished and not allied,
    // through retailAnswersAttacker. No sight, cloak or fire-state test.
    int retailMoveFallback(const Unit& u) const;
    // The attack a mission builds itself (4de530 kind 3, then 4d6c40(kind,
    // target, 0, 0, mode, leash) with origin +0x2e/+0x30): Move_Ground,
    // Move_Ground_Formation, VTOL_Move and Standby_Wander. mode != 0 is an
    // automatic chase; a leash retires it that far from `origin`. Not
    // inserted: the caller inserts it once its own mission state is safe.
    std::optional<Order> retailBuiltAttack(const Unit& u,int targetId,bool automatic,uint16_t leash,
                                           RetailGroundPoint origin) const;
    // Standby_Wander's scan (40797e): 4d8370, the Maneuver post's leash on
    // the pick, then the attack it builds (no return move). Whether it went in.
    bool retailWanderScan(Unit& u);
    // Guard_NoMove's armed step (401a90): no pick. When a shot of a slot
    // posted 0x200000 << slot to the mission event word (4099e0: it did the
    // other side no more than twice what it did its own), rand(3) == 0
    // releases each such slot, and the bits clear on either draw; then each
    // present slot (a switcher's selected one alone) whose target is gone,
    // inside its minimum range (52adf0) or out of reach (52ad60, plus the
    // engine's clear-shot test, slotClearShot) is released. Release is 519b70 then 519b10: the slot ends automatic with
    // no target, back to the sweep. `draw` >= 0 stands in for the rand(3)
    // (the oracle's); otherwise pathRand, Guard_NoMove's own stream.
    void retailGuardNoMoveSlots(Unit& u,int draw=-1);
    // Attack_Melee's retarget (4049a0) for a ground melee walker closing on
    // an automatic attack's target: at each stage-2 timer wake. True when the
    // front order took another target (the caller returns).
    bool retailMeleeRetarget(Unit& u);
    // Port of 4f7210 (LosOn): `player` sees `u` when it owns it; otherwise
    // never when u is cloaked, carried or dead, else when any of the four
    // projected bounding-box corners lies on a cell `player`'s current-view
    // counters hold. Without terrain (no view plane) every placed unit is seen.
    bool playerSees(int player,const Unit& u) const;
    // Owner kind 2 (computer AI): an AI seat, the campaign's strategic
    // players, the benchmark (Player::automaticGates, hashed).
    bool computerOwned(int player) const {
        return player>=0 && size_t(player)<players_.size() && players_[size_t(player)].automaticGates;
    }
    // 411700 list +5 as of the last 7-tick rebuild (may hold units that have
    // since died or left sight; callers re-check alive and distance).
    std::span<const int> visibleEnemies(int player) const {
        if (player<0 || size_t(player)>=players_.size()) return {};
        return players_[size_t(player)].visibleEnemies;
    }
    // The pool size N the sweep budget N/30+1 is computed from when the
    // restored retail entity pools are not in use.
    static constexpr int kRetailSweepPool=250;
private:
    void rebuildOccupancy();
    bool canFollowTraffic(const Unit& self, const Unit& other) const;
    // Is the footprint free of any other body? Terrain bounds are checked by
    // the caller. The centre anchor matches NavGrid::fits and the route scorer.
    bool cellFree(Fixed nx, Fixed nz, int selfId, int foot = 1) const {
        if (occW_ <= 0) return true;
        int cx = footprintOrigin(nx, foot), cz = footprintOrigin(nz, foot);
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
    std::vector<size_t> gTouched_;
    // Conservative owner mask for rejecting all-allied target cells. Capture
    // invalidates it until the next rebuild; linked-list order stays unchanged.
    std::vector<uint64_t> gPlayers_;
    // The same masks ORed over 8x8-cell blocks (cell (gx,gz) is in block (gx>>3,gz>>3)),
    // gBlockW_ blocks per row; rebuilt with gPlayers_ and valid exactly when it is.
    std::vector<uint64_t> gBlockPlayers_;
    int gBlockW_ = 0;
    bool gPlayersValid_=false;
    int gW_ = 0, gH_ = 0;
    float gCell_ = 32.0f, gOx_ = 0, gOz_ = 0;
#ifndef NDEBUG
    // TA_SIMSTATS (simprobe.h) only. probeNear walks forEachNear's cells a second time and
    // counts the plain scan's cells and what the A2 culls skip or would skip: an 8x8-cell
    // block whose owner mask has none of the wanted players, and, for findTarget (disk), a
    // cell whose box lies wholly outside the search radius + 2 px.
    void probeNear(float x, float z, float radius, uint64_t players, bool disk) const;
    // TA_VERIFY_NEAR: the unit sequence of the block-skipping scan must equal the plain
    // square scan's (every in-bounds cell, per-cell mask only); throws if not.
    void verifyNear(float x, float z, float radius, uint64_t players) const;
#endif

    CopyableAtomic<uint32_t> featGen_{0};   // see featGeneration()
    void bumpFeatGen() { featGen_.fetch_add(1, std::memory_order_release); }

    bool doubleSight_ = false;  // synchronized skirmish/MP rule; campaigns use authored sight
    bool roomMapRevealed_ = false, roomLosOff_ = false;   // setRoomVision
    // Power codes (setPowerCodes / applyPowerCode). Hashed while powerCodes_ is on.
    bool powerCodes_ = false;
    DamageMode damageMode_ = DamageMode::Normal;   // DoubleShot / HalfShot: every player's damage
    uint16_t powerLosOff_ = 0, powerMappingOff_ = 0;   // per-player bits
    bool patrolRepairs_ = true;
    // Raw TNT heightmap. Like slope_, depth_ and installedMapFeatures_, set with the
    // terrain and never written again: a world and its copies share it (SharedArray).
    SharedArray<uint8_t> heights_;
    std::vector<RetailExplorationHeight> explorationHeights_;
    std::vector<uint16_t> navigationExplored_; // simulation-owned persistent owner bits
    // 4c6800's per-player current-view counts (player +0x88) on the same
    // projected 32px grid, player-major. uint16 instead of retail's wrapping
    // byte: our unit caps make 256 overlapping footprints reachable; equal to
    // retail below that. Hashed.
    PackedWhenKept<uint16_t> viewCounts_;
    // 4c6750/4c6b30: a dying human-owned unit's footprint stays counted for
    // 60 ticks (at most 20 per player), in the planes it was counted in.
    // Hashed, in order.
    struct RetailDeathSight { RetailSightFootprint sight; int8_t player=-1; uint32_t expiry=0; uint16_t planes=0; };
    std::vector<RetailDeathSight> deathSights_;
    bool viewCountsByRebuild_ = false;   // setViewCountsByRebuildForTest; not hashed
    std::vector<uint8_t> viewDirty_;   // viewDirty(); display bookkeeping, not hashed
    int viewDirtyW_ = 0;
    void markViewDirty(const RetailSightFootprint& sight);   // the squares it covers (relaxed stores)
    void markViewDirtyAll();
    void rebuildVisibleLists(int player);
    int restoredNavigationViewer_=-1; // captured query context; live matches use the unit owner
public:
    // Corpse lifecycle, in ticks. Public because the RENDERER shares the contract:
    // it decides the death-animation window and the corpse cull from the same
    // numbers the sim counts with.
    static constexpr int32_t kRetiredTicks = 1000 * 30;    // record explicitly retired
    // Terrain accessors for offline analysis tools (tools/footprobe): the raw
    // heightmap and its dimensions. Read-only; nothing in the sim uses these.
    const std::vector<uint8_t>& mapHeights() const { return heights_.values(); }
    int mapW() const { return terW_; }
    int mapH() const { return terH_; }
    int mapSea() const { return seaLevel_; }
private:
    int hW_ = 0, hH_ = 0;
    std::vector<Unit> units_;
    // IDs remain monotonic; storage slots may move after final corpse retirement.
    std::vector<int32_t> unitSlotById_;
    // A fully retired id and its last owner (folded into the hash once): a lookup
    // of one answers nothing. No unit/VM state survives final retirement.
    std::unordered_map<int,int> retiredOwners_;
    uint64_t retiredHash_=0;
    void compactRetiredUnits();
    LeftEmptyOnCopy<std::unique_ptr<MissionScript>> mission_;   // optional campaign mission runner
    // Mission orders' state (ScriptedOrder), indexed by Order::scripted - 1.
    // Appended in order-creation order on every peer; hashed through the
    // orders that refer to it. Full (16-bit index), it is compacted: entries
    // no order refers to go and the rest are renumbered in unit order.
    std::vector<ScriptedOrder> scriptedOrders_;
    uint16_t addScriptedOrder(const ScriptedOrder& s);
    void compactScriptedOrders();
    // The head order's mission when it is a scripted AttackType, Train or
    // SelfDestruct (before the unit's combat), and the scripted waits' ends.
    void tickScriptedOrder(Unit& u);
    bool scriptedWaitDone(Unit& u);
    // A factory finished a unit of `t`: a scripted Train at its head counts it.
    void noteScriptedProduction(Unit& producer, const UnitType* t);
    LeftEmptyOnCopy<std::unique_ptr<ScenarioScript>> scenario_; // optional .crt trigger runner
    std::vector<uint8_t> forcedDefeat_;        // scenario Victory/Defeat: forced-defeated slots
    // Units retired (512ae0) since the last feed to the mission/scenario, which
    // hear a death at retirement (523510, +0x18); hashed while pending.
    std::vector<int> justDied_;
    // The old objects of changes of owner (514da0, changeOwner) whose death
    // (kind 6, retired at once) the mission and scenario have yet to hear:
    // their id (which the new object keeps), type, old owner and last hitter's
    // player. Consumed with justDied_; one made after that waits a tick.
    struct ReplacedObject { int id; const UnitType* type; int owner, lastHitPlayer; };
    std::vector<ReplacedObject> replacedObjects_;
    std::vector<std::pair<float, float>> manaSpots_;
    std::vector<Feature> features_;             // reclaimable map features
    PackedWhenKept<RetailMapFeatureCell> mapPlacementCells_;
    SharedArray<uint16_t> installedMapFeatures_;   // load-time survivors (installedMapFeatures)
    std::vector<RetailMapFeatureType> mapPlacementTypes_;
    struct CorpseFootprint { int x=0,z=0,type=-1; int32_t damage=0; };
    std::unordered_map<int,int> corpseAnchors_; // derived anchor -> unit ID, for weapon hits
    std::map<int,CorpseFootprint> corpseFootprints_; // unit id -> installed feature anchor
    // Water graves (512ee0's -0x2ccc velocity), by unit id: the feature
    // record's motion and fade, and whether it has left the moving list
    // (494a80: settled, never updated again). Hashed.
    struct CorpseSink { RetailFeatureSink motion; bool settled=false; };
    std::map<int,CorpseSink> corpseSinks_;
    std::vector<FeatType> featTypes_;           // per-type burn data (setup-time, static)
    std::unordered_map<const UnitType*, int> corpseType_;   // unit -> corpse FeatType
    std::unordered_map<const UnitType*, int> stoneType_, frozenType_;   // statue defs
    // Retail's game stream (0x535cc0 / 0x64186c). Consumers below share it;
    // matching the full retail call order and match seed remains a parity task.
    uint32_t initialGameRng_ = retailSeed(0);
    uint32_t gameRng_ = initialGameRng_;
    RetailWind wind_;
    float wind_tidal_ = econ::kTidalDefault;
    float scoreKillMul_ = 0, scoreTimeMul_ = 0;
    uint8_t surfaceMetal_ = 0;
    std::vector<uint8_t> cellMetal_;   // empty until a feature stamps metal
    int cellMetalW_ = 0, cellMetalH_ = 0;
    void taEconomyTick();
    void taSettle(int player);
    void taShare();
    bool windEnabled_ = false;
    // Wind and restored construction particles share retail's CRT stream.
    // Allocation and render-cadence consumers are not yet fully integrated.
    uint32_t windRng_ = 0, initialWindRng_ = 0;
    bool damageCrtUsed_ = false;
    std::function<void(const RngObservation&)> rngObserver_;
    std::function<void(const RngObservation&)> crtRngObserver_;
    bool retailAllocation_=false;
    std::optional<std::array<std::pair<int,int>,10>> retailEntityPools_;
    uint64_t spawnGeneration_=0; // transient mutation detection; not gameplay state
    // Retail's per-player path-budget class (player +0x24e7): a flagged player
    // draws five scheduler shares (0x4164a0-0x416517). 0x4f6290 sets the byte
    // for EVERY player it initializes (0x4f6379), so in skirmish and multiplayer
    // every seat -- human or AI -- is flagged; every saved retail capture shows
    // it set for all ten slots. Only a campaign mission overrides it, from its
    // Player<N> entry (0x4c83fb: "strategic" -> 1, else 0; stored at 0x4d2a12);
    // the human, player 0, keeps 1. Consumed in PathfindingMode::Retail only.
    uint32_t pathBudgetClassMask_ = ~0u;
    // Builders whose queued build order has come due this tick. startBuild spawns
    // the site, which can REALLOCATE units_, so it must never be called while the
    // per-unit loop holds a Unit& -- that reference dangles the moment it returns.
    // Collected in unit order (deterministic) and drained after the loop.
    std::vector<int> buildDue_;
    uint32_t gameRand(int32_t n, std::source_location caller = std::source_location::current()) {
        const uint32_t before = gameRng_;
        const uint32_t result = retailRandom(gameRng_, n);
        if (rngObserver_) rngObserver_({tickCounter_, n, before, gameRng_, result, caller});
        return result;
    }
    uint32_t crtRand(uint32_t origin,std::source_location caller=std::source_location::current()) {
        const uint32_t before=windRng_,result=retailCrtRandom(windRng_);
        if (crtRngObserver_) crtRngObserver_({tickCounter_,32768,before,windRng_,result,caller,origin});
        return result;
    }
    uint32_t fireRand(uint32_t n, std::source_location caller = std::source_location::current()) {
        return gameRand(int32_t(n), caller);
    }
    int burnRand(int n, std::source_location caller = std::source_location::current()) {
        return int(gameRand(n, caller));
    }
    uint32_t pathRand(uint32_t n, std::source_location caller = std::source_location::current()) {
        return gameRand(int32_t(n), caller);
    }

    void igniteFeature(Feature& f);
    void forgetCorpseAt(int cx,int cz);
    bool placeCorpse(Unit& unit,int type);
    void retireCorpse(Unit& unit);
    // taKind: the TA death kind each removal counts as (0: nothing; the
    // commander rule passes 3, TotalA.exe 0x486f10).
    void removeScenarioUnits(int player, uint8_t taKind = 0);
    void removeMapFeature(int cx,int cz);
    bool placeMapFeature(const Feature& f,uint8_t owner=kRetailWorldFeatureOwner);
    // A FLYING reclaimer's acceptance (VTOL_RECLAIM 420xxx is not ported):
    // within 24 px plus half the target plus builddistance of its centre.
    bool flyerReclaimInReach(const Unit& b,const Order& leg) const;
    // Resolve a reclaim target to its installed plane anchor and footprint
    // (a corpse: its corpse feature). False when it is gone.
    bool reclaimTargetFootprint(int target,int& x,int& z,int& fx,int& fz,float& energy) const;
    // Retail's per-wake re-lookup (4930d0 -> 496ea0): still reclaimable. For
    // a RESURRECT leg (`revive`) the body need only still lie there: the kind
    // it was ordered as depends on the two types alone, which do not change.
    bool reclaimTargetValid(int target,bool revive=false) const;
    // The unit a RESURRECT leg on `target` (its reclaimFeat) raises: the
    // feature's FeatType::raises (TotalA.exe 0x404f36), else -- a body whose
    // corpse feature names none -- the dead unit's own type.
    const UnitType* raisedType(int target) const;
    void swapFeature(Feature& f, int newType);   // chain stage swap (burnt/dead)
    void destroyFeature(Feature& f);   // weapon damage reached its `damage` (494ce0 mode 0)
    void tickBurning();
    void tickFeatureDeaths();   // seqnamedie playback ends: featuredead (495ef7)
    std::unordered_map<int, size_t> featureIdx_;   // feature id -> index in features_
    std::vector<Projectile> projectiles_;
    std::vector<BurstEmitter> bursts_;
    std::vector<FlameShot> flames_;
    void tickFlames(std::span<const int> airGrid);
    void tickStraightProjectiles(std::span<const int> airGrid);
    // TotalA.exe 0x49b9c2..0x49bb5b plus the collision test 0x49b090: one
    // selfprop/vlaunch shot's tick.
    void tickTaSelfProp(Projectile& p,std::span<const int> airGrid);
    std::vector<HitFx> hits_;
    // Units a damage record other than a weapon hit's asked the under-attack
    // alert for this tick (0x4071d8, taHitResponse). Cosmetic, not hashed.
    std::vector<int> recordHurt_;
    std::vector<TransportFx> transportEffects_;
    std::vector<ScriptEmission> scriptEmissions_;
    std::vector<ScriptSound> scriptSounds_;
    ShakeReq shakeReq_;
    bool noSeaLevelTrigger_ = false; // OTA: projectile/effect sea-surface bypass.
    int32_t ballisticGravityRaw_ = 8155; // retail default OTA gravity 112, in native 16.16/tick².
    int32_t mapGravity_ = 112;           // the OTA gravity integer (setMapGravity)
    int32_t waterDamage_ = 0;   // .ota waterdamage when waterdoesdamage=1
    SoundReq soundReq_;
    std::vector<SoundReq> pendingSounds_;
    // [EXPLODEAS] blasts queued during the death sweep and applied just after it
    // (applyHit mutates units_, which the sweep is walking). Transient within one
    // tick -- always empty at tick end, so it needs no hashing.
    // 52ac00 detonates at the dying unit's 3D position: a flyer's blast is in the air.
    // source: the retired unit. 512ae0 fires the blast (52ac00) before it
    // places the corpse (512ee0), so the blast never reaches that corpse;
    // World places it in the sweep and drains the blast after, sparing it
    // (sparedCorpse_, read by applyHit only while the blast lands).
    struct DeathBlast { const Weapon* w; std::array<int32_t,3> position; int player, fromId, source=0; };
    std::vector<DeathBlast> deathBlasts_;
    int sparedCorpse_=0;
    // Remote Effect (FBI type=Remote Effect): the effect materialises at the AIMED
    // GROUND POINT after `builduptime`, then applies its damage/status/conversion
    // once over areaofeffect. Earthquakes, monarch waves, god spells, Area Mind
    // Control. Lives across ticks, so it IS hashed.
    struct PendingEffect {
        const Weapon* w = nullptr;
        Fixed x = Fixed(), z = Fixed();   // 16.16, the same world units as a unit
        Fixed y = Fixed();                // the aimed point's height: the area pass is 3D
        int player = 0, fromId = 0;
        // TICKS, like every other retail timer (tools/re/emufields.py).
        int32_t at = 0;      // ticks until the next pulse
        int32_t endAt = 0;   // ticks until the effect expires
        int32_t period = 0;  // ticks between pulses (<=0 = a single pulse)
        bool casterGated = false;   // dies with its caster (mind control / freeze)
    };
    std::vector<PendingEffect> pendingEffects_;
    // Wandering (FBI type=wandering): a storm entity that roams for `duration`,
    // drifting at weaponvelocity and wobbling its heading every `variationtime`.
    // Damages everything it passes on a fixed cadence. Hashed.
    // Per unit id: the tick and the pace (raw Fixed px a tick) the mover was
    // given, for its Legion headway window. Scratch, rewritten every tick.
    std::vector<std::pair<uint32_t,int32_t>> legionPace_;
    bool scoreAutomaticDisabled_=false;
    int scenarioParticipantCount_ = -1; // ninth owner is scenery, never a lobby participant
    std::map<std::string, std::array<int,3>> scenarioTypeStats_;
    std::vector<Player> players_ = []{
        std::vector<Player> v(4);
        for (int i = 0; i < 4; ++i) v[size_t(i)].team = i;
        return v;
    }();
    int winningTeam_ = -1;
    bool monarchExpendable_ = true;      // default: Monarch is just a unit (net option overrides)
    // Derived, unhashed: per 8x8-cell block, the AND of navigationExplored_,
    // so fully explored blocks are skipped. Every writer of the plane keeps it
    // equal to that AND (the exploration pass per block it changes,
    // applyPowerCode per bit) or empties it (setTerrain, resetForReplay,
    // editNavigationExploration); the next pass rebuilds an emptied one whole.
    static constexpr int kExplorationBlockShift=3;
    std::vector<uint16_t> explorationBlocks_;
    // Scratch per SimPool slot: the blocks a worker summarized during a pooled
    // exploration pass, summarized again exactly after the join. A block is
    // listed once per pass (its flag, per block, is set while it is listed).
    std::vector<std::vector<uint32_t>> explorationTouched_;
    std::vector<uint8_t> explorationTouchedFlags_;
    bool serialThreads_ = false;
    int simWorkers_ = -1;
    // What AUTO came to (SimPool::defaultWorkers()) when simPool() first needed it
    // since setSimWorkers; -1 until then. Kept, and this World's own over
    // restoreFrom, so a session's width is fixed whatever the CPU mask does later.
    int autoWorkers_ = -1;
    bool simPin_ = false;          // setSimCpus: the pool's workers are pinned ...
    std::vector<int> simCpus_;     // ... to these, by slot
    // The intra-tick worker pool (0 workers when serialThreads_), created on
    // first use; see simPool().
    LeftEmptyOnCopy<std::unique_ptr<SimPool>> pool_;
    SimPool& simPool();
    std::vector<uint8_t> hadMonarch_;   // per-player: ever fielded a Monarch (for the loss rule)
    bool buildRestrictionsEnabled_ = false;
    std::vector<std::string> buildRestrictions_; // lowercase, sorted, unique
    int unitCap_ = 0;                 // per-player live-unit limit (0 = unlimited)
    void startEmote(int player, int unitId, bool disco, bool append);
    uint32_t tickCounter_ = 0;   // ticks elapsed; staggers per-unit auto-acquisition
    uint64_t burnSequence_ = 0;  // display order for active feature burns; not lockstep state
    uint32_t fadingFeatures_ = 0; // features with fadeLeft > 0 (tickFeatureFades walks only then)
    // Derived marks, one bit per features_ index, never hashed and never the
    // authority: tickBurning and tickFeatureFades walk only the marked indices
    // (ascending, the order of the old full pass) and decide on the feature's
    // own burn/alive/fadeLeft. A mark is a SUPERSET: every feature that is
    // alive with burn != 0 is marked in burningMarks_ (igniteFeature, the only
    // way into that state, sets it) and every feature with fadeLeft != 0 in
    // fadingMarks_ (finishReclaim); a stale mark (a swap put the fire or the
    // fade out) is dropped when the walk reaches it. burningMarked_ counts the
    // set bits, so a tick without fire visits nothing.
    // dyingMarks_ likewise holds every feature with dieLeft != 0
    // (destroyFeature); dyingMarked_ counts its set bits.
    std::vector<uint64_t> burningMarks_, fadingMarks_, dyingMarks_;
    uint32_t burningMarked_ = 0, dyingMarked_ = 0;
    void clearFeatureMarks() {
        burningMarks_.clear(); fadingMarks_.clear(); dyingMarks_.clear();
        burningMarked_ = dyingMarked_ = 0;
    }
    std::vector<BenchStage> benchPlan_;   // stress-fill staged spawns (executed in tick, by tick)
    size_t benchCursor_ = 0;              // next unexecuted stage
    std::vector<BenchStage> benchStages_; // benchmark ramp, fired as Cmd::BenchClock makes them due
    uint32_t benchDue_ = 0, benchFired_ = 0;
    bool benchmark_ = false;              // a benchmark run
    bool benchEnded_ = false;             // its 60 real seconds are over (Cmd::BenchClock)
    PathService paths_;          // retail's request queue + budget scheduler
    // Per-player entity pools for live matches (5121d8 placement, retirement
    // frees). Restored retail fixtures use retailEntityPools_ instead.
    EntitySlotTable entitySlots_;
    // 499590's maxunits bounds and 4c7c7a's default (entityPoolCapacity).
    static constexpr int kMinEntityCapacity = 20, kMaxEntityCapacity = 500;
    static constexpr int kDefaultEntityCapacity = 250;
    void allocateEntitySlot(int unitId,int player);
    void releaseEntitySlot(int unitId);
    PathfindingMode pathfindingMode_=PathfindingMode::Retail;
    LeftEmptyOnCopy<std::unique_ptr<LegionNavigator>> legion_; // Legion mode only
    ConvoyTable convoys_;                     // Legion mode only; hashed while not empty
    uint32_t joinConvoy(const Unit& u, ConvoyClass cls, Fixed x, Fixed z);
    uint32_t joinArea(int player, ConvoyClass cls, Fixed x, Fixed z, bool shared, uint32_t key);
    // The identity a new leg of `u` of class `cls` at (x, z) gets under Legion:
    // the active explicit order's (its area, joined once per class), else a
    // per-unit convoy join (serial 0).
    OrderIdentity stampIdentity(const Unit& u, ConvoyClass cls, Fixed x, Fixed z);
    struct ActiveOrder {
        uint32_t serial = 0;
        int player = 0, member = -1;
        bool point = false;
        Fixed x, z;
        std::array<uint32_t, 3> area{ConvoyTable::kNone, ConvoyTable::kNone, ConvoyTable::kNone};
    };
    std::optional<ActiveOrder> activeOrder_;   // never hashed: lives within one applyCommand
    // Bumped whenever terrain/feature placement legality may change. Derived
    // planes compare it; never hashed (it is a cache key, not state).
    uint64_t placementEpoch_=0;
    // Placement-cell rectangles {x,z,w,h} changed since Legion last consumed
    // them (its static planes update only those cells). Recorded only while a
    // Legion navigator exists; `All` stands for "everything" (map load, or an
    // overflowing list). Derived-cache bookkeeping, never hashed.
    std::vector<std::array<int,4>> placementDirty_;
    bool placementDirtyAll_=false;
    void notePlacementDirty(int x,int z,int w,int h) {
        if(!legion_||placementDirtyAll_)return;
        if(placementDirty_.size()>=1024) {placementDirtyAll_=true;placementDirty_.clear();return;}
        placementDirty_.push_back({x,z,w,h});
    }
    // Retail mode only: the native group records (51b890), rebuilt before
    // missions and movers each tick. Centres persist when a class empties,
    // as retail's setters leave them. groupPaced_ is the per-unit navigator
    // formation bit (vt+0x30): the current mission has 0x2000000.
    std::array<std::array<RetailGroupRecord,100>,kMaxPlayers> retailGroups_{};
    std::vector<uint8_t> retailGroupPaced_;
    std::array<std::bitset<100>,kMaxPlayers> aiFormation_{};   // retail AI records' +8 (setAiGroup)
    Fixed retailGroupMaximum_;   // 4d95f0 cap for the unit being moved; 0 = none
    // Legion: where a flyer in a mixed formation (squad < 0 with ground
    // members) keeps station this tick, by unit index. Rebuilt each tick from
    // hashed state before the unit loop; not persistent, so not hashed.
    struct LegionFlightStation {
        bool active=false;
        bool steer=false;   // fly at (x,z) instead of the order's own point
        bool hold=false;    // the ground is still under way: the leg does not complete
        bool capped=false;  // limit the flyer's maximum to cap
        Fixed x, z, cap;
    };
    std::vector<LegionFlightStation> legionFlightStations_;
    bool legionFlightStationsLive_=false;
    // Legion's formation re-form (ours, mirroring Retail's standby group
    // check; see legionStandbyCheck). legionFormationSlots_: each formation
    // ground member's slot -- the footprint it stood on when the settled
    // arrival of its last formation move began -- by unit id. Persistent,
    // hashed. legionFormationClasses_: [player][formation 1..99][ground,
    // boat], the standby test's areas and movers; legionSlotDests_: how many
    // of a formation's slots share each move's point ([player, formation,
    // x, z]). Both rebuilt each tick before the unit loop from hashed state;
    // not hashed.
    std::map<int,LegionFormationSlot> legionFormationSlots_;
    std::array<std::array<std::array<LegionSlotClass,2>,100>,kMaxPlayers> legionFormationClasses_{};
    std::map<std::tuple<int,int,int32_t,int32_t>,int> legionSlotDests_;
    // The formation a unit belongs to (retailGroupSlot with the formation
    // flag: Alt+N, or an AI squad while the AI flags it), 0 for none.
    int legionFormationGroup(const Unit& u) const;
    void updateLegionFormationSlots();
    // At a ground standby's wake, where Retail runs its group check: 5 when
    // the member's walk back to its slot was ordered (the standby retires and
    // the move runs), -1 otherwise.
    int legionStandbyCheck(Unit& u);
    const LegionFlightStation* legionFlightStation(const Unit& u) const;
    static int retailGroupIndex(const Unit& u);
    RetailGroupMember retailGroupMember(const Unit& u) const;
    const RetailGroupRecord* retailGroupOf(const Unit& u) const;
    uint32_t retailMissionFlags(Unit& u);
    // The registry flags (+0x5a) of the mission an order runs: its
    // Order::mission.flags, with the row's own flags for SUPPRESS and the
    // flyer attacks, which World does not keep there.
    uint32_t retailOrderFlags(const Unit& u,const Order& o) const;
    // A walker's guard (Follow_Ground) or attack on a unit (Attack_Chase,
    // Attack_Melee): the orders retailOrderFlags gives those rows' flags.
    static bool retailGroundUnitMission(const Unit& u,const Order& o);
    // Whether the unit's current mission carries TotalA.exe's mission flag 0x80
    // (definition table 0x4fc490, +0x11): only Attack_NoMove and Attack_Chase
    // (0x280) and AttackSpecial, the D-gun (0x680); no VTOL mission has it.
    static bool retailTaAttackMission(const Unit& u);
    // The next mission's flags (+0x66's +0x5a) behind orders[index]; 0 for none.
    uint32_t retailNextMissionFlags(const Unit& u,size_t index) const;
    // 51d1e0(unit, 1, level) for an attack's group check (its flags lack 0x4000000).
    bool retailAttackOutOfSlot(const Unit& u,int level) const;
    // The attack a formation leg its group check put ahead of it leads
    // (SUPPRESS, a flyer's attack), or null: the push left its slots as they were.
    static const Order* formationAttack(const std::vector<Order>& orders);
    // The flyer attacks' group check (retailAirGroupCheck), shared by the
    // AirToGround, AirStrike and AirToGroundHover hosts: VTOL_Move_Formation
    // (level -4) goes ahead once the dispatch is done (push).
    struct AirAttackGroup {
        World& w; Unit& u;
        bool formationTo=false;
        bool outOfSlot() const { return w.retailAttackOutOfSlot(u,5); }
        // 50c480 then 4d6a50(unit, 0): a guard's group check lets it go.
        void cancel() const { w.leaveRetailGroupCentre(u);w.cancelPath(u);u.orders.clear();u.routeStamp=-1; }
        uint32_t nextFlags() const { return w.retailNextMissionFlags(u,0); }
        void leave() const { w.leaveRetailGroupCentre(u); }
        bool formation() { formationTo=true;return true; }
        // Puts the formation ahead (level -4) and runs it at once, as the
        // dispatcher goes on to the head it was handed: true when it did
        // (the caller's mover is done for the tick).
        bool push();
    };
    void tickRetailGroups();
    void planLegionFlightStations();
    // Legion: idle landed flyers make way for allied ground groups.
    static constexpr uint32_t kLegionLiftQuiet=90;   // ticks after the last request
    static constexpr uint32_t kLegionLiftRest=240;   // ticks from the hand-over to landing until it may lift again
    static constexpr uint32_t kLegionLiftCap=1800;   // ticks one episode may last (60 s, user decision 4)
    static constexpr uint32_t kLegionLiftCapRest=600; // the rest after a capped episode
    static constexpr uint32_t kLegionLiftPoll=8;     // a lifted flyer polls for a target every 8 ticks, staggered by id
    bool legionLiftable(const Unit& flyer,int player) const;
    // blocked: the flyer covers one of the requesting member's next planned
    // cells (the only request that extends an episode past kLegionLiftCap).
    // Returns whether the request started or extended the episode.
    bool requestLegionLift(Unit& flyer, bool blocked);
    bool tickLegionLift(Unit& u);
    bool tickHoldLift(Unit& u);
    static constexpr uint32_t kHoldLiftQuiet=90;     // ticks hovering after its last target in range
    static constexpr uint32_t kHoldLiftNoRelease=45; // ticks a started shot may go unreleased before a lift
    void leaveRetailGroupCentre(const Unit& u);
    Fixed retailGroupLimit(const Unit& u) const;
    // A finished output joins its maker's group (human owners): a factory's
    // as GetBuilt releases it (40238a), a mobile builder's when it is done.
    void joinFactoryGroup(const Unit& factory,Unit& output);
    void joinBuilderGroup(const Unit& builder,Unit& output);
    bool pathPending(int id) const {return paths_.pending(id);}

    // A unit's authoritative COB VM plus the host flags its scripts read.
    //
    // Sleep skip (WE A1). Most of a big army's VMs spend most ticks with every thread
    // asleep (or waiting on a child that sleeps) and no piece in motion: a real tick of
    // such a VM only counts each sleeper's remaining ticks down by one. tickUnitScript
    // skips those ticks and counts them as debt; the debt is paid -- every sleeper's
    // count lowered by it, exactly what the skipped ticks would have done -- before the
    // VM is next touched for any reason. So the VM is private: state() pays the debt and
    // makes the VM run on its next tick (a thread may be started, a count read);
    // peek() is for reads the debt cannot change (pieces, statics, the active masks,
    // flags, which a skipped tick never touches); stateHash folds the debt in instead
    // (debt()). Exact by construction: the skip never passes the tick a sleeper wakes
    // on (words[3] is already in ticks: the SLEEP opcode converts its milliseconds at
    // ticksPerSecond), and nothing else in the VM changes on a skipped tick.
    // TA_VERIFY_SCRIPT_SKIP (debug): every payment re-runs the skipped ticks on a copy
    // with a host that throws, and compares.
    struct UnitScript {
    private:
        cob::RetailScriptState state_;
        // Right after the VM's tail (its active count and thread mask, the piece mask), with
        // the flags after them: the skip test and a real tick's first reads share these lines,
        // which World::tick prefetches two units ahead.
        uint32_t skipLeft_=0;   // further ticks to skip; kSkipIdle = until touched
        uint32_t debt_=0;       // ticks skipped and not yet paid
    public:
        bool activated=false,ready=false,yardOpen=false,buggerOff=false;
        // BUSY (TotalA.exe unit +0x10f bit 1): GET 6 reads it, SET 6 writes it
        // (0x480b84) and GROUND_PICKUP / GROUND_UNLOAD wait it out (0x438730).
        // Hashed while set.
        bool busy=false;
        // An Activate switchScript holds until Create returns (activated is
        // already set). Hashed while set.
        bool activateHeld=false;
        bool createRunning(const cob::File& file) const {
            const auto& hold=file.createHold;
            if (hold.begin==hold.end) return false;
            const auto& vm=state_.vm;
            for (uint32_t m=vm.activeThreadMask();m;m&=m-1)
                if (hold.contains(vm.threads[size_t(std::countr_zero(m))].words[1])) return true;
            return false;
        }
        uint32_t movementRate=0;
        // TurnDirection's last reported turn, in whole degrees (mover +0x26,
        // 4d9593), and whether this tick's turn was the idle navigator's
        // pivot, which 4d9ad0 asks for after a zero request (notifyTurnDirection).
        int16_t turnDirection=0;
        bool turnInPlace=false;
        bool dying=false;
        int deathStopValue=0;
        // Last SweetSpot drawn centre (queryUnitScriptPoint): derived, not hashed.
        RetailDrawnCenterCache sweetSpotCache;
        explicit UnitScript(const cob::File& file):state_(file) {
#ifndef NDEBUG
            file_=&file;
#endif
        }
        cob::RetailScriptState& state() { pay(true); skipLeft_=0; return state_; }
        const cob::RetailScriptState& peek() const { return state_; }
        uint32_t debt() const { return debt_; }
        // tickUnitScript only: true if this tick is skipped (and owed).
        bool skipTick() {
            if (!skipLeft_) return false;
            if (skipLeft_!=kSkipIdle) --skipLeft_;
            ++debt_;
            return true;
        }
        // tickUnitScript only: state() for the VM's own tick (not counted as a touch).
        cob::RetailScriptState& stateForTick() { pay(false); skipLeft_=0; return state_; }
        // tickUnitScript only, after a real elapsed tick: how many ticks may be skipped.
        void scheduleSkip(bool eligible);
        static constexpr uint32_t kSkipIdle=UINT32_MAX;
        bool idle() const { return skipLeft_==kSkipIdle; }   // no thread will wake by itself
        bool willSkip() const { return skipLeft_!=0; }       // the next tick is skipped (prefetch hint)
        const void* skipLine() const { return &skipLeft_; }
    private:
        void pay(bool touch);   // touch: from outside the VM's own tick (TA_SIMSTATS vm_debt_flushes)
#ifndef NDEBUG
        const cob::File* file_=nullptr;   // TA_VERIFY_SCRIPT_SKIP re-runs the skipped ticks
#endif
    };
    std::map<int,UnitScript> unitScripts_;
    // Non-owning direct lookup; the map still owns stable script objects and
    // supplies the unchanged ID-ordered state-hash traversal.
    std::vector<UnitScript*> unitScriptById_;
    // Derived from the canonical script flag; dense reads avoid touching a large
    // VM object per unit when publishing render snapshots. Not separately hashed.
    std::vector<uint8_t> scriptYardById_;
    UnitScript* unitScript(int id) {
        return size_t(id)<unitScriptById_.size() ? unitScriptById_[size_t(id)] : nullptr;
    }
    const UnitScript* unitScript(int id) const {
        return size_t(id)<unitScriptById_.size() ? unitScriptById_[size_t(id)] : nullptr;
    }
    struct ScriptHost;
    void tickUnitScript(Unit& unit);
    void notifyTurnDirection(Unit& u,UnitScript& script);
    void beginUnitDeath(Unit& unit);
    // taKind: the TA death kind to count (retailVeterancy.h); -1 takes it from
    // the unit's deathType (retailTaDeathKind).
    void sweepDeath(Unit& unit, int taKind = -1);
    // The carrier's TA death kind decides its passengers' (6, or 3 when 3).
    void killPassengers(Unit& carrier, uint8_t carrierTaKind);
    // TotalA.exe's damage record 0x489bb0 -> 0x489ce0 for a cause other than
    // a weapon's hit (applyHit): the damagemodifier below 30000, the victim's
    // kill scaling, the hit response 0x406f80 (not for water, 11), +0xf5 /
    // +0xf0 / +0xf4, and the HP. Nothing for a dead or dying victim.
    void taDamageRecord(Unit* attacker, Unit& victim, int32_t amount, uint8_t kind);
    // TotalA.exe's hit response 0x406f80..0x4071aa (every record but a repair
    // and deadly water).
    void taHitResponse(Unit& victim, const Unit* attacker);
    // The commander rule's sweep 0x486f10: a 30000 record of kind 3 for each
    // unit of the player not already dying.
    void commanderSweep(int player);
    void updateCorpseWindow(Unit& unit);
    bool waterGrave(const Unit& unit) const;
    void tickCorpseSinks();
    // Enabled by setupMatch; a bare test World leaves it off.
    bool pathService_ = false;
    // The retry constants that lived here (kPathRetryTicks, kPathFailBackoff)
    // are gone with the sweep and backoff they drove: the cadence ladder in the
    // mover (the full 0x4e51f3-0x4e5491 table, dice rolled per frame) is
    // retail's one and only re-ask engine, failed-quiet branch included.
    // The give-up-and-rescue apparatus that lived here (AbandonedGoal, abandoned_,
    // the bounded second look) is deleted with the no-headway abandon it served.
    // Retail neither abandons nor rescues: a blocked unit keeps its order, presses
    // under the refusal caps, and re-asks on the randomised deadlines -- the failed
    // shape (rand(8)+rand(8)+30 x halfCellTicks, icd 0x4e535d) probes gently for
    // ever, which is both the crowd-convergence loop and the cliff-wedge behaviour.
    // Indices into units_ of the units channelling a revive this tick
    // (tickAbilities), ascending.
    std::vector<uint32_t> actorIdx_;
    struct AbilityChunk { std::vector<uint32_t> actors; };
    std::vector<AbilityChunk> abilityChunks_;   // tickAbilities on the SimPool (WE E4.3)
    // The post-loop passes go to the SimPool from this many units.
    static constexpr size_t kPostMinParallel = 2048;
    // Per player, this tick's retail construction demand (World::tick's economy step).
    std::vector<float> retailDemand_;
    // The economy walk on the SimPool (WE E4.3): per chunk, its units'
    // nonzero contributions and its sites, in unit order.
    enum class EconomyKind : uint8_t { Site, Income, SacredIncome, Storage, Demand };
    struct EconomyItem { uint32_t index; int16_t player; EconomyKind kind; float f; double d; };
    std::vector<std::vector<EconomyItem>> economyChunks_;
    // The tick prologue's passes go to the SimPool from this many units.
    static constexpr size_t kPrologueMinParallel = 2048;
    // Indices into units_ of this tick's construction sites, ascending, as the
    // economy walk found them (World::tick's decay pass).
    std::vector<uint32_t> sitesIdx_;
    // Flyers, gates and aura emitters for the passes that want only those.
    UnitClassLists unitLists_;
    void syncUnitLists();
    NavGrid nav_, navWater_, navHover_;
    NavGrid navalSight_; // combat only: terrain at/above the water surface
    // Per-cell terrain metrics (16px cells) for per-unit passability limits.
    SharedArray<uint8_t> slope_;   // local height spread
    SharedArray<uint8_t> depth_;   // water depth (sea level - height), 0 on land
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
    uint64_t nextMovementController_ = 0;
};

} // namespace ta::sim
