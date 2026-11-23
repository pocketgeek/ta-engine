// The Classic AI level's building blocks (src/ai/classic*): the retail AI profile
// baker, the per-type facts and the bookkeeping clock with its dynamic build
// priorities on a fixed World, the squad layer (constructor rolls, assignment),
// and (with a data directory) TA's map AI-profile lookup through the real VFS.
//
//   classic_ai_test                 unit tests
//   classic_ai_test <retail-root>   + the shipped data checks
//   classic_ai_test --u13 <retail-root>     U13: a column on Move and on
//       AttackMove (TA's Fight) walking through enemies
//   (--bases, --raid, --waves, --duels and --expansion, Kingdoms' Classic seat
//   games, are gone: ai::Controller no longer builds a ClassicBrain.)
//   classic_ai_test --profile       oracle mode for tools/re/check_ai_profile.py:
//       stdin  "types <n>" then n lines "<name> <category words...>", then any
//              number of "case <hex script>" lines
//       stdout per case, for each catalogue index 1..n: "pref limit prefLock limitLock"
//   classic_ai_test --printweights <retail-root>
//       oracle mode for tools/re/check_ai_printweights.py: the shipped types and
//       Abnar's Terrace's profile, as a Classic seat sees them. stdin lines:
//         facts <name>   -> "<found> <flags> <secondary> <minwaterdepth> <cost bits> <pref> <limit>"
//         state <population> <stored> <capacity> <n> (<name> <count> <completed>)*n
//               <30 x (income usage)>   -> no output; sets the table (retail's order)
//         creation <0|1> -> 1: the manager's creation pass, before the profile
//                           file is read (every limit still -1)
//         seed <s>       -> one priority pass over that table from seed s:
//                           "<priority>*n <final seed> <draws>"
//   classic_ai_test --squad-ctor    oracle mode for tools/re/check_ai_squad_ctor.py
//   classic_ai_test --assignment    oracle mode for tools/re/check_ai_assignment.py
//   classic_ai_test --factory-think oracle mode for tools/re/check_ai_factory_think.py
//   classic_ai_test --placement     oracle mode for tools/re/check_ai_placement.py
//   classic_ai_test --constructor   oracle mode for tools/re/check_ai_constructor.py
//   classic_ai_test --static-grade  oracle mode for tools/re/check_ai_static_grade.py
//   classic_ai_test --threat        oracle mode for tools/re/check_ai_threat.py
//   classic_ai_test --defence       oracle mode for tools/re/check_ai_defence.py
//   classic_ai_test --retreat       oracle mode for tools/re/check_ai_retreat.py
//   classic_ai_test --pick          oracle mode for tools/re/check_ai_target_pick.py
//   classic_ai_test --strike / --backup / --vtol   oracle modes for tools/re/check_ai_strike.py
//   classic_ai_test --launch        oracle mode for tools/re/check_ai_launch.py
//       (line layouts in those scripts and in the oracle functions below)
#include "ai/ai.h"
#include "ai/builtinprofile.h"
#include "ai/classicbrain.h"
#include "ai/classicdefence.h"
#include "ai/classicexpand.h"
#include "ai/classicprofile.h"
#include "ai/classicplacement.h"
#include "ai/classicproduction.h"
#include "ai/classictypes.h"
#include "ai/classicwaves.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/retailconstruction.h"
#include "sim/retailreach.h"
#include "sim/sim.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>

using namespace ta;
static int fails=0;
static void check(bool ok,const char* label) {
    std::printf("[%s] %s\n",ok ? "PASS" : "FAIL",label);if (!ok) ++fails;
}

static int profileOracle() {
    std::string line,word;
    ai::ClassicCatalogue catalogue;
    size_t n=0;
    if (!std::getline(std::cin,line)) return 2;
    {std::istringstream in(line);in>>word>>n;if (word!="types") return 2;}
    std::vector<std::pair<std::string,std::vector<std::string>>> entries;
    for (size_t i=0;i<n && std::getline(std::cin,line);++i) {
        std::istringstream in(line);std::string name;in>>name;
        std::vector<std::string> cats;while (in>>word) cats.push_back(word);
        entries.push_back({name,cats});
    }
    // The oracle hands the names in retail's table order already.
    for (const auto& [name,cats] : entries) {
        std::string words;for (const auto& c : cats) words+=c+" ";
        catalogue.add(nullptr,name,words);
    }
    while (std::getline(std::cin,line)) {
        if (line.rfind("case ",0)!=0) continue;
        std::string script;
        for (size_t i=5;i+1<line.size();i+=2) script.push_back(char(std::stoi(line.substr(i,2),nullptr,16)));
        ai::ClassicProfile p(catalogue.size());p.execute(script,catalogue);
        for (size_t t=1;t<catalogue.size();++t)
            std::printf("%s%u %d %u %u",t>1 ? " " : "",unsigned(p.pref[t]),p.limit[t],
                        unsigned(p.prefLocked[t]),unsigned(p.limitLocked[t]));
        std::printf("\n");
    }
    return 0;
}

static ai::ClassicCatalogue sample() {
    return ai::ClassicCatalogue::synthetic({
        {"araarch",{"ARA"}},{"arabuild",{"ARA","builder"}},{"aracastl",{"ARA"}},{"aradrag",{"ARA"}},
        {"arakeep",{"ARA"}},{"aralode",{"ARA"}},{"arasiege",{"ARA"}},{"tararch",{"TAR"}},
    });
}

static void profileRules() {
    const auto c=sample();
    auto at=[&](const ai::ClassicProfile&,const char* name){return size_t(c.byName.at(name));};
    check(c.size()==9 && c.names[0].empty() && c.names[1]=="araarch" && c.names[8]=="tararch",
          "the catalogue is name-ordered behind retail's empty slot 0");
    {
        ai::ClassicProfile p(c.size());
        bool fresh=true;
        for (size_t t=0;t<c.size();++t) fresh=fresh && p.pref[t]==100 && p.limit[t]==-1 && !p.prefLocked[t] && !p.limitLocked[t];
        check(fresh,"every type starts at preference 100, unlimited, unlocked");
    }
    {
        // The weight store's emulated values (0x410c80 on a fresh 100).
        ai::ClassicProfile p(c.size());
        p.execute("weight araarch 0.5\nweight arabuild 0.999\nweight aracastl 0.019\nweight aradrag 0.0099\n"
                  "weight arakeep 2\nweight aralode -1\nweight arasiege 25\nweight tararch 0\n",c);
        check(p.pref[at(p,"araarch")]==50 && p.pref[at(p,"arabuild")]==99 && p.pref[at(p,"aracastl")]==1 &&
              p.pref[at(p,"aradrag")]==0 && p.pref[at(p,"arakeep")]==100 && p.pref[at(p,"aralode")]==0 &&
              p.pref[at(p,"arasiege")]==100 && p.pref[at(p,"tararch")]==0,
              "weight = clamp(trunc(100 * w), 0, 100): 0.5->50 0.999->99 0.019->1 0.0099->0 2->100 -1->0 25->100 0->0");
        p.execute("weight araarch 2\nweight tararch 5\n",c);
        check(p.pref[at(p,"araarch")]==50 && p.pref[at(p,"tararch")]==0 && p.prefLocked[at(p,"tararch")],
              "an exact unit line locks its type: the first writer wins, a zero weight included");
    }
    {
        ai::ClassicProfile p(c.size());
        p.execute("weight ALL 0.5\nweight all 0.5\nweight araarch 0.5\nweight ara 2\n",c);
        check(p.pref[at(p,"arabuild")]==50 && p.pref[at(p,"araarch")]==12 && p.pref[at(p,"tararch")]==25 &&
              !p.prefLocked[at(p,"arabuild")] && p.prefLocked[at(p,"araarch")],
              "category lines (ALL, the FBI category words) compound without locking; an exact line scales the result");
    }
    {
        ai::ClassicProfile p(c.size());
        p.execute("// Kingdoms AI Profile\r\n\r\nWEIGHT ARAARCH 0.25\r\n  limit\tarabuild 3\r\nlimit arabuild 9\r\n"
                  "bogus foo 1\r\nweight arakeep 0.5 extra tokens\r\nlimit aradrag\r\nweight araseige 0\r\n"
                  "weight aracastl 0.5 # 0.1\r\n# weight aralode 0\r\nlimit #aralode 1\r\n",c);
        check(p.pref[at(p,"araarch")]==25 && p.limit[at(p,"arabuild")]==3 && p.pref[at(p,"arakeep")]==50 &&
              p.limit[at(p,"aradrag")]==0 && p.limitLocked[at(p,"aradrag")] && p.pref[at(p,"aracastl")]==50 &&
              p.pref[at(p,"aralode")]==100 && p.limit[at(p,"aralode")]==-1,
              "case-insensitive commands and names, CRLF and tabs, // and # comments, extra tokens ignored, a missing value is 0");
        check(p.pref[at(p,"arasiege")]==0 && p.prefLocked[at(p,"arasiege")] && p.limit[at(p,"arasiege")]==-1,
              "the shipped misspelling araseige is read as the Rolling Tower (retail ignores it)");
        check(p.allows(at(p,"arabuild"),2) && !p.allows(at(p,"arabuild"),3) && !p.allows(at(p,"aradrag"),0) &&
              p.allows(at(p,"arasiege"),100000),"the limit check: -1 unlimited, else count < limit");
    }
    {
        ai::ClassicProfile p(c.size());
        p.execute("weight aracastle 0\nlimit arasiegee 3\n",c);
        check(p.pref[at(p,"aracastl")]==100 && p.limit[at(p,"arasiege")]==-1,
              "any other misspelt name does nothing: the type keeps 100 and unlimited");
    }
    check(ai::classicAtof("1.5x")==1.5f && ai::classicAtof("  -2")==-2.0f && ai::classicAtof("1e2")==100.0f &&
          ai::classicAtof("1d2")==100.0f && ai::classicAtof(".5")==0.5f && ai::classicAtof("abc")==0.0f &&
          ai::classicAtof("1e")==1.0f,"atof: prefix parse, Fortran d exponent, junk is 0");
    check(ai::classicAtoi("12abc")==12 && ai::classicAtoi(" -7")==-7 && ai::classicAtoi("4294967297")==1 &&
          ai::classicAtoi("x")==0,"atoi: prefix parse with the C library's wrap-around");
    check(ai::classicScaledPref(64,67108864.0f)==0 && ai::classicScaledPref(65,67108864.0f)==100 &&
          ai::classicScaledPref(100,1e30f)==0 && ai::classicScaledPref(0,1e38f*10.0f)==0,
          "the store keeps _ftol's low 32 bits: huge weights wrap or give 0");
    {
        // The Park-Miller fixed point: a seed that is a multiple of 2^31-1.
        const int player=1;
        const uint32_t room=0x7fffffffu^0x66e29572u^(0x9e3779b9u*uint32_t(player+1));
        check(sim::retailSeed(room^(0x9e3779b9u*uint32_t(player+1)))==0x7fffffffu,
              "this room seed would give retail's seeding the fixed point");
        ai::ClassicBrain b(player,room);
        const uint32_t s0=b.seed();b.rand(1000);
        check(s0!=0x7fffffffu && b.seed()!=s0,"seedFor steps off the fixed point, so the stream moves");
    }
}

static void typeFacts() {
    bool site=false,lode=false;
    auto flags=[&](const char* ym,int fx,int fz){ai::classicYardMapFlags(ym,fx,fz,site,lode);return std::pair{site,lode};};
    check(flags("S",3,3)==std::pair{true,true} && flags(" S",2,2)==std::pair{true,true} &&
          flags("oooS",2,2)==std::pair{true,false} && flags("ooo ooo ooS",3,3)==std::pair{true,false} &&
          flags("oS",1,1)==std::pair{false,false} && flags("o",3,3)==std::pair{false,false} &&
          flags("xSo",1,1)==std::pair{true,true} && flags("",2,2)==std::pair{false,false},
          "yardmap walk: S anywhere in the cells = mana site, S first = lodestone class; the last char repeats, unknown chars are skipped");
}

// A self-contained registry (the same synthetic-VFS trick as ai_behavior_test).
// Each FBI carries TA's copyright line: TotalA.exe's pre-pass (0x42a8d0) skips a
// unit without it.
static sim::TypeRegistry syntheticRegistry() {
    auto files=std::make_shared<hpi::Vfs::Files>();
    auto file=[&](const std::string& path,const std::string& text) {
        std::string lines;
        for (char ch : text) { lines += ch; if (ch == ';' || ch == '{' || ch == '}') lines += '\n'; }
        (*files)[path]={lines.begin(),lines.end()};
    };
    auto type=[&](const char* id,const char* fields,const char* extra="") {
        file(std::string("units/")+id+".fbi",std::string("[UNITINFO]{unitname=")+id+";objectname="+id+
             ";side=ARA;maxdamage=100;footprintx=2;footprintz=2;buildtime=30;buildcostmetal=10;"
             "copyright=Copyright 1997 Humongous Entertainment. All rights reserved.;"+fields+"}"+extra);
    };
    // TA names its weapons from the FBI (weapon1..3=) as weapons/*.tdf sections.
    file("weapons/test.tdf","[GUN]{id=1;range=100;[DAMAGE]{default=5;}}[DUD]{id=2;range=200;[DAMAGE]{default=0;}}");
    type("soldier","canmove=1;maxvelocity=1;category=ARA LAND;weapon1=GUN;");
    type("tower","yardmap=oooo;weapon2=DUD;");            // a weapon, no damage
    type("lode","buildcostmetal=0;buildtime=92;yardmap=S;category=ARA;");
    type("mobilelode","canmove=1;maxvelocity=1;bmcode=1;yardmap=S;");  // yardmap ignored: bmcode 1
    type("ship","canmove=1;maxvelocity=1;minwaterdepth=20;floater=1;");
    type("worker","builder=1;workertime=30;canmove=1;maxvelocity=1;builddistance=90;sightdistance=300;");
    type("castle","builder=1;workertime=30;buildcostmetal=2000;");
    type("plain","isfeature=1;");
    // TA build menus: guis/<builder>1.gui, one button gadget (id 1) per unit.
    file("guis/worker1.gui","[GADGET0]{[COMMON]{id=0;name=worker1;}}[GADGET1]{[COMMON]{id=1;name=lode;}}");
    file("guis/castle1.gui","[GADGET0]{[COMMON]{id=0;name=castle1;}}[GADGET1]{[COMMON]{id=1;name=soldier;}}");
    hpi::Vfs vfs;vfs.setMapFiles(files);
    sim::TypeRegistry reg;reg.loadDir(vfs,"units");reg.loadBuildTree(vfs,"canbuild");
    return reg;
}

