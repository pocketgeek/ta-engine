// TotalA.exe's repair pads: a fighter on patrol below 3/4 of its hit points
// (VTOL_Patrol 0x410f3e) puts VTOL_LANDING (0x4118e0) on an air repair pad
// of its own side within 3840 px (0x40b530) ahead of its patrol, lands on
// the piece QueryLandingPad names, is mended there by SelfRepair (0x402430,
// one hit point and one energy a tick through 0x41bd10) and, whole, lets go
// of the pad and patrols on (its patrol back from stage 0).
//
// Checked: the damaged fighter lands on the pad (attached at a piece) and
// is repaired to full at about a hit point a tick, paid in energy; then it
// leaves the pad and its patrol order leads again. A healthy fighter, a
// damaged one with no pad in reach and one whose only pad is the enemy's
// never land. Two damaged fighters share a one-piece pad (armasp's script
// names piece 1 twice): the second circles until the first has gone, then
// lands; both end whole. A Coral Carrier (corcarry) names two pieces and
// takes both at once.
//
// Landing by order (Cmd::Land; TotalA.exe's default order on an allied air
// base, 0x43f0e0 action 1 -> VTOL_LANDING, 0x43faf5..0x43fb26): a whole
// fighter lands and stays on the piece, a damaged one is mended there; an
// enemy pad, a unit that cannot fly and a target that is no air base refuse
// it. The attacks' detours: a damaged flyer on AirToGround (0x412b3f) and on
// AirToGroundHover (0x413a8a) goes to the pad. The pad list (0x40b530's,
// AIPlayer +0x25) is in slot order: a pad in a freed lower slot comes first.
//
//   air_pad_test <retail-install-dir>
#include "hpi/hpi.h"
#include "net/lockstep.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ta::sim;

namespace {

constexpr float kTick=1.f/30;
int failures=0;
void check(bool ok,const std::string& what) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",what.c_str());
    if (!ok) ++failures;
}

struct Fighter {
    int id=0;
    bool landed=false,repaired=false,resumed=false;
    int landTick=-1,wholeTick=-1,leaveTick=-1,piece=-1;
    float hpAtLanding=0;
};

void watch(World& w,Fighter& f,int pad,int tick) {
    const Unit* u=w.unit(f.id);
    if (!u || !u->alive()) return;
    if (u->inTransport==pad && u->padPiece>=0 && !f.landed) {
        f.landed=true;f.landTick=tick;f.piece=u->padPiece;f.hpAtLanding=u->hp.toFloat();
    }
    if (f.landed && !f.repaired && u->hp>=Fixed::fromInt(u->type->maxHp)) { f.repaired=true;f.wholeTick=tick; }
    if (f.repaired && !f.resumed && u->inTransport==0 && !u->orders.empty() && u->orders.front().patrol &&
        u->flightGroundMode==2) { f.resumed=true;f.leaveTick=tick; }
}

struct Field {
    World w;
    int pad=0;
};

void setup(Field& f,const TypeRegistry& registry,const char* padType,float padX,float padZ,int padPlayer,
           uint8_t height=60) {
    World& w=f.w;
    w.setSerialThreads(true);
    w.setPlayerCount(2);
    w.setTerrain(std::vector<uint8_t>(256*256,height),256,256,20);
    w.buildNavClasses(registry);
    w.setStartResources(0,1e6f,1e6f);w.setStartResources(1,1e6f,1e6f);
    if (padType) f.pad=w.spawn(registry.find(padType),padX,padZ,0.f,padPlayer);
}

