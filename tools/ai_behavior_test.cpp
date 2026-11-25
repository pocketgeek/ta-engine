// ai_behavior_test -- the computer player's levels, as the server wires them and as
// they play on the shipped Total Annihilation data.
//
// Every level now runs Total Annihilation's own AI (ai/taretail.h, a TotalA.exe
// port, docs/retail-ai.md): Controller hands each tick to TaRetailBrain, with the
// engine's levels mapped onto its plans (Easy -> easy, Normal -> medium,
// Hard/Absurd -> hard, the RETAIL levels exactly) and the defensive levels never
// launching strike groups. The command-level checks this file used to carry
// exercised the TAK engine's own heuristic AI (stations, interception, builder
// caps, lodestones, Monarchs, mana, raising the dead), which no level runs any
// more, so they are gone; TA has no counterpart to test them against.
//
// What is left:
//  - the level wiring every peer derives from the wire aiLevel (income
//    multiplier, defensive stance, room and replay records), data-free;
//  - with the retail install (argv[1]): every level, given CORE against ARM on
//    Ashap Plateau, opens a base from its commander's build menu, the defensive
//    and passive levels never send an attack in the opening, and a level's game
//    is the same game every time (lockstep).
#include "ai/ai.h"
#include "ai/taretail.h"
#include "ai/classicprofile.h"
#include "net/matchconfig.h"
#include "net/replayhdr.h"
#include "sim/matchsetup.h"
#include "sim/sides.h"
#include "hpi/hpi.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace ta;
static int fails=0;
static void check(bool ok,const std::string& label) {
    std::printf("[%s] %s\n",ok ? "PASS" : "FAIL",label.c_str());if (!ok) ++fails;
}