static void typeInfoAndPriorities() {
    const auto reg=syntheticRegistry();
    ai::ClassicBrain brain(0,11,reg);
    const uint32_t built=brain.seed();
    const auto& c=brain.catalogue();
    auto idx=[&](const char* n){return size_t(c.byName.at(n));};
    const auto& info=brain.typeInfo();
    check(c.size()==9 && info[idx("tower")].armed && info[idx("soldier")].armed &&
          !info[idx("worker")].armed,"armed = any weapon1..3 naming a weapon, even one with no damage");
    check(info[idx("lode")].manaSite && info[idx("lode")].lodestoneClass && info[idx("lode")].cost==92 &&
          info[idx("lode")].bmcode==0 && info[idx("tower")].structure && !info[idx("tower")].manaSite,
          "a yardmap S building is a mana site in the lodestone class; buildcost 0 falls back to buildtime");
    check(!info[idx("mobilelode")].manaSite && info[idx("mobilelode")].bmcode==1,
          "the yardmap is read for bmcode 0 only (retail's default bmcode is 0)");
    check(info[idx("ship")].minWaterDepth==20 && info[idx("ship")].floater && info[idx("soldier")].minWaterDepth==-10000,
          "minwaterdepth (type+0x194) is carried, -10000 for land units");
    check(info[idx("worker")].builder && info[idx("worker")].buildDistance==90 && info[idx("worker")].sight==300 &&
          info[idx("worker")].maxDamage==100 && info[idx("castle")].menu && info[idx("worker")].menu && !info[idx("tower")].menu &&
          info[idx("plain")].isFeature && c.categories.at("land").size()==1,
          "builder flag, builddistance, sight, maxdamage, menus, isfeature and category words");
    {
        const auto in=info[idx("tower")].priorityInputs(3,2,7);
        check(in.flags==0x10000u && in.secondaryFlags==0 && in.count==3 && in.completed==2 && in.desired==7 &&
              in.weapon==-10000 && in.cost==10,"the adapter fills retailBuildPriority's inputs by their retail meaning");
    }

    // A fixed World: what the clock sees, and the priorities it computes. Costs
    // stay <= 500 except the castle, so most values are fixed numbers.
    sim::World w;
    {
        std::vector<uint8_t> h(64*64,100);w.setTerrain(std::move(h),64,64,20);
    }
    w.spawn(reg.find("tower"),200,200,0,0);
    const int unfinished=w.spawn(reg.find("worker"),300,300,0,0);
    w.unit(unfinished)->underConstruction=true;
    w.spawn(reg.find("soldier"),400,400,0,1);   // another player's: not counted
    auto& me=w.player(0);me.mana=0;me.storage=1000;
    for (auto& sample : me.displayResources.samples) sample={0.0f,5.0f};   // draining: shortfall
    ai::ClassicProfileScript script{"ai/test.txt","limit worker 4\nlimit castle -1\n"};
    brain.useProfile(script);
    auto think=[&](uint32_t tick){brain.tick(w,tick,[](const net::Command&){});};
    think(0);
    const auto& pr=brain.priorities();
    check(brain.priorityRefreshes()==1 && pr[idx("soldier")]==84 && pr[idx("tower")]==84 &&
          pr[idx("worker")]==24 && pr[idx("lode")]==100 && pr[idx("ship")]==12 && pr[idx("plain")]==0,
          "the creation pass (in the constructor, before the profile) sees empty caches, no limits and no store: "
          "armed 21x4, a builder with limit -1 (+15, a third short of mana) x4, the empty store wants lodestones, "
          "naval x3, isfeature 0");
    check(brain.counts()[idx("tower")]==1 && brain.completed()[idx("tower")]==1 &&
          brain.counts()[idx("worker")]==1 && brain.completed()[idx("worker")]==0 && brain.counts()[idx("soldier")]==0,
          "the per-tick recount: own live units by type, and the finished ones");
    // Mirror the brain's stream through its construction: the AI manager's
    // squads and rolls first (0x40fed0), then the creation pass.
    uint32_t mirror=ai::ClassicBrain::seedFor(0,11);
    {
        ai::ClassicSquads squads;
        ai::classicConstructSquads(squads,0,[&](int n){return sim::retailRandom(mirror,n);});
        ai::classicDrawBlockGrids([&](int n){return sim::retailRandom(mirror,n);});
    }
    for (size_t t=1;t<c.size();++t)
        if (info[t].cost>500.0f) { // the creation pass's draws (only the castle costs > 500)
            sim::RetailBuildPriority in=info[t].priorityInputs(0,0,-1);
            sim::retailBuildPriority(in,uint16_t(0),0.0f,true,[&](int n){return sim::retailRandom(mirror,n);});
        }
    check(mirror==built,"the brain is built in retail's order: the manager's squad rolls, then the build "
          "manager's block grids (eight draws), then the creation pass, which draws only for types costing over "
          "500, at population 0");
    // The scheduler's draws now share the stream, so watch for the refresh.
    uint32_t refreshAt=1;
    while (refreshAt<10000) { think(refreshAt);if (brain.priorityRefreshes()>1) break;++refreshAt; }
    check(brain.priorityRefreshes()==2 && refreshAt%7==0,"a refresh comes only in a 7-tick window");
    check(brain.priorityRefreshes()==2 && pr[idx("tower")]==42 && pr[idx("worker")]==2 &&
          pr[idx("soldier")]==84 && pr[idx("lode")]==100,
          "a refresh uses the cached counts: one tower x2, an unfinished builder gets no builder bonus");
    me.mana=900;   // 90% full: no lodestone wanted
    for (auto& sample : me.displayResources.samples) sample={5.0f,0.0f};
    while (brain.priorityRefreshes()==2) think(++refreshAt);
    check(pr[idx("lode")]==0 && pr[idx("worker")]==2,"a full, growing store wants no lodestone");
    check(brain.buildWeight(idx("soldier"))==84 && brain.buildWeight(idx("worker"))==2 &&
          brain.buildWeight(idx("plain"))==0,"build weight = pref x priority / 100 under the limit");
}

static void shippedData(const char* root) {
    // The map's AI profile lookup, which the server and TA's own AI still use
    // (docs/retail-ai.md, "AI profile"): the chosen schema's aiprofile= names
    // ai\<value>.txt; an empty value or a file that fails to load is ai\default.txt.
    auto vfs=hpi::mountRetailRoot(root);
    const auto sea=hpi::findMap(vfs,"Anteer Strait");
    check(ai::classicMapAiProfile(vfs,sea)=="SeaBattle","a sea map's .ota names its SeaBattle profile");
    const auto script=ai::classicProfileScript(vfs,ai::classicMapAiProfile(vfs,sea));
    check(script.path=="ai/SeaBattle.txt" && !script.text.empty(),"aiprofile=SeaBattle reads ai\\SeaBattle.txt");
    const auto plain=hpi::findMap(vfs,"Ashap Plateau");
    check(ai::classicMapAiProfile(vfs,plain).empty(),"Ashap Plateau's schema leaves aiprofile empty");
    const auto fallback=ai::classicProfileScript(vfs,"");
    check(fallback.path=="ai/default.txt" && ai::classicProfileScript(vfs,"AIR").path=="ai/default.txt" &&
          ai::classicProfileScript(vfs,"default.txt").text==fallback.text,
          "an empty or unreadable aiprofile falls back to ai/default.txt; an extension is dropped");
    check(fallback.text.find("plan easy")!=std::string::npos && fallback.text.find("ARMACK")!=std::string::npos,
          "the VFS serves TA's shipped default.txt (plan blocks, ARM/CORE weights)");
}


// --- P3: the squad layer -----------------------------------------------------

static void squadConstruction() {
    using K=sim::RetailAiSquadKind;
    uint32_t seed=12345;
    ai::ClassicSquads s;
    ai::classicConstructSquads(s,0,[&](int n){return sim::retailRandom(seed,n);});
    bool kinds=s.schedule.assignmentCountdown==30 && !s.schedule.groups[0].present && !s.schedule.groups[99].present &&
               !s.firstPassDone;
    for (int i=1;i<=98;++i) {
        const auto& q=s.squads[size_t(i)];
        const K want=i<=20 ? K::Base : i<=80 ? (i%2 ? K::Strike : K::Backup) : i<=95 ? K::Vtol : i==96 ? K::Reserve :
                     i==97 ? K::Strike : K::Backup;
        kinds=kinds && s.schedule.groups[size_t(i)].present && s.schedule.groups[size_t(i)].deadline==0 && q.kind==want;
        if (want==K::Strike)
            kinds=kinds && q.parameters[0]==i+1 && q.parameters[3]==6 &&
                  q.parameters[1]==(i<51 ? 200000 : i<97 ? 500000 : 1000000);
        if (want==K::Backup) kinds=kinds && q.parameters[0]==i-1;
    }
    check(kinds,"0x40fed0: bases 1..20, ground strike/backup pairs 21..50, naval 51..80, VTOL 81..95, homeless 96, "
                "mass attack 97/98; every clock at 0, the countdown at 30");
    // The quota ranges and the wave rolls' shape over many constructions.
    bool ranges=true,negative=false,positive=false,capped=true;
    for (uint32_t k=1;k<400;++k) {
        uint32_t r=k*2654435761u|1u;
        ai::classicConstructSquads(s,k%300,[&](int n){return sim::retailRandom(r,n);});
        for (int i=1;i<=20;++i) {
            const auto& p=s.squads[size_t(i)].parameters;
            if (i==1) ranges=ranges && p[1]>=1 && p[1]<=2 && p[2]>=2 && p[2]<=5 && p[3]>=0 && p[3]<=3 && p[4]>=0 && p[4]<=3;
            else ranges=ranges && p[1]>=1 && p[1]<=2 && p[2]>=1 && p[2]<=2 && p[3]>=0 && p[3]<=1 && p[4]>=0 && p[4]<=1;
            for (int w=5;w<=7;++w) { capped=capped && p[w]!=0 && std::abs(p[w])<=50;negative|=p[w]<0;positive|=p[w]>0; }
        }
    }
    check(ranges,"0x40c560 quotas: home base p1 1..2, p2 2..5, p3/p4 0..3; other bases 1..2, 1..2, 0..1, 0..1");
    check(capped && negative && positive,"wave sizes are never 0, at most 50 in size, and either sign");
    // Draw counts of the rolls, by hand: rand(4)==0 skips the added terms.
    {
        std::vector<int> script{0,0, 59, 1, 1};   // rand(max(p2,2)), rand(4)=0, rand(60)!=0, rand(6)!=0, rand(2)!=0, sign
        size_t at=0;std::vector<int> asked;
        auto fake=[&](int n){asked.push_back(n);return at<script.size() ? script[at++]%std::max(n,1) : 1;};
        const int32_t p=ai::classicGroundWave(3,120,fake);
        check(p==1 && asked==std::vector<int>({3,4,60,6,2,2}),"ground roll: rand(4)==0 skips both enemy-size terms");
        asked.clear();at=0;script={1,1,1,0,1,0,0};
        // rand(3) 1, rand(4) 1 -> +rand(E/10=12) 1 -> rand(3) 0: no E/5 term; rand(60) 1, rand(6) 0 -> x5; sign 0
        const int32_t q=ai::classicGroundWave(3,120,fake);
        check(q==-15 && asked==std::vector<int>({3,4,12,3,60,6,2}),"ground roll: the E/5 term only inside the E/10 branch; x5; negated");
        asked.clear();at=0;script={1,1,1,1,1,1,1};
        const int32_t a=ai::classicAirWave(0,fake);
        check(a==3 && asked==std::vector<int>({2,4,2,100,10,2,2}),"air roll: a second rand(max(p3,2)), multipliers 1/100, 1/10, 1/2");
        asked.clear();at=0;
        ai::classicNavalWave(5,fake);
        check(asked==std::vector<int>({5,4,5,100,20,3,2,2}) || asked==std::vector<int>({5,4,5,100,20,3,2}),
              "naval roll: multipliers 1/100, 1/20, 1/3");
    }
    check(ai::classicMassAttackTicks("strategic opponent aramon massattack 90 logo 2")==2700 &&
          ai::classicMassAttackTicks("massattack0x10")==480 && ai::classicMassAttackTicks("massattack 010")==240 &&
          ai::classicMassAttackTicks("massattack x")==0 && ai::classicMassAttackTicks("opponent")==0,
          "massattack n: sscanf %i (0x and 0 prefixes), x30 ticks; none or no number is 0");
}

