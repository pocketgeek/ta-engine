#pragma once

// Deterministic in-sim runner for a campaign mission's "god" `.cob` script plus the
// data-driven victory/defeat rules from the mission `.ota [GlobalHeader]`. It is built
// on every peer from identical data and ticked inside the sim (World::tick), so its
// spawns / orders / triggers stay in lockstep without relaying any script actions --
// unlike unit-animation cobs, which run viewer-side. The mission `.cob` has no pieces,
// so it never executes the VM's (float) animation opcodes; it uses only integer
// game-logic opcodes, and the COB VM already builds with -ffp-contract=off. See
// docs/campaign-design.md for the reverse-engineered scripting/win-lose model.

#include "cob/cob.h"
#include "sim/fixed.h"
#include "sim/retailcampaignconditions.h"
#include "cob/vm.h"
#include "tdf/tdf.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tak::sim {

class World;
class TypeRegistry;
struct UnitType;

class MissionScript {
public:
    // cobBytes: the mission `.cob`. header: the parsed `.ota [GlobalHeader]` node (for the
    // win/lose conditions + the default script player). reg: type registry (name lookup).
    // humanPlayer: the campaign player's slot (0-based). origin: for diagnostics.
    // playerMap: .ota player id (1-based) -> World slot, so the script's Create player
    // refs (0-based .ota) land in the same compacted slots as the placed units.
    MissionScript(std::vector<uint8_t> cobBytes, const tak::tdf::Node& header,
                  const TypeRegistry& reg, int humanPlayer, std::string origin,
                  std::vector<int> playerMap = {});

    void setPlayerCommanders(std::vector<const UnitType*> types) { playerCommanders_=std::move(types); }
    // An exact copy of the mission's state, for a copy of its world (World::clone).
    std::unique_ptr<MissionScript> clone() const;

    bool ok() const { return bool(vm_); }

    // Per-placed-unit `InitialMission=` order queues from the .ota, applied at start()
    // in the same mini-language SetMission uses. This is the shipped missions' NPC
    // choreography (patrol routes, ambush holds, factory waves, troops aboard ships).
    void setInitialOrders(std::vector<std::pair<int, std::string>> o) {
        initialOrders_ = std::move(o);
    }
    // The .ota's [units] records in file order: what an order clause's NAME is
    // looked up in (retail 0x513ec0) -- a record's Ident=, then its Unitname, the
    // first record that matches and whose unit was made. `unitId` 0: none made.
    struct Placement { std::string ident, unitname; int unitId = 0; };
    void setPlacements(std::vector<Placement> p) { placements_ = std::move(p); }

    // An order clause's point coordinate in world px, as 0x513fb0 builds it for
    // m/ma/p/u/b: ftol(X * 65536.0), chopped toward zero, times `scale` -- the
    // scenario's +0xdc5, 16 when the .ota has [Map Data] (every shipped mission),
    // else 1 -- a 16.16 pixel value with no half cell, exact in a float on any map.
    static float orderPoint(float cell, int32_t scale);

    // Where retail puts a unit, as a 16.16 pixel point.
    struct Point { Fixed x, z; };
    // An .ota [units] record (4c8ce1, 514b40): the pixel XPos*16,ZPos*16 with no
    // half cell; a building (bmcode other than 1) is then snapped by 5085c0
    // (footprintSnap), so XPos is its centre cell. A mobile stays at X*16.
    static Point placedPoint(const UnitType& type, int xpos, int zpos);
    // The record's Angle as the raw retail heading word, (A<<16)/360 chopped
    // toward zero (4c8d2b); 514d02 stores it over the birth heading.
    static uint16_t placedHeading(int angle) { return uint16_t(int32_t(uint32_t(angle) << 16) / 360); }
    // A record's HealthPercentage / ManaPercentage (514cb9, 514cd3) and the
    // script's SetAttribute (4d3e6a, 4d3eb9): hit points (maxdamage * pct) / 100
    // in unsigned 32 bits, kept as a word; mana MaxMana * pct * 0.01f at double
    // precision, stored as a float through 401120, which caps it at MaxMana. A
    // new unit's pool is otherwise empty (511990); ManaPercentage defaults to 0
    // and HealthPercentage to 100 (4c8dc5..4c8dec).
    static int32_t percentHp(const UnitType& type, int32_t pct);
    static double percentMana(const UnitType& type, int32_t pct);
    // Create X Z (4d35c0): the type's footprint corner at the cell,
    // (X*2 + footprint) << 19, for mobiles and buildings alike.
    static Point createPoint(const UnitType& type, int x, int z);

    void start(World& w);              // run the "Start" script once (initial spawns/triggers)
    void step(World& w, float dt);     // tick the VM, sweep triggers, evaluate conditions
    void unitCaptured(World& w, int id); // called before changing ownership
    void unitDied(World& w, int id);   // a unit died -> UnitDestroyed(id)
    // A change of owner (514da0) is a new object (512060) and the old one's
    // death. The new object's trigger latch (+0xd4) starts at 0 (511990), so
    // the next sweep fires UnitCreated for its new owner and TriggerHit for
    // every armed region holding it again: objectReplaced, at the change. The
    // old object retires with death kind 6 (512ae0): the condition deaths for
    // its old owner and UnitDestroyed (replacedObjectDied, with the deaths).
    void objectReplaced(int id);
    void replacedObjectDied(World& w, int id, const UnitType* type, int oldPlayer);
    // The retirements World is about to feed, in order, each with the World
    // slot of the owner whose population word it still holds (its old owner
    // for a change of owner's old object). GET(5) counts the ones from the
    // unit whose UnitDestroyed runs to the end (runUnitDestroyed).
    void setRetiring(std::vector<std::pair<int, int>> r) { retiring_ = std::move(r); }