// The wire level -> Difficulty, and the per-seat sim config each peer derives from
// it (income multiplier, defensive stance), through the room and replay records.
static void levelWiring() {
    // Every level is a TaRetailBrain: the three RETAIL levels at retail Easy,
    // Medium (level 6, Difficulty::Classic) and Hard; the engine's levels mapped
    // onto its plans, the defensive ones and Passive never attacking.
    {
        sim::TypeRegistry none;
        ai::Profile profile;
        struct Row { ai::Difficulty level; int difficulty; bool aggressive; };
        const Row rows[] = {
            {ai::Difficulty::RetailEasy, 0, true}, {ai::Difficulty::Classic, 1, true},
            {ai::Difficulty::RetailHard, 2, true}, {ai::Difficulty::Easy, 0, true},
            {ai::Difficulty::Normal, 1, true}, {ai::Difficulty::Hard, 2, true},
            {ai::Difficulty::Absurd, 2, true}, {ai::Difficulty::Defensive, 1, false},
            {ai::Difficulty::AbsurdlyDefensive, 1, false}, {ai::Difficulty::Passive, 0, false}};
        bool all = true;
        for (const auto& r : rows) {
            ai::Controller c(1, none, profile, 7, r.level);
            const auto* brain = c.retailBrain();
            all = all && brain && brain->difficulty() == r.difficulty && brain->aggressive() == r.aggressive &&
                  c.difficulty() == r.level;
        }
        check(all, "RETAIL EASY / MEDIUM / HARD run TaRetailBrain at 0 / 1 / 2, and every engine level a mapped one");
        check(int(ai::Difficulty::Classic) == 6 && int(ai::Difficulty::RetailEasy) == 8 &&
              int(ai::Difficulty::RetailHard) == 9, "the retail levels keep their wire numbers (6 Medium, 8 Easy, 9 Hard)");
    }
    check(ai::kAiLevels==10 && ai::difficultyFromLevel(5)==ai::Difficulty::AbsurdlyDefensive &&
          ai::difficultyFromLevel(6)==ai::Difficulty::Classic && ai::difficultyFromLevel(7)==ai::Difficulty::Passive &&
          ai::difficultyFromLevel(8)==ai::Difficulty::RetailEasy && ai::difficultyFromLevel(9)==ai::Difficulty::RetailHard &&
          ai::difficultyFromLevel(10)==ai::Difficulty::Normal && ai::difficultyFromLevel(255)==ai::Difficulty::Normal,
          "wire levels 0-9 name the levels; an unknown level (10 and up) is Normal");
    // docs/retail-ai.md section 2: a computer player's credits are scaled 0.5/0.7/1.0
    // by retail difficulty; Absurd and Absurdly Defensive are the engine's 2x cheat.
    check(ai::incomeMultFor(ai::Difficulty::RetailEasy)==0.5f && ai::incomeMultFor(ai::Difficulty::Classic)==0.7f &&
          ai::incomeMultFor(ai::Difficulty::RetailHard)==1.0f,"the RETAIL levels earn retail's 0.5 / 0.7 / 1.0");
    check(ai::incomeMultFor(ai::Difficulty::AbsurdlyDefensive)==2.0f && ai::incomeMultFor(ai::Difficulty::Absurd)==2.0f &&
          ai::incomeMultFor(ai::Difficulty::Defensive)==1.0f && ai::incomeMultFor(ai::Difficulty::Passive)==1.0f,
          "Absurd and Absurdly Defensive earn double; Defensive and Passive do not");
    check(ai::defensiveLevel(ai::Difficulty::Defensive) && ai::defensiveLevel(ai::Difficulty::AbsurdlyDefensive) &&
          !ai::defensiveLevel(ai::Difficulty::Passive) && !ai::defensiveLevel(ai::Difficulty::Classic),
          "only Defensive and Absurdly Defensive take the defensive stance");
    net::SlotInfo slots[net::kMaxSlots]{};
    slots[0].type=1;slots[0].aiLevel=2;
    slots[1].type=2;slots[1].aiLevel=5;slots[1].faction=1;slots[1].team=1;
    slots[2].type=2;slots[2].aiLevel=0;slots[2].team=2;
    slots[3].type=2;slots[3].aiLevel=8;slots[3].faction=1;slots[3].team=3;
    {
        net::Writer wr;net::put(wr,slots[1]);net::Reader rd(wr.b.data(),wr.b.size());net::SlotInfo back;
        net::get(rd,back);
        check(rd.ok && back.aiLevel==5,"the room's slot record carries the level");
    }
    auto expect=[](const sim::MatchConfig& cfg) {
        return cfg.slots.size()==4 && cfg.slots[0].incomeMult==1 && !cfg.slots[0].defensiveAi &&
            cfg.slots[1].incomeMult==2 && cfg.slots[1].defensiveAi &&
            cfg.slots[2].incomeMult==1 && cfg.slots[2].defensiveAi &&
            cfg.slots[3].incomeMult==0.5f && !cfg.slots[3].defensiveAi;
    };
    net::GameOptions opts;
    check(expect(net::matchConfigFor(opts,slots,7)),"the match config gives each seat its level's multiplier and stance");
    // Through a replay header and back, as the replay loader rebuilds the config.
    net::Writer wr;net::writeReplayHeader(wr,net::replayHeaderFor(opts,"Ashap Plateau","",slots,7));
    net::Reader rd(wr.b.data()+4,wr.b.size()-4);   // past the magic, as the loader reads
    net::ReplayHeader h;uint32_t fmt=0,proto=0;
    const bool read=net::readReplayHeader(rd,h,fmt,proto);
    net::GameOptions o2;net::SlotInfo back[net::kMaxSlots];net::matchInputsFrom(h,o2,back);
    check(read && proto==net::kNetVersion && back[1].aiLevel==5 && back[3].aiLevel==8 &&
          expect(net::matchConfigFor(o2,back,7)),"a replay header keeps the levels and replays them alike");
    check(!net::supportedReplayProtocol(fmt,272),"a replay of another protocol is refused");
}

namespace {
struct Opening { int begun=0, attacks=0, builds=0; uint64_t hash=0; };
}