static ai::ClassicSquadUnit squadUnit(int id,int32_t x,int32_t z) {
    ai::ClassicSquadUnit u;u.id=id;
    u.flags=ai::ClassicSquadUnit::kLive|ai::ClassicSquadUnit::kSelectable;
    u.x=x<<16;u.z=z<<16;u.mover=true;
    return u;
}

// A strike's target (p4) is a unit id. Retail keeps a 16-bit slot there; World's
// ids are never reused and pass 65,535 in a long game, and the port used to mask
// them the same way, so a strike on unit 70,000 asked for unit 4,464 (none, or
// the wrong one), dropped its target and slept. The id is kept whole now.
static void strikeWideTargetIds() {
    struct Host {
        int picked=0;std::vector<ai::ClassicEnemy> enemies;int orders=0;ai::ClassicPoint goal{};
        int random(int k) { return k>1 ? 1 : 0; }           // never rand(250)==0: keep a live target
        int pick(const ai::ClassicPoint&,size_t) { return picked; }
        const ai::ClassicEnemy* target(int id) {
            for (const auto& e:enemies) if (id && e.id==id) return &e;
            return nullptr;
        }
        bool threat(std::pair<int32_t,int32_t>,int32_t) { return false; }
        bool reachable(const ai::ClassicSquadUnit&,const ai::ClassicEnemy&,int16_t,bool) { return true; }
        uint8_t resolve(int,size_t,const ai::ClassicPoint&) { return 1; }
        void order(uint8_t,bool,size_t,const ai::ClassicPoint& at,int32_t,int32_t) { ++orders;goal=at; }
        void clear(size_t) {}
        int width() const { return 256; }
        int height() const { return 256; }
    };
    auto run=[](int32_t target,int32_t contact,int picked,int32_t& p4,int32_t& p5,int& orders,ai::ClassicPoint& goal) {
        const int slot=ai::classic_slot::kFirstStrike;
        ai::ClassicSquads s;
        auto& strike=s.squads[size_t(slot)];
        strike.kind=sim::RetailAiSquadKind::Strike;strike.parameters[0]=slot+1;
        strike.parameters[4]=target;strike.parameters[5]=contact;strike.members={1};
        s.squads[size_t(slot+1)].kind=sim::RetailAiSquadKind::Backup;s.squads[size_t(slot+1)].parameters[0]=slot;
        std::vector<ai::ClassicSquadUnit> units{squadUnit(1,1000,1000)};
        units[0].flags|=ai::ClassicSquadUnit::kArmed;units[0].hp=100;units[0].maxDamage=100;units[0].squad=slot;
        Host h;h.picked=picked;
        for (const int id:{70000,70001}) {
            ai::ClassicEnemy e;e.id=id;e.flags=ai::ClassicEnemy::kLive;
            e.x=(id==70000 ? 3000 : 4000)<<16;e.z=2000<<16;h.enemies.push_back(e);
        }
        sim::RetailAiGroupClock clock{true,0};
        ai::classicStrikeThink(s,units,slot,clock,100,h);
        p4=strike.parameters[4];p5=strike.parameters[5];orders=h.orders;goal=h.goal;
    };
    int32_t p4=0,p5=0;int orders=0;ai::ClassicPoint goal{};
    run(70000,1,0,p4,p5,orders,goal);
    std::printf("  strike on unit 70000: p4 %d p5 %d, %d orders to x %d\n",p4,p5,orders,goal[0]>>16);
    check(p4==70000 && p5!=0,"a strike keeps its target unit 70000 (no 16-bit mask)");
    check(orders==1 && goal[0]==3000<<16,"the strike marches on unit 70000");
    run(0,0,70001,p4,p5,orders,goal);
    std::printf("  strike picks unit 70001: p4 %d p5 %d, %d orders to x %d\n",p4,p5,orders,goal[0]>>16);
    check(p4==70001 && p5!=0,"a strike stores a picked target unit 70001 whole");
    check(orders==1 && goal[0]==4000<<16,"the strike marches on unit 70001");
}

static void squadAssignment() {
    uint32_t seed=7;
    ai::ClassicSquads s;
    ai::classicConstructSquads(s,0,[&](int n){return sim::retailRandom(seed,n);});
    s.starts={ai::ClassicCell{3,4},std::nullopt,ai::ClassicCell{60,60}};
    std::vector<ai::ClassicSquadUnit> u;
    u.push_back(squadUnit(10,100,100));u[0].commander=true;u[0].mover=true;
    u.push_back(squadUnit(11,900,900));u[1].flags|=ai::ClassicSquadUnit::kArmed;
    u.push_back(squadUnit(12,950,950));u[2].finished=false;u[2].flags&=~ai::ClassicSquadUnit::kSelectable;
    u.push_back(squadUnit(13,400,400));u[3].carried=true;
    std::vector<std::pair<char,int>> log;
    auto standing=[&](size_t i){log.push_back({'s',u[i].id});};
    auto clear=[&](size_t i){log.push_back({'c',u[i].id});};
    ai::classicAssign(s,u,{20,2,2},standing,clear);
    check(s.firstPassDone && u[0].squad==1 && s.squads[1].parameters[0]==3 && u[1].squad==1 && u[2].squad==0 &&
          u[3].squad==0 && log==std::vector<std::pair<char,int>>({{'s',11}}),
          "first pass (tick 20): the Monarch joins base 1 without standing orders and anchors it on start 2; "
          "a finished unit gets standing orders and joins the nearest base; unfinished and carried units wait");
    {
        const ai::detail::ClassicUnitIndex index(u);
        const auto c=ai::classicBaseCentre(s,u,index,1);
        check(c && c->first==(60<<20)+0x80000 && c->second==(60<<20)+0x80000,
              "an anchored base's centre is its start cell's centre, whatever its members' positions");
    }
    // Later passes: a second base, the swap-remove order and the dying cleanup.
    u.push_back(squadUnit(14,10,10));u.push_back(squadUnit(15,20,20));
    u[2].finished=true;u[2].flags|=ai::ClassicSquadUnit::kSelectable;
    log.clear();
    ai::classicAssign(s,u,{500,2,2},standing,clear);
    check(s.squads[1].members==std::vector<int>({10,11,12,14,15}) && log.size()==3,
          "later passes append newly finished units to the nearest occupied base");
    u[1].flags|=ai::ClassicSquadUnit::kDying;
    ai::classicDropDying(s,u);
    check(u[1].squad==-1 && s.squads[1].members==std::vector<int>({10,15,12,14}),
          "0x40fe80: a dying member leaves (squad -1); the squad's last member takes its place");
    ai::classicAssign(s,u,{510,2,2},standing,clear);
    check(u[1].squad==-1,"a dropped unit is never assigned again");
    {
        // No first pass after tick 30 and no occupied base: base 2.
        ai::ClassicSquads late=s;late.firstPassDone=false;for (auto& q:late.squads) q.members.clear();
        late.squads[1].parameters[0]=0;
        std::vector<ai::ClassicSquadUnit> v{squadUnit(1,5,5),squadUnit(2,6,6)};v[0].commander=true;
        ai::classicAssign(late,v,{31,2,0},[](size_t){},[](size_t){});
        check(late.firstPassDone && late.squads[1].parameters[0]==0 && v[0].squad==2 && v[1].squad==2,
              "a brain whose first pass comes after tick 30 never anchors base 1; with no occupied base units join base 2");
    }
    {
        // The campaign mass attack: armed movers without a menu go to 98, every pass.
        ai::ClassicSquads m=s;m.massAttack=600;
        std::vector<ai::ClassicSquadUnit> v=u;
        for (auto& w:v) w.flags|=ai::ClassicSquadUnit::kArmed;
        v[4].menu=2;v[5].mover=false;v[3].finished=false;
        log.clear();
        ai::classicAssign(m,v,{599,2,2},standing,clear);
        const bool before=log.empty();
        ai::classicAssign(m,v,{600,2,2},standing,clear);
        check(before && v[0].squad==98 && v[3].squad==98 && v[2].squad==98 && v[4].squad==1 && v[5].squad==1 &&
              log.front()==std::pair<char,int>{'s',10} && log[1]==std::pair<char,int>{'c',10},
              "from the massattack tick: standing orders, missions cleared, slot 98 -- the Monarch and unfinished "
              "units too, not a unit with a menu or without a mover");
        ai::classicAssign(m,v,{900,2,2},standing,clear);
        check(m.squads[98].members.size()==3,"the mass attack takes nobody twice");
    }
}


// U13, the engine half: what our Move and AttackMove do to a column walking
// past enemies. Ten ARM Stumpies on open ground (both seats' standing
// orders: Stance 0) are ordered 1000 px down a lane that runs through ten CORE
// Raiders, once with Move and once with AttackMove (TA's Fight). The addresses
// below are KINGDOMS.icd's, inherited from TAK and unverified for TotalA.exe:
// retail's MOVE_GROUND
// is a point move without the interruptible flag 0x10000: the idle retaliation
// mission (0x409a50) takes only units with no mission, 0x10000 or a class-7
// one, and the fire-at-will sweep (0x40f330) fires every unit's auto weapon
// slots whatever its mission. So the column must walk through firing, never
// stop to engage, and arrive; AttackMove is what stops and fights.
static void classicMoveVsAttackMove(const char* root) {
    auto vfs=hpi::mountRetailRoot(root);
    sim::TypeRegistry reg;sim::setupRegistry(reg,vfs,false);
    // Two light-cannon tanks, the Stumpy and the Raider.
    const auto* mine=reg.find("armstump");
    const auto* theirs=reg.find("corraid");
    if (!mine || !theirs) { check(false,"armstump and corraid exist");return; }
    std::printf("  U13 (open ground, 10 %s through 10 %s):\n",mine->id.c_str(),theirs->id.c_str());
    for (int mode=0;mode<2;++mode) {
        // Open, level ground: the lane must not hang on a map's cliffs.
        sim::World w;w.setPlayerCount(2);
        w.setTerrain(std::vector<uint8_t>(256*256,70),256,256,20);
        w.buildNavClasses(reg);w.setPathService(true);
        const std::vector<std::pair<float,float>> starts{{3600,2048},{500,2048}};
        // The lane: from beside the ARM start toward the other start.
        const float sx=starts[1].first,sz=starts[1].second;
        float dx=starts[0].first-sx,dz=starts[0].second-sz;
        const float len=std::hypot(dx,dz);dx/=len;dz/=len;
        const float px=-dz,pz=dx;   // across the lane
        std::vector<int> column,enemies;
        for (int k=0;k<10;++k) {
            const float off=float(k%5-2)*36.0f,back=float(k/5)*36.0f;
            column.push_back(w.spawn(mine,sx+dx*(200-back)+px*off,sz+dz*(200-back)+pz*off,std::nullopt,1));
            enemies.push_back(w.spawn(theirs,sx+dx*(700+back)+px*(off+60),sz+dz*(700+back)+pz*(off+60),std::nullopt,0));
        }
        for (int id:column) { net::Command c;c.kind=net::Cmd::Stance;c.player=1;c.unitId=id;c.targetId=0;sim::applyCommand(w,reg,c); }
        for (int id:enemies) { net::Command c;c.kind=net::Cmd::Stance;c.player=0;c.unitId=id;c.targetId=0;sim::applyCommand(w,reg,c); }
        const float gx=sx+dx*1200,gz=sz+dz*1200;
        for (int k=0;k<10;++k) {
            net::Command c;c.kind=mode==0 ? net::Cmd::Move : net::Cmd::AttackMove;c.player=1;c.unitId=column[size_t(k)];
            c.x=gx+px*float(k%5-2)*36.0f;c.z=gz+pz*float(k%5-2)*36.0f;
            sim::applyCommand(w,reg,c);
        }
        auto enemyHp=[&]{ double hp=0;for (int id:enemies) if (const auto* e=w.unit(id);e && e->alive()) hp+=e->hp.toFloat();return hp; };
        const double hp0=enemyHp();
        double hpWhileWalking=hp0;
        std::set<int> engaged,arrived;
        uint32_t lastArrival=0;
        for (uint32_t tick=0;tick<1200;++tick) {
            w.tick(1.f/30.f);
            bool walking=false;
            for (int id:column) {
                const auto* u=w.unit(id);
                if (!u || !u->alive()) continue;
                // Its own order still pending (the goal leg is in the queue).
                bool pending=false;
                for (const auto& o:u->orders) if (o.goal && !o.autoTarget) pending=true;
                if (pending) {
                    walking=true;
                    if (!u->orders.empty() && u->orders.front().autoTarget) engaged.insert(id);
                } else if (!arrived.count(id) && std::hypot(u->x.toFloat()-gx,u->z.toFloat()-gz)<160) {
                    arrived.insert(id);lastArrival=tick;
                }
            }
            if (walking) hpWhileWalking=enemyHp();
        }
        int alive=0;for (int id:column) if (const auto* u=w.unit(id);u && u->alive()) ++alive;
        const char* name=mode==0 ? "Move" : "AttackMove";
        std::printf("    %-10s: %zu of 10 stopped to engage on the way, %zu arrived (the last at %.1f s), %d alive; "
                    "the enemy lost %.0f of %.0f hp while the column walked\n",name,engaged.size(),arrived.size(),
                    double(lastArrival)/30.0,alive,hp0-hpWhileWalking,hp0);
        if (mode==0) {
            check(engaged.empty(),"U13: a column on Move never stops to engage (retail MOVE_GROUND)");
            check(hp0-hpWhileWalking>0,"U13: a column on Move fires while it walks (the fire-at-will sweep)");
            // Who wins the firefight is not the point, and it turns on small
            // timing changes: shifting the column's start 2-24 px makes it fall
            // to the last man in most runs (main 259d26a5: 8 of 12 shifts).
            // With nobody left there is no surviving column to arrive.
            if (alive==0) std::printf("    (the whole column fell on the way: no arrival to check)\n");
            check(alive==0 || arrived.size()>=size_t(std::max(1,alive-1)),"U13: the surviving column on Move arrives");
        } else {
            check(!engaged.empty(),"U13: a column on AttackMove stops to engage (not what a strike gives)");
        }
    }
}