    int outcome() const { return outcome_; }   // 0 running / +1 victory / -1 defeat
    // Run one MAP_COMMAND by its name, as the script would (tests).
    int32_t command(World& w, const std::string& name, const std::vector<int32_t>& args);
    // A GET or GET_UNIT_VALUE as the script makes it (4d4060; tests).
    int32_t get(World& w, int32_t valId, const std::vector<int32_t>& args) { world_ = &w; return getValue(w, valId, args); }
    // A WriteValue store, 0 if unset (tests).
    int32_t value(const std::string& name) const;
    // Fold mission state into the sim's state hash (so mission progress is checksummed).
    void foldHash(uint64_t& h) const;

private:
    // A trigger region armed by SetTrigger, in cells as given (4d2ae0 / 4d2b00):
    // a rectangle x1,z1..x2,z2 or a circle centred on x1,z1 with radius r.
    struct Region { bool armed = false, rect = false; int16_t x1 = 0, z1 = 0, x2 = 0, z2 = 0; int32_t r = 0; };

    // A parsed `.ota` victory/defeat rule. `kind` names the retail *Condition_* class;
    // `type` (optional unit type) + up to 4 numeric args carry its parameters.
    std::vector<const UnitType*> playerCommanders_; // authored sidedata commander per compact player slot
    struct Cond : RetailCampaignConditionState {
        Kind kind=DestroyAllUnits;
        const UnitType* type=nullptr;
        int32_t a=0,b=0,c=0;
        bool victory=false,wildcard=false;
        bool soundPlayed=false; // native +8: cosmetic latch, independent of completion
    };

    // ---- VM callback plumbing ----
    MissionScript(const MissionScript&)=default;
    void wireVm();   // the hooks bind this object, so a copy wires its own
    int32_t mapCommand(World& w, int nameIdx, const std::vector<int32_t>& args) {
        return command(w, cob_.name(uint32_t(nameIdx)), args);
    }
    int retailOwner(int player) const;   // World slot -> retail player byte (+0xfd)
    int worldPlayer(int record) const;   // retail player record index -> World slot
    void runUnitDestroyed(World& w, int id, int owner);
    std::vector<std::pair<int, int>> retiring_;   // see setRetiring
    int32_t getValue(World& w, int32_t valId, const std::vector<int32_t>& args);
    void setUnitValue(int32_t valId, int32_t value);

    // ---- verb + order handling ----
    int32_t doCreate(World& w, const std::string& rest, const std::vector<int32_t>& args);
    // One order string, clause by clause (retail 0x513fb0). `initial`: an
    // InitialMission, whose clauses may name placed units; SetMission's may not
    // (retail hands that call an empty unit table).
    void applyOrders(World& w, int unitId, const std::string& orders, bool initial);
    int resolvePlacement(const std::string& name) const;
    void doSetAttribute(World& w, const std::string& sub, int unitId, int pct);

    // ---- per-step evaluation ----
    void sweepTriggers(World& w);
    void evalConditions(World& w, float dt);
    void emitConditionSound(World& w, Cond& condition);
    void parseConditions(const tak::tdf::Node& header);
    int conditionOwner(int player) const;
    // The condition deaths (523510) of a unit of `type` owned by `player`;
    // `dead` is the id whose unit no longer stands (0: none, the old object
    // of a change of owner, whose id the new object now holds).
    void conditionDeaths(World& w, const UnitType* type, int player, int dead);

    const UnitType* findType(const std::string& name) const;   // registry lookup (lowercased)
    float orderPoint(float cell) const { return orderPoint(cell, pointScale_); }

    cob::File cob_;
    cob::CopiedPtr<cob::Vm> vm_;
    const TypeRegistry& reg_;
    World* world_ = nullptr;   // set on each entry (start/step/unit*) for the VM callbacks
    int human_;
    std::string origin_;
    std::vector<int> playerMap_;   // .ota player (1-based) -> World slot (empty = identity)
    int32_t pointScale_ = 16;      // scenario +0xdc5 (orderPoint): 16 with [Map Data], else 1

    std::array<Region, 16> regions_{};
    // Per unit id, 4d33a0's latch (+0xd4): bit N once region N fired for the
    // unit, bit 31 once UnitCreated did. Only RemoveTrigger clears a region bit.
    std::unordered_map<int, uint32_t> triggerBits_;
    std::unordered_map<std::string, int32_t> vars_;      // WriteValue / ReadValue store

    std::vector<Cond> conds_;

    std::vector<std::pair<int, std::string>> initialOrders_;   // .ota InitialMission=, applied at start
    std::vector<Placement> placements_;                        // .ota [units], for InitialMission names

    int32_t clock_ = 0;       // mission time in TICKS, for timer conditions
    int outcome_ = 0;         // 0 / +1 / -1
    bool started_ = false;
    // Start runs before the first tick (4d2dc0); the lines it plays wait for
    // the first step, so a paused opening briefing does not hear them.
    // Presentation only, not hashed.
    bool holdSounds_ = false;
    std::vector<std::pair<std::string, int32_t>> heldSounds_;
};

}  // namespace tak::sim
