// World::applyHit on a unit that is already dying, and on hits too big for a
// 16.16 whole number. Retail 51a140 returns before touching a unit flagged
// dying (+0x130 & 0x1000, set by 51a639 the moment its HP reaches zero), so a
// second hit in the same tick changes nothing -- not its HP, not who gets the
// kill. Before the fix World kept subtracting: a unit at -25000 hit for 14000
// wrapped past -32768 to about +26500 HP, signed overflow in fixed.h, and the
// last hitter stole the kill credit. A hit above 32767 overflowed fromInt.
#include "sim/sim.h"
#include <cmath>
#include <cstdio>
using namespace tak;

namespace tak::sim { struct RetailReplayProbe {
    static void hit(World& w,const Weapon& weapon,int from,int target) {
        auto* victim=w.unit(target);
        w.applyHit(weapon,victim->x.toFloat(),victim->z.toFloat(),w.unit(from)->player,from,victim);
    }
}; }

namespace {
int failures=0;
void check(bool ok,const char* label) {
    std::printf("%s %s\n",ok?"PASS":"FAIL",label);failures+=!ok;
}
sim::UnitType body(const char* id,int hp) {
    sim::UnitType t;t.id=id;t.maxHp=hp;t.footX=t.footZ=2;t.maxVel=sim::Fixed::fromInt(1);
    t.canMove=true;t.upright=true;t.sight=40;t.sightHeight=24;return t;
}
sim::Weapon blow(float damage) {
    sim::Weapon w;w.name="blow";w.damage=damage;w.range=100;return w;
}
void setup(sim::World& w) {
    w.setSerialThreads(true);
    w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);w.setPlayerCount(2);
}
}