// The port's side of tools/re/check_ai_assignment.py: 0x40fe80 then 0x40eeb0 on
// the case's units and squads (see the script for the line layout).
static int assignmentOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        uint32_t tick,mass;int32_t mode,start;int first;
        in>>tick>>mode>>mass>>start>>first;
        ai::ClassicSquads s;
        uint32_t seed=1;
        ai::classicConstructSquads(s,0,[&](int n){return sim::retailRandom(seed,n);});
        s.firstPassDone=first!=0;s.massAttack=mass;
        auto table=[&](std::vector<std::optional<ai::ClassicCell>>& t) {
            size_t n;in>>n;t.resize(n);
            for (auto& r:t) { int valid,x,z;in>>valid>>x>>z;if (valid) r=ai::ClassicCell{int16_t(x),int16_t(z)}; }
        };
        table(s.starts);table(s.sites);
        for (int i=1;i<=20;++i) in>>s.squads[size_t(i)].parameters[0];
        size_t n;in>>n;
        std::vector<ai::ClassicSquadUnit> units(n);
        for (size_t i=0;i<n;++i) {
            auto& u=units[i];u.id=int(i);
            uint32_t flags,f108;int commander,sacred,builder,held,carrier,mover,fx,fz;
            in>>flags>>commander>>sacred>>builder>>f108>>held>>carrier>>mover>>u.menu>>u.x>>u.z>>fx>>fz>>u.squad;
            u.flags=flags;u.commander=commander;u.sacredSite=sacred;u.builder=builder;
            const float remaining=std::bit_cast<float>(f108);
            u.finished=!(remaining<0.0f || remaining>0.0f);   // fcomp: equal or unordered
            u.held=held!=0;u.carried=carrier!=0;u.carrierHolds=carrier==1;u.mover=mover!=0;
            u.footX=int16_t(fx);u.footZ=int16_t(fz);
        }
        for (int slot=1;slot<=98;++slot) {
            size_t k;in>>k;auto& m=s.squads[size_t(slot)].members;m.resize(k);
            for (auto& id:m) in>>id;
        }
        std::vector<std::pair<int,int>> log;
        ai::classicDropDying(s,units);
        ai::classicAssign(s,units,{tick,mode,start},
            [&](size_t i){log.push_back({int(i),2});},[&](size_t i){log.push_back({int(i),0});});
        std::printf("%d",int(s.firstPassDone));
        for (int i=1;i<=20;++i) std::printf(" %d",s.squads[size_t(i)].parameters[0]);
        for (const auto& u:units) std::printf(" %d",u.squad);
        for (int slot=1;slot<=98;++slot) {
            std::printf(" %zu",s.squads[size_t(slot)].members.size());
            for (int id:s.squads[size_t(slot)].members) std::printf(" %d",id);
        }
        std::printf(" %zu",log.size());
        for (const auto& [i,k]:log) std::printf(" %d %d",i,k);
        std::printf("\n");
    }
    return 0;
}

// The port's side of tools/re/check_ai_factory_think.py: the economy step's
// allocation (sim::RetailConstructionResources), then one base think through
// the brain's own templates -- the squad clock, classicBaseMembers and
// classicPlanFactory -- with the case's build-manager and type tables (see the
// script for the line layout).
static int factoryThinkOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        uint32_t tick,seed,storedBits,overrideBits,producedBits,incomeBits,storageBits,demandBits;
        int slot,limited,population,threat;int32_t anchor;double total,excess;
        in>>tick>>slot>>anchor>>seed>>limited>>population>>threat>>storedBits>>overrideBits>>producedBits
          >>total>>excess>>incomeBits>>storageBits>>demandBits;
        sim::RetailConstructionResources r;
        r.stored=std::bit_cast<float>(storedBits);r.capacityOverride=std::bit_cast<float>(overrideBits);
        r.produced=std::bit_cast<float>(producedBits);r.totalProduced=total;r.excess=excess;
        r.beginTick(std::bit_cast<float>(incomeBits),std::bit_cast<float>(storageBits),std::bit_cast<float>(demandBits));
        ai::ClassicSquads s;
        auto table=[&](std::vector<std::optional<ai::ClassicCell>>& t) {
            size_t n;in>>n;t.resize(n);
            for (auto& c:t) { int valid,x,z;in>>valid>>x>>z;if (valid) c=ai::ClassicCell{int16_t(x),int16_t(z)}; }
        };
        table(s.starts);table(s.sites);
        struct Type { int pref,prio,limit,count,lode,side,commander,menuField;float cost;std::vector<uint16_t> menu; };
        size_t ntypes;in>>ntypes;
        std::vector<Type> types(ntypes);
        for (auto& t:types) {
            uint32_t cost;size_t k;
            in>>t.pref>>t.prio>>t.limit>>t.count>>t.lode>>t.side>>cost>>t.commander>>t.menuField>>k;
            t.cost=std::bit_cast<float>(cost);t.menu.resize(k);
            for (auto& e:t.menu) in>>e;
        }
        size_t n;in>>n;
        std::vector<ai::ClassicSquadUnit> units(n);
        for (size_t i=0;i<n;++i) {
            auto& u=units[i];u.id=int(i);
            uint32_t flags,f108;int t,held,carrier,mover,fx,fz,producing,first;
            in>>flags>>t>>f108>>held>>carrier>>mover>>u.x>>u.z>>fx>>fz>>producing>>first;
            u.flags=flags;u.type=uint16_t(t);u.commander=types[size_t(t)].commander!=0;
            const float remaining=std::bit_cast<float>(f108);
            u.finished=!(remaining<0.0f || remaining>0.0f);   // fcomp: equal or unordered
            u.held=held!=0;u.carried=carrier!=0;u.carrierHolds=carrier==1;u.mover=mover!=0;
            u.menu=types[size_t(t)].menuField;u.footX=int16_t(fx);u.footZ=int16_t(fz);
            u.producing=producing!=0;u.missionClass=uint8_t(first);u.squad=slot;
        }
        auto& base=s.squads[size_t(slot)];
        base.kind=sim::RetailAiSquadKind::Base;base.parameters[0]=anchor;
        size_t m;in>>m;base.members.resize(m);
        for (auto& id:base.members) in>>id;
        std::vector<int> draws;
        std::vector<std::array<int,3>> log;
        auto random=[&](int k){draws.push_back(k);return sim::retailRandom(seed,k);};
        sim::RetailAiGroupClock clock{true,0};
        sim::retailTickAiSquad(base,clock,tick,random,[](auto&){},[&](auto&) {
            ai::classicBaseMembers(s,units,slot,
                [&](const std::pair<int32_t,int32_t>&,int32_t){log.push_back({0,0,0});return threat!=0;},
                [&](size_t i) {
                    const auto& b=types[units[i].type];
                    ai::classicPlanFactory(r.allocation,units[i].producing,uint16_t(population),b.menu,
                        [&](uint16_t t){const auto& e=types[t];
                            return sim::retailAiBuildWeight(limited!=0,e.limit,int16_t(e.count),uint8_t(e.pref),int8_t(e.prio));},
                        [&](uint16_t t){return types[t].lode!=0;},
                        [&](uint16_t t){return types[t].side==b.side;},
                        [&](uint16_t t){return types[t].cost;},random,
                        [&](uint16_t t){log.push_back({1,int(i),int(t)});});
                },
                [&](size_t i,bool danger){log.push_back({2,int(i),int(danger)});},
                [&](size_t i,bool danger){log.push_back({3,int(i),int(danger)});});
        });
        std::printf("%u %d %u %zu",clock.deadline,base.parameters[0],seed,draws.size());
        for (int d:draws) std::printf(" %d",d);
        std::printf(" %zu",log.size());
        for (const auto& e:log) std::printf(" %d %d %d",e[0],e[1],e[2]);
        std::printf("\n");
    }
    return 0;
}

// The port's side of tools/re/check_ai_placement.py: the build manager's block
// grids (classicDrawBlockGrids, 0x4110f0) from a seed, then one 0x412f80 with
// them (classicPlaceSite over classicDepositSite / classicScatterSite) on the
// case's feature plane, sacred-site records and placement answers. Line:
//   seed width height ntypes <sacredsite bits>*ntypes ncells (x z feature backX backZ)*ncells
//   nsites (valid x z)*nsites centreX centreZ radius footX footZ minwaterdepth
//   bmcode armed range menu yardmap(-1 none, else its first byte) nanswers answer*nanswers
// Out: the ten grid fields, the seed after them, then "ok x z" (or "0"), the final
// seed, the draws (count then each n) and the placement test's cells (count then x z).
static int placementOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        uint32_t seed;int w,h;size_t ntypes;
        in>>seed>>w>>h>>ntypes;
        std::vector<bool> sacred(ntypes);
        for (size_t i=0;i<ntypes;++i) { uint32_t b;in>>b;sacred[i]=ai::classicSacredValue(std::bit_cast<float>(b)); }
        std::vector<ai::ClassicFeatureCell> plane(size_t(w)*size_t(h));
        size_t n;in>>n;
        for (size_t i=0;i<n;++i) { int x,z,f,bx,bz;in>>x>>z>>f>>bx>>bz;plane[size_t(z)*w+x]={uint16_t(f),uint8_t(bx),uint8_t(bz)}; }
        std::vector<std::optional<std::pair<int16_t,int16_t>>> sites;
        in>>n;
        for (size_t i=0;i<n;++i) { int valid,x,z;in>>valid>>x>>z;sites.push_back(valid ? std::optional{std::pair{int16_t(x),int16_t(z)}} : std::nullopt); }
        int32_t cx,cz,radius;int fx,fz,mwd,bm,armed,range,menu,yard;
        in>>cx>>cz>>radius>>fx>>fz>>mwd>>bm>>armed>>range>>menu>>yard;
        std::vector<int> answers;in>>n;answers.resize(n);
        for (auto& a:answers) in>>a;
        std::vector<int> draws;
        auto random=[&](int k){draws.push_back(k);return sim::retailRandom(seed,k);};
        const auto grids=ai::classicDrawBlockGrids(random);
        const uint32_t gridSeed=seed;
        draws.clear();
        ai::ClassicSiteType type;
        type.footX=int16_t(fx);type.footZ=int16_t(fz);type.minWaterDepth=int16_t(mwd);
        type.tower=ai::classicSiteTower(uint8_t(bm),armed!=0,range,menu);
        type.lodestone=yard>=0 && (yard&0x80);
        std::vector<std::pair<int,int>> asked;size_t next=0;
        auto valid=[&](int16_t x,int16_t z){asked.push_back({x,z});return next<answers.size() && answers[next++]!=0;};
        auto cellAt=[&](int x,int z){
            if (x<0 || z<0 || x>=w || z>=h) return ai::ClassicFeatureCell{};
            return plane[size_t(z)*w+x];
        };
        auto isSacred=[&](int x,int z){
            return ai::classicSacredCell(x,z,uint32_t(ntypes),cellAt,[&](uint16_t f){return f<sacred.size() && sacred[f];});
        };
        const auto site=ai::classicPlaceSite(type,
            [&]{return ai::classicDepositSite(sites,{cx,cz},radius,type.footX,type.footZ,
                    [&](int16_t x,int16_t z){return x>=0 && z>=0 && x<w && z<h && isSacred(x,z);},valid);},
            [&]{return ai::classicScatterSite(grids,{cx,cz},radius,type,random,valid,isSacred);});
        for (const auto& g:{grids.land,grids.water})
            std::printf("%d %d %d %d %d ",g.stepX,g.stepZ,g.offX,g.offZ,g.margin);
        std::printf("%u",gridSeed);
        if (site) std::printf(" 1 %d %d",site->first,site->second); else std::printf(" 0");
        std::printf(" %u %zu",seed,draws.size());
        for (int d:draws) std::printf(" %d",d);
        std::printf(" %zu",asked.size());
        for (const auto& [x,z]:asked) std::printf(" %d %d",x,z);
        std::printf("\n");
    }
    return 0;
}

