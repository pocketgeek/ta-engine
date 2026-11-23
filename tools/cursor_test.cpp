#include "client/cursors.h"
#include "client/cursorrange.h"
#include "client/cursortiming.h"
#include "gaf/gaf.h"
#include "hpi/hpi.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <tuple>
#include <vector>

namespace {
struct TimedFrame { uint16_t delayTicks; };
struct ExpectedCursor { tak::CursorId id; const char* name; };

constexpr std::array<ExpectedCursor, 21> roster{{
    {tak::CursorId::Normal, "cursornormal"},
    {tak::CursorId::Select, "cursorselect"},
    {tak::CursorId::Move, "CursorMove"},
    {tak::CursorId::Attack, "CursorAttack"},
    {tak::CursorId::Airstrike, "cursorairstrike"},
    {tak::CursorId::TooFar, "cursortoofar"},
    {tak::CursorId::Patrol, "CursorPatrol"},
    {tak::CursorId::Defend, "CursorDefend"},
    {tak::CursorId::Repair, "cursorrepair"},
    {tak::CursorId::Load, "Cursorload"},
    {tak::CursorId::Unload, "CursorUnload"},
    {tak::CursorId::Reclaim, "Cursorreclamate"},
    {tak::CursorId::Revive, "cursorrevive"},
    {tak::CursorId::FindSite, "cursorfindsite"},
    {tak::CursorId::Green, "cursorgrn"},
    {tak::CursorId::Red, "cursorred"},
    {tak::CursorId::Hourglass, "cursorhourglass"},
    {tak::CursorId::PathIcon, "pathicon"},
    {tak::CursorId::Capture, "cursorcapture"},
    {tak::CursorId::Teleport, "cursorteleport"},
    {tak::CursorId::Pickup, "cursorpickup"},
}};

int fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

bool checkAssets(const char* root) {
    tak::hpi::Vfs vfs;
    vfs.addLayer(tak::hpi::MountSet(root));
    const auto palette = tak::gaf::Palette::fromBytes(vfs.read("palettes/cursors.pcx"));
    const auto sequences = tak::gaf::load(vfs.read("anims/cursors.gaf"), palette, -1,
                                          "anims/cursors.gaf");
    std::unordered_map<std::string, const tak::gaf::Sequence*> byName;
    for (const auto& sequence : sequences) {
        std::string key = sequence.name;
        for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
        byName[key] = &sequence;
    }
    const auto normal = byName.at("cursornormal");
    constexpr std::array<std::tuple<const char*, size_t, uint16_t>, 8> markerSequences{{
        {"CursorMove", 12, 2}, {"CursorAttack", 10, 2}, {"CursorPatrol", 20, 2},
        {"CursorDefend", 17, 2}, {"cursorrepair", 10, 2}, {"Cursorload", 10, 3},
        {"CursorUnload", 10, 3}, {"Cursorreclamate", 16, 3},
    }};
    for (const auto& [name, expectedFrames, expectedDelay] : markerSequences) {
        std::string key = name;
        for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
        const auto it = byName.find(key);
        if (it == byName.end() || it->second->frames.size() != expectedFrames ||
            it->second->frames.front().retailDelayTicks != expectedDelay) return false;
    }
    for (const auto& entry : roster) {
        std::string key = entry.name;
        for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
        const auto it = byName.find(key);
        if (it == byName.end() || it->second->frames.empty()) return false;
        if (tak::hpi::MountSet::key(tak::cursorSequenceName(entry.id)) != key) return false;
        if (entry.id == tak::CursorId::Capture || entry.id == tak::CursorId::Teleport ||
            entry.id == tak::CursorId::Pickup) {
            const auto& frame = it->second->frames.front();
            const auto& arrow = normal->frames.front();
            if (frame.width != arrow.width || frame.height != arrow.height ||
                frame.xoff != arrow.xoff || frame.yoff != arrow.yoff ||
                frame.rgba != arrow.rgba) return false;
        }
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    for (const auto& entry : roster)
        if (std::strcmp(tak::cursorSequenceName(entry.id), entry.name) != 0)
            return fail("cursor ID to retail sequence-name mapping");
    if (static_cast<size_t>(tak::CursorId::Count) != roster.size())
        return fail("cursor enum and retail sequence roster size");

    // Which cursor shows when: tools/cursor_rules_test.cpp.

    const std::array<std::vector<TimedFrame>, 2> liveSequences{{
        std::vector<TimedFrame>{{2}, {2}, {2}},
        std::vector<TimedFrame>{{2}, {2}, {2}}
    }};
    tak::CursorAnimationClock<2> liveClock;
    // The software and hardware entry points share this clock. Retail advances
    // the first selected frame on the first 30 Hz update, retains each sequence's
    // frame index, and keeps consuming the one countdown while another is active.
    if (liveClock.frameAt(0, 0, liveSequences) != 0 ||
        liveClock.frameAt(0, 34, liveSequences) != 1 ||       // native update: A -> frame 1
        liveClock.frameAt(1, 67, liveSequences) != 0 ||       // switch: retain A, select B
        liveClock.frameAt(1, 100, liveSequences) != 0 ||
        liveClock.frameAt(1, 134, liveSequences) != 1 ||     // shared timer expires on B
        liveClock.frameAt(0, 167, liveSequences) != 1 ||     // returning resumes A at frame 1
        liveClock.frameAt(0, 200, liveSequences) != 1 ||
        liveClock.frameAt(0, 234, liveSequences) != 2)       // shared timer then advances A
        return fail("live cursor switches retain sequence frames and share retail countdown");

    tak::CursorAnimationClock<1> cappedClock;
    const std::array<std::vector<TimedFrame>, 1> cappedSequence{{
        std::vector<TimedFrame>{{1}, {1}, {1}, {1}, {1}, {1}, {1}, {1}}
    }};
    if (cappedClock.frameAt(0, 0, cappedSequence) != 0 ||
        cappedClock.frameAt(0, 1000, cappedSequence) != 3)
        return fail("live cursor manager caps a delayed catch-up to five native updates");

    if (tak::cursorOrderMarkerFrameAt(12, 2, 0) != 0 ||
        tak::cursorOrderMarkerFrameAt(12, 2, 3) != 0 ||
        tak::cursorOrderMarkerFrameAt(12, 2, 4) != 1 ||
        tak::cursorOrderMarkerFrameAt(12, 2, 100) != 1 ||
        tak::cursorOrderMarkerFrameAt(12, 2, 48) != 0 ||
        tak::cursorOrderMarkerFrameAt(10, 3, 5) != 0 ||
        tak::cursorOrderMarkerFrameAt(10, 3, 6) != 1 ||
        tak::cursorOrderMarkerFrameAt(10, 3, 59) != 9 ||
        tak::cursorOrderMarkerFrameAt(10, 3, 60) != 0 ||
        tak::cursorOrderMarkerFrameAt(0, 2, 99) != 0)
        return fail("waypoint marker frames use the global game tick and twice the first GAF delay");

    using tak::client::CursorWeaponRange;
    using tak::client::cursorWeaponRange;
    using tak::client::unitHasAirstrikeCursor;
    using tak::sim::UnitType;
    using tak::sim::Weapon;
    UnitType attacker, target;
    const std::array<int32_t, 3> origin{0, 0, 0};
    std::array<int32_t, 3> at100{100 * 65536, 0, 0};
    std::array<int32_t, 3> at101{101 * 65536, 0, 0};
    Weapon ranged;
    UnitType cursorAirstrike;
    cursorAirstrike.hasPrimaryWeaponBlock=true;
    cursorAirstrike.weaponSwitching=true;
    cursorAirstrike.weapons.resize(2);
    cursorAirstrike.weaponNativeSlotForLocal={0,2,0};
    cursorAirstrike.weaponAirstrikeCursor[2]=true;
    if(!unitHasAirstrikeCursor(cursorAirstrike,1) ||
        unitHasAirstrikeCursor(cursorAirstrike,0))
        return fail("Airstrike flag follows the selected native slot through the local compressed weapon list");
    cursorAirstrike.weaponSwitching=false;
    cursorAirstrike.weaponAirstrikeCursor[0]=true;
    if(!unitHasAirstrikeCursor(cursorAirstrike,1))
        return fail("non-switching Airstrike selection always uses native slot zero");
    cursorAirstrike.hasPrimaryWeaponBlock=false;
    if(unitHasAirstrikeCursor(cursorAirstrike,0))
        return fail("Airstrike cursor requires the native WEAPON1 UnitDef gate");

    ranged.range = 100;
    if (cursorWeaponRange(ranged, attacker, origin, target, at100, 1) !=
            CursorWeaponRange::InRange ||
        cursorWeaponRange(ranged, attacker, origin, target, at101, 1) !=
            CursorWeaponRange::OutOfRange)
        return fail("cursor range matches inclusive native maxrange boundary");
    ranged.minRange = 100;
    const std::array<int32_t, 3> at99{99 * 65536, 0, 0};
    if (cursorWeaponRange(ranged, attacker, origin, target, at99, 1) !=
            CursorWeaponRange::OutOfRange ||
        cursorWeaponRange(ranged, attacker, origin, target, at100, 1) !=
            CursorWeaponRange::InRange)
        return fail("cursor range matches strict native minrange boundary");

    const std::array<int32_t, 3> fractionalDiagonal{111411, 0, 111411};
    if (tak::client::cursorDistanceSquaredPixels(origin, fractionalDiagonal) != 4)
        return fail("native range drops carry between fractional x/z square products");
    ranged.range = 2;
    ranged.minRange = 0;
    if (cursorWeaponRange(ranged, attacker, origin, target, fractionalDiagonal, 1) !=
            CursorWeaponRange::InRange)
        return fail("fractional diagonal matches native per-axis squared-distance sum");

    const std::array<int32_t, 3> maxCorner{
        std::numeric_limits<int32_t>::max(), 0, std::numeric_limits<int32_t>::max()};
    const std::array<int32_t, 3> minCorner{
        std::numeric_limits<int32_t>::min(), 0, std::numeric_limits<int32_t>::min()};
    if (tak::client::cursorDistanceSquaredPixels(maxCorner, minCorner) != 8589934588ULL)
        return fail("range arithmetic remains exact across full signed 16.16 coordinates");

    Weapon melee;
    melee.melee = true;
    const std::array<int32_t, 3> at23{23 * 65536, 0, 0};
    const std::array<int32_t, 3> at24{24 * 65536, 0, 0};
    if (cursorWeaponRange(melee, attacker, origin, target, at23, 1) !=
            CursorWeaponRange::InRange ||
        cursorWeaponRange(melee, attacker, origin, target, at24, 1) !=
            CursorWeaponRange::OutOfRange ||
        cursorWeaponRange(melee, attacker, origin, target, at23, 2) !=
            CursorWeaponRange::OutOfRange)
        return fail("melee cursor uses footprint contact and airborne target gate");

    Weapon ballistic;
    ballistic.ballistic = true;
    ballistic.range = 2000;
    ballistic.projVel = 300;
    const std::array<int32_t, 3> at1000{1000 * 65536, 0, 0};
    if (cursorWeaponRange(ballistic, attacker, origin, target, at100, 1) !=
            CursorWeaponRange::InRange ||
        cursorWeaponRange(ballistic, attacker, origin, target, at1000, 1) !=
            CursorWeaponRange::OutOfRange)
        return fail("ballistic cursor checks native arc reachability before range");

    Weapon categoryWeapon;
    categoryWeapon.damage = 100.0f;
    categoryWeapon.dmgVs.emplace("monster", 0.0f);
    target.categories = {"monster"};
    target.damageCategory.clear();
    if (tak::client::cursorDamageVs(categoryWeapon, target) != 100.0f)
        return fail("cursor damage lookup ignores broad categories when damagecategory is absent");
    target.damageCategory = "monster";
    if (tak::client::cursorDamageVs(categoryWeapon, target) != 0.0f)
        return fail("cursor damage lookup uses the native damagecategory field");

    const std::vector<TimedFrame> load(10, {3});
    const std::vector<TimedFrame> revive(22, {10});
    const std::array<std::vector<TimedFrame>, 1> loadSequence{{load}};
    const std::array<std::vector<TimedFrame>, 1> reviveSequence{{revive}};
    tak::CursorAnimationClock<1> loadClock, reviveClock;
    if (loadClock.frameAt(0, 0, loadSequence) != 0 ||
        loadClock.frameAt(0, 34, loadSequence) != 1 ||
        loadClock.frameAt(0, 134, loadSequence) != 1 ||
        loadClock.frameAt(0, 167, loadSequence) != 2 ||
        reviveClock.frameAt(0, 0, reviveSequence) != 0 ||
        reviveClock.frameAt(0, 34, reviveSequence) != 1 ||
        reviveClock.frameAt(0, 134, reviveSequence) != 1 ||
        reviveClock.frameAt(0, 234, reviveSequence) != 1 ||
        reviveClock.frameAt(0, 334, reviveSequence) != 1 ||
        reviveClock.frameAt(0, 367, reviveSequence) != 1 ||
        reviveClock.frameAt(0, 400, reviveSequence) != 2)
        return fail("load/revive cursors retain their slower authored retail cadence");

    if (argc == 2 && !checkAssets(argv[1]))
        return fail("mapped cursor names or placeholder pixels differ from the shipped GAF");
    if (argc > 2) return fail("usage: cursor_test [extracted-retail-data-root]");
    std::puts("PASS: retail cursor roster, pointer delays, order-marker clock, and native range gates");
    if (argc == 2) std::puts("PASS: all mapped cursor sequences exist; placeholder art matches cursornormal");
    return 0;
}
