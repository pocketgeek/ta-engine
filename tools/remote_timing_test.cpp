// Remote Effect timing and aim against the retail handlers, emulated tick by
// tick (RemoteEffect/Earthquake/AreaMindControl/AreaTurnToFrozen init 52e890 +
// update 52e940, Hailstorm 52ed60/52ee80; the slot aim point from 51aa50).
// Every expected timeline below is what the emulated retail code produced for
// the same timers, in ticks after the fire tick T.
#include "sim/sim.h"
#include "sim/retailaim.h"
#include "sim/retailmotion.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace tak::sim { struct RetailReplayProbe {
    static void shoot(World& w,int from,int target){w.fire(*w.unit(from),*w.unit(target),0);}
    static size_t pending(const World& w){return w.pendingEffects_.size();}
}; }

using namespace tak::sim;
namespace {
int failures=0;
void check(bool ok,const std::string& what,const std::string& detail={}) {
    std::printf("[%s] %s%s%s\n",ok?"PASS":"FAIL",what.c_str(),detail.empty()?"":"  ",detail.c_str());
    if(!ok)++failures;
}
std::string list(const std::vector<int>& v) {
    std::string s="[";
    for(size_t i=0;i<v.size();++i)s+=(i?",":"")+std::to_string(v[i]);
    return s+"]";
}

struct Spell {
    Weapon::RemoteKind kind=Weapon::RemoteKind::Plain;
    int32_t buildUp=0,decay=0,shake=0,duration=0,pps=0;
    float velocity=1000;
    bool shotArt=false;
    int distance=150;
    int killAt=-1;        // destroy the caster before this tick's update
};
struct Timeline { std::vector<int> hits; int destroyed=-1; };

Timeline run(const Spell& spell) {
    UnitType caster,victim;
    caster.id="caster";victim.id="victim";
    caster.maxHp=victim.maxHp=1000000;caster.footX=caster.footZ=victim.footX=victim.footZ=1;
    Weapon w;w.kind=Weapon::Kind::Remote;w.remote=spell.kind;
    w.damage=1;w.aoe=400;w.range=2000;w.reload=1000;w.projVel=spell.velocity;
    w.buildUpTicks=spell.buildUp;w.decayTicks=spell.decay;w.shakeTicks=spell.shake;
    w.durationTicks=spell.duration;w.particlesPerSec=spell.pps;
    // The parsed seconds too, as a loaded weapon carries both.
    w.buildUp=float(spell.buildUp)/30;w.decay=float(spell.decay)/30;
    w.shakeDur=float(spell.shake)/30;w.duration=float(spell.duration)/30;
    if(spell.shotArt)w.shotArt="cannbsm";
    caster.weapon=w;caster.weapons={w};
    World world;
    world.setPlayerCount(2);
    world.setTerrain(std::vector<uint8_t>(128*128,40),128,128,0);
    const int from=world.spawn(&caster,1000,1000,0,0);
    const int target=world.spawn(&victim,1000,float(1000+spell.distance),0,1);
    world.setStance(from,2);world.setStance(target,2);
    const Weapon* fired=&world.unit(from)->type->weapons[0];
    RetailReplayProbe::shoot(world,from,target);
    Timeline out;
    // The first tick after the shot is the fire tick T: the channel loop runs
    // after the unit updates, as retail's shot loop (52afd0) does.
    for(int k=0;k<400;++k) {
        if(k==spell.killAt)world.scenarioDestroy(from);
        world.tick(1.f/30);
        for(const auto& hit:world.hits())
            if(hit.weapon==fired){out.hits.push_back(k);break;}
        if(out.destroyed<0 && RetailReplayProbe::pending(world)==0)out.destroyed=k;
    }
    return out;
}

std::vector<int> every(int first,int step,int count) {
    std::vector<int> v;
    for(int i=0;i<count;++i)v.push_back(first+i*step);
    return v;
}

void expect(const char* name,const Spell& spell,const std::vector<int>& hits,int destroyed) {
    const auto got=run(spell);
    check(got.hits==hits,std::string(name)+" hits as retail",
          "ours "+list(got.hits)+" retail "+list(hits));
    check(got.destroyed==destroyed,std::string(name)+" expires as retail",
          "ours "+std::to_string(got.destroyed)+" retail "+std::to_string(destroyed));
}
}