// The port's side of tools/re/check_ai_constructor.py: one constructor planner
// call (ai::classicPlanConstructor) for a base member, over the case's base,
// build manager, types, map and answers, with the brain's squad helpers and
// placers under it. Line layout (see the script):
//   tick alarm allocBits threat limited slot anchor p1 seed builder
//   grids(10) width height nft <sacredsite bits>*nft ncells (x z f bx bz)* nsites (valid x z)*
//   nstarts (valid x z)* ntypes (pref prio limit count yard side costBits commander menuField k menu*k
//     bmcode armed range footX footZ mwd buildDistance maxDamage manaSite unitCost canFly floater)*ntypes
//   nunits (flags type x z fx fz mission class flags5a buildingBuild buildsManaSite mover damage reload)*nunits
//   hp manaBits maxManaBits weapon costBits multBits cellX cellZ navigator
//   nmembers member* nplace answer* nreach answer*
// Out: the final seed, then the event log: 0 n (a draw), 1 x z (placement test),
// 2 sx sz x z w h (reachability), 3 type x z (build), 4 threat (defence),
// 5 (retreat), 6 (cancel).
static int constructorOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        uint32_t tick,alarm,allocBits,seed;int threat,limited,slot,p1;int32_t anchor;size_t builder;
        in>>tick>>alarm>>allocBits>>threat>>limited>>slot>>anchor>>p1>>seed>>builder;
        ai::ClassicBlockGrids grids;
        for (auto* g:{&grids.land,&grids.water}) { int a,b,c,d,e;in>>a>>b>>c>>d>>e;*g={int16_t(a),int16_t(b),int16_t(c),int16_t(d),e}; }
        int w,h;size_t nft;in>>w>>h>>nft;
        std::vector<bool> sacred(nft);
        for (size_t i=0;i<nft;++i) { uint32_t b;in>>b;sacred[i]=ai::classicSacredValue(std::bit_cast<float>(b)); }
        std::vector<ai::ClassicFeatureCell> plane(size_t(w)*size_t(h));
        size_t n;in>>n;
        for (size_t i=0;i<n;++i) { int x,z,f,bx,bz;in>>x>>z>>f>>bx>>bz;plane[size_t(z)*w+x]={uint16_t(f),uint8_t(bx),uint8_t(bz)}; }
        ai::ClassicSquads s;
        auto table=[&](std::vector<std::optional<ai::ClassicCell>>& t) {
            size_t k;in>>k;t.resize(k);
            for (auto& c:t) { int valid,x,z;in>>valid>>x>>z;if (valid) c=ai::ClassicCell{int16_t(x),int16_t(z)}; }
        };
        table(s.sites);table(s.starts);
        struct Type { int pref,prio,limit,count,yard,side,commander,menuField,bmcode,armed,range,fx,fz,mwd,reach,maxDamage,manaSite,unitCost,canFly,floater;float cost;std::vector<uint16_t> menu; };
        size_t ntypes;in>>ntypes;
        std::vector<Type> types(ntypes);
        for (auto& t:types) {
            uint32_t cost;size_t k;
            in>>t.pref>>t.prio>>t.limit>>t.count>>t.yard>>t.side>>cost>>t.commander>>t.menuField>>k;
            t.cost=std::bit_cast<float>(cost);t.menu.resize(k);
            for (auto& e:t.menu) in>>e;
            in>>t.bmcode>>t.armed>>t.range>>t.fx>>t.fz>>t.mwd>>t.reach>>t.maxDamage>>t.manaSite>>t.unitCost>>t.canFly>>t.floater;
        }
        in>>n;
        std::vector<ai::ClassicSquadUnit> units(n);
        for (size_t i=0;i<n;++i) {
            auto& u=units[i];u.id=int(i);
            uint32_t flags;int t,fx,fz,mission,cls,f5a,bb,ms,mover,damage,reload;
            in>>flags>>t>>u.x>>u.z>>fx>>fz>>mission>>cls>>f5a>>bb>>ms>>mover>>damage>>reload;
            const Type& ty=types[size_t(t)];
            u.flags=flags;u.type=uint16_t(t);u.footX=int16_t(fx);u.footZ=int16_t(fz);
            u.commander=ty.commander!=0;u.menu=ty.menuField;u.mover=mover!=0;
            u.typeFlags=(ty.armed ? 0x10000u : 0u)|(ty.canFly ? 0x800u : 0u)|(ty.floater ? 0x80000u : 0u);
            u.secondaryFlags=(ty.commander ? 0x40000u : 0u)|(ty.manaSite ? 0x80000000u : 0u);
            u.minWaterDepth=int16_t(ty.mwd);
            u.mission=mission!=0;u.missionClass=uint8_t(cls);u.missionFlags=uint8_t(f5a);
            u.buildingBuild=bb!=0;u.buildsManaSite=ms!=0;
            u.weaponDamage=uint16_t(damage);u.weaponReload=uint16_t(reload);u.squad=slot;
        }
        int hp,weapon,cellX,cellZ,navigator;uint32_t manaBits,maxManaBits,costBits,multBits;
        in>>hp>>manaBits>>maxManaBits>>weapon>>costBits>>multBits>>cellX>>cellZ>>navigator;
        auto& base=s.squads[size_t(slot)];
        base.kind=sim::RetailAiSquadKind::Base;base.parameters[0]=anchor;base.parameters[1]=p1;
        in>>n;base.members.resize(n);
        for (auto& id:base.members) in>>id;
        std::vector<int> place,reach;
        in>>n;place.resize(n);for (auto& a:place) in>>a;
        in>>n;reach.resize(n);for (auto& a:reach) in>>a;

        std::vector<std::vector<int>> log;
        auto random=[&](int k){log.push_back({0,k});return sim::retailRandom(seed,k);};
        const ai::detail::ClassicUnitIndex index(units);
        const auto centre=*ai::classicBaseCentre(s,units,index,slot);
        const auto& me=units[builder];
        const Type& bt=types[me.type];
        ai::ClassicConstructorUnit u;
        u.commander=bt.commander!=0;u.hp=int16_t(hp);u.maxDamage=bt.maxDamage;
        u.mana=std::bit_cast<float>(manaBits);u.maxMana=std::bit_cast<float>(maxManaBits);
        u.weapon=weapon!=0;
        u.shotCost=ai::classicShotCost(std::bit_cast<float>(costBits),bt.unitCost ? 1.0f : std::bit_cast<float>(multBits));
        u.mission=me.mission;u.missionClass=me.missionClass;u.building=me.mission && (me.missionFlags&8);
        u.navigator=navigator!=0;
        size_t nextPlace=0,nextReach=0;
        auto cellAt=[&](int x,int z){
            if (x<0 || z<0 || x>=w || z>=h) return ai::ClassicFeatureCell{};
            return plane[size_t(z)*w+x];
        };
        auto isSacred=[&](int x,int z){
            return ai::classicSacredCell(x,z,uint32_t(nft),cellAt,[&](uint16_t f){return f<sacred.size() && sacred[f];});
        };
        struct Host {
            std::function<float()> allocation;
            std::function<int32_t()> firepower,builders,desiredBuilders;
            std::function<uint32_t()> tick,alarm;
            std::function<uint16_t(bool)> draw;
            std::function<ai::ClassicConstructorType(uint16_t)> type;
            std::function<bool(uint16_t)> idleFactory;
            std::function<int32_t(uint16_t)> typeCount;
            std::function<std::optional<int32_t>(unsigned)> radius;
            std::function<std::optional<std::pair<int32_t,int32_t>>(uint16_t,int32_t)> site;
            std::function<bool(uint16_t,std::pair<int32_t,int32_t>)> reachable;
            std::function<bool()> lodestoneBusy;
            std::function<void(uint16_t,std::pair<int32_t,int32_t>)> build;
            std::function<void()> cancel,retreat,combat;
        } host;
        host.allocation=[&]{return std::bit_cast<float>(allocBits);};
        host.firepower=[&]{return ai::classicBaseFirepower(s,units,index,slot,4);};
        host.builders=[&]{return ai::classicBaseCount(s,units,index,slot,2);};
        host.desiredBuilders=[&]{return base.parameters[1];};
        host.tick=[&]{return tick;};host.alarm=[&]{return alarm;};
        host.draw=[&](bool special){
            return ai::classicMenuDraw(bt.menu,special,
                [&](uint16_t t){const auto& e=types[t];
                    return sim::retailAiBuildWeight(limited!=0,e.limit,int16_t(e.count),uint8_t(e.pref),int8_t(e.prio));},
                [&](uint16_t t){return types[t].yard>=0 && (types[t].yard&0x80);},
                [&](uint16_t t){return types[t].side==bt.side;},random);
        };
        host.type=[&](uint16_t t){const auto& e=types[t];return ai::ClassicConstructorType{uint8_t(e.bmcode),e.menuField,e.armed!=0};};
        host.idleFactory=[&](uint16_t t){return ai::classicIdleFactory(s,units,index,slot,t);};
        host.typeCount=[&](uint16_t t){return t>=1 && t<types.size() ? int32_t(int16_t(types[t].count)) : 0;};
        host.radius=[&](unsigned ring){return ai::classicBaseRadius(s,units,index,slot,ring);};
        host.site=[&](uint16_t t,int32_t r){
            const auto& e=types[t];
            ai::ClassicSiteType st;
            st.footX=int16_t(e.fx);st.footZ=int16_t(e.fz);st.minWaterDepth=int16_t(e.mwd);
            st.tower=ai::classicSiteTower(uint8_t(e.bmcode),e.armed!=0,e.range,e.menuField);
            st.lodestone=e.yard>=0 && (e.yard&0x80);
            auto valid=[&](int16_t x,int16_t z){log.push_back({1,x,z});return nextPlace<place.size() && place[nextPlace++]!=0;};
            return ai::classicPlaceSite(st,
                [&]{return ai::classicDepositSite(s.sites,centre,r,st.footX,st.footZ,
                        [&](int16_t x,int16_t z){return x>=0 && z>=0 && x<w && z<h && isSacred(x,z);},valid);},
                [&]{return ai::classicScatterSite(grids,centre,r,st,random,valid,isSacred);});
        };
        host.reachable=[&](uint16_t t,std::pair<int32_t,int32_t> at){
            const auto& e=types[t];
            const auto r=ai::classicReachRect(at,int16_t(e.fx),int16_t(e.fz),uint16_t(bt.reach));
            log.push_back({2,cellX,cellZ,r.x,r.z,r.w,r.h});
            return nextReach<reach.size() && reach[nextReach++]!=0;
        };
        host.lodestoneBusy=[&]{return ai::classicLodestoneBusy(s,units,index,slot);};
        host.build=[&](uint16_t t,std::pair<int32_t,int32_t> at){log.push_back({3,int(t),at.first,at.second});};
        host.cancel=[&]{log.push_back({6});};
        host.retreat=[&]{log.push_back({5});};
        host.combat=[&]{log.push_back({4,threat});};
        struct Calls {
            Host& h;
            float allocation(){return h.allocation();}
            int32_t firepower(){return h.firepower();}
            int32_t builders(){return h.builders();}
            int32_t desiredBuilders(){return h.desiredBuilders();}
            uint32_t tick(){return h.tick();}
            uint32_t alarm(){return h.alarm();}
            uint16_t draw(bool special){return h.draw(special);}
            ai::ClassicConstructorType type(uint16_t t){return h.type(t);}
            bool idleFactory(uint16_t t){return h.idleFactory(t);}
            int32_t typeCount(uint16_t t){return h.typeCount(t);}
            std::optional<int32_t> radius(unsigned ring){return h.radius(ring);}
            std::optional<std::pair<int32_t,int32_t>> site(uint16_t t,int32_t r){return h.site(t,r);}
            bool reachable(uint16_t t,std::pair<int32_t,int32_t> at){return h.reachable(t,at);}
            bool lodestoneBusy(){return h.lodestoneBusy();}
            void build(uint16_t t,std::pair<int32_t,int32_t> at){h.build(t,at);}
            void cancel(){h.cancel();}
            void retreat(){h.retreat();}
            void combat(){h.combat();}
        } calls{host};
        ai::classicPlanConstructor(calls,u,threat!=0);
        std::printf("%u %zu",seed,log.size());
        for (const auto& e:log) for (int v:e) std::printf(" %d",v);
        std::printf("\n");
    }
    return 0;
}

// The port's side of tools/re/check_ai_threat.py: ai::classicThreat then
// ai::classicEnemiesNear on the case's lists (see the script for the layout).
static int threatOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        int32_t cx,cz,radius;int upgrade;size_t n;
        in>>cx>>cz>>radius>>upgrade>>n;
        std::vector<ai::ClassicEnemy> all(n);
        for (size_t i=0;i<n;++i) { auto& e=all[i];uint32_t f;in>>f>>e.x>>e.z;e.flags=f;e.id=int(i); }
        auto list=[&]{ size_t k;in>>k;std::vector<ai::ClassicEnemy> l(k);for (auto& e:l) { size_t i;in>>i;e=all[i]; }return l; };
        const auto visible=list(),upgraded=list();
        const bool threat=ai::classicThreat(visible,{cx,cz},radius);
        const auto out=ai::classicEnemiesNear(visible,{cx,cz},radius,upgrade!=0,&upgraded);
        std::printf("%d %zu",int(threat),out.size());
        for (const auto& e:out) std::printf(" %d",e.id);
        std::printf("\n");
    }
    return 0;
}