// CORE (player 1, the AI) against an idle ARM commander on Ashap Plateau, the AI
// set up as the server does (the map's aiprofile), for `ticks`.
static Opening playOpening(const hpi::Vfs& vfs,const sim::TypeRegistry& reg,ai::Difficulty level,uint32_t ticks) {
    sim::World w;
    sim::MatchConfig cfg;cfg.vfs=&vfs;cfg.mapPath=hpi::findMap(vfs,"Ashap Plateau");
    cfg.slots.resize(2);
    cfg.slots[0].used=true;cfg.slots[0].faction=0;cfg.slots[0].team=0;
    cfg.slots[1].used=true;cfg.slots[1].faction=1;cfg.slots[1].team=1;
    cfg.slots[1].incomeMult=ai::incomeMultFor(level);cfg.slots[1].defensiveAi=ai::defensiveLevel(level);
    const auto starts=sim::setupMatch(w,reg,cfg);
    ai::Profile profile;
    ai::Controller controller(1,reg,profile,0x1234,level,{starts.empty() ? std::pair{0.f,0.f} : starts.front()});
    controller.setClassicProfile(ai::classicProfileScript(vfs,ai::classicMapAiProfile(vfs,cfg.mapPath)));
    Opening out;
    for (uint32_t tick=0;tick<ticks;++tick) {
        controller.tick(w,tick,[&](const net::Command& c) {
            if (c.kind==net::Cmd::Attack || c.kind==net::Cmd::AttackMove) ++out.attacks;
            if (c.kind==net::Cmd::Build) ++out.builds;
            sim::applyCommand(w,reg,c);
        });
        w.tick(1.f/30.f);
    }
    for (const auto& u:w.units())
        if (u.alive() && u.player==1 && u.type && u.type->isStructure()) ++out.begun;
    out.hash=w.stateHash();
    return out;
}

static void retailOpenings(const char* root) {
    auto vfs=hpi::mountRetailRoot(root);
    sim::TypeRegistry reg;sim::setupRegistry(reg,vfs);
    check(sides::count()==2,"TA has two sides (ARM, CORE)");
    for (int side=0;side<sides::count();++side) {
        const auto* commander=reg.find(sides::commander(side));
        check(commander && commander->isBuilder && !commander->buildMenu.empty(),
              "side "+std::to_string(side)+"'s commander ("+sides::commander(side)+") is a builder with a menu");
    }
    check(!hpi::findMap(vfs,"Ashap Plateau").empty(),"Ashap Plateau is in the install");
    const uint32_t ticks=2*60*30;
    for (auto level:{ai::Difficulty::Easy,ai::Difficulty::Normal,ai::Difficulty::Hard,ai::Difficulty::Absurd,
                     ai::Difficulty::Defensive,ai::Difficulty::AbsurdlyDefensive,ai::Difficulty::Passive,
                     ai::Difficulty::RetailEasy,ai::Difficulty::Classic,ai::Difficulty::RetailHard}) {
        const std::string name="level "+std::to_string(int(level));
        const Opening o=playOpening(vfs,reg,level,ticks);
        std::printf("  %s: %d structures, %d build orders, %d attacks\n",name.c_str(),o.begun,o.builds,o.attacks);
        check(o.builds>0 && o.begun>0,name+": the AI's commander opens a base from its build menu in two minutes");
        if (ai::defensiveLevel(level) || level==ai::Difficulty::Passive)
            check(o.attacks==0,name+": a defensive or passive level sends no attack");
    }
    const Opening a=playOpening(vfs,reg,ai::Difficulty::Normal,30*30),b=playOpening(vfs,reg,ai::Difficulty::Normal,30*30);
    check(a.hash==b.hash,"the same opening twice is the same game (lockstep)");
}

int main(int argc,char** argv) {
    levelWiring();
    if (argc>1) retailOpenings(argv[1]);
    std::printf(fails ? "%d FAILED\n" : "all passed\n",fails);
    return fails ? 1 : 0;
}
