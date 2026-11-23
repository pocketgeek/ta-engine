// Text driver for tools/re/check_retail_group.py: runs the engine's Retail
// group helpers (src/sim/retailgroup.h, retailGroundFormation) on states the
// checker also loads into the original routines. Built by the checker.
#include "sim/retailgroup.h"
#include "sim/retailmission.h"
#include "sim/retailairattack.h"
#include <cstdio>
#include <iostream>
#include <string>

using namespace tak::sim;

static RetailGroupRecord readRecord(std::istream& in,bool& formation) {
    RetailGroupRecord r;int m,a,c;
    in>>m>>a>>c>>r.groundSpeed>>r.boatSpeed;formation=m;r.active=a;r.changed=c;
    for (int k=0;k<3;++k) for (auto* g:{&r.all[size_t(k)],&r.moving[size_t(k)]})
        in>>g->count>>g->area>>g->centre.x>>g->centre.y>>g->centre.z;
    return r;
}
static void writeRecord(const RetailGroupRecord& r) {
    std::printf("%d %d %d %d",int(r.active),int(r.changed),r.groundSpeed,r.boatSpeed);
    for (int k=0;k<3;++k) for (const auto* g:{&r.all[size_t(k)],&r.moving[size_t(k)]})
        std::printf(" %d %d %d %d %d",g->count,g->area,g->centre.x,g->centre.y,g->centre.z);
    std::printf("\n");
}
static RetailGroupMember readMember(std::istream& in) {
    RetailGroupMember m;int kind,mover;
    in>>kind>>mover>>m.position.x>>m.position.y>>m.position.z>>m.area;
    m.kind=RetailGroupClass(kind);m.mover=mover;return m;
}