// The port's side of tools/re/check_ai_defence.py (--defence: 0x40d370) and
// check_ai_retreat.py (--retreat: 0x40d010): ai::classicPlanCombat /
// ai::classicRetreat with the brain's squad helpers on the case's base, units,
// enemies and answers (see check_ai_defence.py for the line layout).
static int defenceOracle(bool retreat) {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        int w,h,threat,upgrade;int32_t anchor;uint32_t seed;
        in>>w>>h>>anchor>>threat>>seed>>upgrade;
        ai::ClassicSquads s;
        auto table=[&](std::vector<std::optional<ai::ClassicCell>>& t) {
            size_t k;in>>k;t.resize(k);
            for (auto& c:t) { int valid,x,z;in>>valid>>x>>z;if (valid) c=ai::ClassicCell{int16_t(x),int16_t(z)}; }
        };
        table(s.sites);table(s.starts);
        constexpr int slot=1;
        size_t n;in>>n;
        std::vector<ai::ClassicSquadUnit> units(n);
        for (size_t i=0;i<n;++i) {
            auto& u=units[i];u.id=int(i);
            int fx,fz;in>>u.flags>>u.x>>u.z>>fx>>fz;
            u.footX=int16_t(fx);u.footZ=int16_t(fz);u.squad=slot;u.mover=true;
        }
        auto& base=s.squads[size_t(slot)];
        base.kind=sim::RetailAiSquadKind::Base;base.parameters[0]=anchor;
        in>>n;base.members.resize(n);
        for (auto& id:base.members) in>>id;
        ai::ClassicCombatUnit u;
        int commander,builder,canFly,hp,sight,melee,has,target;int64_t maxDamage;
        in>>commander>>builder>>canFly>>maxDamage>>hp>>sight>>u.range>>melee>>u.at[0]>>u.at[1]>>u.at[2];
        in>>has>>u.missionFlags>>target>>u.goalX>>u.goalZ;
        u.armed=units[0].flags&0x8000000u;u.commander=commander!=0;u.builder=builder!=0;u.canFly=canFly!=0;
        u.maxDamage=uint32_t(maxDamage);u.hp=int16_t(hp);u.sight=int16_t(sight);u.melee=melee!=0;
        u.mission=has!=0;u.missionTarget=target>=0 ? 1000+target : 0;
        size_t nen,nen2;in>>nen>>nen2;
        std::vector<ai::ClassicEnemy> visible,upgraded;std::map<int,int32_t> damage;
        for (size_t k=0;k<nen+nen2;++k) {
            ai::ClassicEnemy e;int cloak;int32_t d;
            in>>e.flags>>e.x>>e.y>>e.z>>cloak>>d;
            e.id=1000+int(k);e.cloaked=cloak!=0;damage[e.id]=d;
            (k<nen ? visible : upgraded).push_back(e);
        }
        std::vector<int> place,kinds;
        in>>n;place.resize(n);for (auto& a:place) in>>a;
        in>>n;kinds.resize(n);for (auto& a:kinds) in>>a;

        std::vector<std::vector<int>> log;
        const ai::detail::ClassicUnitIndex index(units);
        size_t nextPlace=0,nextKind=0;
        struct Host {
            std::function<std::optional<ai::ClassicPoint>()> centre;
            std::function<std::optional<int32_t>(unsigned)> radius;
            std::function<std::vector<ai::ClassicEnemy>(std::pair<int32_t,int32_t>,int32_t)> enemies;
            std::function<int32_t(const ai::ClassicEnemy&)> damage;
            std::function<int(int)> random;
            std::function<bool(int32_t,int32_t)> placeable;
            int w,h;
            std::function<uint8_t(int,const ai::ClassicPoint&)> resolve;
            std::function<void(uint8_t,bool,const ai::ClassicPoint&,int32_t,int32_t)> order;
            int width() const { return w; }
            int height() const { return h; }
        } host;
        host.centre=[&]()->std::optional<ai::ClassicPoint> {
            const auto c=ai::classicBaseCentre(s,units,index,slot);
            if (!c) return std::nullopt;
            return ai::ClassicPoint{c->first,0,c->second};
        };
        host.radius=[&](unsigned ring){return ai::classicBaseRadius(s,units,index,slot,ring);};
        host.enemies=[&](std::pair<int32_t,int32_t> at,int32_t r){return ai::classicEnemiesNear(visible,at,r,upgrade!=0,&upgraded);};
        host.damage=[&](const ai::ClassicEnemy& e){return damage[e.id];};
        host.random=[&](int k){log.push_back({0,k});return int(sim::retailRandom(seed,k));};
        host.placeable=[&](int32_t x,int32_t z){
            log.push_back({1,ai::classicFootprintCell(x,units[0].footX),ai::classicFootprintCell(z,units[0].footZ)});
            return nextPlace<place.size() && place[nextPlace++]!=0;
        };
        host.w=w;host.h=h;
        host.resolve=[&](int command,const ai::ClassicPoint& p){
            log.push_back({2,command,p[0],p[1],p[2]});
            return uint8_t(nextKind<kinds.size() ? kinds[nextKind++] : 0);
        };
        host.order=[&](uint8_t kind,bool queue,const ai::ClassicPoint& p,int32_t a6,int32_t a7){
            log.push_back({3,int(kind),int(queue),p[0],p[1],p[2],a6,a7});
        };
        struct Calls {
            Host& h;
            std::optional<ai::ClassicPoint> centre(){return h.centre();}
            std::optional<int32_t> radius(unsigned ring){return h.radius(ring);}
            std::vector<ai::ClassicEnemy> enemies(std::pair<int32_t,int32_t> at,int32_t r){return h.enemies(at,r);}
            int32_t damage(const ai::ClassicEnemy& e){return h.damage(e);}
            int random(int k){return h.random(k);}
            bool placeable(int32_t x,int32_t z){return h.placeable(x,z);}
            int width(){return h.width();}
            int height(){return h.height();}
            uint8_t resolve(int c,const ai::ClassicPoint& p){return h.resolve(c,p);}
            void order(uint8_t k,bool q,const ai::ClassicPoint& p,int32_t a6,int32_t a7){h.order(k,q,p,a6,a7);}
        } calls{host};
        if (retreat) ai::classicRetreat(calls,u);
        else ai::classicPlanCombat(calls,u,threat!=0);
        std::printf("%u %zu",seed,log.size());
        for (const auto& e:log) for (int v:e) std::printf(" %d",v);
        std::printf("\n");
    }
    return 0;
}

// The port's side of tools/re/check_ai_target_pick.py: ai::classicPickTarget
// over the case's player records (retail's filter: active, kind 1-3, an index
// that is not 10 and not allied), with the case's damage, visibility and
// reachability answers (see the script for the line layout).
static int pickOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        int cap;int32_t cx,cz;uint32_t seed;
        in>>cap>>cx>>cz>>seed;
        int allied[16];for (int& a:allied) in>>a;
        ai::ClassicSquadUnit m;
        int mover,nav,floater,depth,mfx,mfz,mcx,mcz;
        in>>m.flags>>mover>>nav>>floater>>depth>>m.range>>m.x>>m.z>>mcx>>mcz>>mfx>>mfz;
        m.mover=mover!=0;m.navigator=m.mover && nav!=0;m.naval=m.mover && floater && int16_t(depth)>0;
        m.cellX=int16_t(mcx);m.cellZ=int16_t(mcz);m.footX=int16_t(mfx);m.footZ=int16_t(mfz);
        std::vector<ai::ClassicPickPlayer> players;
        std::map<int,int32_t> damage;std::map<int,int> visible;
        for (int r=0;r<10;++r) {
            int active,kind,idx,pop;size_t n;in>>active>>kind>>idx>>pop>>n;
            ai::ClassicPickPlayer player;player.population=uint16_t(pop);
            for (size_t k=0;k<n;++k) {
                ai::ClassicEnemy e;int ecx,ecz,cloak,mana,vis;uint32_t f108;int32_t d;
                in>>e.id>>e.flags>>e.x>>e.z>>ecx>>ecz>>cloak>>f108>>mana>>d>>vis;
                e.cellX=int16_t(ecx);e.cellZ=int16_t(ecz);e.cloaked=cloak!=0;e.manaSite=mana!=0;
                const float remaining=std::bit_cast<float>(f108);
                e.unfinished=remaining<0.0f || remaining>0.0f;   // fcomp: equal or unordered is finished
                damage[e.id]=d;visible[e.id]=vis;
                player.units.push_back(e);
            }
            if (active && kind>=1 && kind<=3 && idx!=10 && !allied[idx]) players.push_back(std::move(player));
        }
        std::vector<int> reach;size_t n;in>>n;reach.resize(n);for (int& r:reach) in>>r;
        std::vector<std::vector<int>> log;size_t nextReach=0;
        struct Host {
            std::vector<ai::ClassicPickPlayer>& players;
            int cap;uint32_t& seed;std::vector<std::vector<int>>& log;
            std::map<int,int32_t>& damages;std::map<int,int>& visibles;
            std::vector<int>& reach;size_t& next;
            const std::vector<ai::ClassicPickPlayer>& pickPlayers() const { return players; }
            uint16_t unitCap() const { return uint16_t(cap); }
            int random(int k) { log.push_back({0,k});return int(sim::retailRandom(seed,k)); }
            int32_t damage(const ai::ClassicSquadUnit&,const ai::ClassicEnemy& e) { log.push_back({1,e.id});return damages[e.id]; }
            bool visible(const ai::ClassicEnemy& e) { log.push_back({2,e.id});return visibles[e.id]!=0; }
            bool reachable(const ai::ClassicSquadUnit& u,const ai::ClassicEnemy& e,int16_t range,bool naval) {
                log.push_back({3,u.cellX,u.cellZ,u.footX,u.footZ,e.cellX,e.cellZ,range,range,1,int(naval)});
                const int ok=next<reach.size() ? reach[next] : 1;++next;return ok!=0;
            }
        } host{players,cap,seed,log,damage,visible,reach,nextReach};
        const int picked=ai::classicPickTarget(host,m,{cx,cz});
        std::printf("%d %u %zu",picked,seed,log.size());
        for (const auto& e:log) for (int v:e) std::printf(" %d",v);
        std::printf("\n");
    }
    return 0;
}

