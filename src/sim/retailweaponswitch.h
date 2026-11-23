#pragma once

#include <array>
#include <cstdint>

namespace tak::sim {

// Retail's AI weapon switching (icd 40f411..40fb92), the decision only. The
// fire-at-will walker runs it for each visited computer-owned unit whose type
// has weaponswitching, before that unit's sweep. The host supplies the
// per-slot facts and evaluates the enemy/ally counts lazily, in retail's
// order (E first, F only when needed), through the callbacks.
struct RetailWeaponSwitchSlot {
    bool present=false;
    bool affordable=false;   // manapershot/costMult < 0.01 or <= the unit's mana
    bool usable=false;       // the target is not immune to it (stone/freeze/mind/paralyze)
    int32_t aoe=0;           // areaofeffect (u16)
    bool mindControl=false;
    // stone, freeze, mind control, paralyzer, aoe >= 200, or a per-shot cost
    // above a tenth of the unit's max mana: the no-target revert's test.
    bool costlyOrSpecial=false;
    int32_t damage=0;        // DAMAGE default (+0x88)
};

struct RetailWeaponSwitchInput {
    std::array<RetailWeaponSwitchSlot,3> slots{};
    int selected=0;          // current slot, 0..2
    bool target=false;       // the selected slot holds a live, non-dying target
    int32_t targetHp=0;      // its int16 HP
    bool lowMonarch=false;   // commander below maxdamage/3
};

// The slot to switch to, or -1 to keep the current one. enemies(r) and
// allies(r) count the owner's visible enemies / allies within r of the
// target; random(n) is the game RNG (drawn at most once, and only on the
// no-target path for a costly or special selected weapon).
template<class Enemies,class Allies,class Random>
int retailWeaponSwitchChoice(const RetailWeaponSwitchInput& in,Enemies enemies,Allies allies,Random random) {
    const int sel=in.selected;
    if (sel<0 || sel>=3) return -1;
    if (in.target) {
        int choice=0;
        for (int s:{2,1}) {
            const auto& w=in.slots[size_t(s)];
            if (!w.present || !w.affordable || !w.usable) continue;
            const int32_t a=w.aoe;
            if (a<=50) choice=s;
            else if (a>=200) {
                const int e=enemies(a);
                if (e>2 && (in.lowMonarch || 3*allies(a)<e)) choice=s;
            } else {
                const int e=enemies(a);
                if (in.lowMonarch || 5*allies(a)<e) choice=s;
            }
            if (choice==s) break;
        }
        if (choice==sel) return -1;
        if (sel!=0) return choice;
        const auto& next=in.slots[size_t(choice)];
        if (next.mindControl || next.aoe>=200) return choice;
        if (in.slots[0].damage>=in.targetHp/2) return -1;   // the primary finishes it
        return choice;
    }
    if (sel==0) return -1;
    if (!in.slots[size_t(sel)].costlyOrSpecial) return -1;
    return random(10)==0 ? 0 : -1;
}

} // namespace tak::sim