int main() {
    using K=Weapon::RemoteKind;
    // Plain Remote Effect: one hit at T+1+buildup, expiry T+1+buildup+decay.
    expect("plain araking 1/0.9 s",{K::Plain,30,27},{31},58);
    expect("plain tarhel 0.9/0.85 s",{K::Plain,27,25},{28},53);
    expect("plain zondrag 0.25/1 s",{K::Plain,7,30},{8},38);
    expect("plain tarlich 0.5/0.9 s",{K::Plain,15,27,0,0,0,100},{16},43);
    // The expiry test runs first; the landing tick never runs the hook.
    expect("plain decaytime 0",{K::Plain,30,0},{},31);
    expect("plain builduptime 0",{K::Plain,0,30},{},31);
    // Earthquake: pulses at landing+k*shake, strictly before expiry.
    expect("quake aradrag 0.5/3 s shake 2",{K::Earthquake,15,90,60},{61},106);
    expect("quake aragod 1.5/1.5 s shake 1",{K::Earthquake,45,45,30},{31,61},91);
    // shotart: flight = floor(3D px * 65536 / speed) from the caster to the aim
    // point, starting at T+1; the buildup clock starts on landing.
    {
        Spell acolyte{K::Earthquake,90,90,30,0,0,200,true,200};
        expect("quake arapries shotart 200 px",acolyte,every(61,30,5),211);
        acolyte.distance=250;
        expect("quake arapries shotart 250 px",acolyte,every(68,30,5),218);
        expect("plain shotart 100 px at 200 px/s",{K::Plain,30,27,0,0,0,200,true,100},{46},73);
    }
    // Area charm / freeze: one sweep at the impact tick; the caster's death at
    // any point before it aborts the spell (52f000/52f360).
    expect("area mind control tarmind 1.75/0.5 s",{K::MindCtl,52,15,0,0,0,100},{53},68);
    expect("area freeze credrag 1/1 s",{K::Freeze,30,30},{31},61);
    {
        Spell killed{K::MindCtl,52,15,0,0,0,100};killed.killAt=10;
        check(run(killed).hits.empty(),"area mind control aborts when its caster dies mid-channel");
        Spell quake{K::Earthquake,45,45,30};quake.killAt=10;
        check(run(quake).hits==std::vector<int>{31,61},"an Earthquake outlives its caster");
    }
    // Hailstorm: first stone at T+19, then every 30/pps ticks (integer division)
    // until the last stone of the truncated duration lands, which ends it.
    expect("hail arapries 2.5 s pps 5",{K::Hailstorm,0,0,0,75,5},every(19,6,13),91);
    expect("hail cregod 1.8 s pps 8",{K::Hailstorm,0,0,0,54,8},every(19,3,19),73);
    expect("hail tarang 2 s pps 8",{K::Hailstorm,0,0,0,60,8},every(19,3,21),79);
    expect("hail tarmage 3 s pps 5",{K::Hailstorm,0,0,0,90,5},every(19,6,16),109);
    expect("hail tarwitch 2.75 s pps 10",{K::Hailstorm,0,0,0,82,10},every(19,3,28),100);

    // Aim: the effect lands on the slot's aim point -- the target's SweetSpot
    // led by 80% of its motion over the weapon's flight time (51aa50) -- not
    // where the target stood when the spell was cast.
    for(auto kind:{K::Plain,K::Earthquake,K::Hailstorm,K::MindCtl}) {
        UnitType caster,victim;
        caster.id="caster";victim.id="victim";victim.canMove=true;
        caster.maxHp=victim.maxHp=1000000;caster.footX=caster.footZ=victim.footX=victim.footZ=1;
        Weapon w;w.kind=Weapon::Kind::Remote;w.remote=kind;w.damage=1;w.aoe=64;w.range=2000;
        w.reload=1000;w.projVel=100;w.buildUpTicks=30;w.decayTicks=30;w.shakeTicks=30;
        w.durationTicks=30;w.particlesPerSec=5;
        w.buildUp=w.decay=w.shakeDur=w.duration=1;
        caster.weapon=w;caster.weapons={w};
        World world;
        world.setPlayerCount(2);
        world.setTerrain(std::vector<uint8_t>(128*128,40),128,128,0);
        const int from=world.spawn(&caster,1000,1000,0,0);
        const int target=world.spawn(&victim,1199,1000,0,1);
        world.setStance(from,2);world.setStance(target,2);
        Unit& t=*world.unit(target);
        t.speed=Fixed::fromInt(1);           // 1 px per tick
        const auto step=retailGroundStep(t.heading,t.speed);
        const float x0=t.x.toFloat(),z0=t.z.toFloat();
        const Weapon* fired=&world.unit(from)->type->weapons[0];
        RetailReplayProbe::shoot(world,from,target);
        float hx=0,hz=0;bool hit=false;
        for(int k=0;k<120 && !hit;++k) {
            world.tick(1.f/30);
            for(const auto& h:world.hits())
                if(h.weapon==fired){hx=h.x;hz=h.z;hit=true;break;}
        }
        // ~199 px at 100 px/s is ~59 ticks of flight; 80% of that is a 47 px lead.
        const float lead=std::hypot(hx-x0,hz-z0);
        const float along=(hx-x0)*step.s.toFloat()+(hz-z0)*step.c.toFloat();
        check(hit && std::fabs(lead-47.f)<=1.5f && along>45.f,
              std::string("aim leads a moving target, kind ")+std::to_string(int(kind)),
              "lead "+std::to_string(lead)+" px");
    }
    std::printf(failures?"FAIL: %d remote timing checks\n":"PASS: remote effect timing and aim match retail\n",failures);
    return failures?1:0;
}