// The port's side of tools/re/check_ai_strike.py (--strike: 0x40e180, --backup:
// 0x40ea30, --vtol: 0x40eb40): the shared clock template sim::retailTickAiSquad
// with ai::classicStrikeRecruit and the army squads' thinks under it, on the
// case's squads, units, targets and answers (see the script for the layout).
static int strikeOracle(const char* routine) {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        uint32_t tick,seed;int w,h,side,strike,vtol;
        in>>tick>>w>>h>>side>>seed>>strike>>vtol;
        ai::ClassicSquads s;
        {
            uint32_t dummy=1;
            ai::classicConstructSquads(s,0,[&](int n){return sim::retailRandom(dummy,n);});
            for (auto& q:s.squads) { q.parameters={};q.members.clear();q.active=q.dirty=0; }
        }
        auto table=[&](std::vector<std::optional<ai::ClassicCell>>& t) {
            size_t k;in>>k;t.resize(k);
            for (auto& c:t) { int valid,x,z;in>>valid>>x>>z;if (valid) c=ai::ClassicCell{int16_t(x),int16_t(z)}; }
        };
        table(s.sites);table(s.starts);
        for (int b=1;b<=20;++b) in>>s.squads[size_t(b)].parameters[0];
        int nown,nen;in>>nown>>nen;
        std::vector<ai::ClassicSquadUnit> units;
        std::map<int,ai::ClassicEnemy> all;
        for (int k=0;k<nown+nen;++k) {
            ai::ClassicSquadUnit u;int own,cx,cz,fx,fz,hp,sight,menu,depth,dmg,reload,mover,nav,has,mkind,mclass;
            int64_t maxd;int32_t y;
            in>>u.id>>own>>u.squad>>u.flags>>u.x>>y>>u.z>>cx>>cz>>fx>>fz>>hp>>maxd>>sight>>u.typeFlags>>u.secondaryFlags
              >>menu>>depth>>u.range>>dmg>>reload>>mover>>nav>>has>>mkind>>u.missionFlags>>mclass>>u.goalX>>u.goalZ;
            u.cellX=int16_t(cx);u.cellZ=int16_t(cz);u.footX=int16_t(fx);u.footZ=int16_t(fz);
            u.hp=int16_t(hp);u.maxDamage=uint32_t(maxd);u.sight=int16_t(sight);u.menu=menu;u.minWaterDepth=int16_t(depth);
            u.weaponDamage=uint16_t(dmg);u.weaponReload=uint16_t(reload);u.mover=mover!=0;u.navigator=u.mover && nav!=0;
            u.mission=has!=0;u.missionKind=uint8_t(mkind);u.missionClass=uint8_t(mclass);
            u.commander=(u.secondaryFlags&0x40000u)!=0;
            ai::ClassicEnemy e;e.id=u.id;e.flags=u.flags;e.x=u.x;e.y=y;e.z=u.z;e.cellX=u.cellX;e.cellZ=u.cellZ;
            all[u.id]=e;
            if (own) units.push_back(u);
        }
        const int total=nown+nen;
        for (int slot=1;slot<=98;++slot) {
            size_t k;in>>k;auto& m=s.squads[size_t(slot)].members;m.resize(k);
            for (auto& id:m) in>>id;
        }
        auto& sp=s.squads[size_t(strike)].parameters;
        uint32_t active,dirty;
        in>>sp[0]>>sp[1]>>sp[2]>>sp[3]>>sp[4]>>sp[5]>>active>>dirty;
        s.squads[size_t(strike+1)].parameters[0]=strike;
        const int slot=std::strcmp(routine,"strike")==0 ? strike : std::strcmp(routine,"backup")==0 ? strike+1 : vtol;
        s.squads[size_t(slot)].active=active;s.squads[size_t(slot)].dirty=dirty;
        std::vector<ai::ClassicEnemy> visible;
        size_t n;in>>n;for (size_t k=0;k<n;++k) { int id;in>>id;visible.push_back(all[id]); }
        int pick;in>>pick;
        std::vector<int> kinds,reach;
        in>>n;kinds.resize(n);for (int& v:kinds) in>>v;
        in>>n;reach.resize(n);for (int& v:reach) in>>v;

        std::vector<std::vector<int>> log;
        size_t nextKind=0,nextReach=0;
        struct Host {
            std::vector<ai::ClassicSquadUnit>& units;std::map<int,ai::ClassicEnemy>& all;int total;
            std::vector<ai::ClassicEnemy>& visible;int pick;std::vector<int>& kinds;std::vector<int>& reach;
            size_t& nextKind;size_t& nextReach;std::vector<std::vector<int>>& log;uint32_t& seed;int w,h;
            int random(int k) { log.push_back({0,k});return int(sim::retailRandom(seed,k)); }
            int pick_(const ai::ClassicPoint& c,size_t i) { log.push_back({6,units[i].id,c[0],c[2]});return pick; }
            const ai::ClassicEnemy* target(int id) {
                if (id<=0 || id>total) return nullptr;
                const auto& e=all.at(id);
                return (e.flags&ai::ClassicEnemy::kLive) ? &e : nullptr;
            }
            bool threat(std::pair<int32_t,int32_t> c,int32_t r) { return ai::classicThreat(visible,c,r); }
            bool reachable(const ai::ClassicSquadUnit& m,const ai::ClassicEnemy& t,int16_t range,bool naval) {
                log.push_back({5,m.cellX,m.cellZ,m.footX,m.footZ,t.cellX,t.cellZ,range,range,1,int(naval)});
                const int ok=nextReach<reach.size() ? reach[nextReach] : 1;++nextReach;return ok!=0;
            }
            uint8_t resolve(int command,size_t i,const ai::ClassicPoint& at) {
                log.push_back({2,command,units[i].id,0,at[0],at[2]});
                return uint8_t(nextKind<kinds.size() ? kinds[nextKind++] : 0);
            }
            void order(uint8_t kind,bool queue,size_t i,const ai::ClassicPoint& at,int32_t a6,int32_t a7) {
                log.push_back({3,int(kind),int(queue),units[i].id,0,at[0],at[2],a6,a7});
            }
            void clear(size_t i) { log.push_back({4,units[i].id,0}); }
            int width() const { return w; }
            int height() const { return h; }
        } host{units,all,total,visible,pick,kinds,reach,nextKind,nextReach,log,seed,w,h};
        struct Calls {
            Host& h;
            int random(int k) { return h.random(k); }
            int pick(const ai::ClassicPoint& c,size_t i) { return h.pick_(c,i); }
            const ai::ClassicEnemy* target(int id) { return h.target(id); }
            bool threat(std::pair<int32_t,int32_t> c,int32_t r) { return h.threat(c,r); }
            bool reachable(const ai::ClassicSquadUnit& m,const ai::ClassicEnemy& t,int16_t r,bool f) { return h.reachable(m,t,r,f); }
            uint8_t resolve(int c,size_t i,const ai::ClassicPoint& at) { return h.resolve(c,i,at); }
            void order(uint8_t k,bool q,size_t i,const ai::ClassicPoint& at,int32_t a6,int32_t a7) { h.order(k,q,i,at,a6,a7); }
            void clear(size_t i) { h.clear(i); }
            int width() const { return h.width(); }
            int height() const { return h.height(); }
        } calls{host};
        sim::RetailAiGroupClock clock{true,0};
        auto& squad=s.squads[size_t(slot)];
        sim::retailTickAiSquad(squad,clock,tick,[&](int k){return host.random(k);},
            [&](auto&) { ai::classicStrikeRecruit(s,units,slot); },
            [&](auto&) {
                if (squad.kind==sim::RetailAiSquadKind::Strike) ai::classicStrikeThink(s,units,slot,clock,tick,calls);
                else if (squad.kind==sim::RetailAiSquadKind::Backup) ai::classicBackupThink(s,units,slot,calls);
                else if (squad.kind==sim::RetailAiSquadKind::Vtol) ai::classicVtolThink(s,units,slot,calls);
            });
        std::printf("%u %u %zu",seed,clock.deadline,log.size());
        for (const auto& e:log) for (int v:e) std::printf(" %d",v);
        for (const auto& u:units) std::printf(" %d",u.squad);
        for (int k=1;k<=98;++k) {
            std::printf(" %zu",s.squads[size_t(k)].members.size());
            for (int id:s.squads[size_t(k)].members) std::printf(" %d",id);
        }
        for (int k:{strike,strike+1,vtol}) {
            for (int32_t v:s.squads[size_t(k)].parameters) std::printf(" %d",v);
            std::printf(" %u %u",s.squads[size_t(k)].active,s.squads[size_t(k)].dirty);
        }
        for (int b=1;b<=20;++b) std::printf(" %d",s.squads[size_t(b)].parameters[0]);
        std::printf("\n");
    }
    return 0;
}

// The port's side of tools/re/check_ai_launch.py: one base think through the
// shared clock template sim::retailTickAiSquad, ai::classicBaseMembers (its
// planners logged) and ai::classicBaseLaunch, with the case's squads, units,
// population, records (the ground roll's E by retail's player filter) and P7
// answers (see the script for the line layout).
static int launchOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        uint32_t tick,seed;int own,slot;int32_t population;
        in>>tick>>own>>seed>>slot>>population;
        int allied[12];for (int& a:allied) in>>a;
        int32_t enemy=0;
        for (int r=0;r<10;++r) {
            int active,kind,idx,pop;in>>active>>kind>>idx>>pop;
            if (active && kind>=1 && kind<=3 && idx!=10 && !allied[idx]) enemy=std::max(enemy,int32_t(pop));
        }
        ai::ClassicSquads s;
        {
            uint32_t dummy=1;
            ai::classicConstructSquads(s,0,[&](int n){return sim::retailRandom(dummy,n);});
            for (auto& q:s.squads) { q.parameters={};q.members.clear();q.active=q.dirty=0; }
        }
        auto table=[&](std::vector<std::optional<ai::ClassicCell>>& t) {
            size_t k;in>>k;t.resize(k);
            for (auto& c:t) { int valid,x,z;in>>valid>>x>>z;if (valid) c=ai::ClassicCell{int16_t(x),int16_t(z)}; }
        };
        table(s.sites);table(s.starts);
        for (auto& v:s.squads[size_t(slot)].parameters) in>>v;
        size_t n;in>>n;
        std::vector<ai::ClassicSquadUnit> units(n);
        for (auto& u:units) {
            int fx,fz,hp,menu,depth,mover,held,carrier,has,mclass;int64_t maxd;uint32_t f108;
            in>>u.id>>u.flags>>u.x>>u.z>>fx>>fz>>hp>>maxd>>u.typeFlags>>u.secondaryFlags>>menu>>depth>>mover
              >>f108>>held>>carrier>>has>>u.missionFlags>>mclass;
            u.footX=int16_t(fx);u.footZ=int16_t(fz);u.hp=int16_t(hp);u.maxDamage=uint32_t(maxd);u.menu=menu;
            u.minWaterDepth=int16_t(depth);u.mover=mover!=0;
            const float remaining=std::bit_cast<float>(f108);
            u.finished=!(remaining<0.0f || remaining>0.0f);
            u.held=held!=0;u.carried=carrier!=0;u.carrierHolds=carrier==1;
            u.mission=has!=0;u.missionClass=uint8_t(mclass);u.commander=(u.secondaryFlags&0x40000u)!=0;
            u.squad=(u.flags&ai::ClassicSquadUnit::kLive) ? slot : 0;
        }
        for (int k=1;k<=98;++k) {
            size_t c;in>>c;auto& m=s.squads[size_t(k)].members;m.resize(c);
            for (auto& id:m) in>>id;
        }
        int bases[3],expand;in>>bases[0]>>bases[1]>>bases[2]>>expand;
        std::vector<std::vector<int>> log;
        struct Host {
            std::vector<ai::ClassicSquadUnit>& units;std::vector<std::vector<int>>& log;uint32_t& seed;
            int* bases;int expansion;
            int random(int k) { log.push_back({0,k});return int(sim::retailRandom(seed,k)); }
            void clear(size_t i) { log.push_back({4,units[i].id,0}); }
            int builderBase() { log.push_back({8,0});return bases[0]; }
            int groundBase() { log.push_back({8,1});return bases[1]; }
            int airBase() { log.push_back({8,2});return bases[2]; }
            void transfer(int b,int a,int c,int d) { log.push_back({9,b,a,c,d}); }
            bool expand(int at,int32_t escort) { log.push_back({10,at,escort});return expansion!=0; }
        } host{units,log,seed,bases,expand};
        sim::RetailAiGroupClock clock{true,0};
        auto& squad=s.squads[size_t(slot)];
        sim::retailTickAiSquad(squad,clock,tick,[&](int k){return host.random(k);},[](auto&){},
            [&](auto&) {
                const bool planned=ai::classicBaseMembers(s,units,slot,
                    [](const std::pair<int32_t,int32_t>&,int32_t){return false;},
                    [&](size_t i){log.push_back({7,0,units[i].id});},
                    [&](size_t i,bool){log.push_back({7,1,units[i].id});},
                    [&](size_t i,bool){log.push_back({7,2,units[i].id});});
                if (planned) ai::classicBaseLaunch(s,units,slot,population,enemy,host);
            });
        std::printf("%u %u %zu",seed,clock.deadline,log.size());
        for (const auto& e:log) for (int v:e) std::printf(" %d",v);
        for (const auto& u:units) std::printf(" %d",u.squad);
        for (int k=1;k<=98;++k) {
            const auto& q=s.squads[size_t(k)];
            std::printf(" %zu",q.members.size());
            for (int id:q.members) std::printf(" %d",id);
            for (int32_t v:q.parameters) std::printf(" %d",v);
            std::printf(" %u %u",q.active,q.dirty);
        }
        std::printf("\n");
    }
    return 0;
}

// The port's side of tools/re/check_ai_static_grade.py: "<width> <height> <footX>
// <footZ> <rating>*" per line (each cell's own rating, row-major) -> the static
// bit of every cell (ai::classicStaticPlane), row-major.
static int staticGradeOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        int w,h,fx,fz;in>>w>>h>>fx>>fz;
        std::vector<int> rating(size_t(w)*size_t(h));
        for (auto& r:rating) in>>r;
        const auto plane=ai::classicStaticPlane(w,h,fx,fz,[&](int x,int z){return rating[size_t(z)*w+x]==0;});
        for (size_t i=0;i<plane.size();++i) std::printf(i ? " %d" : "%d",int(plane[i]));
        std::printf("\n");
    }
    return 0;
}

// The port's side of tools/re/check_ai_squad_ctor.py: "<seed> <E>" per line ->
// the manager's squads as 0x40fed0 leaves them, then the final seed.
static int squadCtorOracle() {
    uint32_t seed;int32_t e;
    while (std::cin>>seed>>e) {
        ai::ClassicSquads s;
        ai::classicConstructSquads(s,e,[&](int n){return sim::retailRandom(seed,n);});
        std::printf("%d",s.schedule.assignmentCountdown);
        for (int i=0;i<100;++i) {
            const auto& q=s.squads[size_t(i)];
            if (!s.schedule.groups[size_t(i)].present) { std::printf(" -1");continue; }
            std::printf(" %d %u",int(q.kind),s.schedule.groups[size_t(i)].deadline);
            for (int k=0;k<9;++k) std::printf(" %d",q.parameters[size_t(k)]);
            std::printf(" %u",q.active);
        }
        std::printf(" %u\n",seed);
    }
    return 0;
}

// The port's side of the PrintWeights check: the brain's own facts, profile and
// priority pass (ClassicBrain::recomputePriorities' steps) over retail's table.
static int printWeightsOracle(const char* root) {
    auto vfs=hpi::mountRetailRoot(root);
    sim::TypeRegistry reg;sim::setupRegistry(reg,vfs,false);
    ai::ClassicBrain brain(1,7,reg);
    brain.useProfile(ai::classicProfileScript(vfs,ai::classicMapAiProfile(vfs,hpi::findMap(vfs,"Abnar's Terrace"))));
    const auto& c=brain.catalogue();const auto& p=brain.profile();const auto& info=brain.typeInfo();
    auto find=[&](const std::string& n){auto it=c.byName.find(n);return it==c.byName.end() ? size_t(0) : size_t(it->second);};
    struct Row { size_t t; int count,completed; };
    std::vector<Row> table;
    unsigned population=0;float ratio=0;bool shortfall=false,creation=false;
    std::string line,word;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);in>>word;
        if (word=="facts") {
            std::string name;in>>name;const size_t t=find(name);
            if (!t) { std::printf("0 0 0 0 0 0 0\n");continue; }
            const auto f=info[t].priorityInputs(0,0,p.limit[t]);
            std::printf("1 %u %u %d %u %u %d\n",f.flags,f.secondaryFlags,int(f.weapon),
                        std::bit_cast<uint32_t>(f.cost),unsigned(p.pref[t]),p.limit[t]);
        } else if (word=="state") {
            float stored,capacity;size_t n;in>>population>>stored>>capacity>>n;
            table.clear();
            for (size_t i=0;i<n;++i) { std::string name;Row r{};in>>name>>r.count>>r.completed;r.t=find(name);table.push_back(r); }
            sim::RetailResourceHistory h;
            for (auto& s : h.samples) in>>s[0]>>s[1];
            ratio=ai::ClassicBrain::storeFill(stored,capacity);
            shortfall=h.shortfall(stored);
        } else if (word=="creation") {
            int on=0;in>>on;creation=on!=0;
        } else if (word=="seed") {
            uint32_t seed;in>>seed;unsigned draws=0;
            for (const auto& r : table) {
                // A name the port has no type for (retail's own table entry) prints -1.
                if (!r.t) { std::printf("-1 ");continue; }
                const auto in2=info[r.t].priorityInputs(r.count,r.completed,creation ? -1 : p.limit[r.t]);
                const unsigned v=sim::retailBuildPriority(in2,uint16_t(population),ratio,shortfall,
                    [&](int n){if (n>=2) ++draws;return sim::retailRandom(seed,n);});
                std::printf("%u ",v);
            }
            std::printf("%u %u\n",seed,draws);
        }
    }
    return 0;
}