int main() {
    std::string op;
    while (std::cin>>op) {
        if (op=="tick") {
            int notify,n;std::cin>>notify;
            std::array<RetailGroupRecord,100> records{};std::array<bool,100> formation{};
            for (int g=1;g<100;++g) {bool m;records[size_t(g)]=readRecord(std::cin,m);formation[size_t(g)]=m;}
            std::cin>>n;std::vector<RetailGroupUnit> units(static_cast<size_t>(n));
            for (auto& u:units) {int e;std::cin>>u.group>>e;u.eligible=e;u.member=readMember(std::cin);
                std::cin>>u.missionFlags>>u.typeMaximum;}
            std::vector<uint8_t> paced;
            retailGroupTick(units,formation,records,paced);
            std::vector<size_t> notified;
            if (notify) notified=retailGroupNotify(units,records);
            for (int g=1;g<100;++g) writeRecord(records[size_t(g)]);
            for (auto p:paced) std::printf("%d ",int(p));
            std::printf("\n");
            for (auto i:notified) std::printf("%zu ",i);
            std::printf("\n");
        } else if (op=="world") {
            // The World's own mapping (retailGroupSlot): each unit's engine
            // squad, or its AI squad and that squad's formation flag, picks
            // its record and whether that record paces (+8);
            // the records' own +8 inputs are ignored. Then 51b890 and, per
            // grouped unit, 51d1e0(u,1,3), 51d1e0(u,0,4) and 51c700(u,0,1).
            int n;
            std::array<RetailGroupRecord,100> records{};std::array<bool,100> formation{};
            for (int g=1;g<100;++g) {bool m;records[size_t(g)]=readRecord(std::cin,m);}
            std::cin>>n;std::vector<RetailGroupUnit> units(static_cast<size_t>(n));
            for (auto& u:units) {int squad,ai,aiFormation,e;std::cin>>squad>>ai>>aiFormation>>e;
                const auto slot=retailGroupSlot(squad,ai,aiFormation);
                u.group=slot.group;
                if (slot.group>0 && slot.group<100) formation[size_t(slot.group)]|=slot.formation;
                u.eligible=e;u.member=readMember(std::cin);std::cin>>u.missionFlags>>u.typeMaximum;}
            std::vector<uint8_t> paced;
            retailGroupTick(units,formation,records,paced);
            for (int g=1;g<100;++g) writeRecord(records[size_t(g)]);
            for (auto p:paced) std::printf("%d ",int(p));
            std::printf("\n");
            for (const auto& u:units) {
                if (u.group<1 || u.group>=100) { std::printf("-\n");continue; }
                const auto& r=records[size_t(u.group)];
                const bool all=(u.missionFlags&0x4000000u)!=0;
                const auto c=retailGroupCentre(r,u.member,false,true);
                std::printf("%d %d %d %d\n",int(retailGroupOutOfSlot(r,u.member,true,3,all)),
                            int(retailGroupOutOfSlot(r,u.member,false,4,false)),c.x,c.z);
            }
        } else if (op=="query") {
            bool m;auto r=readRecord(std::cin,m);auto u=readMember(std::cin);int a,b,level,all;
            std::cin>>a>>b>>level>>all;
            const auto c=retailGroupCentre(r,u,a,b);
            std::printf("%d %d %d %d %d\n",c.x,c.y,c.z,retailGroupRadius(r,u,a,b),
                        int(retailGroupOutOfSlot(r,u,b,level,all)));
        } else if (op=="check") {
            bool m;auto r=readRecord(std::cin,m);auto u=readMember(std::cin);uint32_t flags;
            RetailGroupPoint16 p,g;int draw;
            std::cin>>flags>>p.x>>p.y>>p.z>>g.x>>g.y>>g.z>>draw;
            int draws=0;
            const int result=retailGroundGroupCheck(r,u,flags,p,g,[&](int){++draws;return draw;});
            if (result==1) retailGroupLeave(r,u);
            std::printf("%d %u %d\n",result,flags,draws);writeRecord(r);
        } else if (op=="formation") {
            bool m;auto r=readRecord(std::cin,m);auto u=readMember(std::cin);
            int stage,level,attached,mode,draw;uint32_t flags;
            std::cin>>stage>>level>>attached>>mode>>flags>>draw;
            RetailMissionState s;s.stage=uint8_t(stage);s.flags=flags;
            std::string calls;int gx=0,gz=0;
            const bool all=(flags&0x4000000u)!=0;
            const int result=retailGroundFormation(s,1000,level,attached,mode,
                [&]{return level>0 ? retailGroupOutOfSlot(r,u,false,level,all) : retailGroupOutOfSlot(r,u,true,-level,all);},
                [&]{calls+="stop,";},
                [&]{const auto c=retailGroupCentre(r,u,all,level<=0);gx=c.x;gz=c.z;calls+="centre,";},
                [&]{calls+="move,";},[&]{calls+="scan,";return false;},[&](int){calls+="rand,";return draw;});
            if (s.waitMask&1) calls+="sleep"+std::to_string(s.deadline-1000)+",";
            std::printf("%d %d %d %d %s\n",result,int(s.stage),gx,gz,calls.empty()?"-":calls.c_str());
        } else if (op=="suppresshead") {
            // SUPPRESS's group check (404e12..404f1b): retailAttackGroupCheck
            // at level 4, leave on rand(3), clear on rand(15).
            bool m;auto r=readRecord(std::cin,m);auto u=readMember(std::cin);uint32_t flags,next;int draw;
            std::cin>>flags>>next>>draw;
            std::string calls;
            const int result=retailAttackGroupCheck(flags,retailGroupOutOfSlot(r,u,true,4,(flags&0x4000000u)!=0),next,3,15,
                [&](int n){calls+="rand"+std::to_string(n)+",";return draw;},
                [&]{calls+="leave,";retailGroupLeave(r,u);},[&]{calls+="formation,";return true;});
            std::printf("%d %u %s\n",result,flags,calls.empty()?"-":calls.c_str());writeRecord(r);
        } else if (op=="followhead") {
            // Follow_Ground's group check (40800c..40810e): retailGuardGroupCheck
            // (51d1e0(unit, 1, 4), cancel on rand(4), clear on rand(10)).
            bool m;auto r=readRecord(std::cin,m);auto u=readMember(std::cin);uint32_t flags;int draw;
            std::cin>>flags>>draw;
            std::string calls;
            const int result=retailGuardGroupCheck(flags,retailGroupOutOfSlot(r,u,true,4,(flags&0x4000000u)!=0),
                [&](int n){calls+="rand"+std::to_string(n)+",";return draw;},
                [&]{calls+="leave,cancel,";retailGroupLeave(r,u);},[&]{calls+="formation,";return true;});
            std::printf("%d %u %s\n",result,flags,calls.empty()?"-":calls.c_str());writeRecord(r);
        } else if (op=="standbyhead") {
            // The standbys' group check (retailStandbyGroupCheck): the
            // result, and the FormationStandby's response mode (+0x52) and
            // level (+0x56) when it goes ahead; -1 0 0: the stages run.
            bool m;auto r=readRecord(std::cin,m);auto u=readMember(std::cin);int site;uint32_t flags;
            std::cin>>site>>flags;
            const auto kind=site==0 ? RetailStandbyCheck::VtolStandby : site==1 ? RetailStandbyCheck::Standby
                                    : RetailStandbyCheck::LandIfCan;
            int level=0,mode=0;
            const int result=retailStandbyGroupCheck(kind,[&](int lv){
                return retailGroupOutOfSlot(r,u,true,lv,(flags&0x4000000u)!=0);},
                [&](int lv,int md){level=lv;mode=md;return true;});
            std::printf("%d %d %d\n",result,result<0 ? 0 : mode,result<0 ? 0 : level);
        } else if (op=="vtolformation") {
            // 417750, the FormationStandby branch too (41797e..417ad1): in
            // the slot with 0x4000000 and the group moving (retailGroupMoving),
            // the scan's attack (answered) or the loiter point. The draws
            // are taken in call order, as the checker's 535cc0 hook does.
            bool m;auto r=readRecord(std::cin,m);auto u=readMember(std::cin);
            int stage,level,canFly,attack;uint32_t flags,heading,draws[3];RetailFlightVector at;
            std::cin>>stage>>level>>canFly>>flags>>draws[0]>>draws[1]>>draws[2]>>heading>>attack>>at.x>>at.y>>at.z;
            RetailMissionState s;s.stage=uint8_t(stage);s.flags=flags;
            std::string calls;int gx=0,gz=0;RetailFlightVector point{};unsigned next=0;
            const bool all=(flags&0x4000000u)!=0;
            const int result=retailFlightFormation(s,1000,level,canFly,uint16_t(heading),
                [&]{return level>0 ? retailGroupOutOfSlot(r,u,false,level,all) : retailGroupOutOfSlot(r,u,true,-level,all);},
                [&]{const auto c=retailGroupCentre(r,u,all,level<=0);gx=c.x;gz=c.z;calls+="centre,";},
                [&]{calls+="begin,";},[&]{calls+="move,";},
                [&](int){calls+="rand,";return next<3 ? int(draws[next++]) : 0;},
                [&]{return retailGroupMoving(r);},
                [&]{calls+="scan,";return attack!=0;},
                [&](uint16_t angle,uint32_t distance){calls+="loiter,";point=retailAirRunPoint(at,at.y,angle,distance);});
            if (s.waitMask&1) calls+="sleep"+std::to_string(s.deadline-1000)+",";
            std::printf("%d %d %d %d %s %d %d %d %u\n",result,int(s.stage),gx,gz,calls.empty()?"-":calls.c_str(),
                        point.x,point.y,point.z,s.waitMask);
        }
        std::fflush(stdout);
    }
}