int damagedFighter(World& w,const TypeRegistry& registry,float x,float z,float fraction) {
    const UnitType* fig=registry.find("armfig");
    const int id=w.spawn(fig,x,z,0.f,0);
    Unit& u=*w.unit(id);
    u.hp=Fixed::fromInt(int(float(fig->maxHp)*fraction));
    u.fireState=0;   // nothing to fight here anyway
    return id;
}

} // namespace

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    auto vfs=ta::hpi::mountRetailRoot(argv[1]);
    TypeRegistry registry;setupRegistry(registry,vfs);
    for (const char* id:{"armasp","corcarry","armfig"})
        if (!registry.find(id)) { std::printf("FAIL missing type %s\n",id);return 1; }

    // One damaged fighter, one armasp.
    {
        Field f;setup(f,registry,"armasp",1400,1400,0);
        World& w=f.w;
        Fighter a;a.id=damagedFighter(w,registry,1800,1800,0.4f);
        w.patrolTo(a.id,2600,1800,false);
        bool sawLanding=false;
        float spent=0;   // the most energy the pad asked for in a second while mending
        for (int t=0;t<2400;++t) {
            w.tick(kTick);
            const Unit* u=w.unit(a.id);
            if (u && !u->orders.empty() && u->orders.front().airPad==Order::kAirPadLanding) sawLanding=true;
            watch(w,a,f.pad,t);
            if (a.landed && !a.repaired) spent=std::max(spent,w.unit(f.pad)->books.r[econ::E].lastReq);
        }
        const UnitType* fig=registry.find("armfig");
        const float needed=float(fig->maxHp)-a.hpAtLanding;
        std::printf("  landed %d at %d (piece %d, hp %.0f), whole at %d, resumed %d at %d, energy asked %.0f/s\n",
                    int(a.landed),a.landTick,a.piece,a.hpAtLanding,a.wholeTick,int(a.resumed),a.leaveTick,spent);
        check(sawLanding,"the damaged fighter's patrol puts VTOL_LANDING ahead of itself");
        check(a.landed && a.piece==1,"it lands on the pad at the piece QueryLandingPad names (armasp: 1)");
        check(a.repaired,"SelfRepair mends it to full on the pad");
        // 0x41bd10: one hit point a tick (each value capped at 1).
        if (a.repaired)
            check(std::abs(float(a.wholeTick-a.landTick)-needed)<=needed*0.1f+10,
                  "about a hit point a tick ("+std::to_string(a.wholeTick-a.landTick)+" ticks for "+
                  std::to_string(int(needed))+")");
        check(spent>=25 && spent<=31,"the pad pays for it in energy (one a hit point, 30 a second)");
        check(a.resumed,"whole, it leaves the pad and its patrol leads again");
        const Unit* u=w.unit(a.id);
        check(u && u->hp>=Fixed::fromInt(fig->maxHp) && u->inTransport==0,"it ends whole and off the pad");
    }

    // The controls: healthy, no pad in reach, the enemy's pad.
    struct Control { const char* label; float fraction; float padX; int padPlayer; };
    for (const Control& c:{Control{"a fighter above 3/4 health",0.8f,1400,0},
                           Control{"a damaged fighter with its pad 4000 px away",0.4f,-2200,0},
                           Control{"a damaged fighter with only an enemy pad",0.4f,1400,1}}) {
        Field f;setup(f,registry,"armasp",c.padX,1400,c.padPlayer);
        World& w=f.w;
        const int id=damagedFighter(w,registry,1800,1800,c.fraction);
        w.patrolTo(id,2600,1800,false);
        bool landing=false;
        for (int t=0;t<900;++t) {
            w.tick(kTick);
            const Unit* u=w.unit(id);
            if (u && (u->inTransport || (!u->orders.empty() && u->orders.front().airPad))) landing=true;
        }
        check(!landing,std::string(c.label)+" never goes to a pad");
    }

    // Two damaged fighters, one piece.
    {
        Field f;setup(f,registry,"armasp",1400,1400,0);
        World& w=f.w;
        Fighter a,b;
        a.id=damagedFighter(w,registry,1800,1800,0.4f);
        b.id=damagedFighter(w,registry,1850,1750,0.4f);
        w.patrolTo(a.id,2600,1800,false);w.patrolTo(b.id,2600,1750,false);
        bool both=false;
        for (int t=0;t<4800;++t) {
            w.tick(kTick);
            watch(w,a,f.pad,t);watch(w,b,f.pad,t);
            const Unit* ua=w.unit(a.id);const Unit* ub=w.unit(b.id);
            if (ua && ub && ua->inTransport==f.pad && ub->inTransport==f.pad) both=true;
        }
        std::printf("  first landed %d..%d, second landed %d..%d\n",std::min(a.landTick,b.landTick),
                    a.landTick<b.landTick ? a.leaveTick : b.leaveTick,std::max(a.landTick,b.landTick),
                    a.landTick<b.landTick ? b.leaveTick : a.leaveTick);
        check(!both,"armasp's one piece holds one fighter at a time");
        check(a.landed && b.landed && a.repaired && b.repaired,"both land in turn and are mended");
        const Fighter& first=a.landTick<b.landTick ? a : b;
        const Fighter& second=a.landTick<b.landTick ? b : a;
        check(second.landTick>=first.wholeTick,"the second lands once the first has gone");
    }

    // A carrier's two pieces.
    {
        Field f;setup(f,registry,"corcarry",1400,1400,0,5);   // at sea
        World& w=f.w;
        // corcarry is Core's: the pads are its own player's.
        Fighter a,b;
        const UnitType* fig=registry.find("corveng") ? registry.find("corveng") : registry.find("armfig");
        a.id=w.spawn(fig,1800,1800,0.f,0);b.id=w.spawn(fig,1850,1750,0.f,0);
        w.unit(a.id)->hp=Fixed::fromInt(fig->maxHp*4/10);w.unit(b.id)->hp=Fixed::fromInt(fig->maxHp*4/10);
        w.unit(a.id)->fireState=0;w.unit(b.id)->fireState=0;
        w.patrolTo(a.id,2600,1800,false);w.patrolTo(b.id,2600,1750,false);
        bool both=false;
        for (int t=0;t<2400;++t) {
            w.tick(kTick);
            watch(w,a,f.pad,t);watch(w,b,f.pad,t);
            const Unit* ua=w.unit(a.id);const Unit* ub=w.unit(b.id);
            if (ua && ub && ua->inTransport==f.pad && ub->inTransport==f.pad && ua->padPiece!=ub->padPiece) both=true;
        }
        std::printf("  carrier: pieces %d / %d, landed at %d / %d\n",a.piece,b.piece,a.landTick,b.landTick);
        check(both,"a carrier's two pieces take two fighters at once");
        check(a.repaired && b.repaired,"both are mended on the carrier");
    }

    // The take-off of VTOL_RepairUnit's stage 0 (0x414fe1): a landed
    // construction aircraft sent to mend a unit 400 px off climbs straight
    // up over its spot to cruisealt / 2 before it heads there.
    {
        Field f;setup(f,registry,nullptr,0,0,0);
        World& w=f.w;
        const UnitType* ca=registry.find("armca");
        const UnitType* pw=registry.find("armpw");
        if (!ca || !pw) { check(false,"armca and armpw load"); }
        else {
            const int b=w.spawn(ca,1800,1800,0.f,0);
            for (int t=0;t<120;++t) w.tick(kTick);
            const bool landed=w.unit(b)->flightGroundMode==1;
            const float x0=w.unit(b)->x.toFloat(),z0=w.unit(b)->z.toFloat(),y0=w.unit(b)->flightY.toFloat();
            UnitType sturdy=*pw;sturdy.maxVel={};
            const int t=w.spawn(&sturdy,x0+400,z0,0.f,0);
            w.unit(t)->hp=Fixed::fromInt(sturdy.maxHp/2);
            w.repair(b,t,false);
            float drift=0,climbed=0;bool reached=false,mended=false;
            for (int k=0;k<900;++k) {
                w.tick(kTick);
                const Unit& u=*w.unit(b);
                if (!reached) {
                    drift=std::max(drift,std::hypot(u.x.toFloat()-x0,u.z.toFloat()-z0));
                    climbed=u.flightY.toFloat()-y0;
                    if (climbed>=float(ca->cruiseAlt/2)-1.f) reached=true;
                }
                if (w.unit(t)->hp>=Fixed::fromInt(sturdy.maxHp)) mended=true;
            }
            std::printf("  armca take-off: landed %d, climbed %.1f of %d with %.1f px drift, mended %d\n",
                        int(landed),climbed,ca->cruiseAlt/2,drift,int(mended));
            check(landed && reached && drift<=2.f,"a landed repairer climbs to cruisealt / 2 over its spot first");
            check(mended,"and then flies over and mends the unit");
        }
    }

    // Landing by order.
    {
        Field f;setup(f,registry,"armasp",1400,1400,0);
        World& w=f.w;
        const UnitType* fig=registry.find("armfig");
        const int whole=w.spawn(fig,1800,1800,0.f,0);w.unit(whole)->fireState=0;
        for (int t=0;t<30;++t) w.tick(kTick);
        auto land=[&](int unit,int pad,int player) {
            ta::net::Command c;c.kind=ta::net::Cmd::Land;c.player=uint8_t(player);c.unitId=unit;c.targetId=pad;
            applyCommand(w,registry,c);
        };
        land(whole,f.pad,0);
        const Unit* u=w.unit(whole);
        check(u && !u->orders.empty() && u->orders.front().airPad==Order::kAirPadLanding &&
              u->orders.front().airPadId==f.pad,"Cmd::Land gives the flyer VTOL_LANDING on its own pad");
        Fighter a;a.id=whole;
        for (int t=0;t<900;++t) { w.tick(kTick);watch(w,a,f.pad,t); }
        u=w.unit(whole);
        check(a.landed && a.piece==1,"the whole fighter lands on the pad's piece");
        check(u && u->inTransport==f.pad && u->orders.empty(),"and stays there once the landing is done");
        // A damaged one is mended there.
        const int hurt=damagedFighter(w,registry,2000,1700,0.4f);
        for (int t=0;t<30;++t) w.tick(kTick);
        // armasp has one piece: the whole fighter makes room first.
        w.order(whole,2400,2400,false);
        land(hurt,f.pad,0);
        Fighter b;b.id=hurt;
        for (int t=0;t<3000;++t) { w.tick(kTick);watch(w,b,f.pad,t); }
        check(b.landed && b.repaired,"a damaged fighter sent to the pad lands and is mended");
        // Refused: the enemy's pad, a unit that cannot fly, a target that is no air base.
        const int enemyPad=w.spawn(registry.find("armasp"),2400,1400,0.f,1);
        const int other=w.spawn(fig,1700,2000,0.f,0);w.unit(other)->fireState=0;
        const UnitType* pw=registry.find("armpw");
        const int walker=pw ? w.spawn(pw,1500,1700,0.f,0) : 0;
        for (int t=0;t<30;++t) w.tick(kTick);
        land(other,enemyPad,0);
        check(w.unit(other)->orders.empty() || !w.unit(other)->orders.front().airPad,"the enemy's pad is refused");
        if (walker) {
            land(walker,f.pad,0);
            check(w.unit(walker)->orders.empty() || !w.unit(walker)->orders.front().airPad,
                  "a unit that cannot fly does not land");
            land(other,walker,0);
            check(w.unit(other)->orders.empty() || !w.unit(other)->orders.front().airPad,
                  "a unit that is no air base is no pad");
        }
        land(other,f.pad,1);
        check(w.unit(other)->orders.empty() || !w.unit(other)->orders.front().airPad,"another player's unit is not ordered");
    }

    // The attacks' detours: AirToGround (0x412b3f) and AirToGroundHover (0x413a8a).
    for (const bool hover:{false,true}) {
        Field f;setup(f,registry,"armasp",1400,1400,0);
        World& w=f.w;
        // A fighter whose missile may hit the ground: AirToGround, or with
        // hoverattack AirToGroundHover.
        UnitType gun=*registry.find("armfig");
        for (auto& wp:gun.weapons) { wp.toAir=false;wp.cursorAirstrike=false; }
        gun.hoverAttack=hover;
        const UnitType* pw=registry.find("armpw");
        UnitType post=*pw;post.maxVel={};post.maxHp=100000;post.weapons.clear();
        const int target=w.spawn(&post,2200,1800,0.f,1);
        const int id=w.spawn(&gun,1800,1800,0.f,0);
        w.unit(id)->hp=Fixed::fromInt(gun.maxHp*4/10);
        for (int t=0;t<30;++t) w.tick(kTick);
        w.attack(id,target,false);
        bool attacking=false,landing=false;Fighter a;a.id=id;
        for (int t=0;t<1500;++t) {
            const Unit* u=w.unit(id);
            if (u && !u->orders.empty() && u->orders.front().targetId==target && u->orders.front().mission.stage) attacking=true;
            w.tick(kTick);
            u=w.unit(id);
            if (u && !u->orders.empty() && u->orders.front().airPad==Order::kAirPadLanding) landing=true;
            watch(w,a,f.pad,t);
        }
        const std::string kind=hover ? "AirToGroundHover" : "AirToGround";
        check(attacking,kind+": the damaged flyer starts its attack");
        check(landing && a.landed,kind+": under 3/4 health it puts VTOL_LANDING ahead and lands on the pad");
        check(a.repaired,kind+": it is mended there");
    }

    // The pad list's order (0x40aa40: the unit array in index order; a new
    // unit takes its owner's first free slot, 0x485fea): a pad built in a
    // slot freed by a death comes before one in a later slot.
    {
        Field f;setup(f,registry,nullptr,0,0,0);
        World& w=f.w;
        const UnitType* pw=registry.find("armpw");
        const int doomed=w.spawn(pw,1000,1000,0.f,0);           // slot 0
        const int first=w.spawn(registry.find("armasp"),1400,1400,0.f,0);   // slot 1
        w.unit(doomed)->hp=Fixed();
        for (int t=0;t<5;++t) w.tick(kTick);
        const bool dead=!w.unit(doomed) || !w.unit(doomed)->alive();
        const int second=w.spawn(registry.find("armasp"),1700,1400,0.f,0); // slot 0 again
        for (int t=0;t<90;++t) w.tick(kTick);
        const auto& list=w.airPadList(0);
        std::printf("  pad list:");for (int id:list) std::printf(" %d",id);std::printf(" (first %d, second %d)\n",first,second);
        check(dead && list.size()==2 && list[0]==second && list[1]==first,
              "the pad in the freed lower slot leads the list, not the lower id");
    }

    std::printf("air_pad: %d failures\n",failures);
    return failures?1:0;
}