// The manager tools/re/classic_base_env.py builds, as the port reads it: the
// anchor tables, bases 1..20's p0..p8, the units (with their navigator grid
// and squad), every squad's members, three grids' zone labels and the
// visible-enemy list.
struct BaseEnv {
    ai::ClassicSquads s;
    std::vector<ai::ClassicSquadUnit> units;
    std::vector<int> nav;   // per unit: 0 no ground grid, else 1..3
    std::array<ai::ClassicZoneLabels,3> labels;
    std::vector<ai::ClassicEnemy> enemies;
};
static BaseEnv readBaseEnv(std::istringstream& in) {
    BaseEnv e;
    {
        uint32_t dummy=1;
        ai::classicConstructSquads(e.s,0,[&](int n){return sim::retailRandom(dummy,n);});
        for (auto& q:e.s.squads) { q.parameters={};q.members.clear();q.active=q.dirty=0; }
    }
    auto table=[&](std::vector<std::optional<ai::ClassicCell>>& t) {
        size_t k;in>>k;t.resize(k);
        for (auto& c:t) { int valid,x,z;in>>valid>>x>>z;if (valid) c=ai::ClassicCell{int16_t(x),int16_t(z)}; }
    };
    table(e.s.sites);table(e.s.starts);
    for (int b=1;b<=20;++b) for (auto& v:e.s.squads[size_t(b)].parameters) in>>v;
    size_t n;in>>n;
    e.units.resize(n);e.nav.resize(n);
    for (size_t k=0;k<n;++k) {
        auto& u=e.units[k];
        int fx,fz,hp,menu,depth,mover,held,carrier,has,mclass;int64_t maxd;uint32_t f108;
        in>>u.id>>u.flags>>u.x>>u.z>>fx>>fz>>hp>>maxd>>u.typeFlags>>u.secondaryFlags>>menu>>depth>>mover
          >>f108>>held>>carrier>>has>>u.missionFlags>>mclass>>e.nav[k]>>u.squad;
        u.footX=int16_t(fx);u.footZ=int16_t(fz);u.hp=int16_t(hp);u.maxDamage=uint32_t(maxd);u.menu=menu;
        u.minWaterDepth=int16_t(depth);u.mover=mover!=0;u.navigator=e.nav[k]!=0;
        const float remaining=std::bit_cast<float>(f108);
        u.finished=!(remaining<0.0f || remaining>0.0f);
        u.held=held!=0;u.carried=carrier!=0;u.carrierHolds=carrier==1;
        u.mission=has!=0;u.missionClass=uint8_t(mclass);u.commander=(u.secondaryFlags&0x40000u)!=0;
    }
    for (int k=1;k<=98;++k) {
        size_t c;in>>c;auto& m=e.s.squads[size_t(k)].members;m.resize(c);
        for (auto& id:m) in>>id;
    }
    for (auto& z:e.labels)
        for (auto* v:{&z.starts,&z.sites}) { size_t c;in>>c;v->resize(c);for (auto& x:*v) in>>x; }
    in>>n;e.enemies.resize(n);
    for (auto& en:e.enemies) in>>en.flags>>en.x>>en.z;
    return e;
}
static void printBaseEnv(const BaseEnv& e) {
    for (const auto& u:e.units) std::printf(" %d",u.squad);
    for (int k=1;k<=98;++k) {
        const auto& q=e.s.squads[size_t(k)];
        std::printf(" %zu",q.members.size());
        for (int id:q.members) std::printf(" %d",id);
        for (int32_t v:q.parameters) std::printf(" %d",v);
    }
    std::printf("\n");
}
struct BaseEnvHost {
    BaseEnv& e;uint32_t& seed;std::vector<std::vector<int>>& log;
    int random(int k) { log.push_back({0,k});return int(sim::retailRandom(seed,k)); }
    void clear(size_t i) { log.push_back({4,e.units[i].id,0}); }
    const ai::ClassicZoneLabels* labels(size_t i) const { return e.nav[i] ? &e.labels[size_t(e.nav[i]-1)] : nullptr; }
    size_t enemies(std::pair<int32_t,int32_t> at,int32_t r) const { return ai::classicEnemiesNear(e.enemies,at,r).size(); }
};

// The port's side of tools/re/check_ai_transfer.py: "<seed> <slot> <to> <n>
// <category> <clear> <manager>" -> the three transfer targets
// (ai::classicShortBase), then ai::classicTransfer's draws, clears and the
// manager after it.
static int transferOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        uint32_t seed;int slot,to,n,category,clear;
        in>>seed>>slot>>to>>n>>category>>clear;
        BaseEnv e=readBaseEnv(in);
        const int t[3]={ai::classicShortBase(e.s,e.units,slot,2,1),ai::classicShortBase(e.s,e.units,slot,4,2),
                        ai::classicShortBase(e.s,e.units,slot,5,3)};
        std::vector<std::vector<int>> log;
        BaseEnvHost h{e,seed,log};
        ai::classicTransfer(e.s,e.units,slot,to,n,unsigned(category),clear!=0,h);
        std::printf("%d %d %d %u %zu",t[0],t[1],t[2],seed,log.size());
        for (const auto& v:log) for (int x:v) std::printf(" %d",x);
        printBaseEnv(e);
    }
    return 0;
}

// The port's side of tools/re/check_ai_expand.py: "<seed> <slot> <newSlot>
// <escort> <manager>" -> ai::classicExpand's answer, draws and the manager after it.
static int expandOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        uint32_t seed;int slot,to,escort;
        in>>seed>>slot>>to>>escort;
        BaseEnv e=readBaseEnv(in);
        std::vector<std::vector<int>> log;
        BaseEnvHost h{e,seed,log};
        const bool founded=ai::classicExpand(e.s,e.units,slot,to,escort,h);
        std::printf("%d %u %zu",int(founded),seed,log.size());
        for (const auto& v:log) for (int x:v) std::printf(" %d",x);
        printBaseEnv(e);
    }
    return 0;
}

// The port's side of tools/re/check_ai_zone_labels.py: one navigator grid and
// map per line -- "<w> <h> <grade>*w*h <nstarts> (<valid> <x> <z>)* <nsites>
// (<valid> <x> <z>)* <ntypes> (<sacredsite bits> <footX> <footZ>)* (<feature>
// <backX> <backZ>)*w*h" -> the zone count, the start and site labels
// (ai::classicZoneLabels over sim::retailRectangleReachable, static grade), and
// every reachability query in order (source and target rectangles).
static int zoneLabelsOracle() {
    std::string line;
    while (std::getline(std::cin,line)) {
        std::istringstream in(line);
        int w,h;in>>w>>h;
        std::vector<int> grade(size_t(w)*size_t(h));
        for (int& g:grade) in>>g;
        auto table=[&](std::vector<std::optional<ai::ClassicCell>>& t) {
            size_t k;in>>k;t.resize(k);
            for (auto& c:t) { int valid,x,z;in>>valid>>x>>z;if (valid) c=ai::ClassicCell{int16_t(x),int16_t(z)}; }
        };
        std::vector<std::optional<ai::ClassicCell>> starts,sites;
        table(starts);table(sites);
        size_t nt;in>>nt;
        std::vector<bool> sacred(nt);std::vector<std::pair<int16_t,int16_t>> foot(nt);
        for (size_t k=0;k<nt;++k) {
            uint32_t bits;int fx,fz;in>>bits>>fx>>fz;
            sacred[k]=ai::classicSacredValue(std::bit_cast<float>(bits));
            foot[k]={int16_t(fx),int16_t(fz)};
        }
        std::vector<ai::ClassicFeatureCell> plane(size_t(w)*size_t(h));
        for (auto& c:plane) { int f,bx,bz;in>>f>>bx>>bz;c={uint16_t(f),uint8_t(bx),uint8_t(bz)}; }
        std::vector<std::array<int,8>> log;
        const auto z=ai::classicZoneLabels(starts,sites,
            [&](const ai::ClassicCell& c) -> std::pair<int16_t,int16_t> {
                const auto f=ai::classicSacredFeature(c.first,c.second,uint32_t(nt),
                    [&](int x,int zz) {
                        if (x<0 || zz<0 || x>=w || zz>=h) return ai::ClassicFeatureCell{};
                        return plane[size_t(zz)*size_t(w)+size_t(x)];
                    },
                    [&](uint16_t t) { return t<nt && sacred[t]; });
                if (!f || *f>=nt) return {1,1};
                return foot[*f];
            },
            [&](int sx,int sz,int sw,int sh,int tx,int tz,int tw,int th) {
                log.push_back({sx,sz,sw,sh,tx,tz,tw,th});
                return sim::retailRectangleReachable(w,h,sx,sz,sw,sh,tx,tz,tw,th,true,false,
                    [&](int x,int zz) { return grade[size_t(zz)*size_t(w)+size_t(x)]; });
            });
        int32_t zones=0;
        for (int32_t v:z.starts) zones=std::max(zones,v);
        for (int32_t v:z.sites) zones=std::max(zones,v);
        std::printf("%d",zones);
        for (int32_t v:z.starts) std::printf(" %d",v);
        for (int32_t v:z.sites) std::printf(" %d",v);
        std::printf(" %zu",log.size());
        for (const auto& e:log) for (int v:e) std::printf(" %d",v);
        std::printf("\n");
    }
    return 0;
}

int main(int argc,char** argv) {
    if (argc==2 && std::strcmp(argv[1],"--profile")==0) return profileOracle();
    if (argc==3 && std::strcmp(argv[1],"--printweights")==0) return printWeightsOracle(argv[2]);
    if (argc==2 && std::strcmp(argv[1],"--assignment")==0) return assignmentOracle();
    if (argc==2 && std::strcmp(argv[1],"--squad-ctor")==0) return squadCtorOracle();
    if (argc==2 && std::strcmp(argv[1],"--factory-think")==0) return factoryThinkOracle();
    if (argc==2 && std::strcmp(argv[1],"--placement")==0) return placementOracle();
    if (argc==2 && std::strcmp(argv[1],"--constructor")==0) return constructorOracle();
    if (argc==2 && std::strcmp(argv[1],"--static-grade")==0) return staticGradeOracle();
    if (argc==2 && std::strcmp(argv[1],"--threat")==0) return threatOracle();
    if (argc==2 && std::strcmp(argv[1],"--defence")==0) return defenceOracle(false);
    if (argc==2 && std::strcmp(argv[1],"--retreat")==0) return defenceOracle(true);
    if (argc==2 && std::strcmp(argv[1],"--pick")==0) return pickOracle();
    if (argc==2 && std::strcmp(argv[1],"--launch")==0) return launchOracle();
    if (argc==2 && std::strcmp(argv[1],"--zone-labels")==0) return zoneLabelsOracle();
    if (argc==2 && std::strcmp(argv[1],"--transfer")==0) return transferOracle();
    if (argc==2 && std::strcmp(argv[1],"--expand")==0) return expandOracle();
    if (argc==2 && (std::strcmp(argv[1],"--strike")==0 || std::strcmp(argv[1],"--backup")==0 ||
                    std::strcmp(argv[1],"--vtol")==0)) return strikeOracle(argv[1]+2);
    // The Controller-driven Classic games (--bases, --raid, --waves, --duels,
    // --expansion) are gone: ai::Controller no longer builds a ClassicBrain (every
    // level runs TA's own AI, ai/taretail.h), and they played Kingdoms' factions,
    // Monarchs, sacred sites and lodestones.
    for (const char* gone:{"--bases","--raid","--waves","--duels","--expansion"})
        if (argc==3 && std::strcmp(argv[1],gone)==0) {
            std::printf("%s: removed (Kingdoms' Classic seat; TA runs ai/taretail)\n",gone);
            return 1;
        }
    if (argc==3 && std::strcmp(argv[1],"--u13")==0) {
        classicMoveVsAttackMove(argv[2]);
        std::printf("%s\n",fails ? "FAILED" : "all passed");
        return fails ? 1 : 0;
    }
    profileRules();
    typeFacts();
    typeInfoAndPriorities();
    squadConstruction();
    squadAssignment();
    strikeWideTargetIds();
    // The Controller-driven Classic games on Abnar's Terrace (a seat's squads, its
    // factories) went with the modes above: a Classic seat has no ClassicBrain now.
    if (argc>1) shippedData(argv[1]);
    std::printf("%s\n",fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