int main() {
    using sim::Fixed;
    const auto attacker=body("attacker",500),victim=body("victim",500),giant=body("giant",29999);
    {
        sim::World w;setup(w);
        const int a=w.spawn(&attacker,200,200,0,0),b=w.spawn(&attacker,260,200,0,0);
        const int v=w.spawn(&victim,230,260,0,1);
        sim::RetailReplayProbe::hit(w,blow(26000),a,v);   // 500 - 26000 +-15%: -29400..-21600
        const Fixed dying=w.unit(v)->hp,overkill=w.unit(v)->overkill;
        check(dying<=Fixed() && dying>=-Fixed::fromInt(32767),"the fatal hit leaves hp in range");
        check(w.unit(v)->lastHitBy==a,"the fatal hitter holds the kill");
        sim::RetailReplayProbe::hit(w,blow(14000),b,v);   // same tick, before the sweep: 11900+
        std::printf("  hp %.1f -> %.1f\n",dying.toFloat(),w.unit(v)->hp.toFloat());
        check(w.unit(v)->hp==dying,"a dying unit takes no further damage (51a140)");
        check(w.unit(v)->hp<=Fixed(),"a dying unit never wraps back to positive hp");
        check(w.unit(v)->lastHitBy==a,"a later hit does not steal the kill credit");
        check(w.unit(v)->overkill==overkill,"overkill is the fatal hit's");
        w.tick(1.f/30);
        check(!w.unit(v)->alive(),"the death sweep still kills it");
    }
    {
        sim::World w;setup(w);
        const int a=w.spawn(&attacker,200,200,0,0),v=w.spawn(&giant,230,260,0,1);
        sim::RetailReplayProbe::hit(w,blow(40000),a,v);   // dealt 34000..46000 > 32767
        const Fixed hp=w.unit(v)->hp;
        std::printf("  29999 hp after a 40000 hit: %.1f\n",hp.toFloat());
        check(hp<=-Fixed::fromInt(4000) && hp>=-Fixed::fromInt(16002),
              "a hit above 32767 subtracts its whole value");
        check(w.unit(v)->overkill==-hp,"overkill matches the saturated hp");
    }
    {
        // A retail construction site is not dying at zero hp, so a big hit
        // saturates it at -INT32_MAX; the unattended site's decay that tick
        // must not wrap that back to positive and leave the site standing.
        sim::World w;setup(w);
        const int a=w.spawn(&attacker,200,200,0,0),s=w.spawn(&victim,230,260,0,1);
        auto* site=w.unit(s);site->underConstruction=true;site->retailSite.emplace();site->hp=Fixed();
        sim::RetailReplayProbe::hit(w,blow(40000),a,s);
        check(w.unit(s)->hp==Fixed::raw(-INT32_MAX),"a retail site at zero saturates under a huge hit");
        w.tick(1.f/30);
        check(!w.unit(s)->alive() || w.unit(s)->hp<=Fixed(),"its decay does not wrap the hp back to positive");
    }
    {
        // Construction growth. A mod with a tiny buildtime on a tall type makes one
        // tick's growth as large as the whole bar; the site's hp (already above
        // zero from the first tick) plus that growth wrapped the 16.16 word, leaving
        // a negative hp, or UB under UBSan. Retail bounds a site's hp by maxdamage.
        sim::World w;setup(w);
        sim::UnitType site=body("tower",32767);site.maxVel=Fixed();site.canMove=false;
        site.buildTime=0.001f;site.buildCost=0;
        sim::UnitType worker=body("worker",100);worker.isBuilder=true;worker.workerTime=1;worker.buildDist=64;
        const int b=w.spawn(&worker,200,200,0,0);
        const int s=w.startBuild(b,&site,200,240,sim::World::Approach::None);
        check(s!=0,"a short build of a 32767-hp type is placed");
        if(s) {
            w.unit(s)->hp=Fixed::fromInt(30000);   // growth below adds ~32767 more
            bool over=false,neg=false;
            for(int i=0;i<30 && w.unit(s)->underConstruction;++i) {
                w.tick(1.f/30);
                const Fixed hp=w.unit(s)->hp;
                over|=hp>Fixed::fromInt(32767);neg|=hp<Fixed();
            }
            check(!neg,"a one-tick build never wraps the site's hp negative");
            check(!over,"construction never raises hp above maxdamage");
            check(!w.unit(s)->underConstruction && w.unit(s)->hp==Fixed::fromInt(32767),
                  "the short build completes at exactly maxdamage");
        }
    }
    {
        // Retail 51a140 (51a3c2..51a408): a hit on an unfinished site rewrites its
        // progress (hp word and remaining = 1 - hp/maxdamage); the next work tick
        // otherwise restored the stale hp and erased the damage.
        sim::World w;setup(w);
        const int a=w.spawn(&attacker,200,200,0,0),s=w.spawn(&victim,230,260,0,1);
        auto* site=w.unit(s);site->underConstruction=true;site->retailSite.emplace();
        site->retailSite->progress.remaining=0.4f;site->retailSite->progress.hp=300;
        site->retailSite->type.maxHp=500;
        site->hp=Fixed::fromInt(300);
        sim::RetailReplayProbe::hit(w,blow(100),a,s);
        const auto& pr=w.unit(s)->retailSite->progress;
        check(pr.hp==uint16_t(w.unit(s)->hp.floorInt()),"a hit on a retail site updates its progress hp");
        check(std::fabs(pr.remaining-float(1.0-double(w.unit(s)->hp.floorInt())/500))<1e-6f,
              "...and its remaining work (1 - hp/maxdamage)");
    }
    {
        // A LETHAL hit on a retail site: 51a3e0 reads the hp word signed (movsx), so
        // remaining = 1 - (negative)/maxdamage lands above 1. Read unsigned, the word
        // (e.g. 65486) made remaining about -130 and the next work call (429af0)
        // completed the site at full health.
        sim::World w;setup(w);
        const int a=w.spawn(&attacker,200,200,0,0),s=w.spawn(&victim,230,260,0,1);
        auto* site=w.unit(s);site->underConstruction=true;site->retailSite.emplace();
        site->retailSite->progress.remaining=0.9f;site->retailSite->progress.hp=50;
        site->retailSite->type.maxHp=500;site->retailSite->type.inverseTime=0.01f;
        site->hp=Fixed::fromInt(50);
        sim::RetailReplayProbe::hit(w,blow(100),a,s);   // 85..115: hp -65..-35
        auto& pr=w.unit(s)->retailSite->progress;
        const int hp=w.unit(s)->hp.floorInt();
        check(hp<0,"the lethal hit takes the site's hp below zero");
        check(pr.remaining>1.0f && std::fabs(pr.remaining-float(1.0-double(hp)/500))<1e-6f,
              "...and its remaining work above 1 (signed hp word)");
        sim::RetailConstructionResources res;res.allocation=1;res.stored=1e6f;
        const auto r=sim::retailConstructionWork(pr,w.unit(s)->retailSite->type,res,1.0f);
        check(r==sim::RetailConstructionResult::Worked,"the next work call does not complete it");
        check(pr.hp==uint16_t(sim::retailFtol((1.0-double(pr.remaining))*500.0)),
              "...and stores _ftol's low word of the negative hp");
    }
    return failures?1:0;
}
