#include "sim/retailconstruction.h"
#include "sim/retailconstructionparticles.h"
#include "sim/sim.h"
#include <bit>
#include <cstdio>
#include <cstring>

using namespace tak::sim;
int main(int argc,char** argv) {
    if (argc==2 && std::strcmp(argv[1],"--build-reach")==0) {
        int32_t x,z,gx,gz;int fx,fz,sx,sz;unsigned reach;
        while (std::scanf("%d %d %d %d %d %d %d %d %u",&x,&z,&gx,&gz,&fx,&fz,&sx,&sz,&reach)==9)
            std::printf("%d\n",int(retailBuildWithinReach(Fixed::raw(x),Fixed::raw(z),Fixed::raw(gx),Fixed::raw(gz),
                int16_t(fx),int16_t(fz),int16_t(sx),int16_t(sz),uint16_t(reach))));
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--percent")==0) {
        float remaining;
        while (std::scanf("%f",&remaining)==1) std::printf("%u\n",retailConstructionPercent(remaining));
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--complete")==0) {
        RetailConstructionProgress site;RetailConstructionCompletion in;
        unsigned present,forced,same,allied,owner,attached,role,death;
        while (std::scanf("%f %u %u %u %u %u %u %u %u %u %u %u %u %u",&site.remaining,&site.flags,
                &present,&forced,&same,&allied,&in.builderFlags,&in.builderTypeFlags,&in.builderSecondaryFlags,
                &in.siteTypeFlags,&owner,&attached,&role,&death)==14) {
            in.builderPresent=present;in.forced=forced;in.sameUnit=same;in.allied=allied;
            in.ownerPresent=owner;in.attached=attached;in.ownerRole=uint8_t(role);in.deathType=uint8_t(death);
            const auto out=retailCompleteConstruction(site,in);
            std::printf("%u %u %u %u %u %u\n",std::bit_cast<uint32_t>(site.remaining),site.flags,
                unsigned(out.deathType),unsigned(out.detach),unsigned(out.activate),unsigned(out.notify));
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--get-built")==0) {
        unsigned tick,stage,events,wait,deadline;
        while (std::scanf("%u %u %u %u %u",&tick,&stage,&events,&wait,&deadline)==5) {
            RetailMissionState m;m.stage=uint8_t(stage);m.waitMask=wait;m.deadline=deadline;
            unsigned stop=0,decay=0;
            const int result=retailGetBuiltWaiting(m,tick,events,[&]{++stop;},[&]{++decay;});
            std::printf("%d %u %u %u %u %u\n",result,unsigned(m.stage),m.waitMask,m.deadline,stop,decay);
        }
        return 0;
    }
    // tools/re/check_site_decay.py: an abandoned site's GetBuilt under the
    // dispatcher, tick by tick. Header "ticks inverse-bits cost-bits remaining-bits
    // stored-bits start" then one "early late" pair per tick (work events set
    // before and after the site's dispatch). Per tick: remaining, stored, removed.
    if (argc==2 && std::strcmp(argv[1],"--site-timeline")==0) {
        unsigned ticks,inverseBits,costBits,remainingBits,storedBits,start;
        while (std::scanf("%u %u %u %u %u %u",&ticks,&inverseBits,&costBits,&remainingBits,&storedBits,&start)==6) {
            RetailMissionState m;
            float remaining=std::bit_cast<float>(remainingBits),stored=std::bit_cast<float>(storedBits);
            bool late=false,removed=false;
            const auto step=retailSiteDecay(std::bit_cast<float>(inverseBits),std::bit_cast<float>(costBits));
            for (unsigned t=0;t<ticks;++t) {
                unsigned early,after;
                if (std::scanf("%u %u",&early,&after)!=2) return 1;
                if (!removed && retailSiteTimer(m,start+t,early || late)) {
                    const double sum=double(remaining)+step.progress;
                    remaining=float(sum);
                    if (sum>=1) removed=true;
                    else stored=float(double(stored)+double(step.refund));
                }
                late=after!=0;
                std::printf(t ? " %u %u %u" : "%u %u %u",std::bit_cast<uint32_t>(remaining),
                            std::bit_cast<uint32_t>(stored),unsigned(removed));
            }
            std::putchar('\n');
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--sacred")==0) {
        int x,z,fx,fz,count;float income,prior;
        World world;
        while (std::scanf("%d %d %d %d %f %f %d",&x,&z,&fx,&fz,&income,&prior,&count)==7) {
            std::vector<SacredSite> sites;
            for (int i=0;i<count;++i) {
                SacredSite s;
                if (std::scanf("%d %d %d %d %f",&s.x,&s.z,&s.fx,&s.fz,&s.multiplier)!=5) return 1;
                sites.push_back(s);
            }
            world.setSacredSites(std::move(sites));
            const float value=float(double(prior)+double(income)*double(world.sacredIncomeMultiplier(x,z,fx,fz)));
            std::printf("%u\n",std::bit_cast<uint32_t>(value));
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--particles")==0) {
        unsigned capacity,count,rising,seed,steps;
        int radius,height,ownerHeight;
        while (std::scanf("%u %u %d %d %d %u %u %u",&capacity,&count,&radius,
                &height,&ownerHeight,&rising,&seed,&steps)==8) {
            RetailConstructionEmitter emitter{capacity,radius,height,{}};
            emitter.emit(count,ownerHeight,rising!=0,[&] {
                seed=seed*214013u+2531011u;return (seed>>16)&32767u;
            });
            for (unsigned i=0;i<steps;++i) emitter.advance();
            std::printf("%u %zu",seed,emitter.particles.size());
            for (const auto& p:emitter.particles)
                std::printf(" %d %d %d %d %d",p.x,p.y,p.z,p.speed,p.ceiling);
            std::putchar('\n');
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--economy")==0) {
        RetailConstructionResources r;
        float income,storage,demand;
        while (std::scanf("%f %f %f %lf %lf %f %f %f",&r.stored,
                &r.capacityOverride,&r.produced,&r.totalProduced,&r.excess,
                &income,&storage,&demand)==8) {
            r.beginTick(income,storage,demand);
            std::printf("%u %u %u %u %llu ",std::bit_cast<uint32_t>(r.stored),
                std::bit_cast<uint32_t>(r.capacity),std::bit_cast<uint32_t>(r.allocation),
                std::bit_cast<uint32_t>(r.produced),
                static_cast<unsigned long long>(std::bit_cast<uint64_t>(r.totalProduced)));
            r.finishTick();
            std::printf("%u %u %llu\n",std::bit_cast<uint32_t>(r.stored),
                std::bit_cast<uint32_t>(r.capacity),
                static_cast<unsigned long long>(std::bit_cast<uint64_t>(r.excess)));
        }
        return 0;
    }
    if (argc==2 && std::strcmp(argv[1],"--oracle")==0) {
        RetailConstructionProgress site;
        RetailConstructionType type;
        RetailConstructionResources resources;
        float work;unsigned hp;
        while (std::scanf("%f %u %u %u %f %f %u %f %f %f %f %lf %f",
            &site.remaining,&hp,&site.events,&site.flags,&type.inverseTime,&type.cost,&type.maxHp,
            &resources.stored,&resources.allocation,&resources.requested,&resources.produced,
            &resources.totalProduced,&work)==13) {
            site.hp=uint16_t(hp);
            const auto result=retailConstructionWork(site,type,resources,work);
            std::printf("%u %u %u %u %u %u %u %u %llu\n",unsigned(result),
                std::bit_cast<uint32_t>(site.remaining),unsigned(site.hp),site.events,site.flags,
                std::bit_cast<uint32_t>(resources.stored),std::bit_cast<uint32_t>(resources.requested),
                std::bit_cast<uint32_t>(resources.produced),
                static_cast<unsigned long long>(std::bit_cast<uint64_t>(resources.totalProduced)));
        }
        return 0;
    }
    // tools/re/check_self_repair.py: "hp carry healtime-bits inverse-bits maxdamage calls"
    // per unit; prints hp and carry after each call, gated as 429d90's entry is.
    if (argc==2 && std::strcmp(argv[1],"--self-repair")==0) {
        unsigned hp,carry,healBits,inverseBits,maxHp,calls;
        while (std::scanf("%u %u %u %u %u %u",&hp,&carry,&healBits,&inverseBits,&maxHp,&calls)==6) {
            uint16_t h=uint16_t(hp);uint8_t c=uint8_t(carry);
            for (unsigned i=0;i<calls;++i) {
                if (uint32_t(int32_t(int16_t(h)))<maxHp)
                    h=retailAddHitPoints(h,retailSelfRepairPoints(c,std::bit_cast<float>(healBits),
                        std::bit_cast<float>(inverseBits),maxHp),maxHp);
                std::printf(i ? " %u %u" : "%u %u",unsigned(h),unsigned(c));
            }
            std::putchar('\n');
        }
        return 0;
    }
    // Sixty seconds (225 calls) of self-repair from half health, as the emulated retail
    // path computes them: the Crusades Aramon Keep, an Aramon archer, and the Crusades
    // monarch whose healtime of 216.67 only makes sense against its buildtime of 100000.
    {
        struct Case { uint32_t maxHp; float buildTime,healTime; uint16_t from,to; };
        const Case cases[]={{25116,1622,3.17083f,12558,15503},{1100,125,0.520833333f,550,824},
                            {16000,100000,216.67f,8000,10079}};
        for (const Case& k:cases) {
            uint16_t h=k.from;uint8_t c=0;
            for (int i=0;i<225;++i)
                h=retailAddHitPoints(h,retailSelfRepairPoints(c,k.healTime,
                    retailInverseBuildTime(k.buildTime),k.maxHp),k.maxHp);
            if (h!=k.to) { std::printf("FAIL: self-repair %u -> %u, retail %u\n",k.from,h,k.to);return 1; }
        }
        if (retailInverseBuildTime(0)!=0.01f || retailInverseBuildTime(-5)!=0.01f) return 1;
        uint8_t c=0;
        if (retailAddHitPoints(100,retailSelfRepairPoints(c,600,0.01f,1000),1000)!=1000) return 1;
    }
    RetailConstructionProgress site;
    RetailConstructionResources resources;resources.stored=1;
    RetailConstructionType type{0.01f,100,1000};
    if (retailConstructionWork(site,type,resources,10)!=RetailConstructionResult::Worked ||
        resources.requested<9.9f || resources.stored!=0 || site.remaining<0.98f) return 1;
    const float remaining=site.remaining;
    resources.allocation=0;
    if (retailConstructionWork(site,type,resources,10)!=RetailConstructionResult::Idle || site.remaining!=remaining) return 1;
    RetailConstructionEmitter emitter{1,65536,65536,{}};
    unsigned calls=0;
    auto random=[&] { ++calls;return 16384u; };
    emitter.emit(2,0,false,random);
    if (calls!=2 || emitter.particles.size()!=1 || emitter.particles[0].speed!=-65536) return 1;
    emitter.advance(); // Reaching zero keeps the slot until the following update.
    if (emitter.particles[0].displayAge!=1) return 1;
    emitter.emit(1,0,false,random);
    if (calls!=2 || emitter.particles.size()!=1) return 1;
    emitter.advance();emitter.emit(1,0,false,random);
    if (calls!=4 || emitter.particles.size()!=1) return 1;
    if (emitter.particles[0].displayAge!=0) return 1;
    World world;
    world.setTerrain(std::vector<uint8_t>(32*32,100),32,32,20);
    world.setSacredSites({{10,10,2,2,1.5f}});
    UnitType lodestone;lodestone.id="lodestone";lodestone.maxHp=100;
    lodestone.footX=lodestone.footZ=2;lodestone.income=10;lodestone.storage=1000;
    lodestone.sacredIncome=true;
    const int id=world.spawn(&lodestone,176,176,0,0);
    world.tick(1.f/30);
    if (world.player(0).income!=15) return 1;
    RetailBuildCacheEntry restored;restored.type=&lodestone;
    restored.inputs.secondaryFlags=0x80000000u;
    restored.hasEconomy=true;restored.income=30;restored.storage=2000;
    world.player(0).buildCache.emplace().entries.push_back(restored);
    world.tick(1.f/30);
    if (world.player(0).income!=45 || world.player(0).storage!=2000) return 1;
    const auto hash=world.stateHash();
    world.player(0).buildCache->entries[0].income=20;
    if (world.stateHash()==hash) return 1;
    world.unit(id)->x=Fixed::fromInt(192);
    world.tick(1.f/30);
    if (world.player(0).income!=0) return 1;
    {
        // The site buckets answer what a scan of every site from the list's end
        // does: overlapping sites (the last listed wins a cell), zero and negative
        // multipliers, empty footprints, and queries off every edge.
        uint32_t seed=12345;
        auto rnd=[&](int n) { seed=seed*1103515245u+12345u;return int((seed>>16)%uint32_t(n)); };
        auto scan=[](const std::vector<SacredSite>& sites,int x,int z,int fx,int fz) {
            const SacredSite* last=nullptr;int covered=0;
            for (int dx=0;dx<fx;++dx) for (int dz=0;dz<fz;++dz)
                for (auto it=sites.rbegin();it!=sites.rend();++it) {
                    if (x+dx<it->x || x+dx>=it->x+it->fx || z+dz<it->z || z+dz>=it->z+it->fz) continue;
                    if (it->multiplier>0) { ++covered;last=&*it; }
                    break;
                }
            return last && covered>=last->fx*last->fz ? last->multiplier : 0.f;
        };
        for (int round=0;round<200;++round) {
            std::vector<SacredSite> sites(size_t(rnd(40)));
            for (auto& s:sites) {
                s.x=rnd(60)-10;s.z=rnd(60)-10;s.fx=rnd(5);s.fz=rnd(5);
                s.multiplier=float(rnd(4)-1)*0.5f;
            }
            World w;w.setSacredSites(sites);
            for (int q=0;q<200;++q) {
                const int x=rnd(70)-15,z=rnd(70)-15,fx=1+rnd(4),fz=1+rnd(4);
                if (w.sacredIncomeMultiplier(x,z,fx,fz)!=scan(sites,x,z,fx,fz)) return 1;
            }
        }
    }
    World construction;
    construction.setTerrain(std::vector<uint8_t>(32*32,100),32,32,20);
    UnitType builderType;builderType.id="builder";builderType.maxHp=100;
    builderType.income=31;builderType.storage=6100;
    UnitType siteType;siteType.id="site";siteType.maxHp=16516;
    const int builderId=construction.spawn(&builderType,100,100,0,0);
    const int siteId=construction.spawn(&siteType,200,200,0,0);
    auto* buildSite=construction.unit(siteId);buildSite->underConstruction=true;
    buildSite->retailSite=RetailConstructionSite{{0.3209267258644104f,11215,0,0},{0.0020040080416947603f,2116,16516},{},0};
    RetailConstructionJob job;job.target=siteId;job.workerTime=10;job.working=true;
    job.mission.stage=3;job.mission.deadline=0;
    construction.unit(builderId)->retailBuild=job;
    construction.unit(builderId)->buildSiteId=siteId;
    construction.unit(builderId)->constructionEmitter=RetailConstructionEmitter{1,65536,65536,{}};
    construction.unit(siteId)->constructionEmitter=RetailConstructionEmitter{1,65536,65536,{}};
    unsigned crtCalls=0;
    construction.setCrtRngObserver([&](const auto&){++crtCalls;});
    construction.player(0).retailResources.emplace();construction.player(0).mana=0;
    construction.tick(1.f/30);
    if (construction.unit(siteId)->retailSite->progress.remaining!=0.3204383850097656f ||
        construction.player(0).retailResources->allocation!=0.7310490608215332f ||
        construction.unit(builderId)->retailBuild->mission.stage!=3 || crtCalls!=4) return 1;
    construction.tick(1.f/30);
    if (construction.unit(siteId)->retailSite->progress.remaining!=0.31995004415512085f || crtCalls!=4) return 1;
    // A factory site with no affordable work stays alive at zero HP. Once
    // income arrives, it receives the same fractional allocation as builders.
    World factory;
    factory.setTerrain(std::vector<uint8_t>(32*32,100),32,32,20);
    UnitType producerType;producerType.id="factory";producerType.maxHp=100;
    producerType.maxVel=Fixed();
    producerType.workerTime=10;producerType.storage=100;
    UnitType outputType;outputType.id="output";outputType.maxHp=1000;
    outputType.buildTime=239;outputType.buildCost=1367;
    const int producerId=factory.spawn(&producerType,100,100,0,0);
    const int outputId=factory.spawn(&outputType,200,200,0,0);
    factory.unit(producerId)->productionSiteId=outputId;
    factory.unit(producerId)->buildQueue.push_back(&outputType);
    auto* output=factory.unit(outputId);output->underConstruction=true;output->hp=Fixed();
    output->retailSite=RetailConstructionSite{{1,0,0,0},{1.f/239,1367,1000},{},0};
    factory.player(0).retailResources.emplace();factory.player(0).mana=0;
    factory.tick(1.f/30);
    if (!factory.unit(outputId)->alive() || factory.unit(outputId)->hp!=Fixed() ||
        factory.unit(producerId)->productionSiteId!=outputId ||
        factory.unit(outputId)->retailSite->progress.remaining!=1) return 1;
    producerType.income=31;
    factory.tick(1.f/30);
    if (!factory.unit(outputId)->alive() || factory.unit(outputId)->retailSite->progress.remaining>=1 ||
        factory.player(0).retailResources->allocation>=1 || factory.player(0).mana!=0) return 1;
    // A finished ground output clears the factory target and immediately owns
    // a PARK mission with an annular exit goal, rather than a fixed point.
    outputType.maxVel=Fixed::fromInt(1);
    auto& factoryCatalogue=factory.player(0).buildCache.emplace();
    RetailBuildCacheEntry producerEntry;producerEntry.type=&producerType;producerEntry.inputs.flags=0x100u;
    RetailBuildCacheEntry outputEntry;outputEntry.type=&outputType;
    factoryCatalogue.entries={producerEntry,outputEntry};
    output=factory.unit(outputId);output->retailSite->progress.remaining=0.000001f;
    output->retailSite->progress.flags=0x1000000u;
    output->retailSite->mission.emplace();output->retailSite->builder=producerId;
    factory.player(0).mana=100;factory.tick(1.f/30);
    output=factory.unit(outputId);
    if (factory.unit(producerId)->productionSiteId || !factory.unit(producerId)->buildQueue.empty() ||
        output->underConstruction || output->retailSite->mission || output->orders.empty() ||
        !output->orders.back().park || !output->orders.back().park->ring ||
        output->orders.back().park->attempts!=1 || output->orders.back().mission.stage!=1 ||
        output->orders.back().mission.flags!=0x200u) return 1;
    outputType.maxHp=2000; // captured type stats take precedence over asset defaults
    factory.tick(1.f/30);
    if (factory.unit(outputId)->maximumHp()!=1000 || factory.unit(outputId)->hp!=Fixed::fromInt(1000)) return 1;
    // An abandoned site waits for its mission deadline. A new work event
    // postpones decay for thirty ticks, even if no progress was affordable.
    construction.unit(builderId)->constructionHolding=true;
    construction.unit(builderId)->standbyAllowed=false;
    construction.cancelBuilds(builderId);
    if (construction.unit(builderId)->retailBuild || construction.unit(builderId)->buildSiteId ||
        construction.unit(builderId)->constructionHolding || !construction.unit(builderId)->standbyAllowed) {
        std::puts("FAIL: cancelBuilds must retire the active retail construction job");return 1;
    }
    auto& abandoned=*construction.unit(siteId)->retailSite;
    abandoned.mission.emplace();abandoned.mission->stage=2;
    abandoned.mission->waitMask=0x10000001u;abandoned.mission->deadline=32;
    construction.unit(siteId)->missionEvents=0;
    const float abandonedRemaining=abandoned.progress.remaining;
    for (int i=0;i<29;++i) construction.tick(1.f/30);
    if (construction.unit(siteId)->retailSite->progress.remaining!=abandonedRemaining) return 1;
    construction.tick(1.f/30);
    const float decayed=construction.unit(siteId)->retailSite->progress.remaining;
    if (decayed<=abandonedRemaining || construction.unit(siteId)->retailSite->mission->deadline!=33) return 1;
    construction.unit(siteId)->missionEvents|=0x10000000u;
    construction.tick(1.f/30);
    if (construction.unit(siteId)->retailSite->progress.remaining!=decayed ||
        construction.unit(siteId)->retailSite->mission->deadline!=63 ||
        construction.unit(siteId)->missionEvents&0x10000000u) return 1;
    // Completing work retires both missions in slot order and releases the
    // builder. A finished site's zero HP must no longer receive site immunity.
    construction.unit(builderId)->retailBuild=job;
    construction.unit(builderId)->buildSiteId=siteId;
    construction.unit(builderId)->constructionHolding=true;
    abandoned.progress.remaining=0.000001f;abandoned.progress.flags=0x1000000u;
    abandoned.builder=builderId;
    construction.player(0).mana=100;
    auto& catalogue=construction.player(0).buildCache.emplace();
    RetailBuildCacheEntry builderEntry;builderEntry.type=&builderType;
    builderEntry.inputs.flags=0x100u;
    RetailBuildCacheEntry siteEntry;siteEntry.type=&siteType;
    catalogue.entries={builderEntry,siteEntry};
    construction.tick(1.f/30);
    if (construction.unit(builderId)->retailBuild || construction.unit(builderId)->buildSiteId ||
        construction.unit(builderId)->constructionHolding || construction.unit(siteId)->underConstruction ||
        construction.unit(siteId)->retailSite->mission ||
        construction.unit(siteId)->retailSite->progress.remaining!=0 ||
        construction.unit(siteId)->hp!=Fixed::fromInt(16516)) return 1;
    construction.unit(siteId)->hp=Fixed();
    construction.tick(1.f/30);
    if (construction.unit(siteId)->alive()) return 1;
    // A restored site whose decay takes its remaining work to 1 is removed by
    // 429af0 and dies (429cea: 51a140(site, site, 30000, 11, 0)), its owner
    // refunded nothing for that step.
    {
        World decay;
        decay.setTerrain(std::vector<uint8_t>(32*32,100),32,32,20);
        const int id=decay.spawn(&siteType,200,200,0,0);
        auto* site=decay.unit(id);site->underConstruction=true;
        site->retailSite=RetailConstructionSite{{0.9999f,1,0,0x1000000u},{0.0020040080416947603f,2116,16516},{},0};
        site->retailSite->mission.emplace();site->retailSite->mission->stage=2;
        site->retailSite->mission->waitMask=0x10000001u;site->retailSite->mission->deadline=0;
        decay.player(0).retailResources.emplace();decay.player(0).mana=0;
        decay.tick(1.f/30);
        site=decay.unit(id);
        if (!site || site->alive() || site->deathType!=11 || site->lastHitBy!=id ||
            decay.player(0).retailResources->stored!=0) {
            std::puts("FAIL: a restored site decayed to nothing must die of kind 11");return 1;
        }
    }
    std::puts("PASS: construction payment, completion, allocation and particle capacity/expiry");
}
