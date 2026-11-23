#include "sim/retailmap.h"
#include "sim/retailreclaimarea.h"
#include "sim/sim.h"
#include "net/protocol.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#include <string>
using namespace tak;
int main(int argc,char** argv) {
    if (argc>1 && std::string(argv[1])=="--selector") {
        sim::RetailReclaimArea a;int32_t x,z;uint32_t mask;
        while(std::cin>>a.minX>>a.minZ>>a.maxX>>a.maxZ>>x>>z>>mask) {
            int index=0;
            auto result=sim::retailReclaimAreaTarget(a,x,z,[&](int32_t,int32_t){return (mask>>(index++))&1;});
            if(result)std::cout<<1<<' '<<result->first<<' '<<result->second<<'\n';
            else std::cout<<"0 0 0\n";
        }
        return 0;
    }
    // tools/re/check_reclaim_reach.py: RECLAIM's rectangle (4d4e80) as our
    // RetailRectGoal answers it, and its countdown and work wakes (406529).
    if (argc>1 && std::string(argv[1])=="--reclaim-rect") {
        int ax,az,fw,fh,bw,bh,ox,oz;
        while(std::cin>>ax>>az>>fw>>fh>>bw>>bh>>ox>>oz) {
            const auto rect=sim::retailReclaimRectangle(ax,az,fw,fh,bw,bh);
            const auto [cx,cz]=rect.navigationCell(ox,oz);
            std::cout<<int(rect.accepts(ox,oz))<<' '<<cx<<' '<<cz<<'\n';
        }
        return 0;
    }
    if (argc>1 && std::string(argv[1])=="--reclaim-countdown") {
        uint32_t bits;
        while(std::cin>>bits) {
            float energy;std::memcpy(&energy,&bits,4);
            int32_t countdown=sim::retailReclaimCountdown(energy);
            const int32_t first=countdown;
            sim::RetailMissionState m;m.stage=3;
            int wakes=0;
            do {sim::retailReclaimWork(m,countdown,uint32_t(2*wakes));++wakes;} while(m.stage==3 && wakes<100000);
            sim::retailReclaimWork(m,countdown,uint32_t(2*wakes));
            if(!sim::retailReclaimRemoval(m))wakes=-1;
            std::cout<<first<<' '<<wakes<<'\n';
        }
        return 0;
    }
    int failed=0;
    // Reclaimed: removed, or (494ce0) fading out of the plane after its order ended.
    auto gone=[](const sim::World& w,int id){const auto* f=w.feature(id);return !f || !f->alive || f->fadeLeft>0;};
    auto check=[&](bool ok,const char* text){std::printf("%s %s\n",ok?"PASS":"FAIL",text);failed+=!ok;};
    net::Command command;command.kind=net::Cmd::ReclaimArea;command.x=17.5f;command.z=32;
    command.x2=320;command.z2=144.25f;command.queue=1;
    net::Writer wire;wire.cmd(command);net::Reader reader(wire.b.data(),wire.b.size());
    const auto decoded=reader.cmd();
    check(reader.ok && decoded.kind==command.kind && decoded.x==command.x && decoded.z==command.z &&
          decoded.x2==command.x2 && decoded.z2==command.z2 && decoded.queue==1,"area command wire roundtrip");
    net::Reader truncated(wire.b.data(),wire.b.size()-1);
    (void)truncated.cmd();
    check(!truncated.ok,"truncated area endpoint is rejected");
    net::Command ordinary;ordinary.kind=net::Cmd::Move;
    net::Writer ordinaryWire;ordinaryWire.cmd(ordinary);
    check(ordinaryWire.b.size()==35,"ordinary command wire layout is unchanged");
    net::Writer mixed;mixed.cmd(ordinary);mixed.cmd(command);mixed.cmd(ordinary);
    net::Reader mixedReader(mixed.b.data(),mixed.b.size());
    const auto first=mixedReader.cmd(),middle=mixedReader.cmd(),last=mixedReader.cmd();
    check(mixedReader.ok && first.kind==net::Cmd::Move && middle.x2==command.x2 &&
          middle.z2==command.z2 && last.kind==net::Cmd::Move,
          "mixed area and ordinary replay commands retain framing");
    // RECLAIMAREA spends a few ticks per pick (406670: stage 0 sleeps a tick,
    // stage 1 approaches even from inside the box, stage 2 picks on arrival).
    // Tick until the builder's job moves off `previous` or the queue empties.
    auto untilPick=[](sim::World& w,int id,int previous=0,int limit=90) {
        for(int t=0;t<limit;++t) {
            const auto* u=w.unit(id);
            if(u->orders.empty() || (u->reclaimId && u->reclaimId!=previous))return;
            w.tick(1.0f/30);
        }
    };
    auto untilIdle=[](sim::World& w,int id,int limit=600) {
        for(int t=0;t<limit && !w.unit(id)->orders.empty();++t) w.tick(1.0f/30);
    };
    sim::World world;
    std::vector<uint8_t> heights(32*32,100);
    world.setTerrain(heights,32,32,20);
    world.setMapPlacementFeatures(std::vector<uint16_t>(32*32,0xffff),{});
    sim::UnitType builder;builder.id="area-builder";builder.maxHp=100;
    builder.isBuilder=builder.canMove=builder.canReclaim=true;
    builder.maxVel=sim::Fixed::fromInt(1);builder.buildDist=500;builder.sight=500;
    int id=world.spawn(&builder,152,160,0,0);
    world.addFeature(11,184,168,1,1,1,false);
    world.addFeature(12,136,168,1,1,1,false);
    world.addFeature(13,200,168,1,1,1,false);
    world.reclaimArea(id,128,160,208,176,false);
    check(world.unit(id)->orders.size()==1 && world.unit(id)->orders.front().reclaimArea.has_value(),
          "area remains one persistent command rather than a precomputed queue");
    untilPick(world,id);
    check(world.unit(id)->reclaimId==12 && world.feature(11)->alive,
          "native sampled-cell distance selects the left feature first");
    // Simulate a new builder position while its next child is pending: selection
    // must use this position, not the original click's ordering.
    world.unit(id)->x=sim::Fixed::fromInt(208);
    world.updateNavigationExploration();
    untilPick(world,id,12);
    check(gone(world,12) && world.unit(id)->reclaimId==13 && world.feature(11)->alive,
          "next target is selected from current builder position");
    untilIdle(world,id);
    check(gone(world,11) && world.unit(id)->orders.empty(),"area completes after targets are exhausted");
    world.addFeature(14,168,168,1,1,1,false);
    world.order(id,160,160,false);
    world.reclaimArea(id,128,160,208,176,true);
    check(world.unit(id)->orders.size()>=2 && world.unit(id)->orders.back().reclaimArea.has_value(),
          "shift area reclaim queues behind an existing move");
    world.stop(id);
    check(world.unit(id)->orders.empty(),"stop clears persistent area work");
    world.reclaimArea(id,128,160,208,176,false);
    world.cancelBuilds(id);world.order(id,320,160,false);
    check(world.unit(id)->orders.back().groundMission && !world.unit(id)->orders.back().reclaimArea,
          "replacement move cancels the persistent area");

    sim::World hidden;hidden.setTerrain(heights,32,32,20);
    builder.sight=1;
    const int hb=hidden.spawn(&builder,152,160,0,0);
    hidden.addFeature(20,408,408,1,1,1,false);
    hidden.reclaimArea(hb,128,128,416,416,false);untilPick(hidden,hb);
    check(hidden.unit(hb)->orders.empty() && hidden.feature(20)->alive,
          "headless authoritative area selection excludes unseen features");
    sim::UnitType ally=builder;ally.id="area-observer";
    hidden.setTeam(1,0);
    hidden.spawn(&ally,400,400,1,0);
    hidden.reclaimArea(hb,128,128,416,416,false);untilPick(hidden,hb);
    check(hidden.unit(hb)->reclaimId==20,"allied sight admits visible area features");
    const int other=hidden.spawn(&builder,400,400,0,0);
    hidden.reclaim(other,20,false);
    // The other builder's reclaim ends first; hb's child only finds the
    // target gone when its approach arrives (406250 has no wake before).
    for(int t=0;t<900 && !hidden.unit(hb)->orders.empty();++t)hidden.tick(1.0f/30);
    check(gone(hidden,20) && hidden.unit(hb)->orders.empty(),
          "a target removed while a child is active lets the area complete");
    sim::World footprint;footprint.setTerrain(heights,32,32,20);
    footprint.setMapPlacementFeatures(std::vector<uint16_t>(32*32,0xffff),{});
    sim::UnitType sighted=builder;sighted.id="area-sighted";sighted.sight=500;
    const int fb=footprint.spawn(&sighted,120,184,0,0);
    footprint.addFeature(30,176,176,1,2,2,false);
    footprint.reclaimArea(fb,184,184,184,184,false);untilPick(footprint,fb,0,600);
    check(footprint.unit(fb)->reclaimId==30,
          "area touching only a footprint tail selects the anchored feature");
    {
        // 4066e3: the first pick is compared with the builder's own cell
        // (stage 0's last target), so a builder standing in the only target
        // cell retires the order at once (a retail quirk).
        sim::World own;own.setTerrain(heights,32,32,20);
        own.setMapPlacementFeatures(std::vector<uint16_t>(32*32,0xffff),{});
        const int ob=own.spawn(&sighted,184,184,0,0);
        own.addFeature(30,176,176,1,2,2,false);
        own.reclaimArea(ob,184,184,184,184,false);untilPick(own,ob);
        check(own.unit(ob)->orders.empty() && own.feature(30)->alive,
              "a first pick in the builder's own cell retires the area");
    }

    // Whole runs: the retail selector re-chooses the nearest target from the
    // builder's current position after every reclaim (greedy nearest first).
    sim::UnitType walker=builder;walker.id="area-walker";walker.sight=600;walker.buildDist=24;
    walker.maxVel=sim::Fixed::fromInt(3);walker.footX=walker.footZ=2;walker.upright=true;
    walker.turnRate=walker.turnInPlaceRate=10000;
    const std::vector<uint8_t> wide(64*64,100);
    auto field=[&](sim::World& w) {
        w.setTerrain(wide,64,64,20);
        w.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
        w.addFeature(1,200,200,1,1,1,false);w.addFeature(2,600,200,1,1,1,false);
        w.addFeature(3,200,700,1,1,1,false);w.addFeature(4,900,700,1,1,1,false);
    };
    auto run=[&](sim::World& w,std::vector<int> ids) {
        std::vector<std::vector<int>> got(ids.size());
        for(int tick=0;tick<12000;++tick) {
            bool busy=false;
            for(size_t i=0;i<ids.size();++i) {
                const auto* u=w.unit(ids[i]);busy|=!u->orders.empty();
                if(u->reclaimId && (got[i].empty() || got[i].back()!=u->reclaimId))got[i].push_back(u->reclaimId);
            }
            if(!busy)break;
            w.tick(1.0f/30);
        }
        return got;
    };
    {
        sim::World w;field(w);const int b=w.spawn(&walker,100,200,0,0);
        w.reclaimArea(b,128,128,960,960,false);
        check(run(w,{b})[0]==std::vector<int>({1,2,4,3}),"area clear runs nearest first from the north-west");
    }
    {
        sim::World w;field(w);const int b=w.spawn(&walker,950,950,0,0);
        w.reclaimArea(b,128,128,960,960,false);
        check(run(w,{b})[0]==std::vector<int>({4,2,1,3}),"area clear runs nearest first from the south-east");
    }
    {
        sim::World w;field(w);const int b=w.spawn(&walker,100,200,0,0);
        w.order(b,800,850,false);w.reclaimArea(b,128,128,960,960,true);
        const auto got=run(w,{b})[0];
        check(got==std::vector<int>({4,2,1,3}),"Shift-queued area clear starts from the previous move's end");
    }
    {
        // Equal sampled distance keeps the first sample in Z-then-X scan order.
        sim::World w;w.setTerrain(wide,64,64,20);
        w.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
        w.addFeature(22,200,200,1,1,1,false);w.addFeature(21,600,200,1,1,1,false);
        const int b=w.spawn(&walker,392,192,0,0);
        w.reclaimArea(b,128,128,960,960,false);untilPick(w,b);
        check(w.unit(b)->reclaimId==22,"equidistant area targets: the first sampled (west) one wins");
    }
    {
        // Retail has no claims: two builders on one area may share a target;
        // the one left without it re-selects, and the area still completes.
        // With the navigator on: the second builder must path round the first
        // to another cell of the target's perimeter.
        sim::World w;w.setPathService(true);field(w);
        const int a=w.spawn(&walker,100,200,0,0),b=w.spawn(&walker,100,264,0,0);
        w.reclaimArea(a,128,128,960,960,false);w.reclaimArea(b,128,128,960,960,false);
        run(w,{a,b});
        bool cleared=true;for(int id:{1,2,3,4})cleared&=gone(w,id);
        check(cleared && w.unit(a)->orders.empty() && w.unit(b)->orders.empty(),
              "two builders on one area clear it all and both finish");
    }

    // ---- map install: overlapped features do not survive (495360) ----------
    {
        sim::RetailMapFeatureType big{"big",2,2,true,false},small{"small",1,1,true,false},
            wall{"wall",2,2,true,true};
        big.clearable=small.clearable=true;
        const std::vector<sim::RetailMapFeatureType> types{big,small,wall};
        const int W=24,H=24;
        std::vector<uint16_t> raw(W*H,0xffff);
        auto at=[&](int x,int z)->uint16_t&{return raw[size_t(z)*W+x];};
        at(2,2)=0;at(3,3)=1;              // a 2x2 overlapped by a later 1x1
        at(6,6)=2;at(7,7)=1;              // an indestructible earlier one refuses the later
        at(10,10)=0;at(12,10)=2;at(11,11)=0;  // removal, then a refusal: both lost
        at(16,16)=0xfffc;
        const auto installed=sim::retailInstalledMapFeatures(W,H,raw,types);
        auto got=[&](int x,int z){return installed[size_t(z)*W+x];};
        check(got(2,2)==0xffff && got(3,3)==1,"a later overlapping feature removes the earlier one");
        check(got(6,6)==2 && got(7,7)==0xffff,"an indestructible earlier feature refuses the later one");
        check(got(10,10)==0xffff && got(11,11)==0xffff && got(12,10)==2,
              "a removal before a refusal stays done");
        check(got(16,16)==0xfffc,"blocker markers are kept");
        sim::World w;w.setTerrain(std::vector<uint8_t>(W*H,100),W,H,20);
        w.setMapPlacementFeatures(raw,types);
        check(w.installedMapFeatures()==installed,"the World snapshot is the shared helper's plane");
        // Cell +7: the anchor takes the installer's owner, tails stay 0xa, and
        // removal leaves the byte behind (4958ec, 496380).
        std::vector<sim::RetailMapFeatureCell> cells(W*H);
        auto cell=[&](int x,int z)->sim::RetailMapFeatureCell&{return cells[size_t(z)*W+x];};
        sim::retailMapInstall(W,H,4,4,0,types,cell,[](int,int,uint16_t){},3);
        check(cell(4,4).owner==3 && cell(5,5).owner==sim::kRetailWorldFeatureOwner,
              "a corpse owner is written at the anchor only");
        sim::retailMapInstall(W,H,5,5,1,types,cell,[](int,int,uint16_t){});
        check(cell(4,4).feature==0xffff && cell(4,4).owner==3 && cell(5,5).owner==sim::kRetailWorldFeatureOwner,
              "removal keeps the old owner byte");
    }
    // ---- 509d9f..509e6d fog test and 4066a3 same-cell retire ---------------
    {
        using sim::retailReclaimAreaSeen;
        check(retailReclaimAreaSeen(true,true,false,3) && !retailReclaimAreaSeen(true,false,false,0xa) &&
              retailReclaimAreaSeen(true,false,true,0xa) && !retailReclaimAreaSeen(true,false,true,3) &&
              retailReclaimAreaSeen(false,false,true,3) && !retailReclaimAreaSeen(false,true,false,0xa),
              "seen: current view, else an explored world feature; LOS off reads explored only");
        // Samples at 5, 21, 37, ... px; the one target sample (37,37) is in cell (2,2).
        sim::RetailReclaimArea a{5<<16,5<<16,69<<16,69<<16};
        a.lastX=(40<<16)+5;a.lastZ=(33<<16)+7;
        const auto one=[](int32_t x,int32_t z){return x==(37<<16) && z==(37<<16);};
        auto r=sim::retailReclaimAreaPick(a,8<<16,8<<16,32,32,one);
        check(r.step==sim::RetailReclaimAreaStep::Retire,"a pick in the last target's cell retires");
        a.lastX=a.lastZ=48<<16;
        r=sim::retailReclaimAreaPick(a,8<<16,8<<16,32,32,one);
        check(r.step==sim::RetailReclaimAreaStep::Child && r.x==(32<<16) && r.z==(32<<16) &&
              a.lastX==(32<<16) && a.lastZ==(32<<16),
              "another cell is a child at the snapped pick, which becomes the last target");
        sim::RetailReclaimArea b{(20<<16)+3,(20<<16)+3,(20<<16)+3,(20<<16)+3};
        b.lastX=b.lastZ=900<<16;
        r=sim::retailReclaimAreaPick(b,0,0,1,1,[](int32_t,int32_t){return true;});
        check(r.step==sim::RetailReclaimAreaStep::Retire,"two off-map cells compare equal (NULL == NULL)");
        r=sim::retailReclaimAreaPick(a,0,0,32,32,[](int32_t,int32_t){return false;});
        check(r.step==sim::RetailReclaimAreaStep::Done,"no target ends the area");
    }
    // ---- fog: explored world features stay clearable ------------------------
    {
        sim::UnitType blind=builder;blind.id="area-blind";blind.sight=1;
        sim::UnitType scout=builder;scout.id="area-scout";scout.isBuilder=scout.canReclaim=false;scout.sight=200;
        auto fogged=[&](sim::World& w,bool explore) {
            w.setTerrain(wide,64,64,20);
            w.setMapPlacementFeatures(std::vector<uint16_t>(64*64,0xffff),{});
            const int b=w.spawn(&blind,152,160,0,0);
            w.addFeature(20*64+25,408,328,1,1,1,false);
            if (explore) {
                const int s2=w.spawn(&scout,408,328,0,0);
                w.updateNavigationExploration();
                w.unit(s2)->x=sim::Fixed::fromInt(152);w.unit(s2)->z=sim::Fixed::fromInt(160);
            }
            w.updateNavigationExploration();
            return b;
        };
        {
            sim::World w;const int b=fogged(w,true);
            w.reclaimArea(b,384,304,432,352,false);untilPick(w,b,0,900);
            check(w.unit(b)->reclaimId==20*64+25,"a fogged but explored map feature is picked");
        }
        {
            sim::World w;const int b=fogged(w,false);
            w.reclaimArea(b,384,304,432,352,false);untilPick(w,b,0,900);
            check(w.unit(b)->orders.empty() && w.unit(b)->reclaimId==0,"a never-explored one is not");
        }
        {
            sim::World w;const int b=fogged(w,false);w.setRoomVision(true,false);
            w.reclaimArea(b,384,304,432,352,false);untilPick(w,b,0,900);
            check(w.unit(b)->reclaimId==20*64+25,"a room started with the map revealed picks it");
        }
    }
    // ---- retail exits on a real placement plane (path service on) ----------
    {
        struct Placed{int cx,cz,type;};
        auto fixture=[&](sim::World& w,const std::vector<Placed>& placed,sim::PathfindingMode mode) {
            const int W=192,H=192;
            w.setPathService(true);w.setPathfindingMode(mode);
            w.setTerrain(std::vector<uint8_t>(W*H,100),W,H,20);
            std::vector<uint16_t> raw(W*H,0xffff);
            for(const auto& p:placed)raw[size_t(p.cz)*W+p.cx]=uint16_t(p.type);
            sim::RetailMapFeatureType tree{"tree",1,1,true,false};tree.clearable=tree.treeOrRock=true;
            sim::RetailMapFeatureType wall{"wall",1,1,true,true};
            w.setMapPlacementFeatures(raw,{tree,wall});
            // energy 48: a 63-tick countdown, what one of these trees took
            // before the retail timing, so these crowding guards keep their length.
            sim::FeatType ft;ft.reclaimable=ft.blocking=true;ft.energy=48;
            sim::FeatType fw;fw.blocking=fw.indestructible=true;
            w.setFeatureTypes({ft,fw});
            for(const auto& p:placed) {
                w.blockCells(p.cx,p.cz,1,1,true);
                if(!p.type)w.addFeature(p.cz*W+p.cx,float(p.cx*16+8),float(p.cz*16+8),ft.energy,1,1,true,0,false);
            }
        };
        sim::UnitType worker;worker.id="area-worker";worker.maxHp=100;
        worker.isBuilder=worker.canMove=worker.canReclaim=true;
        worker.maxVel=sim::Fixed::fromFloat(1.35f);worker.buildDist=75;worker.sight=500;
        worker.footX=worker.footZ=2;worker.upright=true;worker.turnRate=worker.turnInPlaceRate=800;
        const int tcx=81,tcz=53,walled=tcz*192+tcx;
        auto ring=[&](std::vector<Placed>& placed) {
            for(int dz=-8;dz<=8;++dz)for(int dx=-8;dx<=8;++dx)
                if(std::max(std::abs(dx),std::abs(dz))==8)placed.push_back({tcx+dx,tcz+dz,1});
            placed.push_back({tcx,tcz,0});
        };
        // Runs until the builder is idle; counts distinct child reclaims.
        auto drive=[&](sim::World& w,int id,int limit,int& children) {
            int last=0,t=0;children=0;
            for(;t<limit && !w.unit(id)->orders.empty();++t) {
                const int r=w.unit(id)->reclaimId;
                if(r && r!=last)++children;
                last=r;
                w.tick(1.0f/30);w.updateNavigationExploration();
            }
            return t;
        };
        for (auto mode:{sim::PathfindingMode::Retail,sim::PathfindingMode::Legion}) {
            const bool legion=mode==sim::PathfindingMode::Legion;
            {
                sim::World w;std::vector<Placed> placed;ring(placed);fixture(w,placed,mode);
                const int b=w.spawn(&worker,650,800,0,0);w.updateNavigationExploration();
                w.reclaimArea(b,tcx*16-4,tcz*16-4,tcx*16+20,tcz*16+20,false);
                int children=0;const int t=drive(w,b,3600,children);
                check(w.unit(b)->orders.empty() && w.feature(walled)->alive && children<=2,
                      legion ? "Legion: an unreachable area target retires the order"
                             : "an unreachable area target retires the order (same-cell re-pick)");
                std::printf("     (idle at t=%d, %d child reclaims)\n",t,children);
            }
            if (!legion) {
                sim::World w;std::vector<Placed> placed;ring(placed);fixture(w,placed,mode);
                const int b=w.spawn(&worker,650,800,0,0);w.updateNavigationExploration();
                const double mana=w.player(0).mana;   // no reclaim income
                w.reclaim(b,walled,false);
                int children=0;const int t=drive(w,b,1500,children);
                check(w.unit(b)->orders.empty() && w.feature(walled)->alive && w.player(0).mana<=mana,
                      "an unreachable single reclaim ends on its failed approach");
                std::printf("     (dropped at t=%d)\n",t);
            }
            {
                sim::World w;std::vector<Placed> placed;ring(placed);
                const int free=tcz*192+tcx-14;placed.push_back({tcx-14,tcz,0});
                fixture(w,placed,mode);
                const int b=w.spawn(&worker,650,860,0,0);w.updateNavigationExploration();
                w.reclaimArea(b,(tcx-15)*16,tcz*16-4,tcx*16+20,tcz*16+20,false);
                int children=0;const int t=drive(w,b,6000,children);
                std::printf("     (idle at t=%d, %d child reclaims)\n",t,children);
                check(w.unit(b)->orders.empty() && gone(w,free) && w.feature(walled)->alive,
                      legion ? "Legion: a mixed area clears the free tree and retires on the walled one"
                             : "a mixed area clears the free tree and retires on the walled one");
            }
        }
        // Guard: a tight box over a forest still clears every tree (every one
        // of those routes is partial, into a blocked tree cell).
        for (float margin:{0.0f,4.0f}) {
            sim::World w;std::vector<Placed> placed;
            for(int j=0;j<10;++j)for(int i=0;i<10;++i)placed.push_back({(800+i*32)/16,(800+j*32)/16,0});
            fixture(w,placed,sim::PathfindingMode::Retail);
            const int b=w.spawn(&worker,650,800,0,0);w.updateNavigationExploration();
            w.reclaimArea(b,800-margin,800-margin,800+9*32+16+margin,800+9*32+16+margin,false);
            int children=0;const int t=drive(w,b,24000,children);
            int alive=0;for(const auto& p:placed)alive+=!gone(w,p.cz*192+p.cx);
            check(w.unit(b)->orders.empty() && alive==0,
                  margin==0 ? "forest edge, tight box: every tree cleared" : "forest edge, margin 4: every tree cleared");
            std::printf("     (margin %.0f: %d left, idle at t=%d)\n",double(margin),alive,t);
        }
        // Guard: three builders on one forest box (the client's fan-out) share
        // targets with no claims; crowding at a shared tree must not make any of
        // them retire early on a same-cell re-pick, in either movement mode.
        for (auto mode:{sim::PathfindingMode::Retail,sim::PathfindingMode::Legion}) {
            sim::World w;std::vector<Placed> placed;
            for(int j=0;j<10;++j)for(int i=0;i<10;++i)placed.push_back({(800+i*32)/16,(800+j*32)/16,0});
            fixture(w,placed,mode);
            std::vector<int> ids;
            for(int k=0;k<3;++k)ids.push_back(w.spawn(&worker,650,770+30*k,0,0));
            w.updateNavigationExploration();
            for(int id:ids)w.reclaimArea(id,796,796,800+9*32+20,800+9*32+20,false);
            int t=0;
            const auto busy=[&]{for(int id:ids)if(!w.unit(id)->orders.empty())return true;return false;};
            for(;t<12000 && busy();++t){w.tick(1.0f/30);w.updateNavigationExploration();}
            int alive=0;for(const auto& p:placed)alive+=!gone(w,p.cz*192+p.cx);
            check(!busy() && alive==0,mode==sim::PathfindingMode::Legion
                  ? "Legion: three builders on one forest box clear every tree"
                  : "three builders on one forest box clear every tree");
            std::printf("     (%d left, all idle at t=%d)\n",alive,t);
        }
    }
    return failed?1:0;
}
