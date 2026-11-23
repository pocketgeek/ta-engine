#include "ai/ai.h"
#include "ai/builtinprofile.h"
#include "ai/classicbrain.h"
#include "ai/classicprofile.h"

#include <cstdlib>
#include <climits>

#include "hpi/hpi.h"
#include "sim/detmath.h"
#include "sim/footprint.h"
#include "sim/retailai.h"
#include "util/devenv.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace tak::ai {

Profile loadProfile(const tak::hpi::Vfs& vfs, const std::string& name) {
    Profile prof;
    // ai/<name>.txt, falling back to default.txt when the named one is absent.
    std::string path = "ai/" + name + ".txt";
    if (name.empty() || !vfs.has(path)) path = "ai/default.txt";
    if (!vfs.has(path)) return prof;
    auto b = vfs.read(path);
    applyProfileText(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()), prof);
    return prof;
}

DiffParams paramsFor(Difficulty d) {
    switch (d) {
        // Defensive: local interception without offensive waves, and production with
        // no AI-imposed limits -- every idle producer may act each think and spending
        // is held to income by the affordability gate and the low-mana throttle alone.
        // 256 bounds one think's orders for the wire frame, not the pace.
        case Difficulty::Defensive: return {30, 5, 256, 100, false, 0, true};
        // Defensive's behaviour, plus Absurd's 2x income cheat (incomeMultFor).
        case Difficulty::AbsurdlyDefensive: return {30, 5, 256, 100, false, 0, true};
        // Sluggish: reacts slowly, builds up slowly, and only commits once it has
        // gathered a sizeable group -- so it's passive and beatable. No raiding.
        case Difficulty::Easy:
        case Difficulty::Passive: return {90, 4, 1, 50,  true,  0, false};
        // Fast, army-heavy, and aggressive: reacts often, musters a large army before
        // the big push, harasses with raids meanwhile, and pushes bigger unit limits.
        case Difficulty::Hard:   return {20, 6, 8, 150, true,  4, false};
        // Hard's behaviour, plus a 2x income cheat applied to the sim (incomeMultFor).
        case Difficulty::Absurd: return {20, 6, 8, 150, true,  4, false};
        // Classic takes Normal's (unused) knobs: its ClassicBrain has none.
        case Difficulty::Normal:
        default:                 return {30, 5, 3, 100, true,  3, false};
    }
}

Controller::Controller(int player, const tak::sim::TypeRegistry& registry,
                       const Profile& profile, uint32_t seed, Difficulty difficulty,
                       std::vector<std::pair<float, float>> enemyStarts)
    : player_(player), registry_(registry), profile_(profile),
      // The seed is taken as-is: the CALLER hands distinct seeds to distinct players
      // when it wants them to diverge from tick one (the lobby does, per game+player).
      // Same-seed controllers still diverge within a few ticks, because every RNG draw
      // is gated on that player's own unit counts / income / producers, which differ
      // by map position. The AI runs server-side only, so this RNG exists just to make
      // a --mpai run repeatable, not for lockstep.
      rng_(seed), diff_(difficulty == Difficulty::Passive ? Difficulty::Easy : difficulty),
      stayHome_(difficulty == Difficulty::Passive), dp_(paramsFor(difficulty)),
      enemyStarts_(std::move(enemyStarts)) {
    if (diff_ == Difficulty::Classic) classic_ = std::make_unique<ClassicBrain>(player, seed, registry);
    for (const auto& [x, z] : enemyStarts_) starts_.push_back({x, z, -1});
}

std::vector<StartSpot> enemyStartSpots(int slot, const std::vector<std::pair<float, float>>& slotPos,
                                       const std::vector<uint8_t>& seated,
                                       const std::function<bool(int, int)>& allied,
                                       const std::vector<std::pair<float, float>>& randomTable) {
    std::vector<StartSpot> out;
    auto isSeated = [&](int j) { return size_t(j) < seated.size() && seated[size_t(j)]; };
    if (randomTable.empty()) {
        for (int j = 0; j < int(slotPos.size()); ++j) {
            if (j == slot || !isSeated(j) || allied(slot, j)) continue;
            if (slotPos[size_t(j)].first == 0.0f && slotPos[size_t(j)].second == 0.0f) continue;
            out.push_back({slotPos[size_t(j)].first, slotPos[size_t(j)].second, j});
        }
        return out;
    }
    for (const auto& spot : randomTable) {
        bool known = false;   // ours or an ally's
        for (int j = 0; j < int(slotPos.size()) && !known; ++j)
            known = isSeated(j) && (j == slot || allied(slot, j)) && slotPos[size_t(j)] == spot;
        if (!known) out.push_back({spot.first, spot.second, -1});
    }
    return out;
}

void Controller::setStartSpots(std::vector<StartSpot> spots) {
    starts_ = std::move(spots);
    enemyStarts_.clear();
    for (const auto& s : starts_) enemyStarts_.emplace_back(s.x, s.z);
    startsSized_ = false;   // updateKnowledge sizes the start state afresh
}

void Controller::setClassicProfile(const ClassicProfileScript& script) {
    if (classic_) classic_->useProfile(script);
}

void Controller::setClassicStart(int index, float x, float z) {
    if (classic_) classic_->setStart(index, x, z);
}

void Controller::setClassicStarts(const std::vector<std::pair<float, float>>& positions) {
    if (classic_) classic_->setStartTable(positions);
}

void Controller::setClassicCampaign(const std::string& otaPlayer) {
    if (!classic_) return;
    classic_->setScenarioMode(1);
    classic_->setMassAttack(classicMassAttackTicks(otaPlayer));
}

Controller::Controller(Controller&&) noexcept = default;
Controller::~Controller() = default;

void Controller::emit(const CommandSink& sink, tak::net::Cmd kind, int unitId,
                      const std::string& type, float x, float z) const {
    tak::net::Command c;
    c.kind = kind;
    c.player = uint8_t(player_);
    c.unitId = unitId;
    c.x = x;
    c.z = z;
    std::snprintf(c.type, sizeof c.type, "%s", type.c_str());
    sink(c);
}

// What is this unit FOR? Derived purely from its stats, so it works for every faction:
//   Economy  - a structure that makes/holds mana (lodestone, mana storage)
//   Factory  - a structure that trains units (keep, castle, hell), OR a mobile
//              production creature (Zhon's Beast Handlers/Tamers/Lords -- Zhon has
//              NO static factories, its creatures ARE the production line)
//   Defense  - any other structure (towers, walls)
//   Builder  - a mobile unit that builds/expands (Monarch, mason, priest, arabuild)
//   Army     - any other mobile unit (the combatants)
BuildCat Controller::categoryOf(const tak::sim::UnitType* t) const {
    if (t->isStructure()) {
        if (!registry_.buildable(t->id).empty()) return BuildCat::Factory;
        if (t->income > 0 || t->storage > 0) return BuildCat::Economy;
        return BuildCat::Defense;
    }
    if (t->isBuilder) {
        // A mobile builder whose menu is DOMINATED by mobile combat units is really a
        // factory (Zhon's tamers train armies), not an economy/expansion builder. A
        // mobile CONSTRUCTOR (arabuild: mostly buildings) or a Monarch (the base
        // builder) stays a Builder. Without this, every Zhon producer landed in the
        // builder bucket -- capped at 2-3 -- so the Zhon AI built one Beast Handler
        // and then stalled with nothing left it wanted to make.
        if (!t->commander) {
            int combat = 0, structs = 0;
            for (const auto& id : registry_.buildable(t->id)) {
                const auto* b = registry_.find(id);
                if (!b) continue;
                if (b->isStructure()) ++structs;
                else if (!b->isBuilder) ++combat;
            }
            if (combat > structs && combat > 0) return BuildCat::Factory;
        }
        return BuildCat::Builder;
    }
    return BuildCat::Army;
}

// A builder=1 unit that walks: the Monarch, constructors, priests, Zhon's producers.
static bool mobileBuilder(const tak::sim::UnitType* t) {
    return t && !t->isStructure() && t->isBuilder;
}

// A building that pays mana: a lodestone, a Divine one, a sacred-site building.
static bool incomeBuilding(const tak::sim::UnitType* t) {
    return t && t->isStructure() && t->income > 0;
}

bool Controller::mobileProduction(const std::string& side) const {
    if (side.empty()) return false;
    auto [it,inserted]=mobileProduction_.try_emplace(side,true);
    if (inserted)
        for (const auto& [id,type]:registry_.types())
            if (type.side==side && type.isStructure() && !registry_.buildable(id).empty()) {
                it->second=false;
                break;
            }
    return it->second;
}

bool Controller::navalFactory(const tak::sim::UnitType* type) const {
    if (!type || !type->isStructure()) return false;
    bool ships = false;
    for (const auto& id : registry_.buildable(type->id)) {
        const auto* child = registry_.find(id);
        if (!child || child->isStructure()) continue;
        if (child->canFly || child->domain != tak::sim::UnitType::Domain::Water) return false;
        ships = true;
    }
    return ships;
}

bool Controller::navalProducer(const tak::sim::UnitType* type) const {
    if (!type) return false;
    for (const auto& id:registry_.buildable(type->id)) {
        const auto* child=registry_.find(id);
        const auto weight=profile_.weight.find(id),limit=profile_.limit.find(id);
        if (child && !child->isStructure() && !child->canFly && !child->isBuilder &&
            child->domain==tak::sim::UnitType::Domain::Water && child->weapon.damage>0 &&
            weight!=profile_.weight.end() && weight->second>0 &&
            (limit==profile_.limit.end() || limit->second!=0)) return true;
    }
    return false;
}

const std::vector<std::pair<float,float>>& Controller::navalLaunchOffsets(const tak::sim::UnitType* type) const {
    auto [it,inserted] = navalOffsets_.try_emplace(type);
    if (!inserted || !type->productionScript) return it->second;
    const tak::sim::UnitType* ship = nullptr;
    for (const auto& id:registry_.buildable(type->id)) {
        const auto* child=registry_.find(id);
        if (child && !child->isStructure() && child->domain==tak::sim::UnitType::Domain::Water) {ship=child;break;}
    }
    if (!ship) return it->second;
    // QueryBuildInfo can change script statics/pieces. Evaluate the authored
    // output in private worlds, never on the referee whose state is replicated.
    // Cache both query phases and birth orientations once per shipyard type.
    const int samples=type->buildAngle ? 16 : 1;
    for (int angle=0; angle<samples; ++angle) {
        tak::sim::World preview;
        const auto heading=tak::sim::bamWrap(int(type->orientation)-int(type->buildAngle)/2+
            (samples>1 ? angle*(int(type->buildAngle)-1)/(samples-1) : 0));
        const int id=preview.spawn(type,2000,2000,tak::sim::radiansFromBam(heading),0);
        preview.tick(1.f/30.f); // Create counter-rotates the yard where the script requests it.
        for (int phase=0; phase<2; ++phase) {
            tak::sim::Fixed x,y,z;
            preview.productionPosition(id,ship,x,y,z);
            it->second.emplace_back(x.toFloat()-2000,z.toFloat()-2000);
        }
    }
    std::sort(it->second.begin(),it->second.end());
    it->second.erase(std::unique(it->second.begin(),it->second.end()),it->second.end());
    return it->second;
}

// Whether the water a ship launched at (x,z) can move in holds at least
// `cells` positions it fits in (a bounded 4-neighbour flood of its nav grid).
// A yard needs a sea to send its fleet into, not a pond.
bool Controller::navalBodyAtLeast(const tak::sim::World& world,const tak::sim::UnitType* ship,
                                  float x,float z,int cells) const {
    const auto& grid=world.navFor(ship);
    const int foot=std::clamp(std::max(ship->footX,ship->footZ),1,15);
    const int sx=tak::sim::footprintCell(x,foot),sz=tak::sim::footprintCell(z,foot);
    if (grid.empty() || !grid.fits(sx,sz,foot)) return false;
    std::vector<std::pair<int,int>> queue{{sx,sz}};
    std::set<std::pair<int,int>> seen{{sx,sz}};
    for (size_t head=0;head<queue.size() && int(queue.size())<cells;++head) {
        const auto [cx,cz]=queue[head];
        for (auto [nx,nz]:{std::pair{cx-1,cz},std::pair{cx+1,cz},std::pair{cx,cz-1},std::pair{cx,cz+1}})
            if (grid.fits(nx,nz,foot) && seen.insert({nx,nz}).second) queue.emplace_back(nx,nz);
    }
    return int(queue.size())>=cells;
}

bool Controller::navalLaunchClear(const tak::sim::World& world,const tak::sim::UnitType* factory,
                                  const tak::sim::UnitType* ship,float x,float z,float dx,float dz) const {
    const auto& grid=world.navFor(ship);
    const int ox=tak::sim::footprintOrigin(x,factory->footX),oz=tak::sim::footprintOrigin(z,factory->footZ);
    const int sx=tak::sim::footprintOrigin(x+dx,ship->footX),sz=tak::sim::footprintOrigin(z+dz,ship->footZ);
    // The pad alone can fit while the mouth of the yard opens onto shallow
    // water. Check a short route out of the planned open yard, including its
    // solid cells. This is an AI site test; it never changes the live nav grid.
    const int x0=std::min(ox-ship->footX-2,sx),z0=std::min(oz-ship->footZ-2,sz);
    const int x1=std::max(ox+factory->footX+2,sx+1),z1=std::max(oz+factory->footZ+2,sz+1);
    const int width=x1-x0,height=z1-z0;
    std::vector<uint8_t> visited(size_t(width)*height,0);
    std::vector<std::pair<int,int>> queue{{sx,sz}};
    visited[size_t(sz-z0)*width+sx-x0]=1;
    for (size_t head=0;head<queue.size();++head) {
        const auto [cx,cz]=queue[head];
        bool fits=true;
        for (int j=0;j<ship->footZ && fits;++j) for (int i=0;i<ship->footX && fits;++i) {
            const int gx=cx+i,gz=cz+j;
            fits=grid.walkable(gx,gz);
            if (gx>=ox && gz>=oz && gx<ox+factory->footX && gz<oz+factory->footZ && !factory->yardMap.empty()) {
                const char cell=factory->yardMap[size_t(gz-oz)*factory->footX+gx-ox];
                fits=fits && (cell=='.' || cell==' ' || cell=='c' || cell=='C' || cell=='y' || cell=='Y');
            }
        }
        if (!fits) continue;
        if (cx+ship->footX<=ox || cx>=ox+factory->footX ||
            cz+ship->footZ<=oz || cz>=oz+factory->footZ) return true;
        for (auto [nx,nz]:{std::pair{cx-1,cz},std::pair{cx+1,cz},std::pair{cx,cz-1},std::pair{cx,cz+1}}) {
            if (nx<x0 || nz<z0 || nx>=x1 || nz>=z1) continue;
            auto& seen=visited[size_t(nz-z0)*width+nx-x0];
            if (!seen) {seen=1;queue.emplace_back(nx,nz);}
        }
    }
    return false;
}

int Controller::navalDoorReach(const tak::sim::UnitType* factory,const tak::sim::UnitType* ship) {
    // PARK's inner radius (retailParkRadius) at its least padding, (0+0+3)*16.
    const uint64_t diagonal=uint64_t(factory->footX*factory->footX+factory->footZ*factory->footZ);
    const int inner=ship->footX*16+48+int(tak::sim::isqrt64(diagonal*256)/2);
    const int beyond=inner-std::min(factory->footX,factory->footZ)*8-std::max(ship->footX,ship->footZ)*8;
    return std::max(2,(beyond+15)/16);
}

int Controller::navalDoorSteps(const tak::sim::World& world,const tak::sim::UnitType* factory,
                               const tak::sim::UnitType* ship,float x,float z,float dx,float dz) const {
    const int ox=tak::sim::footprintOrigin(x,factory->footX),oz=tak::sim::footprintOrigin(z,factory->footZ);
    const int sx=tak::sim::footprintOrigin(x+dx,ship->footX),sz=tak::sim::footprintOrigin(z+dz,ship->footZ);
    const auto key=std::tuple{factory,ship,ox,oz,sx,sz};
    if (const auto it=navalDoorSteps_.find(key); it!=navalDoorSteps_.end()) return it->second;
    const auto& grid=world.navFor(ship);
    const int reach=navalDoorReach(factory,ship);
    // Moves from the pad to the yard's edge, by the shortest side; the budget
    // allows a few more than the straight run out.
    const int door=std::max(0,std::min({oz+factory->footZ-sz,sz+ship->footZ-oz,
                                        ox+factory->footX-sx,sx+ship->footX-ox}));
    const int budget=reach+door+4;
    const int margin=reach+std::max(ship->footX,ship->footZ)+2;
    const int x0=std::min(ox-margin,sx),z0=std::min(oz-margin,sz);
    const int x1=std::max(ox+factory->footX+margin,sx+1),z1=std::max(oz+factory->footZ+margin,sz+1);
    const int width=x1-x0,height=z1-z0;
    std::vector<int16_t> depth(size_t(width)*height,-1);
    std::vector<std::pair<int,int>> queue{{sx,sz}};
    depth[size_t(sz-z0)*width+sx-x0]=0;
    int found=-1;
    for (size_t head=0;head<queue.size() && found<0;++head) {
        const auto [cx,cz]=queue[head];
        const int steps=depth[size_t(cz-z0)*width+cx-x0];
        bool fits=true;
        for (int j=0;j<ship->footZ && fits;++j) for (int i=0;i<ship->footX && fits;++i) {
            const int gx=cx+i,gz=cz+j;
            fits=grid.walkable(gx,gz);
            if (gx>=ox && gz>=oz && gx<ox+factory->footX && gz<oz+factory->footZ && !factory->yardMap.empty()) {
                const char cell=factory->yardMap[size_t(gz-oz)*factory->footX+gx-ox];
                fits=fits && (cell=='.' || cell==' ' || cell=='c' || cell=='C' || cell=='y' || cell=='Y');
            }
        }
        if (!fits) continue;
        const int clear=std::max({ox-(cx+ship->footX),cx-(ox+factory->footX),
                                  oz-(cz+ship->footZ),cz-(oz+factory->footZ)});
        if (clear>=reach) {found=std::max(0,steps-door-reach);break;}
        if (steps>=budget) continue;
        for (auto [nx,nz]:{std::pair{cx-1,cz},std::pair{cx+1,cz},std::pair{cx,cz-1},std::pair{cx,cz+1}}) {
            if (nx<x0 || nz<z0 || nx>=x1 || nz>=z1) continue;
            auto& seen=depth[size_t(nz-z0)*width+nx-x0];
            if (seen<0) {seen=int16_t(steps+1);queue.emplace_back(nx,nz);}
        }
    }
    navalDoorSteps_.emplace(key,found);
    return found;
}

Controller::Rect Controller::footprintRect(const tak::sim::UnitType* type,float x,float z) {
    const float x0=float(tak::sim::footprintOrigin(x,type->footX)*16);
    const float z0=float(tak::sim::footprintOrigin(z,type->footZ)*16);
    return {x0,z0,x0+float(type->footX*16),z0+float(type->footZ*16)};
}

const std::vector<Controller::Rect>& Controller::doorStrips(const tak::sim::UnitType* type) const {
    auto [it,inserted]=doorStrips_.try_emplace(type);
    const int fx=type->footX,fz=type->footZ;
    // Naval yards open onto water (placeSite ranks their doors); Zhon's mobile
    // producers have none. A yardmap without a door edge has no strip.
    if (!inserted || !type->isStructure() || registry_.buildable(type->id).empty() || navalFactory(type) ||
        fx<1 || fz<1 || type->yardMap.size()!=size_t(fx)*fz) return it->second;
    // Deep enough for the largest ground output beside one builder, 3x3 plus 2x2
    // at the least (80 px: a fire wagon passes a mechanic at work) and retail's
    // block-grid street (7 cells, 112 px) at the most.
    int output=0,builder=2;
    for (const auto& id:registry_.buildable(type->id)) {
        const auto* child=registry_.find(id);
        if (!child || child->isStructure() || child->canFly) continue;
        output=std::max({output,int(child->footX),int(child->footZ)});
        if (child->isBuilder) builder=std::max({builder,int(child->footX),int(child->footZ)});
    }
    // Outputs that all fly (or none at all) leave no ground traffic at a door.
    if (output==0) return it->second;
    const float depth=float(std::clamp((output+builder)*16,80,112));
    // A named reference: lambdas may not capture the structured binding `it`
    // (C++20 allows it, Apple clang rejects it).
    auto& strips=it->second;
    auto open=[&](int i,int j){const char c=type->yardMap[size_t(j)*fx+i];return c=='c' || c=='C';};
    // One edge: `n` cells along it, at(k) whether cell k is open, strip(a,b) the
    // strip for the span a..b along it. A corner cell alone opens no door (a yard
    // whose bottom row is open reaches both side columns there); the span runs
    // over every open cell of a door edge.
    auto edge=[&](int n,auto&& at,auto&& strip) {
        int lo=n,hi=-1;bool inner=false;
        for (int k=0;k<n;++k) if (at(k)) {lo=std::min(lo,k);hi=k;inner=inner || (k>0 && k<n-1) || n<3;}
        if (inner) strips.push_back(strip(float((lo-1)*16),float((hi+2)*16)));
    };
    edge(fx,[&](int k){return open(k,0);},[&](float a,float b){return Rect{a,-depth,b,0};});
    edge(fx,[&](int k){return open(k,fz-1);},[&](float a,float b){return Rect{a,float(fz*16),b,float(fz*16)+depth};});
    edge(fz,[&](int k){return open(0,k);},[&](float a,float b){return Rect{-depth,a,0,b};});
    edge(fz,[&](int k){return open(fx-1,k);},[&](float a,float b){return Rect{float(fx*16),a,float(fx*16)+depth,b};});
    return it->second;
}

void Controller::addDoorStrips(const tak::sim::UnitType* type,float x,float z,std::vector<Rect>& out) const {
    const auto origin=footprintRect(type,x,z);
    for (const auto& s:doorStrips(type))
        out.push_back({origin.x0+s.x0,origin.z0+s.z0,origin.x0+s.x1,origin.z0+s.z1});
}

Controller::BaseLayout Controller::baseLayout(const tak::sim::World& world) const {
    BaseLayout layout;
    for (const auto& u:world.units()) {
        if (!u.alive() || u.player!=player_ || !u.type || !u.type->isStructure()) continue;
        const float x=u.x.toFloat(),z=u.z.toFloat();
        layout.rects.push_back(footprintRect(u.type,x,z));
        addDoorStrips(u.type,x,z,layout.doors);
    }
    return layout;
}

// Normal's, Hard's and Absurd's income per factory (assessNeeds).
static constexpr float kIncomePerFactory = 60;

// The levels whose production is sized to what it spends (assessNeeds): one
// factory per kIncomePerFactory, one more while the store fills, and Zhon's
// producers counted the same way.
static bool spendSizedProduction(Difficulty d) {
    return d == Difficulty::Normal || d == Difficulty::Hard || d == Difficulty::Absurd;
}

// How many of a Normal, Hard or Absurd Zhon's `producers` may be away building
// lodestones at a time: a quarter, two at least.
static int producersOut(int producers) { return std::max(2, producers / 4); }

// Count the empire by category and set the targets the planner steers toward.
Needs Controller::assessNeeds(const tak::sim::World& world) const {
    Needs n;
    const auto& me = world.player(player_);
    n.income = me.income;   // spend the income actually available
    std::string side = side_;   // the seat's faction; not known: the Monarch's, else a mobile builder's
    for (const auto& u : world.units()) {
        if (!u.alive() || u.player != player_ || !u.type) continue;
        if (side_.empty() && !u.type->isStructure() && u.type->isBuilder && (side.empty() || u.type->commander))
            side = u.type->side;
        if (u.type->isBuilder) {
            const auto* site = u.buildSiteId ? world.unit(u.buildSiteId) : nullptr;
            bool economyProject = site && site->underConstruction && site->type &&
                                  categoryOf(site->type) == BuildCat::Economy;
            bool lodestoneProject = site && site->underConstruction && site->type && site->type->onMana;
            // Approach waypoints precede the actual build order. Count the queued
            // destination too, so distant expansions do not enlist every producer.
            for (const auto& order : u.orders)
                if (order.buildType) {
                    economyProject = economyProject || categoryOf(order.buildType) == BuildCat::Economy;
                    lodestoneProject = lodestoneProject || order.buildType->onMana;
                }
            if (economyProject) ++n.economyProjects;
            if (lodestoneProject) ++n.lodestoneProjects;
        }
        ++n.population;
        ++n.counts[u.type];
        if (incomeBuilding(u.type)) ++n.lodestones;
        if (u.underConstruction) ++n.unfinished[u.type];
        if (u.type->isStructure() && !tak::sim::World::lodestoneUpgradeBase(*u.type).empty()) {
            ++n.upgrades;
            if (u.underConstruction) ++n.upgradeProjects;
        }
        if (upgrader(u.type)) ++n.priests;
        if (navalFactory(u.type)) ++n.navalFactories;
        if (navalProducer(u.type)) ++n.navalProducers;
        if (!u.type->isStructure() && !u.type->canFly && !u.type->isBuilder &&
            u.type->domain==tak::sim::UnitType::Domain::Water) ++n.navalArmy;
        switch (categoryOf(u.type)) {
            case BuildCat::Economy:  ++n.economy;   break;
            case BuildCat::Factory:  ++n.factories; break;
            case BuildCat::Builder:  ++n.builders;  break;
            case BuildCat::Army:     ++n.army;      break;
            case BuildCat::Defense:  ++n.defenses; break;
        }
        if (mobileBuilder(u.type)) ++n.mobileBuilders;
        // A unit ordered but not yet begun is no unit yet: a factory's queue, a
        // builder walking to its site or conjuring spot. It will take a slot
        // under the unit cap all the same, so it counts as population -- the
        // sim refuses a site at the cap only when the builder gets there, and
        // 873 such orders were walked to and dropped in one long game. Ours,
        // not retail, whose planners order freely at the cap. A builder is
        // counted by type as well, or every think hands the next idle producer
        // one more builder.
        auto planned = [&](const tak::sim::UnitType* t) {
            ++n.population;
            if (t->isStructure() && !tak::sim::World::lodestoneUpgradeBase(*t).empty()) {
                ++n.upgrades;++n.upgradeProjects;
            } else if (incomeBuilding(t)) ++n.lodestones;
            if (upgrader(t)) ++n.priests;
            if (!mobileBuilder(t)) return;
            ++n.counts[t];++n.mobileBuilders;
            if (categoryOf(t) == BuildCat::Factory) ++n.factories; else ++n.builders;
        };
        for (size_t i = 0; i < u.buildQueue.size(); ++i)
            if (i > 0 || !u.productionSiteId) planned(u.buildQueue[i]);
        const auto* bound = u.buildSiteId ? world.unit(u.buildSiteId) : nullptr;
        for (size_t i = 0; i < u.orders.size(); ++i)
            if (u.orders[i].buildType && (i > 0 || !bound || bound->type != u.orders[i].buildType))
                planned(u.orders[i].buildType);
    }
    // One factory per ~40 income so production can actually spend what we earn
    // (Easy; the defensive levels have their own rule below).
    // Normal, Hard and Absurd: one per kIncomePerFactory, and one more than they
    // have while the store is over 60% full (below); Normal at most 8, Hard and
    // Absurd 32. A working factory spends 30 to 120 mana a second (build cost x
    // worker time / build time: 30-45 for a keep's troops, 60-80 for a
    // castle's); at Hard's old one per 25 income the factories asked for half as
    // much again as came in, the store sat empty, and the over-commit throttle
    // (tick) stopped every new order -- the next lodestone too -- in 61-95% of
    // thinks from minute 3 of a game on. Normal's one per 40 did the same more
    // slowly: four of the eight Normal sides of the audited room sat under the
    // throttle in 71-97% of thinks from minute 5 to 20. Normal keeps its own
    // builder cap, army size, pace and profile limits; only the factory rule is
    // Hard's.
    // Builders count the Monarch. Normal grows to the Monarch plus four as income
    // allows: retail's ai/default.txt limits each builder type, not the total --
    // arabuild, arafly and arapries 4 each, cremech and crechie 4, tarpries,
    // verlihr and verliege 3 -- and has no difficulty levels, so four is the most
    // any one type may field. Those per-type limits still apply in weightedPick.
    // Easy keeps the Monarch plus one; Hard/Absurd run hotter; the defensive levels
    // scale with income without a ceiling (below).
    n.desiredFactories = std::clamp(int(n.income / 40.0f) + 1, 1, 8);
    n.desiredArmy = std::clamp(12 + int(n.income * 1.5f), 12, 180);
    const bool aggressive = diff_ == Difficulty::Hard || diff_ == Difficulty::Absurd;
    n.builderCap = aggressive ? std::clamp(4 + int(n.income / 60), 4, 12)
                 : diff_ == Difficulty::Normal ? std::clamp(2 + int(n.income / 50), 2, 5) : 2;
    n.desiredDefenses = std::clamp(1 + int(n.income / 50), 1, 6);
    if (aggressive || defensiveLevel(diff_)) {
        // Income limits production speed, not the eventual size of the force.
        n.desiredArmy = world.unitCap() > 0 ? world.unitCap() : INT_MAX;
    }
    if (spendSizedProduction(diff_)) {
        const int most = aggressive ? 32 : 8;
        n.desiredFactories = std::clamp(1 + int(n.income / kIncomePerFactory), 1, most);
        // A store filling up: production is not spending what comes in. One more
        // factory than it has. Of Zhon's producers that is one more than those
        // its income feeds: its allowance (below) is those and a quarter of them
        // more for the ones away at lodestones (producersOut), so of the producers
        // it has four in five count here, and a quarter of those as away. Taking a
        // quarter of all it has as away would add none: of 12 producers 9 would
        // count, and one more with its 2 away are the 12 it has.
        if (me.storage > 0 && me.mana > me.storage * 0.6f) {
            const int have = mobileProduction(side) ? n.factories - producersOut(n.factories * 4 / 5) : n.factories;
            n.desiredFactories = std::min(std::max(n.desiredFactories, have) + 1, most);
        }
    }
    if (dp_.unbounded) {
        // The defensive levels keep building in step with income, with no ceiling on
        // any target: one factory per 25 income, a builder per 50 income past the
        // Monarch and one, and defenses with income and the army -- and one more
        // factory and tower whenever the store passes 60%. Only game rules
        // bound it: the unit cap, FBI limits, build permissions and placement.
        n.desiredFactories = 1 + int(n.income / 25);
        n.builderCap = 2 + int(n.income / 50);
        n.desiredDefenses = 2 + int(n.income / 40) + n.army / 12;
        if (me.storage > 0 && me.mana > me.storage * 0.6f) {
            // Production is not keeping up (or its output has nowhere to stand): one
            // more factory, and a tower, which needs no room to send units out.
            n.desiredFactories = std::max(n.desiredFactories + 1, n.factories + 1);
            n.desiredDefenses = std::max(n.desiredDefenses, n.defenses + 1);
        }
    }
    if (diff_ == Difficulty::Easy) {
        n.desiredFactories = std::min(n.desiredFactories, 2);
        n.desiredArmy = std::clamp(8 + int(n.income * 0.5f), 8, 36);
        n.desiredDefenses = std::min(n.desiredDefenses, 2);
    }
    // Zhon has no factory buildings: its Beast Handlers, Tamers, Lords and Shamans
    // (builder=1, limit 3 each in ai/default.txt) train its army AND build its
    // lodestones, and they count as factories here. Their allowance is the
    // factories another side would have plus that side's builders past the
    // Monarch, so Zhon is not held to the producers of a side that has both.
    // Every builder=1 mobile unit counts against the cap: Aramon's and Taros's
    // priests are factories by their menus, but builders all the same. Zhon's
    // cap is its producers' allowance plus the Monarch.
    // At Normal, Hard and Absurd a producer conjures troops all the time it is
    // not away at a lodestone, and spends as a factory does. With another side's
    // whole builder allowance (up to 11) on top of its factories, Zhon had twice
    // the producers its income could keep conjuring, and sat under the
    // over-commit throttle like the factory sides (85-88% of thinks in minutes
    // 5-10 at Hard). Its producers are the factories its income feeds plus those
    // it may send to lodestones at a time (producersOut): Normal's most is
    // 8 + 2 = 10, under the four types' retail limits together (12).
    const int builders = spendSizedProduction(diff_) ? producersOut(n.desiredFactories) : n.builderCap - 1;
    n.mobileBuilderCap = n.builderCap;
    if (mobileProduction(side)) {
        n.mobileBuilderCap = 1 + n.desiredFactories + builders;
        n.desiredFactories += builders;
    }
    const float capacity = me.manaCapacity();
    n.storeFull = capacity > 0.01f && float(me.mana) >= capacity * 0.9f;
    return n;
}

// Priority of building one more of a category, given the empire's needs. Higher wins;
// 0 means "have enough, don't". The ladder: guarantee production, floor the economy,
// keep a few builders, grow economy/factories to match income, then army as the sink.
int Controller::desire(BuildCat c, const Needs& n) const {
    switch (c) {
        case BuildCat::Economy:
            // Bootstrap income BEFORE the pricey first factory -- building a 1700-mana
            // keep out of the opening treasury with no income starves everything after.
            if (n.income < 20.0f) return 95;
            if (diff_ == Difficulty::Hard || diff_ == Difficulty::Absurd) {
                // Their ladder never met an income target, so they bought
                // lodestones beside a full store until those filled the unit
                // cap (1,676 of one side's 2,000 units, its army 914 -> 186).
                // A full store: income already goes unspent, and what holds
                // production back is not mana.
                if (n.storeFull) return 0;
                return n.income < 25.0f + 20.0f * n.factories ? 80 : 60;
            }
            // Normal's next factory needs kIncomePerFactory more income (assessNeeds):
            // its lodestones grow to that, or a side whose income stood between the
            // two targets wanted neither -- a Taros side sat on one factory and 58
            // income from minute 6 to 20 with three builders idle.
            if (n.income < (spendSizedProduction(diff_) ?
                    std::max(25.0f + 20.0f * n.factories, kIncomePerFactory * n.factories) :
                    25.0f + 20.0f * n.factories)) return 60;
            // A full store means the income target is not what holds production
            // back. Retail's builders draw from their whole menu, the lodestone
            // among it (weight 10, limit 7 in ai/default.txt); ours stood idle
            // instead: a Taros AI on "4x4 team" kept its Dark Masons idle
            // beside a full store on one lodestone for ten minutes. Below the
            // army, so a producer that can train one still does. Easy stays lean.
            return n.storeFull && diff_ != Difficulty::Easy ? 45 : 0;
        case BuildCat::Factory:
            if (n.factories == 0) return 90;                        // then: some production
            return n.factories < n.desiredFactories ? 70 : 0;       // scale with income
        case BuildCat::Builder:
            if ((diff_==Difficulty::Hard || diff_==Difficulty::Absurd) &&
                n.factories>0 && n.income>=20 && n.builders<n.builderCap) return 88;
            return n.builders < n.builderCap ? 65 : 0;              // a handful, then stop
        case BuildCat::Army:
            return n.army < n.desiredArmy ? 50 : 0;
        case BuildCat::Defense:
            return n.defenses < n.desiredDefenses ?
                (n.army >= 4 ? 55 : 30) : 0;
    }
    return 0;
}

// Retail builds a lodestone when it runs short of mana (icd 0x410810, ported as
// sim::retailBuildPriority): the store under 30% full, mana requested outpacing
// income over the last second (401330 - 401350: requests, not what was paid),
// and none of that lodestone unfinished. Its constructor planner then draws
// among the menu's lodestones separately (0x412d00) and builds that instead of
// the ordinary pick wherever 0x412f80 finds a free deposit in the base radius (retailLodestoneRadius) -- unless a builder of the base is already on
// one (0x40c830). Every level adds that trigger to its own ladder: Normal and the
// tiers sharing its economy, whose income target alone held them to one lodestone
// where a deposit pays 100 a second and the store ran dry; and Hard and Absurd,
// whose over-commit throttle (tick) otherwise stops the lodestone that would end
// the shortage along with everything else. Theirs goes wherever their own
// expansion reaches (weightedPick), not only inside retail's base radius.
bool Controller::manaShort(const tak::sim::World& world, const Needs& needs) const {
    if (!world.hasManaSpots() || needs.lodestoneProjects > 0) return false;
    const auto& me = world.player(player_);
    const float capacity = me.manaCapacity();
    return (capacity > 0.01f ? float(me.mana) / capacity : 0.0f) < 0.3f &&
        me.displayResources.shortfall(float(me.mana));
}

const tak::sim::UnitType* Controller::divineIn(const tak::sim::UnitType* type) const {
    auto [it,inserted]=divineIn_.try_emplace(type,nullptr);
    if (inserted && type)
        for (const auto& id:registry_.buildable(type->id)) {
            const auto* child=registry_.find(id);
            if (child && child->isStructure() && !tak::sim::World::lodestoneUpgradeBase(*child).empty()) {
                it->second=child;
                break;
            }
        }
    return it->second;
}

bool Controller::upgrader(const tak::sim::UnitType* type) const {
    return mobileBuilder(type) && divineIn(type);
}

bool Controller::armedEnemyNear(const tak::sim::World& world, float x, float z, float r) const {
    for (const auto& e:world.units()) {
        if (!e.alive() || !e.type || e.embarked() || e.underConstruction || e.cloaked ||
            world.isNeutralPlayer(e.player) || world.allied(player_,e.player)) continue;
        if (e.type->weapon.damage<=0 &&
            std::none_of(e.type->weapons.begin(),e.type->weapons.end(),[](const auto& w) {
                return w.damage>0 || w.mindControl;
            })) continue;
        const float dx=e.x.toFloat()-x,dz=e.z.toFloat()-z;
        if (dx*dx+dz*dz<=r*r && sees(world,e)) return true;
    }
    return false;
}

// Divine Lodestone upgrades. The store, not the income, is what a level runs
// short of: every level spends as it earns and holds next to nothing, and a
// Divine Lodestone costs 20 to 30 plain ones (its priest as much again) while
// the plain lodestone under it stops paying the moment the upgrade starts. So
// the plan upgrades only a saturated base -- no free deposit within kBase of home
// that a plain lodestone could take, which pays back in seconds -- upgrades only
// what pays back within the level's horizon, one at a time, and saves the next
// purchase's reserve first, holding every other new build back until the store
// has it. Candidates: own finished lodestones at full HP within kBase of home,
// no spotted armed enemy within kSafe, the richest deposit first, then the
// nearest. A threat in home territory stops the saving at once; a stage not
// bought within kSaveTicks gives up for kRetryTicks.
void Controller::planUpgrade(const tak::sim::World& world, Needs& needs) {
    constexpr float kBase = 1600, kSafe = 600, kTerritory = 900;
    constexpr uint32_t kSaveTicks = 3 * 60 * 30, kRetryTicks = 5 * 60 * 30;
    const UpgradeLevel level = upgradeLevelFor(diff_);
    const int was = upgrade_.stage;
    upgrade_.target = 0;
    upgrade_.divine = upgrade_.priest = nullptr;
    upgrade_.stage = 0;
    if (level.limit == 0 || !world.hasManaSpots() || now_ < uint32_t(level.earliestMin) * 60u * 30u ||
        now_ < upgrade_.retryAt || needs.upgradeProjects > 0 ||
        (level.limit > 0 && needs.upgrades >= level.limit)) return;
    const auto home = homeOf(world);
    if (armedEnemyNear(world, home.first, home.second, kTerritory)) return;
    auto near = [&](float x, float z, float r) {
        const float dx = x - home.first, dz = z - home.second;
        return dx * dx + dz * dz <= r * r;
    };
    // Our plain lodestones and what each upgrades to.
    std::unordered_map<std::string, const tak::sim::UnitType*> upgrades;
    for (const auto& [id, type] : registry_.types())
        if (type.isStructure())
            if (auto base = tak::sim::World::lodestoneUpgradeBase(type); !base.empty())
                if (const auto* plain = registry_.find(base); plain && plain->side == type.side)
                    upgrades.emplace(base, &type);
    const tak::sim::UnitType* plainType = nullptr;
    struct Candidate { const tak::sim::Unit* lode; const tak::sim::UnitType* divine; float x, z, mult, d2; };
    std::vector<Candidate> candidates;
    const auto& deposits = world.manaSpots();
    for (const auto& u : world.units()) {
        if (!u.alive() || u.player != player_ || !u.type || u.underConstruction) continue;
        const auto up = upgrades.find(u.type->id);
        if (up == upgrades.end()) continue;
        plainType = u.type;
        const float ux = u.x.toFloat(), uz = u.z.toFloat();
        if (!near(ux, uz, kBase) || u.hp < tak::sim::Fixed::fromInt(u.maximumHp())) continue;
        const std::pair<float, float>* spot = nullptr;
        float best = 24.0f * 24.0f;
        for (const auto& d : deposits) {
            const float dx = d.first - ux, dz = d.second - uz;
            if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; spot = &d; }
        }
        if (!spot || armedEnemyNear(world, ux, uz, kSafe) ||
            !world.canPlace(up->second, spot->first, spot->second, player_)) continue;
        const float hx = spot->first - home.first, hz = spot->second - home.second;
        candidates.push_back({&u, up->second, spot->first, spot->second, 0, hx * hx + hz * hz});
    }
    if (candidates.empty()) return;
    // Saturated: no deposit near home is free for a plain lodestone -- none
    // standing on it, no builder of ours or an ally's bound for it, no ally's.
    std::vector<std::pair<float, float>> reserved;
    for (const auto& u : world.units()) {
        if (!u.alive() || !world.allied(player_, u.player)) continue;
        for (const auto& order : u.orders)
            if (order.buildType && order.buildType->onMana && (order.buildX.v || order.buildZ.v))
                reserved.emplace_back(order.buildX.toFloat(), order.buildZ.toFloat());
    }
    for (size_t i = 0; i < deposits.size(); ++i) {
        const float sx = deposits[i].first, sz = deposits[i].second;
        if (!near(sx, sz, kBase) || (i < allyHeldDeposits_.size() && allyHeldDeposits_[i]) ||
            (i < depositTaken_.size() && depositTaken_[i]) || !knowsGround(world, sx, sz) ||
            std::any_of(reserved.begin(), reserved.end(), [&](const auto& r) {
                const float dx = r.first - sx, dz = r.second - sz; return dx * dx + dz * dz < 32 * 32;
            })) continue;
        if (world.canPlace(plainType, sx, sz, player_)) return;
    }
    // Who builds it: a priest we have (or have ordered), else one an existing
    // factory can train -- no factory is built for this alone.
    auto mult = [&](const tak::sim::UnitType* type, float x, float z) {
        if (!type->sacredIncome) return 1.0f;
        return world.sacredIncomeMultiplier(tak::sim::footprintOrigin(x, type->footX),
                                            tak::sim::footprintOrigin(z, type->footZ), type->footX, type->footZ);
    };
    const auto weighted = [&](const tak::sim::UnitType* type) {
        const auto w = profile_.weight.find(type->id);
        const auto l = profile_.limit.find(type->id);
        return w != profile_.weight.end() && w->second > 0 && (l == profile_.limit.end() || l->second != 0) &&
               world.buildAllowed(type);
    };
    for (auto& c : candidates) c.mult = mult(c.divine, c.x, c.z);
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.mult != b.mult ? a.mult > b.mult : a.d2 < b.d2;
    });
    const float incomeMult = incomeMultFor(diff_);
    for (const auto& c : candidates) {
        if (!weighted(c.divine)) continue;
        // The per-type limits weightedPick keeps (the FBI's, and a bounded
        // level's scaled profile limit) bound the plan too: a reserve saved for a
        // Divine Lodestone weightedPick then refuses would never be spent.
        {
            const auto l = profile_.limit.find(c.divine->id);
            const auto count = needs.counts.find(c.divine);
            const int have = count == needs.counts.end() ? 0 : count->second;
            if ((c.divine->totalAllowed > 0 && have >= c.divine->totalAllowed) ||
                (!dp_.unbounded && l != profile_.limit.end() && l->second > 0 &&
                 have >= std::max(1, l->second * dp_.limitScale / 100))) continue;
        }
        // The priest: one standing (finished or not) or ordered, else the type an
        // existing factory trains.
        const tak::sim::UnitType* priest = nullptr;
        bool ready = false;
        for (const auto& u : world.units())
            if (u.alive() && u.player == player_ && u.type && upgrader(u.type) && divineIn(u.type) == c.divine) {
                priest = u.type;
                ready = ready || !u.underConstruction;
            }
        const bool train = needs.priests == 0;
        if (train) {
            for (const auto& u : world.units()) {
                if (priest || !u.alive() || u.player != player_ || !u.type || u.underConstruction) continue;
                for (const auto& id : registry_.buildable(u.type->id)) {
                    const auto* t = registry_.find(id);
                    if (t && upgrader(t) && divineIn(t) == c.divine && weighted(t)) { priest = t; break; }
                }
            }
            if (!priest) return;
        }
        if (!priest) {   // ordered, not yet begun: the trainee's type
            for (const auto& u : world.units())
                if (u.alive() && u.player == player_)
                    for (const auto* t : u.buildQueue)
                        if (t && upgrader(t) && divineIn(t) == c.divine) priest = t;
            if (!priest) continue;
        }
        const float oldIncome = c.lode->type->income * mult(c.lode->type, c.lode->x.toFloat(), c.lode->z.toFloat()) *
                                incomeMult;
        const float extra = c.divine->income * c.mult * incomeMult - oldIncome;
        if (extra <= 0) continue;
        const float secs = c.divine->buildTime / std::max(priest->workerTime, 1.0f);
        const float cost = c.divine->buildCost + oldIncome * secs + (train ? priest->buildCost : 0.0f);
        if (cost / extra > float(level.horizonMin) * 60.0f) continue;
        upgrade_.target = c.lode->id;
        upgrade_.x = c.x;
        upgrade_.z = c.z;
        upgrade_.divine = c.divine;
        upgrade_.priest = train ? priest : nullptr;
        upgrade_.stage = train ? 1 : ready ? 3 : 2;
        // The save clock runs from the start of each stage, and stands still while
        // the priest is being trained.
        if (upgrade_.stage != was || upgrade_.stage == 2) upgrade_.saveStart = now_;
        // The reserve: the level's fraction of the purchase, and for the Divine
        // Lodestone at least what income over its build time leaves uncovered --
        // weightedPick's affordability gate asks savings plus that income to cover
        // the cost, so a smaller reserve met would buy nothing. A store too small
        // to hold it saves what it can hold, and plans nothing where even that
        // cannot buy the upgrade.
        const auto& me = world.player(player_);
        const float capacity = me.manaCapacity() * 0.9f;
        if (train) {
            needs.reserve = std::min(level.saveFraction * priest->buildCost, capacity);
        } else {
            const float uncovered = c.divine->buildCost - me.income * secs;
            needs.reserve = std::min(std::max(level.saveFraction * c.divine->buildCost, uncovered), capacity);
            if (needs.reserve < uncovered) {
                needs.reserve = 0;
                upgrade_.target = 0;
                upgrade_.divine = upgrade_.priest = nullptr;
                upgrade_.stage = 0;
                return;
            }
        }
        // A stage not bought within kSaveTicks gives up -- the reserve not met, or
        // met with nothing to spend it (the priest busy or unable to reach the
        // site, the unit cap reached) -- rather than hold the store for ever.
        // Stage 2's clock restarts every think while the priest trains.
        if (now_ - upgrade_.saveStart > kSaveTicks) {
            upgrade_.retryAt = now_ + kRetryTicks;
            upgrade_.target = 0;
            upgrade_.divine = upgrade_.priest = nullptr;
            upgrade_.stage = 0;
            needs.reserve = 0;
        }
        return;
    }
}

// The base radius retail searches for that lodestone's deposit (0x412f80 ->
// 0x411510, radius 0x40cf30): 16 * max(5b, b*sqrt(n)/2), n the summed footprint
// (footX + footZ) of the base's buildings and b 7 for the Monarch, 10 for any
// other builder (0x40cfc0) -- 560 and 800 px until n passes 100. Our own
// ladder reaches 1600 from home: with the trigger firing nearly every think on a
// one-deposit-per-player map, a Normal builder walked 1250 px into an enemy's
// base for a freed home deposit and was lost there with the lodestone.
float Controller::retailLodestoneRadius(const tak::sim::World& world,
                                        const tak::sim::Unit& builder) const {
    int32_t footprint = 0;
    for (const auto& u : world.units())
        if (u.alive() && u.player == player_ && u.type && u.type->isStructure())
            footprint += u.type->footX + u.type->footZ;
    return float(tak::sim::retailAiBaseRadius(footprint, builder.type->commander ? 2 : 1));
}

// Pick what a producer should build: the most-needed category its menu can supply
// (desire()), then a weighted-random draw WITHIN that category (profile weights, for
// variety + retail flavour). A candidate must be affordable to finish and under its
// difficulty-scaled limit. Returns nullptr if nothing worth building is affordable now.
const tak::sim::UnitType* Controller::weightedPick(const tak::sim::World& world,
                                                   const tak::sim::Unit& producer,
                                                   const Needs& needs, int excludeCats,
                                                   bool lodestoneOnly, float* siteLimit) {
    if (siteLimit) *siteLimit = 0;
    const auto& menu = registry_.buildable(producer.type->id);
    const auto& me = world.player(player_);
    float income = me.income;   // include the Absurd income bonus
    // A menu entry the AI may build right now: has a positive weight, is under its
    // limit, and savings + income over its build time cover the cost (so a builder
    // never traps itself on a site the mana runs dry beneath).
    const bool aggressive = diff_ == Difficulty::Hard || diff_ == Difficulty::Absurd;
    const bool sustained = aggressive || defensiveLevel(diff_);
    // Orders already given this think draw on the same store (unbounded levels,
    // whose every idle producer may act in one think).
    const double mana = me.mana - (dp_.unbounded ? double(needs.committed) : 0.0);
    // Zhon's mobile producers are its whole production line; a priest is not.
    const bool mobileFactory = !producer.type->isStructure() &&
                               categoryOf(producer.type) == BuildCat::Factory &&
                               mobileProduction(producer.type->side);
    // A Divine Lodestone is built only where planUpgrade names its lodestone, and
    // the priest it needs only when it asks for one; until the store holds the
    // plan's reserve, nothing else is started but the short-of-mana lodestone.
    auto divine = [](const tak::sim::UnitType* type) {
        return type->isStructure() && !tak::sim::World::lodestoneUpgradeBase(*type).empty();
    };
    auto planPick = [&](const tak::sim::UnitType* type) {
        return (upgrade_.stage == 3 && upgrade_.target && type == upgrade_.divine) ||
               (upgrade_.stage == 1 && type == upgrade_.priest);
    };
    if (needs.reserve > 0 && mana < double(needs.reserve)) lodestoneOnly = true;
    std::unordered_map<const tak::sim::UnitType*,bool> coastalProduction;
    auto coastalProducer = [&](const tak::sim::UnitType* type) {
        auto [it,inserted]=coastalProduction.try_emplace(type,false);
        if (!inserted) return it->second;
        if (navalFactory(type)) return it->second=true; // terrainWeight checks its launch
        auto future=producer;future.type=type;
        for (const auto& id:registry_.buildable(type->id)) {
            const auto* ship=registry_.find(id);
            const auto weight=profile_.weight.find(id),limit=profile_.limit.find(id);
            if (!ship || ship->isStructure() || ship->canFly || ship->isBuilder ||
                ship->domain!=tak::sim::UnitType::Domain::Water || !world.buildAllowed(ship) ||
                weight==profile_.weight.end() || weight->second<=0 ||
                (limit!=profile_.limit.end() && limit->second==0)) continue;
            if (terrainWeight(world,future,ship,1)>0) return it->second=true;
        }
        return false;
    };
    // The trigger's lodestone replaces the ordinary pick only where retail would
    // build it: a free deposit within the base radius of home. Otherwise the
    // ordinary ladder, with its own reach, decides as before. Hard's and Absurd's
    // bases are not retail's: theirs goes on any deposit their reach allows (0).
    const bool lodestoneTrigger = manaShort(world, needs) && !producer.type->isStructure() &&
                                  producer.type->isBuilder;
    const float triggerRadius = lodestoneTrigger && !aggressive ? retailLodestoneRadius(world, producer) : 0.0f;
    std::unordered_map<const tak::sim::UnitType*,bool> triggerSites;
    auto triggered = [&](const tak::sim::UnitType* type) {
        if (!lodestoneTrigger || !type->onMana || !type->isStructure() || divine(type) ||
            needs.unfinished.count(type)) return false;
        auto [it,inserted] = triggerSites.try_emplace(type,false);
        if (inserted) {
            float ox = producer.x.toFloat(), oz = producer.z.toFloat(), x, z;
            if (producer.type->commander) { auto h = homeOf(world); ox = h.first; oz = h.second; }
            it->second = placeSite(world, type, producer, ox, oz, x, z, triggerRadius);
        }
        return it->second;
    };
    auto priority = [&](const tak::sim::UnitType* type) {
        if (triggered(type)) return 96;
        if (lodestoneOnly) return 0;
        // The upgrade outranks the Dragon (army, 50); its priest comes just
        // below more factories (70).
        if (divine(type)) return planPick(type) ? 62 : 0;
        if (planPick(type)) return 66;
        const auto cat = categoryOf(type);
        int value = desire(cat, needs);
        const bool navalBudget=needs.income>0 || me.mana>=type->buildCost;
        // Land production must not fill the whole factory budget before the AI
        // establishes its first usable shipyard. Terrain feasibility is checked below.
        if (navalFactory(type) && needs.navalFactories == 0 && needs.factories > 0 &&
            navalBudget) value = std::max(value, 70);
        // Aramon conjures ships directly; Zhon needs a higher-tier conjurer.
        // Reserve a route into naval production even after land factories fill
        // the ordinary budget, but only where its water units can be used.
        if (needs.navalProducers==0 && needs.factories>0 && navalBudget &&
            navalProducer(type) && coastalProducer(type)) value=std::max(value,70);
        if (cat==BuildCat::Army && !type->canFly &&
            type->domain==tak::sim::UnitType::Domain::Water && navalBudget &&
            needs.navalArmy<4) value=std::max(value,70);
        if (mobileFactory && needs.income >= 20 && value > 0) {
            // Zhon's factories can ALSO build economy. Keep them producing an
            // opening force instead of sending every handler off to a lodestone.
            // Hard and Absurd once kept half of them out on lodestones, anywhere on
            // the map: a side lost 1,099 producers in 165 minutes, its army pinned
            // at three per producer beside a full store. A quarter now, on safe
            // deposits on their own side (placeSite), none beside a full store.
            if (cat == BuildCat::Army && needs.army < std::max(4, needs.factories * 3)) return 85;
            if (cat == BuildCat::Economy && needs.economyProjects >=
                (aggressive ? producersOut(needs.factories) : std::max(1,needs.factories/3))) return 40;
        }
        return value;
    };
    std::unordered_map<const tak::sim::UnitType*,int> terrainWeights;
    auto usable = [&](const tak::sim::UnitType* ut) -> int {
        if (!world.buildAllowed(ut) || (world.unitCap() > 0 && needs.population >= world.unitCap())) return 0;
        auto count = needs.counts.find(ut);
        const int have = count == needs.counts.end() ? 0 : count->second;
        if (ut->totalAllowed > 0 && have >= ut->totalAllowed) return 0;
        auto wi = profile_.weight.find(ut->id);
        int w = wi == profile_.weight.end() ? 0 : wi->second;
        if (w <= 0) return 0;
        // Some coastal maps have no mana deposits. Sacred lodestones there
        // add storage but no income; unlimited expansion must not keep buying
        // them while the navy and the rest of the army receive nothing.
        if (ut->onMana && ut->sacredIncome && !world.hasManaSpots()) return 0;
        if (categoryOf(ut)==BuildCat::Economy && ut->income<=0 && needs.income<20) return 0;
        // A lodestone takes a unit-cap slot like any unit, and Hard and Absurd
        // lift its profile limit (below). With a cap set they hold at most a
        // quarter of it in income buildings, and start none in the last
        // twentieth of it but the short-of-mana one, so that a slot freed at the
        // cap goes to a fighter. Ours, not retail (a limit of 7 per type there).
        if (aggressive && world.unitCap() > 0 && incomeBuilding(ut) && !divine(ut)) {
            const int cap = world.unitCap();
            if (needs.lodestones >= std::max(1, cap / 4) ||
                (needs.population >= cap - cap / 20 && !triggered(ut))) return 0;
        }
        auto li = profile_.limit.find(ut->id);
        int lim = li == profile_.limit.end() ? -1 : li->second;
        if (lim == 0) return 0;
        if (const auto r = refused_.find(producer.id);
            r != refused_.end() && r->second.type == ut && now_ < r->second.since) return 0;
        // The difficulty's builder cap, the Monarch included (see assessNeeds);
        // the one priest an upgrade asks for is outside it.
        if (mobileBuilder(ut) && needs.mobileBuilders >= needs.mobileBuilderCap && !planPick(ut)) return 0;
        // A priest starts a Dragon (or any of its troops) only on mana the
        // store is actually floating: half its cost, or a 90% full store.
        if (upgrader(producer.type) && categoryOf(ut) == BuildCat::Army &&
            mana < std::min(double(ut->buildCost) * 0.5, double(me.manaCapacity()) * 0.9)) return 0;
        // The unbounded levels take no per-type limit from the profile; a disabled
        // entry (limit 0, above) stays disabled and FBI limits still apply.
        if (lim > 0 && !dp_.unbounded) {
            int limit = std::max(1, lim * dp_.limitScale / 100);
            const auto cat = categoryOf(ut);
            // Profile limits pace the old small armies. Keep disabled entries
            // disabled and enforce actual gameplay limits, but let these modes
            // grow ordinary troops, defenses and expanding income beyond them.
            // A builder type never passes its own retail limit (ai/default.txt
            // has no difficulty levels): Hard's 150% made four arabuilds six.
            // Zhon's producers are the exception: with no factory buildings they
            // take the factory raise, and only its whole allowance bounds them
            // (mobileBuilderCap, the factories plus the builder cap).
            if (mobileBuilder(ut)) {
                limit = std::min(limit, lim);
                if (sustained && cat == BuildCat::Factory && mobileProduction(ut->side))
                    limit = std::max(limit, needs.desiredFactories);
            } else {
                // Only the troops the army routines command (sendWaves' pool:
                // a primary weapon that does damage). An unarmed scout or a
                // transport keeps its profile limit: 310 Spyhawks and 71 Rocs
                // were built in one game and never given an order.
                if (sustained && cat == BuildCat::Army && ut->weapon.damage > 0)
                    limit = std::max(limit, needs.desiredArmy);
                if (sustained && cat == BuildCat::Factory) limit = std::max(limit, needs.desiredFactories);
                if (sustained && cat == BuildCat::Defense) limit = std::max(limit, needs.desiredDefenses);
            }
            if (aggressive && cat == BuildCat::Economy && ut->income > 0 && !divine(ut)) limit = INT_MAX;
            if (have >= limit) return 0;
        }
        if (ut->buildTime > 0) {
            float secs = ut->buildTime / std::max(producer.type->workerTime, 1.0f);
            // Continuous cheap land production can consume every spare mana
            // forever. Commit one first naval producer/ship using income;
            // the existing low-mana construction throttle then lets it finish.
            // Count unfinished/planned units so this never starts a whole fleet
            // of unaffordable projects at once.
            const bool firstNavy=income>0 &&
                ((needs.navalProducers==0 && needs.factories>0 && navalProducer(ut)) ||
                 (needs.navalArmy==0 && !ut->isStructure() && !ut->canFly && !ut->isBuilder &&
                  ut->domain==tak::sim::UnitType::Domain::Water));
            // Every pick but the plan's own and the short-of-mana lodestone
            // leaves the plan's reserve in the store.
            const double avail = planPick(ut) || triggered(ut) ? mana : mana - double(needs.reserve);
            if (!firstNavy && avail + income * secs < ut->buildCost) return 0;
        }
        auto [it,inserted]=terrainWeights.try_emplace(ut,0);
        if (inserted) it->second=terrainWeight(world,producer,ut,w);
        return it->second;
    };
    // Pass 1: the highest desire among categories this producer can actually build now.
    int best = 0;
    for (const auto& id : menu) {
        const auto* ut = registry_.find(id);
        if (!ut || (excludeCats & (1 << int(categoryOf(ut))))) continue;
        const int value=priority(ut);
        if (value<=0 || usable(ut)<=0) continue;
        best = std::max(best, value);
    }
    // TAK_AI_PICK: why a producer chose nothing. A stalled economy is almost always
    // "every menu entry scored 0", and this says which gate did it.
    static const bool kPickLog = tak::devEnv("TAK_AI_PICK") != nullptr;
    if (kPickLog && best <= 0) {
        std::fprintf(stderr, "    pick %s: nothing usable (mana=%.0f income=%.0f)\n",
                     producer.type->id.c_str(), world.player(player_).mana, income);
        for (const auto& id : menu) {
            const auto* ut = registry_.find(id);
            if (!ut) { std::fprintf(stderr, "      %-9s MISSING from registry\n", id.c_str()); continue; }
            auto wi = profile_.weight.find(ut->id);
            const int w = wi == profile_.weight.end() ? -1 : wi->second;
            auto ci = needs.counts.find(ut);
            const int have = ci == needs.counts.end() ? 0 : ci->second;
            auto li = profile_.limit.find(ut->id);
            const int lim = li == profile_.limit.end() ? -1 : li->second;
            const float secs = ut->buildTime / std::max(producer.type->workerTime, 1.0f);
            std::fprintf(stderr, "      %-9s w=%d lim=%d have=%d cost=%.0f btime=%.0f "
                                 "afford=%s cat=%d usable=%d\n",
                         ut->id.c_str(), w, lim, have, ut->buildCost, ut->buildTime,
                         (world.player(player_).mana + income * secs >= ut->buildCost) ? "Y" : "N",
                         int(categoryOf(ut)), usable(ut));
        }
    }
    if (best <= 0) return nullptr;   // nothing needed is affordable -> wait (no spiral)
    // Pass 2: weighted-random among the usable entries in that top category.
    const tak::sim::UnitType* chosen = nullptr;
    int total = 0;
    for (const auto& id : menu) {
        const auto* ut = registry_.find(id);
        if (!ut || (excludeCats & (1 << int(categoryOf(ut)))) || priority(ut)!=best) continue;
        int w = usable(ut);
        if (w <= 0) continue;
        total += w;
        if (rand(total) < w) chosen = ut;   // reservoir sample
    }
    if (siteLimit && chosen && triggered(chosen)) *siteLimit = triggerRadius;
    return chosen;
}

// These are read-only terrain queries. Orders still go through the normal
// command stream and the existing retail movement controllers.
bool Controller::reachablePoint(const tak::sim::World& world, const tak::sim::UnitType* type,
                                 float fx,float fz,float tx,float tz,float& x,float& z,float pad) const {
    if (type->canFly) { x=tx;z=tz;return true; }
    const auto& nav=world.navFor(type);
    const int foot=std::clamp(std::max(type->footX,type->footZ),1,15);
    auto reachable=[&](float px,float pz) {
        return nav.fits(tak::sim::footprintCell(px,foot),tak::sim::footprintCell(pz,foot),foot) &&
            world.pathExists(type,px,pz,fx,fz);
    };
    if (reachable(tx,tz)) { x=tx;z=tz;return true; }
    // A ship can bombard a shore it cannot occupy. Likewise, approach the edge
    // of an occupied building rather than treating its blocked centre as a wall.
    const float range=type->weapon.melee ? 16.0f : std::max(16.0f,float(type->weapon.range)*0.8f);
    for (float r : {pad+16.0f,pad+range*0.5f,pad+range})
        for (int i=0;i<8;++i) {
            const float a=float(i)*0.785398163f;
            const float px=tx+detmath::cos(a)*r,pz=tz+detmath::sin(a)*r;
            if (reachable(px,pz)) { x=px;z=pz;return true; }
        }
    if (type->domain == tak::sim::UnitType::Domain::Water) {
        // An inland starting position is a strategic destination, not a firing
        // target. Project it onto the enemy's NEAREST coast so ships can contest
        // the sea even when they cannot bombard the Monarch from there: the first
        // ring holding water this ship fits in, or the ring after it, must be
        // connected to the ship. Accepting any connected point out to 1536 px let
        // a landlocked pond 1536 px from an enemy pass as "reaches the enemy", and
        // a Navy Yard went into it (0.9.0 report; generated map, Creon yard at
        // (1800,5272)). This is our levels' rule only: retail's AI site search
        // (classicplacement.h, 0x412f80/0x411200) tests no reachability at all.
        int firstWet = 0;
        for (int ring = 1; ring <= 6 && (!firstWet || ring <= firstWet + 1); ++ring) {
            const float r = 256.0f * float(ring);
            for (int i = 0; i < 16; ++i) {
                const float a = float(i) * 0.392699082f;
                const float px = tx + detmath::cos(a) * r, pz = tz + detmath::sin(a) * r;
                if (!nav.fits(tak::sim::footprintCell(px,foot),tak::sim::footprintCell(pz,foot),foot)) continue;
                if (!firstWet) firstWet = ring;
                if (world.pathExists(type,px,pz,fx,fz)) { x=px;z=pz;return true; }
            }
        }
    }
    return false;
}

int Controller::terrainWeight(const tak::sim::World& world,const tak::sim::Unit& producer,
                               const tak::sim::UnitType* type,int weight) const {
    if (type->isStructure()) {
        if (!navalFactory(type)) return weight;
        float nx = producer.x.toFloat(), nz = producer.z.toFloat();
        if (producer.type->commander) { auto home=homeOf(world);nx=home.first;nz=home.second; }
        float x, z;
        if (!placeSite(world,type,producer,nx,nz,x,z)) return 0;
        auto yard = producer; yard.type=type;
        yard.x = tak::sim::Fixed::fromFloat(x); yard.z = tak::sim::Fixed::fromFloat(z);
        for (const auto& id : registry_.buildable(type->id)) {
            const auto* ship = registry_.find(id);
            const auto wi = profile_.weight.find(id), li = profile_.limit.find(id);
            if (ship && !ship->isBuilder && world.buildAllowed(ship) &&
                wi != profile_.weight.end() && wi->second > 0 &&
                (li == profile_.limit.end() || li->second != 0) && terrainWeight(world,yard,ship,weight) > 0)
                return weight;
        }
        return 0;
    }
    float px=producer.x.toFloat(),pz=producer.z.toFloat();
    bool launch=false;
    if (!producer.type->isStructure() && type->domain==tak::sim::UnitType::Domain::Water && !type->canFly) {
        // Mobile conjurers can walk to a coast. Looking only beside their
        // current inland position silently excluded Aramon and Zhon navies.
        if (!placeSite(world,type,producer,px,pz,px,pz)) return 0;
        launch=true;
    }
    if (navalFactory(producer.type)) {
        const auto& outputs=navalLaunchOffsets(producer.type);
        launch=!outputs.empty();
        for (const auto& offset:outputs) {
            const float x=producer.x.toFloat()+offset.first,z=producer.z.toFloat()+offset.second;
            launch=launch && navalLaunchClear(world,producer.type,type,producer.x.toFloat(),producer.z.toFloat(),offset.first,offset.second);
            px=x;pz=z;
        }
        if (!launch && producer.type->productionScript) return 0;
    }
    for (int r=48;r<=304 && !launch;r+=64)
        for (int i=0;i<8;++i) {
            const float a=float(i)*0.785398163f;
            const float x=producer.x.toFloat()+detmath::cos(a)*r;
            const float z=producer.z.toFloat()+detmath::sin(a)*r;
            if (world.canPlace(type,x,z)) { px=x;pz=z;launch=true;break; }
        }
    if (!launch) return 0;
    if (categoryOf(type)!=BuildCat::Army || enemyStarts_.empty()) return weight;
    float x,z;
    for (const auto& [tx,tz] : enemyStarts_)
        if (reachablePoint(world,type,px,pz,tx,tz,x,z)) return weight;
    // Keep a smaller home defense on isolated land; don't fill a landlocked
    // lake with ships that cannot threaten any enemy shore.
    return type->domain==tak::sim::UnitType::Domain::Water ? 0 : std::max(1,weight/5);
}

// Turn a pick into a command: a factory (keep/castle) trains a mobile unit; a
// mobile builder places a structure or conjures a mobile unit near itself. The
// placement spot is probed against the (const) world, then issued as a Build.
bool Controller::produce(const tak::sim::World& world, const tak::sim::Unit& p,
                         const tak::sim::UnitType* pick, const CommandSink& sink, float siteLimit) {
    if (pick->isStructure()) {                  // structure
        if (!p.type->isStructure() && p.type->isBuilder) {
            // The Monarch builds relative to HOME, not wherever it has drifted -- so a
            // lodestone goes on the nearest deposit to the BASE and other structures
            // ring the base, keeping the Monarch near home instead of trekking across
            // the map (where losing it can lose the game). Other builders build where
            // they stand.
            float ox = p.x.toFloat(), oz = p.z.toFloat();
            if (p.type->commander) { auto h = homeOf(world); ox = h.first; oz = h.second; }
            float x, z;
            if (placeSite(world, pick, p, ox, oz, x, z, siteLimit)) {
                if (pick->onMana) plannedManaSites_.emplace_back(x,z);
                if (dp_.unbounded) {
                    plannedSites_.push_back({pick,x,z});
                    issuedBuilds_.push_back({p.id,pick,x,z,now_});
                }
                emit(sink, tak::net::Cmd::Build, p.id, pick->id, x, z);
                return true;
            } else {
                static const bool kPickLog = tak::devEnv("TAK_AI_PICK") != nullptr;
                if (kPickLog)
                    std::fprintf(stderr, "    NO SITE: %s#%d cannot site %s near (%.0f,%.0f)\n",
                                 p.type->id.c_str(), p.id, pick->id.c_str(), ox, oz);
            }
        }
        return false;
    } else if (p.type->isStructure()) {         // factory trains mobile
        emit(sink, tak::net::Cmd::Train, p.id, pick->id, 0, 0);
        return true;
    } else if (p.type->isBuilder) {             // mobile builder conjures mobile
        if (!pick->canFly && pick->domain==tak::sim::UnitType::Domain::Water) {
            float x,z;
            if (!placeSite(world,pick,p,p.x.toFloat(),p.z.toFloat(),x,z)) return false;
            emit(sink,tak::net::Cmd::Build,p.id,pick->id,x,z);
            return true;
        }
        // A conjurer works where it stands: not in a factory's open yard, where it
        // holds the outputs in (a newborn Aramon Priest conjured a Dragon from its
        // Castle's yard); door clearing walks it out first. Nor does the conjured
        // unit go in a building's yard or a factory door, unless it flies.
        const BaseLayout layout = baseLayout(world);
        const Rect body = footprintRect(p.type, p.x.toFloat(), p.z.toFloat());
        for (const auto& u : world.units())
            if (u.alive() && u.player == player_ && u.type && u.type->isStructure() &&
                !registry_.buildable(u.type->id).empty() &&
                footprintRect(u.type, u.x.toFloat(), u.z.toFloat()).overlaps(body)) return false;
        // The site each ring point snaps to, under the test the conjurer will
        // place it by (World::canPlaceSite): a point's own position passed a
        // test its site then failed, with the conjurer itself in the way.
        const auto snap = [](float v, int foot) {
            return tak::sim::footprintSnap(tak::sim::Fixed::fromFloat(v), foot).toFloat();
        };
        for (float r = 40; r < 170; r += 20)
            for (float a = 0; a < 6.28f; a += 0.6f) {
                const float x = snap(p.x.toFloat() + detmath::cos(a) * r, pick->footX);
                const float z = snap(p.z.toFloat() + detmath::sin(a) * r, pick->footZ);
                if (world.canPlaceSite(pick, x, z) && (pick->canFly || !layout.blocks(footprintRect(pick, x, z)))) {
                    emit(sink, tak::net::Cmd::Build, p.id, pick->id, x, z);
                    return true;
                }
            }
        static const bool kPickLog = tak::devEnv("TAK_AI_PICK") != nullptr;
        if (kPickLog)
            std::fprintf(stderr, "    NO SPOT: %s#%d at (%.0f,%.0f) cannot place %s "
                                 "anywhere in r=40..170\n",
                         p.type->id.c_str(), p.id, p.x.toFloat(), p.z.toFloat(), pick->id.c_str());
    }
    return false;
}

// Find a build site for the AI: lodestones go on the nearest free mana deposit
// (when the map has any), everything else probes outward from the builder.
// Returns true and the chosen (outX,outZ) if a spot was found.
bool Controller::placeSite(const tak::sim::World& world, const tak::sim::UnitType* t,
                           const tak::sim::Unit& builder, float nx, float nz, float& outX, float& outZ,
                           float limit) const {
    if (!t) return false;
    const auto home=homeOf(world);
    const bool aggressive=diff_==Difficulty::Hard || diff_==Difficulty::Absurd;
    const bool naval=navalFactory(t);
    const bool waterUnit=!t->isStructure() && !t->canFly && t->domain==tak::sim::UnitType::Domain::Water;
    // Defensive builds within 600 of home. The unbounded levels' base grows with it:
    // they build within retail's AI base radius (retailLodestoneRadius -- 8*b*sqrt(n)
    // for n the summed footprint, b 7 for the Monarch and 10 for other builders), never
    // under 600 -- a fixed 600 px disc filled at ~31 buildings and left every builder
    // idle with a full store. Builders other than the Monarch also claim deposits out
    // to Normal's 1600, so income -- and with it the build rate -- keeps growing;
    // sendDefenders guards every building, remote ones too.
    float defensiveRadius=600.0f;
    if (dp_.unbounded) {
        defensiveRadius=std::max(defensiveRadius,retailLodestoneRadius(world,builder));
        if (t->onMana && !builder.type->commander) defensiveRadius=std::max(defensiveRadius,1600.0f);
    }
    const float homeRadius=!dp_.attack ? defensiveRadius : (builder.type->commander || stayHome_ ? 900.0f : aggressive ? 100000.0f : 1600.0f);
    // A coastal outpost may be farther away than an ordinary base building.
    // Send an expansion builder; keep the Monarch and Defensive AI near home.
    const float navalRadius=builder.type->commander || !dp_.attack || stayHome_ ? 900.0f : 2400.0f;
    const float fullRadius=naval ? std::max(homeRadius,navalRadius) : waterUnit ? std::max(homeRadius,900.0f) : homeRadius;
    const float radius=limit>0 ? std::min(fullRadius,limit) : fullRadius;
    // Reserve the largest lodestone this faction may use, including upgrades.
    // This is AI policy, not a change to the player's placement/navigation rules.
    int manaFootX=0,manaFootZ=0;
    if (t->isStructure() && !t->onMana && world.hasManaSpots())
        for (const auto& [id,type]:registry_.types())
            if (type.onMana && type.isStructure() && type.side==builder.type->side) {
                manaFootX=std::max(manaFootX,type.footX);
                manaFootZ=std::max(manaFootZ,type.footZ);
            }
    const bool factory=!registry_.buildable(t->id).empty();
    const auto* launches=naval ? &navalLaunchOffsets(t) : nullptr;
    // Zhon's producers are its factories as well as its lodestone builders. At
    // Hard and Absurd, whose reach is the whole map, they went from deposit to
    // deposit until they stood in enemy bases (median 12,000 px from home, median
    // life 5.1 minutes): they take deposits on their own side of the map only.
    const bool producerTrip=!builder.type->commander && categoryOf(builder.type)==BuildCat::Factory &&
        mobileProduction(builder.type->side);
    // The Defensive AI does not send a builder into a fight: a site with a spotted
    // (sees), armed enemy within kFightRadius waits until the defenders have cleared it.
    constexpr float kFightRadius=400;
    std::vector<std::pair<float,float>> fights;
    if (!dp_.attack) {
        for (const auto& e:world.units()) {
            if (!e.alive() || !e.type || e.embarked() || e.underConstruction || e.cloaked ||
                world.isNeutralPlayer(e.player) || world.allied(player_,e.player)) continue;
            if (e.type->weapon.damage<=0 &&
                std::none_of(e.type->weapons.begin(),e.type->weapons.end(),[](const auto& w) {
                    return w.damage>0 || w.mindControl;
                })) continue;
            const float ex=e.x.toFloat(),ez=e.z.toFloat();
            const float hx=ex-home.first,hz=ez-home.second,reach=radius+kFightRadius;
            if (hx*hx+hz*hz>reach*reach) continue;
            if (sees(world,e)) fights.emplace_back(ex,ez);
        }
    }
    // The unbounded levels keep a 48 px lane between buildings (see below), but a
    // lodestone goes where its deposit is.
    const float lane=dp_.unbounded && t->isStructure() && !t->onMana ? 48.0f : 0.0f;
    // The unbounded levels move every idle producer in one think, so two builders
    // could be given one site -- in this think, or one another builder is still
    // walking to -- and one walks there for nothing. Those sites are taken, with
    // the same lane.
    std::vector<PlannedSite> pendingSites;
    if (dp_.unbounded && t->isStructure()) {
        pendingSites=plannedSites_;
        for (const auto& u:world.units()) {
            if (!u.alive() || u.player!=player_ || u.id==builder.id || !u.type || !u.type->isBuilder) continue;
            for (const auto& order:u.orders)
                if (order.buildType && order.buildType->isStructure() && (order.buildX.v || order.buildZ.v))
                    pendingSites.push_back({order.buildType,order.buildX.toFloat(),order.buildZ.toFloat()});
        }
    }
    // Keep production doors clear (doorStrips): no building in front of one, no
    // factory whose own door faces a building, no builder working from a door.
    // Sites still to be built count, at every level. A deposit is where it is.
    const bool doors=t->isStructure() && !t->onMana;
    BaseLayout layout;
    std::vector<Rect> siteDoors;
    if (doors) {
        layout=baseLayout(world);
        auto pending=[&](const tak::sim::UnitType* type,float px,float pz) {
            layout.rects.push_back(footprintRect(type,px,pz));
            addDoorStrips(type,px,pz,layout.doors);
        };
        for (const auto& site:plannedSites_) pending(site.type,site.x,site.z);
        for (const auto& u:world.units()) {
            if (!u.alive() || u.player!=player_ || u.id==builder.id || !u.type || !u.type->isBuilder) continue;
            for (const auto& order:u.orders)
                if (order.buildType && order.buildType->isStructure() && (order.buildX.v || order.buildZ.v))
                    pending(order.buildType,order.buildX.toFloat(),order.buildZ.toFloat());
        }
    }
    // Past the old fixed radii the unbounded levels' grown base stays on its own side
    // of the map -- nearer home than any enemy start -- so a defensive AI does not
    // build its way into an enemy's base (Frey River Plain, Absurdly Defensive: its
    // base and army reached 140 px from the idle enemy's within the hour).
    const float ownRadius=dp_.unbounded ? (t->onMana && !builder.type->commander ? 1600.0f : 600.0f) : radius;
    // Our own structures, gathered on the first site that reaches the spacing test below
    // rather than scanned (with two build-tree lookups apiece) for every candidate: a
    // lodestone search may test thousands of deposits. A structure that can neither be a
    // production building nor (lane 0) take a lane never rejects a site, so it is left out.
    struct OwnStructure {float x,z;int footX,footZ;bool menu;};
    std::vector<OwnStructure> ownStructures;
    bool ownGathered=false;
    // Sites are chosen on ground this side knows (knowsGround), then put to the real
    // placement test, which also sees enemy buildings out of view -- the answer a
    // player's refused order gets: an enemy lodestone that refuses a deposit is
    // learned (learnSiteBlockers), any other refused site passed over.
    auto usable=[&](float x,float z) {
        const float dx=x-home.first,dz=z-home.second;
        if (dx*dx+dz*dz>radius*radius || !knowsGround(world,x,z)) return false;
        if (aggressive && t->onMana && t->isStructure()) {
            // Hard's and Absurd's reach is the whole map: not a deposit inside the
            // base of an enemy who may still be there (its home deposits: within
            // kEnemyBase of its start and nearer it than our home) -- for a
            // producer, none on that enemy's side at all (enemySide) -- nor an
            // unsafe one. Before the placement test, which is the costly one and
            // would learn what stands on a deposit no builder is sent to.
            const float own=dx*dx+dz*dz;
            if (std::any_of(enemyStarts_.begin(),enemyStarts_.end(),[&](const auto& e) {
                    const float ex=e.first-x,ez=e.second-z,d=ex*ex+ez*ez;
                    return d<own && (producerTrip || d<kEnemyBase*kEnemyBase); }) ||
                unsafeDeposit(x,z)) return false;
        }
        if (!world.canPlaceSite(t,x,z,player_)) { learnSiteBlockers(world,t,x,z); return false; }
        if (dx*dx+dz*dz>ownRadius*ownRadius && enemySide(x,z,home.first,home.second)) return false;
        if (std::any_of(refusedSites_.begin(),refusedSites_.end(),[&](const RefusedSite& r) {
                return std::abs(r.x-x)<float(t->footX)*8+16 && std::abs(r.z-z)<float(t->footZ)*8+16; }) ||
            std::any_of(pendingSites.begin(),pendingSites.end(),[&](const PlannedSite& o) {
                return std::abs(o.x-x)<float(t->footX+o.type->footX)*8+lane &&
                       std::abs(o.z-z)<float(t->footZ+o.type->footZ)*8+lane; })) return false;
        if (std::any_of(fights.begin(),fights.end(),[&](const auto& f) {
            const float fx=f.first-x,fz=f.second-z;return fx*fx+fz*fz<kFightRadius*kFightRadius;
        })) return false;
        if (doors) {
            const Rect site=footprintRect(t,x,z);
            if (std::any_of(layout.doors.begin(),layout.doors.end(),[&](const Rect& d){return d.overlaps(site);}))
                return false;
            siteDoors.clear();addDoorStrips(t,x,z,siteDoors);
            for (const auto& door:siteDoors)
                if (std::any_of(layout.rects.begin(),layout.rects.end(),[&](const Rect& r){return r.overlaps(door);}))
                    return false;
        }
        float approachX=x,approachZ=z;
        if (launches && !launches->empty()) {
            bool output=false;
            for (const auto& id:registry_.buildable(t->id)) {
                const auto* ship=registry_.find(id);
                const auto wi=profile_.weight.find(id),li=profile_.limit.find(id);
                if (!ship || ship->isBuilder || !world.buildAllowed(ship) ||
                    wi==profile_.weight.end() || wi->second<=0 || (li!=profile_.limit.end() && li->second==0)) continue;
                if (std::all_of(launches->begin(),launches->end(),[&](const auto& offset) {
                    return world.canPlace(ship,x+offset.first,z+offset.second) &&
                        navalLaunchClear(world,t,ship,x,z,offset.first,offset.second);
                })) {
                    float tx,tz;
                    const auto& offset=launches->front();
                    output=navalBodyAtLeast(world,ship,x+offset.first,z+offset.second,kMinNavalBodyCells) &&
                        (enemyStarts_.empty() || std::any_of(enemyStarts_.begin(),enemyStarts_.end(),[&](const auto& enemy) {
                        return reachablePoint(world,ship,x+offset.first,z+offset.second,enemy.first,enemy.second,tx,tz);
                    }));
                    if (output) break;
                }
            }
            if (!output) return false;
        }
        if (manaFootX>0) for (const auto& [mx,mz]:world.manaSpots()) {
            const int x0=tak::sim::footprintOrigin(x,t->footX);
            const int z0=tak::sim::footprintOrigin(z,t->footZ);
            const int mx0=tak::sim::footprintOrigin(mx,manaFootX);
            const int mz0=tak::sim::footprintOrigin(mz,manaFootZ);
            if (x0<mx0+manaFootX && x0+t->footX>mx0 &&
                z0<mz0+manaFootZ && z0+t->footZ>mz0) return false;
            const float mdx=x-mx,mdz=z-mz;
            const float clearance=float(std::max(manaFootX,manaFootZ))*8+12;
            if (mdx*mdx+mdz*mdz<clearance*clearance) return false;
            // The lodestone planner also leaves working room around factories.
            if (factory && std::abs(mdx)<float(t->footX+manaFootX)*8+48 &&
                std::abs(mdz)<float(t->footZ+manaFootZ)*8+48) return false;
        }
        if (!builder.type->canFly) {
            // Check the position where MobileBuild will bring the constructor,
            // too. A rectangular factory can be clear now but reject its own
            // builder after a short-side approach enters the placement radius.
            const int sx=tak::sim::footprintOrigin(x,t->footX);
            const int sz=tak::sim::footprintOrigin(z,t->footZ);
            const tak::sim::RetailRectGoal rectangle{sx-builder.type->footX,sx+t->footX,
                                                     sz-builder.type->footZ,sz+t->footZ};
            const auto [cx,cz]=rectangle.navigationCell(
                tak::sim::footprintOrigin(builder.x,builder.type->footX),
                tak::sim::footprintOrigin(builder.z,builder.type->footZ));
            // Nor from a door strip or a building's open yard, where it holds the
            // next output in (a Mechanic building an Academy from a Smithy's door
            // kept a fire wagon in for 2.5 minutes). A newborn builder stands in
            // its factory's door, so its nearest sites used to be just these.
            if (doors && layout.blocks({float(cx*16),float(cz*16),float((cx+builder.type->footX)*16),
                                        float((cz+builder.type->footZ)*16)})) return false;
            if (waterUnit || world.lodestoneUpgradeSource(t,x,z,player_)) {
                approachX=float(cx*16+builder.type->footX*8);
                approachZ=float(cz*16+builder.type->footZ*8);
            }
            if (waterUnit) {
                const auto& nav=world.navFor(builder.type);
                const int foot=std::max(builder.type->footX,builder.type->footZ);
                auto reachable=[&](float px,float pz) {
                    return nav.fits(tak::sim::footprintCell(px,foot),tak::sim::footprintCell(pz,foot),foot) &&
                        tak::sim::retailBuildWithinReach(tak::sim::Fixed::fromFloat(px),tak::sim::Fixed::fromFloat(pz),
                            tak::sim::Fixed::fromFloat(x),tak::sim::Fixed::fromFloat(z),builder.type->footX,builder.type->footZ,
                            t->footX,t->footZ,uint16_t(builder.type->buildDist)) &&
                        world.pathExists(builder.type,px,pz,builder.x.toFloat(),builder.z.toFloat());
                };
                bool reach=reachable(approachX,approachZ);
                for (int r=32;r<=builder.type->buildDist+std::max(t->footX,t->footZ)*8+16 && !reach;r+=16)
                    for (int i=0;i<16 && !reach;++i) {
                        const float angle=float(i)*0.392699082f;
                        const float px=x+detmath::cos(angle)*r,pz=z+detmath::sin(angle)*r;
                        if (reachable(px,pz)) {approachX=px;approachZ=pz;reach=true;}
                    }
                if (!reach) return false;
            }
            const float ax=float(cx*16+builder.type->footX*8)-x;
            const float az=float(cz*16+builder.type->footZ*8)-z;
            const float clearance=float(std::max(t->footX,t->footZ))*8+12;
            if (ax*ax+az*az<clearance*clearance) return false;
        }
        if (!ownGathered) {
            ownGathered=true;
            std::unordered_map<const tak::sim::UnitType*,bool> menus;
            for (const auto& u : world.units()) {
                if (!u.alive() || u.player!=player_ || !u.type || !u.type->isStructure()) continue;
                auto m=menus.find(u.type);
                if (m==menus.end()) m=menus.emplace(u.type,!registry_.buildable(u.type->id).empty()).first;
                if (m->second || factory || lane>0)
                    ownStructures.push_back({u.x.toFloat(),u.z.toFloat(),u.type->footX,u.type->footZ,m->second});
            }
        }
        for (const auto& u : ownStructures) {
            if (!u.menu && !factory) {
                // The unbounded levels pack a growing base: keep a 48 px lane
                // between any two of its buildings, or they wall their own builders
                // and siege engines into pockets (Ulasem Arena, Absurdly Defensive:
                // two arabuilds shut in by 46 minutes, every site unreachable, the
                // store full). A deposit is where it is; a lodestone takes no lane.
                if (lane>0 && std::abs(x-u.x) < float(t->footX+u.footX)*8+lane &&
                    std::abs(z-u.z) < float(t->footZ+u.footZ)*8+lane) return false;
                continue;
            }
            // Leave working room around production buildings, including their
            // scripted output point. Placement validity alone allows blocked doors.
            if (std::abs(x-u.x) < float(t->footX+u.footX)*8+48 &&
                std::abs(z-u.z) < float(t->footZ+u.footZ)*8+48) return false;
        }
        if (!builder.type->canFly && !world.pathExists(builder.type,approachX,approachZ,builder.x.toFloat(),builder.z.toFloat())) return false;
        if (waterUnit && !enemyStarts_.empty()) {
            float tx,tz;
            for (const auto& enemy:enemyStarts_)
                if (reachablePoint(world,t,x,z,enemy.first,enemy.second,tx,tz)) return true;
            return false;
        }
        return true;
    };
    // A Divine Lodestone goes only on the lodestone planUpgrade names, never on a
    // free deposit: the plain one there pays back in seconds, and upgrading it
    // later costs less than building the Divine Lodestone outright.
    if (t->isStructure() && !tak::sim::World::lodestoneUpgradeBase(*t).empty()) {
        if (!upgrade_.target || t!=upgrade_.divine || !usable(upgrade_.x,upgrade_.z)) return false;
        outX=upgrade_.x;outZ=upgrade_.z;
        return true;
    }
    if (t->onMana && world.hasManaSpots()) {
        // Every difficulty reserves: Normal allies, and two builders of one AI,
        // raced for the same free deposit as Hard and Absurd once did.
        std::vector<std::pair<float,float>> reservedSites=plannedManaSites_;
        // Walking builders do not occupy their deposits yet. Gather queued
        // destinations once, rather than scan every unit for every deposit.
        // Reserve the SITE (buildX/buildZ): a MobileBuild order's x/z is the
        // builder's approach point beside it (World::makeBuildOrder), 32-70 px
        // from the deposit for a lodestone, so it never matched -- own and
        // allied builders kept walking to a deposit already being claimed.
        // A flyer's conjure orbit carries no site; its building already stands.
        // A builder that has stalled short of its deposit holds it no longer
        // (updateBuilderProgress).
        for (const auto& u:world.units()) {
            if(!u.alive() || u.id==builder.id || !world.allied(player_,u.player))continue;
            for(const auto& order:u.orders)
                if(order.buildType && order.buildType->onMana && (order.buildX.v || order.buildZ.v) &&
                   !stalledBuilder(u.id))
                    reservedSites.emplace_back(order.buildX.toFloat(),order.buildZ.toFloat());
        }
        const auto& deposits=world.manaSpots();
        const bool upgrade=!tak::sim::World::lodestoneUpgradeBase(*t).empty();
        // Nearest first: the first deposit to pass is the nearest that can pass, so the
        // costly usable() test runs on a handful of them rather than on all (an Absurd
        // builder's radius is the whole map). Ties go to the lower index, as the strict
        // < of a scan in index order did.
        std::vector<std::pair<float,size_t>> byDistance(deposits.size());
        for (size_t i=0;i<deposits.size();++i) {
            const float dx=deposits[i].first-nx,dz=deposits[i].second-nz;
            byDistance[i]={dx*dx+dz*dz,i};
        }
        const auto later=[](const auto& a,const auto& b) {return a.first>b.first || (a.first==b.first && a.second>b.second);};
        std::make_heap(byDistance.begin(),byDistance.end(),later);
        for (auto last=byDistance.end();last!=byDistance.begin();) {
            std::pop_heap(byDistance.begin(),last,later);
            --last;
            const size_t i=last->second;
            // Apple Clang 15 cannot capture structured bindings in this lambda.
            const float sx=deposits[i].first,sz=deposits[i].second;
            if (std::any_of(reservedSites.begin(),reservedSites.end(),[sx,sz](const auto& spot) {
                const float dx=spot.first-sx,dz=spot.second-sz;return dx*dx+dz*dz<32*32;
            })) continue;
            // Nor does this builder go back to a deposit it stalled on.
            if (std::any_of(stalledSites_.begin(),stalledSites_.end(),[&](const StalledSite& site) {
                const float dx=site.x-sx,dz=site.z-sz;return site.unit==builder.id && dx*dx+dz*dz<32*32;
            })) continue;
            // An ally still claiming its home deposit keeps it, and a nearer ally
            // takes it after (updateAllyClaims); upgrading our own lodestone
            // there is not a claim on it.
            if (i<allyHeldDeposits_.size() && allyHeldDeposits_[i] &&
                !world.lodestoneUpgradeSource(t,sx,sz,player_)) continue;
            // Only a deposit on ground this side knows, and not one it holds taken by
            // an enemy lodestone (seen, remembered or learned) -- whatever stands
            // there now out of view.
            if ((i<depositTaken_.size() && depositTaken_[i]) || !knowsGround(world,sx,sz) ||
                (!upgrade && i<depositAllied_.size() && depositAllied_[i])) continue;
            if (!usable(sx, sz)) continue;   // taken or blocked
            outX = sx; outZ = sz;
            return true;
        }
        return false;
    }
    const float searchRadius = naval ? std::min(radius,2400.0f) : t->minWaterDepth>0 ? std::min(radius,1600.0f) : 340.0f;
    // A yard's door never turns (buildings do not rotate), so a yard on the
    // nearest coast can open onto a shore pocket, its ships circling it to get
    // out. Prefer open water out of the door (navalDoorSteps) for the largest
    // ship it may build, looking at most kDoorSearch past the first ring with a
    // usable site: the first ring holding a site with water straight out of the
    // door (a detour of at most kStraight moves), its straightest; else the first
    // ring holding any door-clear site, its straightest; else the first usable
    // site, as before -- a preference, never a veto, so a navy still gets built.
    const tak::sim::UnitType* largestShip=nullptr;
    if (launches && !launches->empty())
        for (const auto& id:registry_.buildable(t->id)) {
            const auto* ship=registry_.find(id);
            const auto wi=profile_.weight.find(id),li=profile_.limit.find(id);
            if (!ship || ship->isBuilder || !world.buildAllowed(ship) ||
                wi==profile_.weight.end() || wi->second<=0 || (li!=profile_.limit.end() && li->second==0)) continue;
            if (!largestShip || std::max(ship->footX,ship->footZ)>std::max(largestShip->footX,largestShip->footZ))
                largestShip=ship;
        }
    if (largestShip) {
        constexpr float kDoorSearch=720;
        constexpr int kStraight=1;
        auto detour=[&](float x,float z) {
            int worst=0;
            for (const auto& offset:*launches) {
                const int steps=navalDoorSteps(world,t,largestShip,x,z,offset.first,offset.second);
                if (steps<0) return -1;
                worst=std::max(worst,steps);
            }
            return worst;
        };
        struct Pick { float x=0,z=0; int detour=INT_MAX; };
        std::optional<Pick> first,clear;
        float firstR=0;
        for (float r = 70; r < searchRadius && (!first || r<=firstR+kDoorSearch); r += 30) {
            Pick ring;
            for (float a = 0; a < 6.28f; a += std::min(0.5f,48.0f/r)) {
                const float x = tak::sim::footprintWaypoint(tak::sim::footprintCell(nx + detmath::cos(a) * r,t->footX),t->footX).toFloat();
                const float z = tak::sim::footprintWaypoint(tak::sim::footprintCell(nz + detmath::sin(a) * r,t->footZ),t->footZ).toFloat();
                // Past the first usable site only a better door can win: test the
                // door first, far cheaper than usable() (and cached).
                if (first) {
                    if (!world.canPlace(t,x,z,player_)) continue;
                    const int d=detour(x,z);
                    if (d<0 || (clear && d>kStraight)) continue;
                }
                if (!usable(x,z)) continue;
                if (!first) {first=Pick{x,z};firstR=r;}
                const int d=detour(x,z);
                if (d>=0 && d<ring.detour) ring=Pick{x,z,d};
            }
            if (ring.detour<=kStraight) {outX=ring.x;outZ=ring.z;return true;}
            if (!clear && ring.detour!=INT_MAX) clear=ring;
        }
        if (!first) return false;
        const Pick& pick=clear ? *clear : *first;
        outX=pick.x;outZ=pick.z;return true;
    }
    for (float r = 70; r < searchRadius; r += 30)
        for (float a = 0; a < 6.28f; a += (naval || waterUnit) ? std::min(0.5f,48.0f/r) : 0.5f) {
            float x = tak::sim::footprintWaypoint(tak::sim::footprintCell(nx + detmath::cos(a) * r,t->footX),t->footX).toFloat();
            float z = tak::sim::footprintWaypoint(tak::sim::footprintCell(nz + detmath::sin(a) * r,t->footZ),t->footZ).toFloat();
            if (usable(x, z)) { outX = x; outZ = z; return true; }
        }
    // Nothing fits within 340 of the builder (for the Monarch, of home). The unbounded
    // levels then search their whole base outward from home, so the base keeps
    // growing outward instead of its builders idling beside a filled core; the angle
    // step keeps the probes ~48 px apart on every ring.
    if (dp_.unbounded && !naval && !waterUnit && t->minWaterDepth<=0 && searchRadius<radius) {
        const auto [hx,hz]=home;
        for (float r = 70; r <= radius; r += 30)
            for (float a = 0; a < 6.28f; a += std::min(0.5f,48.0f/r)) {
                const float x = tak::sim::footprintWaypoint(tak::sim::footprintCell(hx + detmath::cos(a) * r,t->footX),t->footX).toFloat();
                const float z = tak::sim::footprintWaypoint(tak::sim::footprintCell(hz + detmath::sin(a) * r,t->footZ),t->footZ).toFloat();
                if (usable(x, z)) { outX = x; outZ = z; return true; }
            }
    }
    return false;
}

bool Controller::enemySide(float x, float z, float hx, float hz) const {
    const float ox=x-hx,oz=z-hz,own=ox*ox+oz*oz;
    return std::any_of(enemyStarts_.begin(),enemyStarts_.end(),[&](const auto& e) {
        const float dx=x-e.first,dz=z-e.second;return dx*dx+dz*dz<own;
    });
}

float Controller::strength(const tak::sim::Unit& u) {
    const float health = u.type->maxHp > 0 ? std::clamp(u.hp.toFloat() / float(u.type->maxHp), 0.1f, 1.0f) : 1.0f;
    return std::max(u.type->buildCost, 20.0f) * health;
}

bool Controller::armed(const tak::sim::UnitType* t) {
    return t->weapon.damage > 0 ||
        std::any_of(t->weapons.begin(), t->weapons.end(), [](const auto& w) { return w.damage > 0; });
}

bool Controller::canHurt(const tak::sim::UnitType* a, const tak::sim::UnitType* t) {
    if (!a || !t) return false;
    const auto hurts = [&](const tak::sim::Weapon& w) { return !(t->canFly && w.noAir) && w.damageVs(t) > 0; };
    return a->weapons.empty() ? hurts(a->weapon) : std::any_of(a->weapons.begin(), a->weapons.end(), hurts);
}

bool Controller::reachesNow(const tak::sim::Unit& u, const tak::sim::Unit& t) {
    const auto& weapons = u.type->weapons;
    const auto& weapon = weapons.empty() ? u.type->weapon
                                         : weapons[size_t(std::clamp(u.weaponSlot, 0, int(weapons.size()) - 1))];
    return !(weapon.melee && t.flightGroundMode == 2);
}

// World::weaponServes for each weapon, as far as a unit's public state tells
// (its aim point under water is the sim's alone to know). The sim refuses an
// Attack no weapon can carry out, or drops it on the next tick: 660 of one long
// game's 2,996 Attack orders went that way -- Harpies sent to charm lodestones
// and factories, or anything at all with their side at the unit cap, Basilisks
// to stone buildings and the immune -- and the unit stood idle under fire until
// the next think sent it the same order.
bool Controller::canTake(const tak::sim::World& world, const tak::sim::Unit& u, const tak::sim::Unit& t) const {
    using Status = tak::sim::Weapon::Status;
    // What a charm, a stone and a freeze each need: a mobile unit (bmcode 1), finished, no Monarch.
    const bool body = t.type->buildMovementCode == 1 && !t.type->commander && !t.underConstruction;
    const auto serves = [&](const tak::sim::Weapon& w) {
        if ((w.noAir && t.flightGroundMode == 2) || (w.toAir && t.flightGroundMode != 2) || w.damageVs(t.type) <= 0)
            return false;
        if (w.mindControl && !(body && !t.type->cantBeCaptured && !t.embarked() && t.stonedFor <= 0 &&
                               !world.atUnitCap(player_))) return false;
        if (w.status == Status::Stoned && !(body && !t.type->cantBeStoned)) return false;
        if (w.status == Status::Frozen && !(body && !t.type->cantBeFrozen)) return false;
        return true;
    };
    const auto& weapons = u.type->weapons;
    return weapons.empty() ? serves(u.type->weapon) : std::any_of(weapons.begin(), weapons.end(), serves);
}

// Every enemy unit this AI knows of (knowledge_), scored where it was last seen.
// A unit's value is its cost, tripled for a lodestone or mana building and
// doubled for production and builders (retail's picker divides everything that
// is not a mana site by about five); a Monarch whose loss defeats its owner is
// worth the game; each unit of a player with few left is worth more, so a winning
// AI finishes a player off instead of leaving a lone lodestone standing; an armed
// unit adds its strength as threat, up to three times over near our home. Its
// armed strength is also the site's defence, summed with the eight cells around
// it so a lodestone under a tower line is not taken for bare. A seated player's
// score table shows who is still in the game, not how many units each has or how
// strong they are, so those are reckoned from what this AI knows. An enemy whose
// home -- its start, as the room showed it or as its buildings found it -- this
// AI has had in sight within kLookedTicks is as strong as the armed units it
// knows of, and has the units it knows of left (for finishing it off); any other
// is taken to be at least as strong as this AI's own fighters, with kUnseenUnits
// more units: an AI that has not looked cannot believe itself far ahead. Being
// ahead of what it knows alone (knownAhead) is reason enough to go and look.
Controller::StrikeBoard Controller::scanTargets(const tak::sim::World& world) const {
    constexpr float kUnseenUnits = 20;
    constexpr uint32_t kLookedTicks = 5 * 60 * 30;
    StrikeBoard board;
    const int players = world.numPlayers();
    std::vector<uint8_t> enemy(size_t(std::max(players, 0)), 0), present(enemy.size(), 0);
    std::vector<float> left(enemy.size(), 0), known(enemy.size(), 0);
    for (int p = 0; p < players; ++p)
        enemy[size_t(p)] = p != player_ && !world.isNeutralPlayer(p) && !world.allied(player_, p) &&
                           !world.player(p).defeated;
    for (const auto& u : world.units()) {
        if (!u.alive() || !u.type || u.player < 0 || u.player >= players) continue;
        present[size_t(u.player)] = 1;
        if (u.player == player_ && !u.embarked() && !u.type->isStructure() && !u.type->isBuilder &&
            !u.underConstruction && armed(u.type))
            board.ours += strength(u);
    }
    for (const auto& [id, k] : knowledge_) {
        if (k.player < 0 || k.player >= players || !enemy[size_t(k.player)]) continue;
        ++left[size_t(k.player)];
        if (armed(k.type) && !k.unfinished) known[size_t(k.player)] += k.strength;
    }
    // With every unit in sight (FULL VISION, or past the first tick with no view
    // plane: World::playerSees) every home is; before the first tick, none is.
    std::vector<uint8_t> looked(enemy.size(),
                                world.roomLosOff() || (world.viewCounts().empty() && world.tickCount() != 0));
    for (size_t i = 0; i < starts_.size() && i < startSeen_.size(); ++i) {
        const int owner = starts_[i].owner;
        if (owner >= 0 && owner < players && startSeen_[i] && now_ + 1 - startSeen_[i] <= kLookedTicks)
            looked[size_t(owner)] = 1;
    }
    bool any = false;   // some enemy is still in the game
    float knownTheirs = 0;
    for (int p = 0; p < players; ++p) {
        if (!enemy[size_t(p)] || !present[size_t(p)]) continue;
        any = true;
        knownTheirs += known[size_t(p)];
        if (looked[size_t(p)]) board.theirs += known[size_t(p)];
        else {
            board.theirs += std::max(known[size_t(p)], board.ours);
            left[size_t(p)] += kUnseenUnits;
        }
    }
    const auto [hx, hz] = homeOf(world);
    const bool expendable = world.monarchExpendable();
    // Passive strikes only at what comes within kPassiveReach of one of its buildings:
    // it defends its base and never goes out after what it has seen elsewhere.
    constexpr float kPassiveReach = 800;
    std::vector<std::pair<float, float>> base;
    if (stayHome_)
        for (const auto& u : world.units())
            if (u.alive() && u.player == player_ && u.type && u.type->isStructure())
                base.emplace_back(u.x.toFloat(), u.z.toFloat());
    const auto nearBase = [&](float x, float z) {
        return std::any_of(base.begin(), base.end(), [&](const auto& b) {
            const float dx = x - b.first, dz = z - b.second;
            return dx * dx + dz * dz <= kPassiveReach * kPassiveReach;
        });
    };
    struct Cell { int cx, cz; float value = 0, defence = 0, anchorValue = -1; int anchor = 0; const Known* known = nullptr; bool seen = false; };
    std::map<std::pair<int, int>, size_t> index;
    std::vector<Cell> cells;
    for (const auto& [id, k] : knowledge_) {
        if (k.player < 0 || k.player >= players || !enemy[size_t(k.player)]) continue;
        const auto* t = k.type;
        const float x = k.x, z = k.z;
        if (stayHome_ && !nearBase(x, z)) continue;
        const float threat = armed(t) && !k.unfinished ? k.strength : 0.0f;
        float v = std::max(t->buildCost, 20.0f);
        if (t->onMana) v *= 3;
        else if (t->isBuilder || categoryOf(t) == BuildCat::Factory) v *= 2;
        if (t->commander && !expendable) v = v * 4 + 4000;
        v += 1500.0f / std::max(left[size_t(k.player)], 1.0f);
        if (threat > 0) {
            const float dx = x - hx, dz = z - hz;
            v += threat * (1 + 2 * std::max(0.0f, 1 - std::sqrt(dx * dx + dz * dz) / 2500));
        }
        if (k.unfinished) v *= 0.5f;
        const std::pair<int, int> key{int(std::floor(x / kSiteCell)), int(std::floor(z / kSiteCell))};
        auto [it, fresh] = index.try_emplace(key, cells.size());
        if (fresh) cells.push_back({key.first, key.second});
        Cell& c = cells[it->second];
        c.value += v;
        c.defence += threat;
        c.seen = c.seen || k.seenAt == now_;
        const auto pursuit = pursuits_.find(id);
        const bool passedOver = pursuit != pursuits_.end() && pursuit->second.until > now_;
        if (!passedOver && v > c.anchorValue) { c.anchorValue = v; c.anchor = id; c.known = &k; }
    }
    for (const auto& c : cells) {
        if (!c.anchor) continue;
        StrikeSite site;
        site.anchor = c.anchor; site.anchorType = c.known->type; site.anchorUnfinished = c.known->unfinished;
        site.x = c.known->x; site.z = c.known->z;
        site.value = c.value; site.seen = c.seen; site.visible = c.known->seenAt == now_ && c.known->sighted;
        for (int i = -1; i <= 1; ++i)
            for (int j = -1; j <= 1; ++j)
                if (const auto it = index.find({c.cx + i, c.cz + j}); it != index.end()) site.defence += cells[it->second].defence;
        board.sites.push_back(site);
    }
    // Clearly ahead: an attacking level then commits whatever is idle and a defensive
    // level goes out to scout and clean up; the defensive levels want a wider margin.
    const float margin = defensiveLevel(diff_) ? 5.0f : 3.0f;
    board.ahead = any && board.ours > 0 && board.ours >= board.theirs * margin;
    board.knownAhead = any && board.ours > 0 && board.ours >= knownTheirs * margin;
    board.winning = board.ahead && !board.sites.empty();
    return board;
}

float Controller::sitePad(const StrikeSite& site, const tak::sim::UnitType* type) {
    if (!site.anchorType || !type) return 0;
    return float(std::max(site.anchorType->footX, site.anchorType->footZ) + std::max(type->footX, type->footZ)) * 8;
}

float Controller::siteNeed(const StrikeSite& site) const {
    switch (diff_) {
        case Difficulty::Easy: return 0;
        case Difficulty::Hard: case Difficulty::Absurd: return site.defence * 1.25f;
        case Difficulty::Defensive: case Difficulty::AbsurdlyDefensive: return site.defence * 1.5f;
        default: return site.defence;
    }
}

// Easy goes for the nearest site, seen ones first. The others weigh value and
// threat against distance, and a site whose defence outweighs the force sent at
// it counts for the square of the shortfall (a raid wants twice the margin). A
// site already given its share this think counts for a quarter. Reachability is
// tried in score order, for the first 48: a fleet's sites out at sea can fill
// the top of an army's list.
int Controller::pickSite(const tak::sim::World& world, const StrikeBoard& board, float x, float z, float force,
                         const tak::sim::UnitType* type, bool raid, const std::vector<float>& taken) const {
    std::vector<std::pair<float, int>> ranked;
    for (size_t i = 0; i < board.sites.size(); ++i) {
        const auto& site = board.sites[i];
        if (type && site.anchorType && !canHurt(type, site.anchorType)) continue;
        const float dx = site.x - x, dz = site.z - z, d = std::sqrt(dx * dx + dz * dz);
        const float seen = site.seen ? 1.0f : diff_ == Difficulty::Easy ? 0.15f : 0.3f;
        float score;
        if (diff_ == Difficulty::Easy) score = seen / (1 + d / 600);
        else {
            score = site.value * seen / (1 + d / 1200);
            const float need = siteNeed(site) * (raid ? 2.0f : 1.0f), sent = force + taken[i];
            if (sent < need) score *= (sent / need) * (sent / need);
            if (taken[i] > 0 && taken[i] >= need) score *= 0.25f;
        }
        ranked.emplace_back(-score, int(i));
    }
    std::sort(ranked.begin(), ranked.end());
    int probes = 0;
    for (const auto& [score, i] : ranked) {
        if (++probes > 48) break;
        const auto& site = board.sites[size_t(i)];
        float tx = 0, tz = 0;
        if (!type || type->canFly || reachablePoint(world, type, x, z, site.x, site.z, tx, tz, sitePad(site, type))) return i;
    }
    return -1;
}

bool Controller::strikeOrder(const tak::sim::World& world, const tak::sim::Unit& u, const StrikeSite& site,
                             uint32_t simTick, const CommandSink& sink) {
    constexpr int kTries = 4;                       // thinks re-sent at an anchor it is not hurting
    constexpr uint32_t kPassOverTicks = 120 * 30;
    const float ux = u.x.toFloat(), uz = u.z.toFloat();
    float tx = 0, tz = 0;
    if (!reachablePoint(world, u.type, ux, uz, site.x, site.z, tx, tz, sitePad(site, u.type))) return false;
    const float reach = std::max(float(u.type->weapon.range), u.type->maxRange()) + 250;
    // Back at the anchor with nothing to do and the anchor no worse off since last
    // time (out of sight, nothing is known to be worse off): after kTries thinks it
    // is passed over for a while.
    auto tried = [&](int id, float hp) {
        auto& p = pursuits_[id];
        if (p.last == simTick + 1) return;   // once per think, however many are sent
        p.tries = p.tries && hp < p.hp ? 1 : p.tries + 1;
        p.hp = hp; p.last = simTick + 1;
        if (p.tries >= kTries) { p.until = simTick + kPassOverTicks; p.tries = 0; }
    };
    // Only an anchor in sight is ordered on by id; one remembered out of sight is
    // gone for where it was last seen.
    const auto* e = site.anchor && site.visible ? world.unit(site.anchor) : nullptr;
    if (!e && site.anchor) {
        const float dx = site.x - ux, dz = site.z - uz;
        if (dx * dx + dz * dz <= reach * reach) tried(site.anchor, 0);
    }
    if (e && e->alive() && e->type && canHurt(u.type, e->type)) {
        const float dx = e->x.toFloat() - ux, dz = e->z.toFloat() - uz;
        const bool close = dx * dx + dz * dz <= reach * reach;
        if (close) tried(e->id, e->hp.toFloat());
        // Auto-acquisition takes a site still being built only behind finished
        // targets (as retail's picker, x10), and up close an order on the anchor
        // itself makes sure of it.
        // A charm cannot take a unit still being built (the sim refuses the
        // order), so a caster attack-moves there instead of standing idle; so
        // does any unit none of whose weapons can take the anchor as it stands
        // (canTake): on the fight-move it takes what it can around the site.
        const bool charmRefused = e->underConstruction &&
            std::any_of(u.type->weapons.begin(), u.type->weapons.end(),
                        [](const auto& w) { return w.mindControl; });
        // A melee unit cannot reach an anchor in the air: it goes for the site.
        // Only up close, a site being built too: ordered on it from afar, the
        // force walked past everything on the way without fighting (an Absurd
        // Creon army sent 3000 px at an Aramon Stronghold going up died walking
        // through the defenders); a fight-move gets it there fighting.
        if (close && !charmRefused && canTake(world, u, *e) && reachesNow(u, *e)) {
            tak::net::Command c; c.kind = tak::net::Cmd::Attack; c.player = uint8_t(player_);
            c.unitId = u.id; c.targetId = e->id;
            sink(c);
            return true;
        }
        // With no weapon for the anchor and standing where that fight-move
        // ends, it stays: its weapons take what comes within reach, and the
        // order would end as it began, on every think of a siege (the anchor
        // is not passed over while others bring it down).
        if (constexpr float kArrived = 48; close && !canTake(world, u, *e) &&
            (tx - ux) * (tx - ux) + (tz - uz) * (tz - uz) <= kArrived * kArrived) return true;
    }
    emit(sink, tak::net::Cmd::AttackMove, u.id, "", tx, tz);
    return true;
}

// A player's base anchor: the centroid of its buildings, else its Monarch, else
// the centroid of its units (0,0 with none).
static std::pair<float, float> anchorOf(const tak::sim::World& world, int player) {
    double sx = 0, sz = 0; int n = 0;
    float kx = 0, kz = 0; bool haveKing = false;
    for (const auto& u : world.units()) {
        if (!u.alive() || u.player != player || !u.type) continue;
        if (u.type->isStructure()) { sx += u.x.toFloat(); sz += u.z.toFloat(); ++n; }
        if (u.type->commander && !haveKing) { kx = u.x.toFloat(); kz = u.z.toFloat(); haveKing = true; }
    }
    if (n) return {float(sx / n), float(sz / n)};   // centroid of the buildings
    if (haveKing) return {kx, kz};                  // no buildings yet: anchor on the Monarch
    for (const auto& u : world.units())
        if (u.alive() && u.player==player) { sx+=u.x.toFloat();sz+=u.z.toFloat();++n; }
    return n ? std::pair{float(sx/n),float(sz/n)} : std::pair{0.0f,0.0f};
}

std::pair<float, float> Controller::homeOf(const tak::sim::World& world) const {
    if (home_) return *home_;
    return anchorOf(world, player_);
}

// Whether a builder of this type can claim a deposit: a lodestone in its menu.
bool Controller::buildsLodestone(const tak::sim::UnitType* type) const {
    auto [it,inserted]=lodestoneBuilders_.try_emplace(type,false);
    if (inserted)
        for (const auto& id:registry_.buildable(type->id)) {
            const auto* child=registry_.find(id);
            if (child && child->onMana && child->isStructure()) { it->second=true; break; }
        }
    return it->second;
}

// Hard/Absurd expand map-wide, and Normal's and Easy's builders 1600 px from home
// (Defensive's 600), far enough to reach a close ally's base. At every difficulty
// a free deposit in a living ally's home area -- within kAllyHome of its start
// and nearer that start than ours -- stays the ally's while the ally is claiming it:
//  - a builder is bound for it (placeSite's reservation never races one, the
//    ally's, another ally's or our own, while it is still getting there), or
//  - the ally has a builder that can reach it -- finished, not carried, not
//    disabled, with a lodestone in its menu -- and it has not sat unclaimed for
//    kAllyClaimTicks.
// Unclaimed means no lodestone on it and no builder bound for it, counted in game
// ticks from the first think or from the end of its last claim, so a lodestone
// lost in a raid gives the ally the same two minutes to rebuild; a builder's
// claim ends when it last gained on the deposit (updateBuilderProgress). A
// defeated ally claims nothing. Retail's AI never builds there at all -- icd
// 411510 takes the nearest free sacred site within its own base radius -- but
// leaving deposits to an ally that never takes them (a Normal or Defensive AI, a
// human who does not expand) cost a mixed team 20-35% of its income.
// Once the ally whose home it is no longer holds it -- two minutes unclaimed, or
// no builder of its own that can reach it (its Monarch lost under Monarch
// Expendable) -- the deposit goes to whichever other living ally's start is
// nearest it, not to all of them: a far ally's builder, once bound for it, held
// it for its whole walk across the map while the nearer allies, the owner
// included, could not race it (Aramon reserved Zhon's deposits from 6,000 map
// units away while Zhon's builders went to the enemy's base). Its cost: a
// deposit whose nearest other ally never builds there -- a human, or a Normal
// AI whose builders do not reach that far -- stays empty, though a farther AI
// would take it.
void Controller::updateAllyClaims(const tak::sim::World& world, uint32_t simTick) {
    constexpr float kAllyHome=900;
    constexpr uint32_t kAllyClaimTicks=2*60*30;   // two minutes of game time
    const auto& deposits=world.manaSpots();
    if (depositClaimedAt_.size()!=deposits.size()) depositClaimedAt_.assign(deposits.size(),simTick);
    allyHeldDeposits_.assign(deposits.size(),0);
    // Every lodestone standing and every site an allied builder is bound for
    // and still getting to, each with the tick it last counted, and each ally's
    // builders that could claim a deposit.
    struct Claim { float x, z; uint32_t tick; };
    std::vector<Claim> claims;
    std::vector<std::vector<const tak::sim::Unit*>> claimers(allyHomes_.size());
    for (const auto& u:world.units()) {
        if (!u.alive() || !u.type || !world.allied(player_,u.player)) continue;
        if (u.type->onMana) claims.push_back({u.x.toFloat(),u.z.toFloat(),simTick});
        const auto progress=builderProgress_.find(u.id);
        if (progress==builderProgress_.end() || !progress->second.stalled)
            for (const auto& order:u.orders)
                if (order.buildType && order.buildType->onMana && (order.buildX.v || order.buildZ.v))
                    claims.push_back({order.buildX.toFloat(),order.buildZ.toFloat(),
                                      progress==builderProgress_.end() ? simTick : progress->second.gainedAt});
        if (u.type->isStructure() || !u.type->isBuilder || u.underConstruction || u.embarked() ||
            u.incapacitated() || !buildsLodestone(u.type)) continue;
        for (size_t a=0;a<allyHomes_.size();++a)
            if (allyHomes_[a].player==u.player) claimers[a].push_back(&u);
    }
    // An enemy's lodestone counts where this side knows of one (knowledge_).
    for (const auto& [id,k]:knowledge_)
        if (k.type->onMana && k.type->isStructure()) claims.push_back({k.x,k.z,simTick});
    const auto home=homeOf(world);
    for (size_t i=0;i<deposits.size();++i) {
        const float sx=deposits[i].first,sz=deposits[i].second;
        // Lodestone placement keeps 44 px between lodestones: a claim that near
        // takes this deposit too.
        bool claimed=false;
        for (const auto& claim:claims) {
            const float dx=claim.x-sx,dz=claim.z-sz;
            if (dx*dx+dz*dz<44*44) { claimed=true; depositClaimedAt_[i]=std::max(depositClaimedAt_[i],claim.tick); }
        }
        if (claimed) continue;
        const float hx=sx-home.first,hz=sz-home.second,own=hx*hx+hz*hz;
        float nearest=std::min(kAllyHome*kAllyHome,own);
        size_t owner=allyHomes_.size();
        for (size_t a=0;a<allyHomes_.size();++a) {
            const float dx=sx-allyHomes_[a].x,dz=sz-allyHomes_[a].z,d=dx*dx+dz*dz;
            if (d<nearest && !world.player(allyHomes_[a].player).defeated) { nearest=d; owner=a; }
        }
        if (owner==allyHomes_.size()) continue;   // nobody's home but perhaps ours
        if (simTick-depositClaimedAt_[i]<kAllyClaimTicks &&
            std::any_of(claimers[owner].begin(),claimers[owner].end(),[&](const tak::sim::Unit* b) {
                return b->type->canFly || world.pathExists(b->type,sx,sz,b->x.toFloat(),b->z.toFloat());
            })) { allyHeldDeposits_[i]=1; continue; }
        allyHeldDeposits_[i]=std::any_of(allyHomes_.begin(),allyHomes_.end(),[&](const AllyHome& ally) {
            const float dx=sx-ally.x,dz=sz-ally.z;
            return &ally!=&allyHomes_[owner] && !world.player(ally.player).defeated && dx*dx+dz*dz<own;
        });
    }
}

// A builder bound for a deposit can stall short of it -- wedged in a crowd, or on
// ground its route does not clear -- while its order still reserves the deposit
// for every ally and claims it for the deposit's owner. In a 4v4 a Zhon handler
// sent 3,300 map units into a human ally's base moved 109 px in 440 s, and no
// ally built there for the rest of the game. So every think follows each allied
// builder whose orders hold a deposit on the task it is on now, the leg the sim
// is working (World::currentLeg): the lodestone itself for an AI's builder, but
// a human's may wait behind a move, a repair or another building. It notes the
// nearest the builder has come to that task's point (its site, or the leg's
// goal) and the tick it last came kGain nearer or was at work -- building, or
// repairing or reclaiming within reach (the sim assigns those jobs before the
// builder gets there) -- and a new task counts as a gain. One that has not for
// kBuilderStallTicks is stalled: placeSite no longer reserves its deposits,
// updateAllyClaims no longer counts them claimed (the claim ended when it last
// gained), and when it is ours this AI stops it, so that it is idle to take
// other work, and never sends it back to those deposits. A builder walking
// across the map gains every few seconds and keeps its claim however long the
// walk. One that arrives to find units on its site gives up on its own within
// the sim's 31 retries (310 ticks).
void Controller::updateBuilderProgress(const tak::sim::World& world, uint32_t simTick,
                                       const CommandSink& sink) {
    constexpr float kGain=64;
    constexpr uint32_t kBuilderStallTicks=60*30;   // a minute of game time
    std::erase_if(stalledSites_,[&](const StalledSite& site) {
        const auto* u=world.unit(site.unit);return !u || !u->alive();
    });
    auto holds=[](const tak::sim::Order& order) {
        return order.buildType && order.buildType->onMana && (order.buildX.v || order.buildZ.v);
    };
    std::unordered_map<int,BuilderProgress> next;
    for (const auto& u:world.units()) {
        if (!u.alive() || !u.type || !world.allied(player_,u.player) ||
            std::none_of(u.orders.begin(),u.orders.end(),holds)) continue;
        // The job, read past a flying builder's leading step-out (its air
        // traffic), which stands where the builder was.
        size_t leg=tak::sim::World::currentLeg(u.orders);
        if (u.orders[leg].stepOut)
            for (size_t i=leg+1;i<u.orders.size();++i)
                if (u.orders[i].goal) { leg=i;break; }
        const auto& task=u.orders[leg];
        const bool site=task.buildType && (task.buildX.v || task.buildZ.v);
        const float sx=(site ? task.buildX : task.x).toFloat(),sz=(site ? task.buildZ : task.z).toFloat();
        const float dx=u.x.toFloat()-sx,dz=u.z.toFloat()-sz,d=std::sqrt(dx*dx+dz*dz);
        BuilderProgress progress{sx,sz,d,simTick,false};
        const auto last=builderProgress_.find(u.id);
        if (last!=builderProgress_.end() && last->second.siteX==sx && last->second.siteZ==sz) {
            progress=last->second;
            if (u.buildSiteId || u.workScriptWorking || d<=progress.best-kGain) {
                progress.best=std::min(progress.best,d);
                progress.gainedAt=simTick;
            }
        }
        progress.stalled=simTick-progress.gainedAt>=kBuilderStallTicks;
        next.emplace(u.id,progress);
        if (!progress.stalled || u.player!=player_) continue;
        bool known=false;
        for (const auto& order:u.orders) {
            if (!holds(order)) continue;
            const float ox=order.buildX.toFloat(),oz=order.buildZ.toFloat();
            const bool listed=std::any_of(stalledSites_.begin(),stalledSites_.end(),[&](const StalledSite& site) {
                return site.unit==u.id && site.x==ox && site.z==oz;
            });
            known=known || listed;
            if (!listed) stalledSites_.push_back({u.id,ox,oz});
        }
        if (known) continue;   // stopped already; the Stop has not reached the world yet
        tak::net::Command stop;stop.kind=tak::net::Cmd::Stop;stop.player=uint8_t(player_);stop.unitId=u.id;
        sink(stop);
    }
    builderProgress_.swap(next);
}

bool Controller::stalledBuilder(int unitId) const {
    const auto progress=builderProgress_.find(unitId);
    return progress!=builderProgress_.end() && progress->second.stalled;
}

// Hard and Absurd remember where a lodestone was lost before it stood: a site of
// ours destroyed unfinished, or the builder bound for the deposit killed. That
// deposit is unsafe for kLostSiteTicks (unsafeDeposit). Without it the next think
// sent the next builder to the same deposit, still the nearest free one: 526
// orders within five seconds of the last at one spot in one game, and a flying
// builder re-placed one site 49 times under an enemy that killed each.
// A killed builder counts only where its death says something about the deposit,
// as the last think saw it: beside the deposit (within kThreatRadius, the danger
// test's own least reach), or on its way there -- the lodestone was the leg it
// was on, it had come kTripGain nearer the deposit than where it was first seen
// with the order, and it was within kTripReach of it. One killed at home before
// it set out, on another leg of its route, or farther off bans nothing: of 54
// builders that died with a lodestone order in five 20-minute rooms, 4 died
// where they stood when ordered and 9 more than 1,000 px from the deposit.
void Controller::updateLostSites(const tak::sim::World& world) {
    std::erase_if(lostSites_,[&](const LostSite& l){return l.until<=now_;});
    auto lost=[&](float x,float z) {
        for (auto& l:lostSites_)
            if (l.x==x && l.z==z) { l.until=now_+kLostSiteTicks; return; }
        lostSites_.push_back({x,z,now_+kLostSiteTicks});
    };
    auto gone=[&](int id) { const auto* u=world.unit(id);return !u || !u->alive(); };
    for (const auto& [id,site]:lodestoneSites_)
        if (gone(id)) lost(site.first,site.second);
    for (const auto& [id,trip]:lodestoneTrips_)
        if (trip.unsafe && gone(id)) lost(trip.x,trip.z);
    lodestoneSites_.clear();
    std::map<int,LodestoneTrip> trips;
    for (const auto& u:world.units()) {
        if (!u.alive() || u.player!=player_ || !u.type) continue;
        if (u.type->isStructure()) {
            if (u.underConstruction && u.type->onMana) lodestoneSites_[u.id]={u.x.toFloat(),u.z.toFloat()};
            continue;
        }
        if (!u.type->isBuilder) continue;
        for (size_t i=0;i<u.orders.size();++i) {
            const auto& order=u.orders[i];
            if (!order.buildType || !order.buildType->onMana || !order.buildType->isStructure() ||
                !(order.buildX.v || order.buildZ.v)) continue;
            const float x=order.buildX.toFloat(),z=order.buildZ.toFloat();
            const float dx=x-u.x.toFloat(),dz=z-u.z.toFloat(),d=std::sqrt(dx*dx+dz*dz);
            const auto seen=lodestoneTrips_.find(u.id);
            const float from=seen!=lodestoneTrips_.end() && seen->second.x==x && seen->second.z==z ? seen->second.from : d;
            const bool onItsWay=i==tak::sim::World::currentLeg(u.orders) && d<=from-kTripGain && d<kTripReach;
            trips[u.id]={x,z,from,d<kThreatRadius || onItsWay};
            break;
        }
    }
    lodestoneTrips_.swap(trips);
}

void Controller::sendDefenders(const tak::sim::World& world, const CommandSink& sink) {
    constexpr float kHomeRadius = 900, kSiteRadius = 400;
    // An enemy shooting into the territory from outside it is answered out to this
    // far from home, and melee defenders give up the chase beyond it.
    constexpr float kPursuitRadius = kHomeRadius + 600, kMeleeRange = 64;
    constexpr int kMaxCommands = 256, kMaxThreats = 16;
    const auto home=homeOf(world);
    std::vector<std::pair<float,float>> sites;
    for (const auto& u:world.units()) {
        if (!u.alive() || !u.type || u.embarked()) continue;
        if (u.player==player_ && (u.type->isStructure() || u.type->commander ||
            categoryOf(u.type)==BuildCat::Factory)) sites.emplace_back(u.x.toFloat(),u.z.toFloat());
    }
    auto local=[&](float x,float z) {
        const float dx=x-home.first,dz=z-home.second;
        if (dx*dx+dz*dz<=kHomeRadius*kHomeRadius) return true;
        return std::any_of(sites.begin(),sites.end(),[&](const auto& site) {
            const float sx=x-site.first,sz=z-site.second;
            return sx*sx+sz*sz<=kSiteRadius*kSiteRadius;
        });
    };
    // Only spotted threats to our territory (sees: in sight or radar now) mobilize
    // the army. Enemy starts and distant enemies revealed by a scout must never
    // become defensive objectives.
    std::vector<std::pair<float,int>> threats;
    for (const auto& e:world.units()) {
        if (!e.alive() || !e.type || e.embarked() || e.underConstruction || e.cloaked ||
            world.isNeutralPlayer(e.player) || world.allied(player_,e.player)) continue;
        if (e.type->weapon.damage<=0 && !e.type->weapon.mindControl &&
            std::none_of(e.type->weapons.begin(),e.type->weapons.end(),[](const auto& weapon) {
                return weapon.damage>0 || weapon.mindControl;
            })) continue;
        const float x=e.x.toFloat(),z=e.z.toFloat();
        if (!local(x,z) || !sees(world,e)) continue;
        const float dx=x-home.first,dz=z-home.second;
        threats.emplace_back(dx*dx+dz*dz,e.id);
    }
    // Whoever is hitting our units inside the territory is a threat too, seen or
    // not ("we know where the arrows come from"): archers that outrange both our
    // sight and the defended radius otherwise pick a parked army apart. Bounded to
    // attackers still within their reach of the victim and near enough to home.
    // A defender we sent out past the territory still names whoever is hitting
    // it; otherwise the archers it was sent at drop off the list as soon as it
    // leaves the radius, and it is retargeted or walked home under fire.
    auto nearHome=[&](float x,float z) {
        const float dx=x-home.first,dz=z-home.second;
        return dx*dx+dz*dz<=kPursuitRadius*kPursuitRadius;
    };
    auto attackerOf=[&](const tak::sim::Unit& v)->const tak::sim::Unit* {
        const auto* e=v.lastHitBy ? world.unit(v.lastHitBy) : nullptr;
        if (!e || !e->alive() || !e->type || e->embarked() || e->underConstruction ||
            world.isNeutralPlayer(e->player) || world.allied(player_,e->player)) return nullptr;
        const float reach=std::max(float(e->type->weapon.range),e->type->maxRange())+64;
        const float dx=e->x.toFloat()-v.x.toFloat(),dz=e->z.toFloat()-v.z.toFloat();
        if (dx*dx+dz*dz>reach*reach || !nearHome(e->x.toFloat(),e->z.toFloat())) return nullptr;
        return e;
    };
    std::vector<int> shooters;
    for (const auto& v:world.units()) {
        if (!v.alive() || v.player!=player_ || !v.lastHitBy || v.embarked()) continue;
        const auto sent=defenders_.find(v.id);
        if (!local(v.x.toFloat(),v.z.toFloat()) &&
            (sent==defenders_.end() || sent->second.returning)) continue;
        const auto* e=attackerOf(v);
        if (!e) continue;
        if (std::any_of(threats.begin(),threats.end(),[&](const auto& t){return t.second==e->id;}) ||
            std::find(shooters.begin(),shooters.end(),e->id)!=shooters.end()) continue;
        shooters.push_back(e->id);
        const float hx=e->x.toFloat()-home.first,hz=e->z.toFloat()-home.second;
        threats.emplace_back(hx*hx+hz*hz,e->id);
    }
    std::sort(threats.begin(),threats.end());
    // Every threat may be answered by the unit it is hitting, and keeps a melee
    // defender already chasing it; only the general response picks among the
    // kMaxThreats nearest home.
    std::vector<int> threatIds;
    for (const auto& t:threats) threatIds.push_back(t.second);
    std::sort(threatIds.begin(),threatIds.end());
    auto isThreat=[&](int id){return std::binary_search(threatIds.begin(),threatIds.end(),id);};
    if (threats.size()>kMaxThreats) threats.resize(kMaxThreats);
    auto shooter=[&](int id){return std::find(shooters.begin(),shooters.end(),id)!=shooters.end();};
    // A chase goes on while its target is a threat, or in sight within the leash.
    auto chased=[&](int id) {
        if (isThreat(id)) return true;
        const auto* e=world.unit(id);
        return e && e->alive() && e->type && !e->embarked() && !e->cloaked &&
            !world.isNeutralPlayer(e->player) && !world.allied(player_,e->player) &&
            nearHome(e->x.toFloat(),e->z.toFloat()) && sees(world,*e);
    };
    for (auto it=defenders_.begin();it!=defenders_.end();) {
        const auto* u=world.unit(it->first);
        if (!u || !u->alive() || u->player!=player_) it=defenders_.erase(it);
        else ++it;
    }
    int issued=0;
    // World order, rather than unordered-map iteration, keeps command emission
    // repeatable. Reachability uses the existing cached component queries.
    for (const auto& u:world.units()) {
        if (issued==kMaxCommands) break;
        if (!u.alive() || u.player!=player_ || !u.type || u.type->isStructure() ||
            u.type->isBuilder || u.type->commander || u.underConstruction || u.embarked() ||
            u.incapacitated() || u.type->weapon.damage<=0) continue;
        auto response=defenders_.find(u.id);
        const bool assigned=response!=defenders_.end();
        const bool melee=std::max(float(u.type->weapon.range),u.type->maxRange())<=kMeleeRange;
        const bool ownsOrder=assigned && std::any_of(u.orders.begin(),u.orders.end(),[&](const auto& order) {
            if (response->second.targetId)
                return !response->second.returning && order.targetId==response->second.targetId && !order.attackMove;
            if (order.targetId || order.buildType || order.patrol || order.guard || order.load || order.unload)
                return false;
            const auto goal=order.missionTarget.value_or(std::pair{order.x,order.z});
            return std::abs(goal.first.toFloat()-response->second.goalX)<1 &&
                std::abs(goal.second.toFloat()-response->second.goalZ)<1 &&
                order.attackMove!=response->second.returning;
        });
        if (!waveFree(u) && !ownsOrder) {
            if (assigned) defenders_.erase(response);
            continue; // preserve unrelated explicit attacks and working orders
        }
        float tx=0,tz=0;
        // A melee defender leaves a flyer in the air alone (it cannot reach it).
        auto answer=[&](const tak::sim::Unit& e) {
            return reachesNow(u,e) && reachablePoint(world,u.type,u.x.toFloat(),u.z.toFloat(),
                e.x.toFloat(),e.z.toFloat(),tx,tz) &&
                (shooter(e.id) ? nearHome(tx,tz) : local(tx,tz));
        };
        // A melee defender goes for the unit hitting it, not for whichever threat
        // is nearest home: a Swordsman shot from 400 away has nothing else to do.
        const tak::sim::Unit* attacker=nullptr;
        if (melee) if (const auto* e=attackerOf(u);e && isThreat(e->id)) attacker=e;
        // It keeps chasing its target while that stays within kPursuitRadius,
        // instead of switching whenever another one edges nearer to home or the
        // target briefly drops off the list (an archer is listed through the
        // last unit it hit, which changes shot by shot in a fight) -- unless it
        // was sent at someone else and is now being hit itself.
        if (ownsOrder && response->second.targetId && chased(response->second.targetId) &&
            (response->second.ownAttacker || !attacker || attacker->id==response->second.targetId))
            continue;
        int targetId=0;
        bool ownAttacker=false;
        if (attacker && answer(*attacker)) {targetId=attacker->id;ownAttacker=true;}
        else if (melee) {
            // Otherwise the threat nearest the unit, so that a melee line spreads over
            // the attackers rather than all walking at the one nearest home.
            std::vector<std::pair<float,int>> near;
            for (const auto& threat:threats) if (const auto* e=world.unit(threat.second)) {
                const float dx=e->x.toFloat()-u.x.toFloat(),dz=e->z.toFloat()-u.z.toFloat();
                near.emplace_back(dx*dx+dz*dz,e->id);
            }
            std::sort(near.begin(),near.end());
            for (const auto& [d,id]:near) if (answer(*world.unit(id))) {targetId=id;break;}
        } else {
            for (const auto& threat:threats) {
                const auto* e=world.unit(threat.second);
                if (e && answer(*e)) {targetId=e->id;break;}
            }
        }
        if (targetId && melee && sees(world,*world.unit(targetId))) {
            // Melee goes straight for the attacker: a direct Attack is not held back
            // by the Defensive stance, so it closes on an archer that backs away. The
            // leash is kPursuitRadius -- once the target leaves it and is no threat,
            // the unit is retargeted or walked home below. Only one in sight is
            // ordered on by id: an attacker only heard is attack-moved on below,
            // to where it stands, as a ranged defender goes.
            if (assigned && ownsOrder && response->second.targetId==targetId) {
                response->second.ownAttacker=ownAttacker;continue;
            }
            DefenseResponse next=assigned ? response->second :
                DefenseResponse{u.x.toFloat(),u.z.toFloat(),tx,tz};
            next.goalX=tx;next.goalZ=tz;next.returning=false;next.targetId=targetId;
            next.ownAttacker=ownAttacker;
            defenders_[u.id]=next;
            tak::net::Command c;c.kind=tak::net::Cmd::Attack;c.player=uint8_t(player_);
            c.unitId=u.id;c.targetId=targetId;
            sink(c);++issued;
        } else if (targetId) {
            if (assigned && !response->second.returning && !response->second.targetId) {
                const float dx=tx-response->second.goalX,dz=tz-response->second.goalZ;
                const float ux=u.x.toFloat()-tx,uz=u.z.toFloat()-tz;
                if (dx*dx+dz*dz<96*96 && (ownsOrder ||
                    (u.orders.empty() && ux*ux+uz*uz<=48*48))) continue;
            }
            DefenseResponse next=assigned ? response->second :
                DefenseResponse{u.x.toFloat(),u.z.toFloat(),tx,tz};
            next.goalX=tx;next.goalZ=tz;next.returning=false;next.targetId=0;next.ownAttacker=false;
            defenders_[u.id]=next;
            emit(sink,tak::net::Cmd::AttackMove,u.id,"",tx,tz);++issued;
        } else if (assigned) {
            auto& next=response->second;
            const float dx=u.x.toFloat()-next.homeX,dz=u.z.toFloat()-next.homeZ;
            if (u.orders.empty() && (next.returning || dx*dx+dz*dz<=48*48)) {
                defenders_.erase(response);continue;
            }
            if (ownsOrder && next.returning) continue;
            next.goalX=next.homeX;next.goalZ=next.homeZ;next.returning=true;next.targetId=0;next.ownAttacker=false;
            emit(sink,tak::net::Cmd::Move,u.id,"",next.homeX,next.homeZ);++issued;
        }
    }
}

// A build order can come to nothing -- the site taken or blocked by the time the
// builder gets there -- and the site, still the nearest free one, was given straight
// back to the idle builder: on Frey River Plain an Absurdly Defensive Aramon sent
// builders to one keep site, think after think, from 50 minutes while its store
// filled. So an order whose builder is idle (or doing something else) with nothing
// started on the site marks that site refused for kRefusedSiteTicks, and placeSite
// passes it over meanwhile.
void Controller::updateIssuedBuilds(const tak::sim::World& world, uint32_t simTick) {
    constexpr uint32_t kRefusedSiteTicks = 60 * 30;
    std::erase_if(refusedSites_, [&](const RefusedSite& r) { return simTick >= r.until; });
    std::erase_if(issuedBuilds_, [&](const IssuedBuild& b) {
        if (b.tick == simTick) return false;          // not yet reached the world
        const auto* u = world.unit(b.unit);
        if (!u || !u->alive()) return true;
        if (u->buildSiteId) return true;              // started
        if (std::any_of(u->orders.begin(), u->orders.end(), [&](const auto& order) {
                return order.buildType == b.type && std::abs(order.buildX.toFloat() - b.x) < 12 &&
                    std::abs(order.buildZ.toFloat() - b.z) < 12; }))
            return false;                             // still on its way
        // Not bound for it any more: did the building go up there?
        const bool started = std::any_of(world.units().begin(), world.units().end(), [&](const auto& s) {
            return s.alive() && s.player == player_ && s.type == b.type &&
                std::abs(s.x.toFloat() - b.x) < 12 && std::abs(s.z.toFloat() - b.z) < 12;
        });
        if (!started) refusedSites_.push_back({b.x, b.z, simTick + kRefusedSiteTicks});
        return true;
    });
}

// A factory can wait for ever on a queue head it cannot place: a Creon Defensive
// AI's Workshop on Frey River Plain held a Mechanic from t=200 s to the end with
// its output beside a rock, so its builder count never grew (the queued Mechanic
// counts as one) and its income stayed on one deposit. Retail keeps a factory
// trying too; the unbounded levels, which rely on every producer working, give
// that order up after kQueueStallTicks and train something else there for a while.
void Controller::unqueueStalled(const tak::sim::World& world, uint32_t simTick,
                                const CommandSink& sink) {
    constexpr uint32_t kQueueStallTicks = 30 * 30, kRefusedTicks = 120 * 30;
    std::unordered_map<int, QueueWait> next;
    for (const auto& u : world.units()) {
        if (!u.alive() || u.player != player_ || !u.type || !u.type->isStructure() ||
            u.underConstruction || u.buildQueue.empty() || !u.buildQueue.front() || u.productionSiteId) continue;
        const auto* head = u.buildQueue.front();
        QueueWait wait{head, simTick};
        if (const auto it = queueWait_.find(u.id); it != queueWait_.end() && it->second.type == head)
            wait = it->second;
        if (simTick - wait.since < kQueueStallTicks) { next.emplace(u.id, wait); continue; }
        tak::net::Command c; c.kind = tak::net::Cmd::Unqueue; c.player = uint8_t(player_);
        c.unitId = u.id; c.targetId = 1;
        std::snprintf(c.type, sizeof c.type, "%s", head->id.c_str());
        sink(c);
        refused_[u.id] = {head, simTick + kRefusedTicks};
    }
    queueWait_.swap(next);
    std::erase_if(refused_, [&](const auto& r) { return simTick >= r.second.since; });
}

// A defensive AI keeps its whole army at home, and with no cap on its size the army
// fills the base: a Creon Defensive AI on Ulasem Arena stood 62 units on its two
// factories' output pads and its builders' sites, and from then on built nothing
// with a full store. So idle fighters inside the base -- within kBaseRadius of home,
// or of its outermost building near home plus a margin -- walk out to the first free
// spot on the rings around it, where they still guard it (sendDefenders sends them
// out from there and back). Outward along their own bearing first, so they do not
// cross the base, then turning further from it -- a base in a map corner has only
// a quarter of each ring. Never onto the enemy's side of the map (enemySide).
void Controller::stationArmy(const tak::sim::World& world, std::vector<int>& assigned,
                             const CommandSink& sink) {
    constexpr float kBaseRadius = 420, kSpacing = 48, kNearHome = 900, kReach = 1600;
    constexpr int kMaxCommands = 64;
    const auto [hx, hz] = homeOf(world);
    float base = kBaseRadius;
    int32_t footprint = 0;
    // Spots already taken: where own units stand idle or are walking to (a unit on
    // another order -- a fight, a build -- holds no spot). Bucketed in kSpacing
    // cells, so each probe checks its 3x3 neighbourhood rather than the whole army.
    constexpr float kTaken = 40;
    std::unordered_map<int64_t, std::vector<std::pair<float, float>>> taken;
    auto cellOf = [](float v) { return int64_t(std::floor(v / kSpacing)); };
    auto take = [&](float x, float z) { taken[(cellOf(x) << 32) ^ (cellOf(z) & 0xffffffff)].emplace_back(x, z); };
    auto isTaken = [&](float x, float z) {
        const int64_t cx = cellOf(x), cz = cellOf(z);
        for (int64_t i = cx - 1; i <= cx + 1; ++i)
            for (int64_t j = cz - 1; j <= cz + 1; ++j) {
                const auto it = taken.find((i << 32) ^ (j & 0xffffffff));
                if (it != taken.end() && std::any_of(it->second.begin(), it->second.end(), [&](const auto& t) {
                        const float tx = t.first - x, tz = t.second - z; return tx*tx + tz*tz < kTaken*kTaken; }))
                    return true;
            }
        return false;
    };
    for (const auto& u : world.units()) {
        if (!u.alive() || u.player != player_ || !u.type) continue;
        if (!u.type->isStructure()) {
            if (u.embarked() || u.underConstruction) continue;
            // A move's route may queue waypoints ahead of its goal: the spot is the last.
            const auto* goal = u.orders.empty() ? nullptr : &u.orders.back();
            // A factory PARK has no point until its handler first runs: its spot is the unit's.
            // A flyer's PARK flies to the point its first run fixed (attempts set).
            if (!goal || (goal->park && !(goal->flightMoveMission && goal->park->attempts)))
                take(u.x.toFloat(), u.z.toFloat());
            else if (!goal->targetId && !goal->buildType && !goal->reclaimFeat && !goal->repairTarget)
                take(goal->x.toFloat(), goal->z.toFloat());
            continue;
        }
        footprint += u.type->footX + u.type->footZ;
    }
    // The base builds out to retail's base radius (placeSite), so buildings that
    // far out still count as the base; stationing troops among them would cover
    // their sites again.
    const float nearHome = std::max(kNearHome, float(tak::sim::retailAiBaseRadius(footprint, 1)) + 300);
    for (const auto& u : world.units()) {
        if (!u.alive() || u.player != player_ || !u.type || !u.type->isStructure()) continue;
        // A remote lodestone is not the base.
        if (u.type->onMana) continue;
        const float dx = u.x.toFloat() - hx, dz = u.z.toFloat() - hz, d = std::sqrt(dx*dx + dz*dz);
        if (d <= nearHome)
            base = std::max(base, d + float(std::max(u.type->footX, u.type->footZ)) * 8 + 96);
    }
    // Nor in a building's open yard or in front of a factory door (doorStrips):
    // canPlace lets a unit stand on a yard's open cells.
    const BaseLayout layout = baseLayout(world);
    int issued = 0;
    for (const auto& u : world.units()) {
        if (issued == kMaxCommands) break;
        if (!u.alive() || u.player != player_ || !u.type || u.type->isStructure() || u.type->isBuilder ||
            u.type->commander || u.underConstruction || u.embarked() || u.incapacitated() ||
            !idle(u) || u.buildSiteId || defenders_.count(u.id) ||
            (!u.type->canFly && u.type->domain == tak::sim::UnitType::Domain::Water) ||
            std::find(assigned.begin(), assigned.end(), u.id) != assigned.end()) continue;
        const float ux = u.x.toFloat(), uz = u.z.toFloat();
        const float dx = ux - hx, dz = uz - hz;
        if (dx*dx + dz*dz >= base*base) continue;
        const float bearing = dx*dx + dz*dz > 1 ? detmath::atan2(dz, dx) : 0.0f;
        bool moved = false;
        for (float r = base + kSpacing; r <= base + kReach && !moved; r += kSpacing)
            for (int i = 0; i < 13 && !moved; ++i) {
                // 0, +30, -30, +60, -60 ... 180 degrees from the unit's own bearing.
                const float a = bearing + float((i + 1) / 2) * (i % 2 ? 0.523598776f : -0.523598776f);
                const float x = hx + detmath::cos(a) * r, z = hz + detmath::sin(a) * r;
                if (isTaken(x, z) || enemySide(x, z, hx, hz) || !world.canPlace(u.type, x, z) ||
                    (!u.type->canFly && layout.blocks(footprintRect(u.type, x, z))) ||
                    (!u.type->canFly && !world.pathExists(u.type, x, z, ux, uz))) continue;
                emit(sink, tak::net::Cmd::Move, u.id, "", x, z);
                take(x, z);
                assigned.push_back(u.id);
                ++issued;
                moved = true;
            }
        static const bool kStationLog = tak::devEnv("TAK_AI_STATION") != nullptr;
        if (!moved && kStationLog)   // a fighter with no free spot it can walk to
            std::fprintf(stderr, "    STATION FAIL %s#%d at (%.0f,%.0f) d=%.0f base=%.0f\n", u.type->id.c_str(), u.id, ux, uz,
                         std::sqrt(dx*dx+dz*dz), base);
    }
}

// What this AI sees: whatever its own and its allies' units see (the shared team
// vision a player's screen shows), by the sim's per-player view counters
// (World::playerSees, viewCount) -- never the display fog, which is local.
bool Controller::teamSees(const tak::sim::World& world, const tak::sim::Unit& u) const {
    return std::any_of(team_.begin(), team_.end(), [&](int p) { return world.playerSees(p, u); });
}

bool Controller::teamViews(const tak::sim::World& world, int cx, int cz) const {
    if (world.viewCounts().empty()) return true;   // no view plane: everything is in sight, as playerSees
    return std::any_of(team_.begin(), team_.end(), [&](int p) { return world.viewCount(p, cx, cz) > 0; });
}

int Controller::sectorOf(float x, float z) const {
    if (!sectorsX_ || sectorSize_ <= 0) return -1;
    const int sx = std::clamp(int(x / sectorSize_), 0, sectorsX_ - 1), sz = std::clamp(int(z / sectorSize_), 0, sectorsZ_ - 1);
    return sx + sz * sectorsX_;
}

// A unit's projected 32 px view cell (World::playerSees' corner rule, at its centre).
static std::pair<int, int> viewCell(const tak::sim::Unit& u) {
    const tak::sim::Fixed y = u.type->canFly ? u.flightY : u.groundY;
    const int px = int16_t(uint32_t(u.x.v) >> 16), py = int16_t(uint32_t(y.v) >> 16), pz = int16_t(uint32_t(u.z.v) >> 16);
    return {px >> 5, (pz - (py >> 1)) >> 5};
}

static int64_t gridKey(int cx, int cz) { return (int64_t(cx) << 32) ^ int64_t(uint32_t(cz)); }
static int floorDiv(int64_t a, int b) { return int(a >= 0 ? a / b : -((-a + b - 1) / b)); }

// The team's radar now: every kRadarCell cell whose centre lies within
// radardistance of a finished unit of the team (our client used to clear that
// whole disc of fog, through terrain; since the retail fog display radar only puts
// minimap dots, but this AI model is unchanged) -- with units sharing a 32 px spot
// and a range counted once. radarSeen_ keeps every cell ever in radar. Radar is mostly on buildings, which do not move: only the
// discs that came or went since the last think are counted off and on, on cells
// coarse enough that a builder walking about recounts a few hundred.
void Controller::stampRadar(int qx, int qz, int r, int delta) {
    constexpr int c = kRadarCell, half0 = kRadarCell / 2;
    const int w = radarW_, h = radarH_;
    const int64_t x = int64_t(qx) * 32 + 16, z = int64_t(qz) * 32 + 16, r2 = int64_t(r) * r;
    const int z0 = std::max(0, -floorDiv(-(z - r - half0), c)), z1 = std::min(h - 1, floorDiv(z + r - half0, c));
    for (int cz = z0; cz <= z1; ++cz) {
        const int64_t dz = int64_t(cz) * c + half0 - z;
        if (dz * dz > r2) continue;
        const int64_t half = int64_t(std::sqrt(double(r2 - dz * dz)));
        const int x0 = std::max(0, -floorDiv(-(x - half - half0), c)), x1 = std::min(w - 1, floorDiv(x + half - half0, c));
        const size_t row = size_t(cz) * size_t(w);
        for (int cx = x0; cx <= x1; ++cx) radar_[row + size_t(cx)] = uint16_t(radar_[row + size_t(cx)] + delta);
        if (delta > 0 && x0 <= x1) std::fill_n(radarSeen_.begin() + ptrdiff_t(row + size_t(x0)), x1 - x0 + 1, uint8_t(1));
    }
}

void Controller::updateRadar(const tak::sim::World& world) {
    const int w = (world.mapW() * 16 + kRadarCell - 1) / kRadarCell, h = (world.mapH() * 16 + kRadarCell - 1) / kRadarCell;
    if (w != radarW_ || h != radarH_) {
        radarW_ = w; radarH_ = h;
        radarSeen_.assign(size_t(std::max(w, 0)) * size_t(std::max(h, 0)), 0);
        radar_.assign(radarSeen_.size(), 0);
        radarSources_.clear();
    }
    if (radar_.empty()) return;
    std::vector<std::tuple<int, int, int>> sources;
    for (const auto& u : world.units()) {
        if (!u.alive() || !u.type || u.underConstruction ||
            std::find(team_.begin(), team_.end(), u.player) == team_.end()) continue;
        const int r = int(world.radarDistance(*u.type));
        if (r > 0) sources.emplace_back(u.x.floorInt() >> 5, u.z.floorInt() >> 5, r);
    }
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    std::vector<std::tuple<int, int, int>> gone, came;
    std::set_difference(radarSources_.begin(), radarSources_.end(), sources.begin(), sources.end(), std::back_inserter(gone));
    std::set_difference(sources.begin(), sources.end(), radarSources_.begin(), radarSources_.end(), std::back_inserter(came));
    for (const auto& [qx, qz, r] : gone) stampRadar(qx, qz, r, -1);
    for (const auto& [qx, qz, r] : came) stampRadar(qx, qz, r, +1);
    radarSources_.swap(sources);
}

bool Controller::radarAt(float x, float z) const {
    if (!(x >= 0) || !(z >= 0)) return false;
    const int cx = int(x) / kRadarCell, cz = int(z) / kRadarCell;
    return cx < radarW_ && cz < radarH_ && radar_[size_t(cz) * size_t(radarW_) + size_t(cx)];
}

bool Controller::knowsGround(const tak::sim::World& world, float x, float z) const {
    if (world.mapRevealedFor(player_) || world.roomLosOff() || world.exploredAt(player_, x, z)) return true;
    if (!(x >= 0) || !(z >= 0)) return false;
    const int cx = int(x) / kRadarCell, cz = int(z) / kRadarCell;
    return cx < radarW_ && cz < radarH_ && radarSeen_[size_t(cz) * size_t(radarW_) + size_t(cx)];
}

bool Controller::inView(const tak::sim::World& world, float x, float z) const {
    if (world.roomLosOff() || radarAt(x, z)) return true;
    const float mapX = float(world.mapW()) * 16, mapZ = float(world.mapH()) * 16;
    return teamViews(world, int(std::clamp(x, 0.0f, std::max(mapX - 1, 0.0f))) >> 5,
                     int(std::clamp(z, 0.0f, std::max(mapZ - 1, 0.0f))) >> 5);
}

// Before the first view pass (tick 0) only radar and FULL VISION show anything:
// World::playerSees would answer "in sight" for every unit (updateKnowledge).
bool Controller::sees(const tak::sim::World& world, const tak::sim::Unit& u) const {
    if (!u.alive() || !u.type || u.cloaked || u.embarked()) return false;
    if (world.roomLosOff()) return true;
    if (const int x = u.x.floorInt(), z = u.z.floorInt(); x >= 0 && z >= 0 && x / kRadarCell < radarW_ && z / kRadarCell < radarH_ &&
        radar_[size_t(z / kRadarCell) * size_t(radarW_) + size_t(x / kRadarCell)]) return true;
    return !(world.viewCounts().empty() && world.tickCount() == 0) && teamSees(world, u);
}

template <class F>
void Controller::forDepositsNear(const tak::sim::World& world, float x, float z, float r, F&& fn) const {
    const auto& deposits = world.manaSpots();
    const int x0 = int(std::floor((x - r) / kDepositCell)), x1 = int(std::floor((x + r) / kDepositCell));
    const int z0 = int(std::floor((z - r) / kDepositCell)), z1 = int(std::floor((z + r) / kDepositCell));
    for (int cz = z0; cz <= z1; ++cz)
        for (int cx = x0; cx <= x1; ++cx) {
            const int64_t key = gridKey(cx, cz);
            auto it = std::lower_bound(depositGrid_.begin(), depositGrid_.end(), std::pair<int64_t, int>{key, INT_MIN});
            for (; it != depositGrid_.end() && it->first == key; ++it) {
                const auto& d = deposits[size_t(it->second)];
                const float dx = d.first - x, dz = d.second - z;
                if (dx * dx + dz * dz < r * r) fn(size_t(it->second));
            }
        }
}

bool Controller::unsafeDeposit(float x, float z) const {
    constexpr float kSameDeposit = 32;
    if (std::any_of(lostSites_.begin(), lostSites_.end(), [&](const LostSite& l) {
            const float dx = l.x - x, dz = l.z - z; return dx * dx + dz * dz < kSameDeposit * kSameDeposit; }))
        return true;
    if (threatsAt_ != now_) {
        threatsAt_ = now_;
        threats_.clear();
        for (const auto& [id, k] : knowledge_) {
            if (k.unfinished || (k.type->weapon.damage <= 0 && !k.type->weapon.mindControl &&
                std::none_of(k.type->weapons.begin(), k.type->weapons.end(),
                             [](const auto& w) { return w.damage > 0 || w.mindControl; }))) continue;
            // No reach past two cells: the search below looks no farther.
            const float reach = std::max(float(k.type->weapon.range), k.type->maxRange()) + 64;
            threats_.emplace_back(gridKey(int(std::floor(k.x / kThreatCell)), int(std::floor(k.z / kThreatCell))),
                                  Threat{k.x, k.z, std::clamp(reach, kThreatRadius, float(kThreatCell) * 2)});
        }
        // knowledge_ is in unit-id order; the stable sort keeps it within a cell.
        std::stable_sort(threats_.begin(), threats_.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    if (threats_.empty()) return false;
    const int cx = int(std::floor(x / kThreatCell)), cz = int(std::floor(z / kThreatCell));
    for (int gz = cz - 2; gz <= cz + 2; ++gz)
        for (int gx = cx - 2; gx <= cx + 2; ++gx) {
            const int64_t key = gridKey(gx, gz);
            auto it = std::lower_bound(threats_.begin(), threats_.end(), key,
                                       [](const auto& entry, int64_t k) { return entry.first < k; });
            for (; it != threats_.end() && it->first == key; ++it) {
                const float dx = it->second.x - x, dz = it->second.z - z;
                if (dx * dx + dz * dz <= it->second.r * it->second.r) return true;
            }
        }
    return false;
}

// A site the real placement test refused (World::canPlace: everything there, in
// view or not -- the answer a player's build order gets). For a lodestone site,
// an enemy lodestone out of view within the 44 px one deposit allows is learned:
// known at the next think (heard, not sighted: an attack-move to it, never an
// attack by id), and the deposit taken at once.
void Controller::learnSiteBlockers(const tak::sim::World& world, const tak::sim::UnitType* t, float x, float z) const {
    if (!t || !t->onMana) return;
    for (const HiddenLode& h : hiddenLodes_) {
        const float dx = h.x - x, dz = h.z - z;
        if (dx * dx + dz * dz >= 44 * 44) continue;
        if (std::find(learned_.begin(), learned_.end(), h.id) == learned_.end()) learned_.push_back(h.id);
        forDepositsNear(world, x, z, 24, [&](size_t i) { if (i < depositTaken_.size()) depositTaken_[i] = 1; });
    }
}

// Once per think, before anything reads knowledge_: every enemy unit the team
// sees now (sees: the fog rule's view), and every one that hit our units within
// kHeardTicks from within its reach, is noted where it stands, and so is every
// enemy lodestone a refused site was learned to stand on (learnSiteBlockers). A
// remembered unit not seen this think is forgotten when the spot it was last seen
// on is in view again (it died, left or cloaked there), a mobile one also once
// kMobileMemoryTicks have passed, and any that is now ours or an ally's, or whose
// owner is beaten or no longer an enemy. Then the deposits held taken (an enemy
// lodestone known on them), and the exploration state: each start and each
// sector in view now is stamped with this think (a sector's scout losses are
// then forgotten), and the starts' owners and bases are worked out (starts_,
// startGone_).
// Before the first tick there is no view plane yet (World::updateViewCounts runs
// after a tick), and World::playerSees then answers "in sight" for everything:
// the think at tick 0 (player 0's) sees only what radar shows (sees) instead --
// unless the room plays with FULL VISION, where every seat sees everything.
void Controller::updateKnowledge(const tak::sim::World& world) {
    constexpr uint32_t kHeardTicks = 5 * 30;
    constexpr float kStartFound = 600;
    const bool blind = world.viewCounts().empty() && world.tickCount() == 0 && !world.roomLosOff();
    const int players = world.numPlayers();
    team_.clear();
    // The players whose sight this one's plane holds (World::viewShareMask:
    // every ally but a passive neutral).
    const uint16_t sharing = world.viewShareMask(player_);
    for (int p = 0; p < players; ++p)
        if (p == player_ || (p < 16 && (sharing >> p & 1))) team_.push_back(p);
    updateRadar(world);
    std::vector<uint8_t> enemies(size_t(std::max(players, 0)), 0), allies(enemies.size(), 0);
    for (int p = 0; p < players; ++p) {
        enemies[size_t(p)] = p != player_ && !world.isNeutralPlayer(p) && !world.allied(player_, p);
        allies[size_t(p)] = world.allied(player_, p);
    }
    auto enemy = [&](int p) { return p >= 0 && p < players && enemies[size_t(p)]; };
    auto note = [&](const tak::sim::Unit& u, bool sighted) {
        const auto [cx, cz] = viewCell(u);
        knowledge_[u.id] = {u.type, u.player, u.x.toFloat(), u.z.toFloat(), strength(u), cx, cz, now_, u.underConstruction,
                            sighted};
    };
    const auto& deposits = world.manaSpots();
    if (depositGrid_.size() != deposits.size()) {
        depositGrid_.clear();
        for (size_t i = 0; i < deposits.size(); ++i)
            depositGrid_.emplace_back(gridKey(int(std::floor(deposits[i].first / kDepositCell)),
                                              int(std::floor(deposits[i].second / kDepositCell))), int(i));
        std::sort(depositGrid_.begin(), depositGrid_.end());
    }
    hiddenLodes_.clear();
    depositAllied_.assign(deposits.size(), 0);
    for (const auto& u : world.units()) {
        if (!u.alive() || !u.type || u.embarked()) continue;
        if (!enemy(u.player)) {
            if (u.player >= 0 && u.player < players && allies[size_t(u.player)] && u.type->onMana && u.type->isStructure())
                forDepositsNear(world, u.x.toFloat(), u.z.toFloat(), 44, [&](size_t i) { depositAllied_[i] = 1; });
            continue;
        }
        if (sees(world, u)) note(u, true);
        else if (u.type->onMana && u.type->isStructure()) hiddenLodes_.push_back({u.id, u.x.toFloat(), u.z.toFloat()});
    }
    for (const auto& v : world.units()) {
        if (!v.alive() || v.player != player_ || !v.lastHitBy || !v.attackedTick ||
            world.tickCount() > v.attackedTick + kHeardTicks) continue;
        const auto* e = world.unit(v.lastHitBy);
        if (!e || !e->alive() || !e->type || e->embarked() || !enemy(e->player)) continue;
        if (const auto k = knowledge_.find(e->id); k != knowledge_.end() && k->second.seenAt == now_ && k->second.sighted)
            continue;   // in sight as well: noted as seen
        const float reach = std::max(float(e->type->weapon.range), e->type->maxRange()) + 64;
        const float dx = e->x.toFloat() - v.x.toFloat(), dz = e->z.toFloat() - v.z.toFloat();
        if (dx * dx + dz * dz <= reach * reach) note(*e, false);
    }
    for (const int id : learned_) {
        const auto* e = world.unit(id);
        if (!e || !e->alive() || !e->type || !enemy(e->player)) continue;
        if (const auto k = knowledge_.find(id); k != knowledge_.end() && k->second.seenAt == now_) continue;
        note(*e, false);
    }
    learned_.clear();
    for (auto it = knowledge_.begin(); it != knowledge_.end();) {
        const auto& k = it->second;
        bool forget = false;
        if (k.seenAt != now_) {
            const auto* u = world.unit(it->first);
            forget = (u && u->alive() && u->player != k.player && !enemy(u->player)) || !enemy(k.player) ||
                     world.player(k.player).defeated ||
                     (!k.type->isStructure() && now_ - k.seenAt > kMobileMemoryTicks) ||
                     world.roomLosOff() || radarAt(k.x, k.z) || teamViews(world, k.cx, k.cz);
        }
        it = forget ? knowledge_.erase(it) : std::next(it);
    }
    // Deposits held taken: an enemy lodestone known within 44 px (World::canPlace's
    // one-lodestone-per-deposit rule).
    depositTaken_.assign(deposits.size(), 0);
    for (const auto& [id, k] : knowledge_)
        if (k.type->onMana && k.type->isStructure())
            forDepositsNear(world, k.x, k.z, 44, [&](size_t i) { depositTaken_[i] = 1; });
    std::erase_if(unreachable_, [&](const auto& e) { return e.second <= now_; });
    const float mapX = float(world.mapW()) * 16, mapZ = float(world.mapH()) * 16;
    if (!sectorsX_ && mapX > 0 && mapZ > 0) {
        sectorSize_ = std::max(1024.0f, std::max(mapX, mapZ) / 32);
        sectorsX_ = std::max(1, int(std::ceil(mapX / sectorSize_)));
        sectorsZ_ = std::max(1, int(std::ceil(mapZ / sectorSize_)));
        sectorSeen_.assign(size_t(sectorsX_) * size_t(sectorsZ_), 0);
        sectorClaim_.assign(sectorSeen_.size(), 0);
        // A deposit draws builders, and so an enemy: a sector holding some is
        // worth more of a look than an empty corner. Every deposit on the map is
        // ground a player sees once it is explored, and is never taken away.
        sectorLure_.assign(sectorSeen_.size(), 1.0f);
        std::vector<int> deposits(sectorSeen_.size(), 0);
        for (const auto& [x, z] : world.manaSpots())
            if (const int sec = sectorOf(x, z); sec >= 0) ++deposits[size_t(sec)];
        for (size_t i = 0; i < deposits.size(); ++i) sectorLure_[i] += 0.5f * float(std::min(deposits[i], 4));
    }
    if (!startsSized_) {
        startsSized_ = true;
        // Our allies' starts follow the enemies' (where each opened: its screen
        // shows a player those from the start), held gone while their owner
        // plays: tracked only for when the ally is beaten and its base stands
        // empty (startVacated_).
        allyStartsFrom_ = starts_.size();
        for (int p = 0; p < players; ++p) {
            if (p == player_ || !world.allied(player_, p) || world.isNeutralPlayer(p)) continue;
            if (std::any_of(starts_.begin(), starts_.end(), [p](const StartSpot& s) { return s.owner == p; }) ||
                std::none_of(world.units().begin(), world.units().end(),
                             [p](const auto& u) { return u.alive() && u.player == p; }))
                continue;
            const auto [ax, az] = anchorOf(world, p);
            starts_.push_back({ax, az, p});
        }
        startSeen_.assign(starts_.size(), 0);
        startClaim_.assign(starts_.size(), 0);
        startGone_.assign(starts_.size(), 0);
        startBase_.assign(starts_.size(), 0);
        startVacated_.assign(starts_.size(), 0);
        for (size_t i = allyStartsFrom_; i < starts_.size(); ++i) startGone_[i] = 1;
    }
    if (!blind) {
        for (size_t i = 0; i < starts_.size(); ++i)
            if (inView(world, starts_[i].x, starts_[i].z)) startSeen_[i] = now_ + 1;
        // A sector is in sight when its centre or a point halfway to a corner is
        // (inView at each, its cells worked out once); its scout losses are then
        // forgotten.
        if (sectorProbes_.size() != sectorSeen_.size() * 5) {
            sectorProbes_.assign(sectorSeen_.size() * 5, Probe{-2, 0, 0});   // -2: no such sector
            for (size_t s = 0; s < sectorSeen_.size(); ++s) {
                float x = 0, z = 0;
                if (!sectorCentre(int(s), mapX, mapZ, x, z)) continue;
                const float q = sectorSize_ / 4;
                const std::pair<float, float> points[5] = {{x, z}, {x - q, z - q}, {x + q, z - q}, {x - q, z + q}, {x + q, z + q}};
                for (int i = 0; i < 5; ++i) {
                    const auto [px, pz] = points[i];
                    const int rx = int(px) / kRadarCell, rz = int(pz) / kRadarCell;
                    sectorProbes_[s * 5 + size_t(i)] = {px >= 0 && pz >= 0 && rx < radarW_ && rz < radarH_ ? rz * radarW_ + rx : -1,
                        int(std::clamp(px, 0.0f, std::max(mapX - 1, 0.0f))) >> 5, int(std::clamp(pz, 0.0f, std::max(mapZ - 1, 0.0f))) >> 5};
                }
            }
        }
        const bool everything = world.roomLosOff();
        for (size_t s = 0; s < sectorSeen_.size(); ++s) {
            if (sectorProbes_[s * 5].radar == -2) continue;
            for (int i = 0; i < 5; ++i) {
                const Probe& p = sectorProbes_[s * 5 + size_t(i)];
                if (everything || (p.radar >= 0 && radar_[size_t(p.radar)]) || teamViews(world, p.cx, p.cz)) {
                    sectorSeen_[s] = now_ + 1;
                    slotLosses_.erase(int(s));
                    break;
                }
            }
        }
    }
    // The starts. A known enemy building near one is that enemy's base: the start
    // takes its owner when the room did not say whose it is. A start whose owner
    // is beaten, or (owner unknown) that is in sight with no enemy building known
    // near it, holds no enemy now; nor does one of unknown owner once every enemy
    // still in the game has been found at another. Who is still in the game a
    // player reads off the score table, which lists every player with a unit left.
    std::vector<uint8_t> present(size_t(std::max(players, 0)), 0), located(present.size(), 0);
    for (const auto& u : world.units())
        if (u.alive() && u.player >= 0 && u.player < players) present[size_t(u.player)] = 1;
    auto beaten = [&](int p) { return world.player(p).defeated || !present[size_t(p)]; };
    for (size_t i = 0; i < starts_.size(); ++i) {
        auto& spot = starts_[i];
        // A start gone is followed on only as a beaten player's base (an
        // enemy's or an ally's): whether someone has moved in.
        const bool fallen = spot.owner >= 0 && spot.owner < players && beaten(spot.owner);
        if (startGone_[i] && !startVacated_[i] && !fallen) continue;
        int nearest = -1;
        float best = kStartFound * kStartFound;
        for (const auto& [id, k] : knowledge_) {
            if (!k.type->isStructure() || !enemy(k.player)) continue;
            const float dx = k.x - spot.x, dz = k.z - spot.z, d = dx * dx + dz * dz;
            if (d <= best) { best = d; nearest = k.player; }
        }
        startBase_[i] = nearest >= 0;
        if (startVacated_[i]) continue;
        if (fallen) {
            // Its owner beaten just now. Scouts it killed there no longer keep
            // it backed off (nor do those still on their way: sendScouts).
            startGone_[i] = 1;
            startVacated_[i] = now_ + 1;
            slotLosses_.erase(-1 - int(i));
            continue;
        }
        if (spot.owner < 0 && nearest >= 0) spot.owner = nearest;
        if (spot.owner >= 0)
            startGone_[i] = !enemy(spot.owner) || beaten(spot.owner);
        else
            startGone_[i] = startSeen_[i] == now_ + 1;
        if (!startGone_[i] && spot.owner >= 0) located[size_t(spot.owner)] = 1;
    }
    bool everyoneFound = false;
    for (int p = 0; p < players; ++p)
        if (enemy(p) && present[size_t(p)] && !world.player(p).defeated) {
            everyoneFound = located[size_t(p)];
            if (!everyoneFound) break;
        }
    enemyStarts_.clear();
    for (size_t i = 0; i < starts_.size(); ++i) {
        if (everyoneFound && starts_[i].owner < 0) startGone_[i] = 1;
        if (!startGone_[i]) enemyStarts_.emplace_back(starts_[i].x, starts_[i].z);
    }
}

bool Controller::backedOff(int slot) const {
    constexpr uint32_t kBackoffTicks = 2 * 60 * 30;
    const auto it = slotLosses_.find(slot);
    return it != slotLosses_.end() && it->second.count > 0 &&
           now_ < it->second.last + (kBackoffTicks << std::min(it->second.count - 1, 3));
}

int Controller::lossesOnRoute(float x, float z, float tx, float tz) const {
    constexpr float kLossRouteRadius = 400;
    const float dx = tx - x, dz = tz - z, len2 = dx * dx + dz * dz;
    return int(std::count_if(losses_.begin(), losses_.end(), [&](const Loss& l) {
        const float t = len2 > 0 ? std::clamp(((l.x - x) * dx + (l.z - z) * dz) / len2, 0.0f, 1.0f) : 0.0f;
        const float ex = x + dx * t - l.x, ez = z + dz * t - l.z;
        return ex * ex + ez * ez <= kLossRouteRadius * kLossRouteRadius;
    }));
}

// The middle of a sector's part of the map (sectors along the far edges are cut short).
bool Controller::sectorCentre(int s, float mapX, float mapZ, float& x, float& z) const {
    const int sx = s % sectorsX_, sz = s / sectorsX_;
    const float x0 = float(sx) * sectorSize_, z0 = float(sz) * sectorSize_;
    const float x1 = std::min(mapX, x0 + sectorSize_), z1 = std::min(mapZ, z0 + sectorSize_);
    if (x1 <= x0 || z1 <= z0) return false;
    x = (x0 + x1) / 2; z = (z0 + z1) / 2;
    return true;
}

// Where a unit of `type` at (x,z) explores next. A start never yet in sight that
// may hold an enemy comes first, the nearest (one another party is bound for
// counts four times as far); then the sector out of sight longest for its
// distance (never seen counts as kNeverSeen more), times its lure, skipping
// those in sight within kRecent; one another party holds counts for a quarter
// and one beside such a sector for half, so parties spread over the map.
// A beaten player's base (startVacated_, an enemy's or an ally's) rejoins the
// starts once out of sight kVacatedRecheck (counted from its owner's defeat when
// not seen since): its free deposits may draw someone else in. One party at a
// time looks in (a claimed one is passed over), and none while an enemy
// building is known near it: someone has moved in, and the strikes take that
// from there.
// A scouting party (`scout`) passes over a start whose base is already found
// (the strikes take it from there, and armies still go to see the spot) and a
// slot backed off after scouts died going there, and counts a start as farther,
// a sector as less worth it, for each loss along the way. Reachability is tried
// in that order for the first kProbes, and a spot this type cannot reach is
// passed over for kUnreachableTicks.
bool Controller::exploreTarget(const tak::sim::World& world, const tak::sim::UnitType* type, float x, float z,
                               float& tx, float& tz, int& slot, bool scout) {
    if (stayHome_) return false;   // Passive looks nowhere: not with scouts, nor with its army or fleet
    constexpr uint32_t kNeverSeen = 3 * 60 * 30, kRecent = 60 * 30, kUnreachableTicks = 3 * 60 * 30;
    constexpr int kProbes = 12;
    constexpr uint32_t kVacatedRecheck = 5 * 60 * 30;
    const float mapX = float(world.mapW()) * 16, mapZ = float(world.mapH()) * 16;
    auto reach = [&](int s, float px, float pz) {
        if (unreachable_.count({s, type})) return false;
        if (reachablePoint(world, type, x, z, px, pz, tx, tz)) { slot = s; return true; }
        unreachable_[{s, type}] = now_ + kUnreachableTicks;
        return false;
    };
    std::vector<std::pair<float, int>> ranked;
    for (size_t i = 0; i < starts_.size() && i < startSeen_.size(); ++i) {
        if (startVacated_[i]) {
            if (startBase_[i] || startClaim_[i] > now_ ||
                now_ + 1 - std::max(startSeen_[i], startVacated_[i]) < kVacatedRecheck)
                continue;
        } else if (startSeen_[i] || startGone_[i]) continue;
        if (scout && (startBase_[i] || backedOff(-1 - int(i)))) continue;
        const float dx = starts_[i].x - x, dz = starts_[i].z - z;
        float d = std::sqrt(dx * dx + dz * dz) * (startClaim_[i] > now_ ? 4.0f : 1.0f);
        if (scout) d *= 1 + 2 * float(lossesOnRoute(x, z, starts_[i].x, starts_[i].z));
        ranked.emplace_back(d, int(i));
    }
    std::sort(ranked.begin(), ranked.end());
    int probes = 0;
    for (const auto& [d, i] : ranked) {
        if (++probes > kProbes) return false;
        if (reach(-1 - i, starts_[size_t(i)].x, starts_[size_t(i)].z)) return true;
    }
    ranked.clear();
    auto claimed = [&](int sx, int sz) {
        return sx >= 0 && sz >= 0 && sx < sectorsX_ && sz < sectorsZ_ && sectorClaim_[size_t(sx + sz * sectorsX_)] > now_;
    };
    for (int s = 0; s < int(sectorSeen_.size()); ++s) {
        const uint32_t seen = sectorSeen_[size_t(s)];
        if (seen && now_ + 1 - seen < kRecent) continue;
        if (scout && backedOff(s)) continue;
        float cx = 0, cz = 0;
        if (!sectorCentre(s, mapX, mapZ, cx, cz)) continue;
        const float age = float(seen ? now_ + 1 - seen : now_ + kNeverSeen);
        const float dx = cx - x, dz = cz - z;
        float score = age / (1 + std::sqrt(dx * dx + dz * dz) / 3000);
        if (size_t(s) < sectorLure_.size()) score *= sectorLure_[size_t(s)];
        const int sx = s % sectorsX_, sz = s / sectorsX_;
        if (claimed(sx, sz)) score *= 0.25f;
        else if (claimed(sx - 1, sz) || claimed(sx + 1, sz) || claimed(sx, sz - 1) || claimed(sx, sz + 1)) score *= 0.5f;
        if (scout) score /= 1 + float(lossesOnRoute(x, z, cx, cz));
        ranked.emplace_back(-score, s);
    }
    std::sort(ranked.begin(), ranked.end());
    for (const auto& [score, s] : ranked) {
        if (++probes > kProbes) break;
        float cx = 0, cz = 0;
        sectorCentre(s, mapX, mapZ, cx, cz);
        if (reach(s, cx, cz)) return true;
    }
    return false;
}

void Controller::claimSlot(int slot, uint32_t ticks) {
    if (slot < 0) startClaim_[size_t(-1 - slot)] = now_ + ticks;
    else sectorClaim_[size_t(slot)] = now_ + ticks;
}

int Controller::explore(const tak::sim::World& world, const std::vector<int>& ids, const CommandSink& sink,
                        bool scout, int* slotOut) {
    constexpr uint32_t kClaimTicks = 3 * 60 * 30;
    // Each kind of mover (flyer or not, and domain) goes where its first unit can.
    std::vector<const tak::sim::Unit*> units;
    for (int id : ids)
        if (const auto* u = world.unit(id); u && u->alive() && u->type) units.push_back(u);
    std::stable_sort(units.begin(), units.end(), [](const auto* a, const auto* b) {
        return !a->type->canFly && b->type->canFly;
    });
    std::vector<uint8_t> done(units.size(), 0);
    int ordered = 0;
    for (size_t first = 0; first < units.size(); ++first) {
        if (done[first]) continue;
        const auto* lead = units[first]->type;
        auto sameKind = [&](const tak::sim::Unit* u) {
            return u->type->canFly == lead->canFly && u->type->domain == lead->domain;
        };
        float ex = 0, ez = 0;
        int slot = 0;
        const bool found = exploreTarget(world, lead, units[first]->x.toFloat(), units[first]->z.toFloat(), ex, ez, slot, scout);
        for (size_t i = first; i < units.size(); ++i) {
            if (done[i] || !sameKind(units[i])) continue;
            done[i] = 1;
            float tx = 0, tz = 0;
            if (!found || !reachablePoint(world, units[i]->type, units[i]->x.toFloat(), units[i]->z.toFloat(), ex, ez, tx, tz))
                continue;
            emit(sink, tak::net::Cmd::AttackMove, units[i]->id, "", tx, tz);
            ++ordered;
        }
        if (found) {
            claimSlot(slot, kClaimTicks);
            if (slotOut) *slotOut = slot;
        }
    }
    return ordered;
}

// Scouting parties: a lone fast fighter at a time sent to look, at the pace
// exploreLevelFor sets (more parties on a large map at the levels that keep
// scouting) -- or, to a slot where scouts have died, a party a fighter larger for
// each loss there (up to four). A scout that is idle again (there, or
// stopped on the way) or out longer than kTripTicks goes on to the next spot at
// the levels that keep scouting, else rejoins the army. A scout killed on its
// trip is a loss (losses_, slotLosses_): later picks steer round it, and while
// the level's maxLosses fell within kLossWindow no new party goes out.
void Controller::sendScouts(const tak::sim::World& world, uint32_t simTick, const CommandSink& sink,
                            const StrikeBoard& board, std::vector<Fighter>& pool) {
    constexpr uint32_t kTripTicks = 4 * 60 * 30, kRetryTicks = 10 * 30, kClaimTicks = 3 * 60 * 30;
    constexpr uint32_t kLossWindow = 5 * 60 * 30, kLossMemory = 10 * 60 * 30;
    constexpr int kMaxParty = 4;
    const ExploreLevel level = exploreLevelFor(diff_);
    const bool scouting = !stayHome_ && level.maxScouts > 0 && (dp_.attack || board.knownAhead);
    std::erase_if(losses_, [&](const Loss& l) { return simTick - l.tick > kLossMemory; });
    std::set<int> parties;
    for (auto it = scouts_.begin(); it != scouts_.end();) {
        auto& trip = it->second;
        const auto* u = world.unit(it->first);
        if (!u || !u->alive() || u->player != player_) {
            if (!u || !u->alive()) ++scoutsLost_;
            losses_.push_back({trip.x, trip.z, simTick});
            // One sent to a start before its owner was beaten counts against
            // that owner, not the empty base.
            const size_t start = size_t(-1 - trip.slot);
            if (trip.slot >= 0 || start >= startVacated_.size() || trip.since + 1 >= startVacated_[start]) {
                auto& l = slotLosses_[trip.slot];
                ++l.count; l.last = simTick;
                if (trip.slot < 0 && start < startVacated_.size() && startVacated_[start]) ++vacatedLost_;
            }
            it = scouts_.erase(it);
            continue;
        }
        if (defenders_.count(it->first)) { it = scouts_.erase(it); continue; }
        trip.x = u->x.toFloat(); trip.z = u->z.toFloat();
        if (!idle(*u) && simTick - trip.since <= kTripTicks) { parties.insert(trip.party); ++it; continue; }
        int slot = trip.slot;
        if (scouting && level.keep && explore(world, {u->id}, sink, true, &slot)) {
            trip = {simTick, slot, trip.x, trip.z, u->id};
            if (slot < 0 && startVacated_[size_t(-1 - slot)]) ++vacatedTrips_;
            parties.insert(u->id);
            ++scoutsSent_; ++it;
        } else it = scouts_.erase(it);   // back to the army from the next think
    }
    const auto recent = std::count_if(losses_.begin(), losses_.end(), [&](const Loss& l) {
        return simTick - l.tick < kLossWindow;
    });
    // The levels that keep scouting send more parties on a larger map: one more for
    // each eight sectors across, up to three more.
    const int maxParties = level.maxScouts +
        (level.keep ? std::min(int(std::sqrt(float(sectorSeen_.size()))) / 8, 3) : 0);
    if (!scouting || pool.empty() || simTick < uint32_t(level.firstMin) * 60 * 30 || simTick < nextScoutTick_ ||
        int(parties.size()) >= maxParties || recent >= level.maxLosses) return;
    // A flyer (it sees over cliffs and crosses water), else the fastest fighter in
    // the pool, the cheapest of those as fast. A ship only when the pool holds
    // nothing else and too few ships to sail as a fleet (sendWaves): a fleet
    // explores the sea as a wave, and a scout taken from it could keep it home.
    auto ship = [&](const Fighter& f) {
        const auto* t = world.unit(f.id)->type;
        return !t->canFly && t->domain == tak::sim::UnitType::Domain::Water;
    };
    const int ships = int(std::count_if(pool.begin(), pool.end(), ship));
    auto faster = [&](const Fighter& a, const Fighter& b) {
        if (ship(a) != ship(b)) return ship(b);
        const auto* ta = world.unit(a.id)->type;
        const auto* tb = world.unit(b.id)->type;
        if (ta->canFly != tb->canFly) return ta->canFly;
        if (ta->maxVel != tb->maxVel) return ta->maxVel > tb->maxVel;
        if (ta->buildCost != tb->buildCost) return ta->buildCost < tb->buildCost;
        return a.id < b.id;
    };
    const auto best = std::min_element(pool.begin(), pool.end(), faster);
    if (ship(*best) && ships >= (diff_ == Difficulty::Easy ? 2 : 3)) return;
    const Fighter lead = *best;
    const auto* leadType = world.unit(lead.id)->type;
    float ex = 0, ez = 0;
    int slot = 0;
    if (!exploreTarget(world, leadType, lead.x, lead.z, ex, ez, slot, true)) { nextScoutTick_ = simTick + kRetryTicks; return; }
    // Where scouts have died, a party: the lead's kind nearest it, one more per loss.
    std::vector<Fighter> party{lead};
    const auto lost = slotLosses_.find(slot);
    const int want = 1 + std::min(lost == slotLosses_.end() ? 0 : lost->second.count, kMaxParty - 1);
    if (want > 1) {
        std::vector<std::pair<float, Fighter>> near;
        for (const auto& f : pool) {
            const auto* t = world.unit(f.id)->type;
            if (f.id == lead.id || t->canFly != leadType->canFly || t->domain != leadType->domain) continue;
            const float dx = f.x - lead.x, dz = f.z - lead.z;
            near.emplace_back(dx * dx + dz * dz, f);
        }
        std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) {
            return a.first < b.first || (a.first == b.first && a.second.id < b.second.id);
        });
        for (size_t i = 0; i < near.size() && int(party.size()) < want; ++i) party.push_back(near[i].second);
    }
    std::vector<int> sent;
    for (const auto& f : party) {
        float tx = 0, tz = 0;
        if (!reachablePoint(world, world.unit(f.id)->type, f.x, f.z, ex, ez, tx, tz)) continue;
        emit(sink, tak::net::Cmd::AttackMove, f.id, "", tx, tz);
        scouts_[f.id] = {simTick, slot, f.x, f.z, lead.id};
        sent.push_back(f.id);
    }
    if (sent.empty()) { nextScoutTick_ = simTick + kRetryTicks; return; }
    claimSlot(slot, kClaimTicks);
    scoutsSent_ += int(sent.size());
    if (slot < 0 && startVacated_[size_t(-1 - slot)]) vacatedTrips_ += int(sent.size());
    scouted_ = true;
    nextScoutTick_ = simTick + uint32_t(level.everySec) * 30;
    std::erase_if(pool, [&](const Fighter& f) { return std::find(sent.begin(), sent.end(), f.id) != sent.end(); });
}

// A fighter sent out with an attack-move -- a wave, a raid, an exploring group --
// is not commanded again while it holds it (waveFree), and nothing promises the
// march ends: two bodies holding orders lock each other at a channel mouth, one
// stands in front of a settled crowd, a route is never answered. Such a unit
// stood where it was for the rest of the game: 55 of one side's fighters before
// it won a long game, 148 by its end. A player clicks again, and so does this:
// it notes where each fighter last made headway -- came kHeadway from that spot
// -- and stops one that has held its march for kStrikeStallTicks without any.
// Idle again, it is a striker free for its next site, or back in the pool, from
// the next think (sendWaves), and bodies waiting on its order are let through.
// A column on the march passes kHeadway every few seconds, slow as it is.
// Never a unit that fights: one with a target, or a weapon of its with one.
// A unit standing at a fight -- one of ours fights, or was hit since the last
// think, in the site cells around it or around the point it marches on -- may
// be waiting its turn behind a siege line, so it is given four times as long;
// not for ever, for the bodies locked beside a fight that goes on all game
// stand as the others do (the 55 stood in a base still being fought over).
// Each stop without headway since doubles the wait before the next, up to eight
// minutes: a unit a fresh order does not free is not ordered about every minute.
// Defenders and scouts keep their own rules (sendDefenders; sendScouts'
// kTripTicks). Ours, not retail, which watches no unit's headway: a strike
// squad's think sends a marching member its MOVE again one time in ten or in
// fifty, whatever it is doing (0x40e180, classicwaves.h).
void Controller::updateStrikeProgress(const tak::sim::World& world, uint32_t simTick, const CommandSink& sink) {
    constexpr float kHeadway = 64;
    constexpr uint32_t kStrikeStallTicks = 60 * 30;   // a minute of game time, as a builder's (updateBuilderProgress)
    constexpr int kFightDoublings = 2, kMaxDoublings = 3;   // four minutes at a fight; eight at most
    constexpr int kMaxStops = 64;                           // stops in one think, for the wire frame
    auto cellAt = [](float x, float z) {
        return std::pair{int(std::floor(x / kSiteCell)), int(std::floor(z / kSiteCell))};
    };
    auto cellOf = [&](const tak::sim::Unit& u) { return cellAt(u.x.toFloat(), u.z.toFloat()); };
    auto fighting = [](const tak::sim::Unit& u) {
        return std::any_of(u.slotTarget.begin(), u.slotTarget.end(), [](int32_t target) { return target != 0; }) ||
            std::any_of(u.orders.begin(), u.orders.end(), [](const tak::sim::Order& o) { return o.targetId && !o.guard; });
    };
    std::vector<std::pair<int, int>> fights;
    for (const auto& u : world.units())
        if (u.alive() && u.player == player_ && u.type && !u.underConstruction && !u.embarked() &&
            (fighting(u) || u.attackedTick > strikeProgressAt_))
            fights.push_back(cellOf(u));
    std::sort(fights.begin(), fights.end());
    fights.erase(std::unique(fights.begin(), fights.end()), fights.end());
    auto fightNear = [&](std::pair<int, int> cell) {
        for (int i = -1; i <= 1; ++i)
            for (int j = -1; j <= 1; ++j)
                if (std::binary_search(fights.begin(), fights.end(), std::pair{cell.first + i, cell.second + j})) return true;
        return false;
    };
    // Around the unit, or around the point it marches on.
    auto atFight = [&](const tak::sim::Unit& u) {
        if (fightNear(cellOf(u))) return true;
        for (auto o = u.orders.rbegin(); o != u.orders.rend(); ++o)
            if (o->attackMove) {
                const auto goal = o->missionTarget.value_or(std::pair{o->x, o->z});
                return fightNear(cellAt(goal.first.toFloat(), goal.second.toFloat()));
            }
        return false;
    };
    // World order, and lookups only in the map: the stops come out the same way every run.
    std::unordered_map<int, StrikeProgress> next;
    next.reserve(strikeProgress_.size());
    int stops = 0;
    for (const auto& u : world.units()) {
        if (!u.alive() || u.player != player_ || !u.type || u.underConstruction || u.embarked() ||
            u.type->isStructure() || u.type->isBuilder || u.type->weapon.damage <= 0) continue;
        const float x = u.x.toFloat(), z = u.z.toFloat();
        StrikeProgress progress{x, z, simTick, 0};
        if (const auto last = strikeProgress_.find(u.id); last != strikeProgress_.end()) {
            progress = last->second;
            const float dx = x - progress.x, dz = z - progress.z;
            if (dx * dx + dz * dz >= kHeadway * kHeadway) progress = {x, z, simTick, 0};
        }
        const bool marching = !defenders_.count(u.id) && !scouts_.count(u.id) &&
            std::any_of(u.orders.begin(), u.orders.end(), [](const tak::sim::Order& o) { return o.attackMove; });
        if (!marching || u.incapacitated() || fighting(u)) progress.since = simTick;
        else {
            // The wait away from a fight first: only a unit that has stood that
            // long is looked up among the fights.
            const uint32_t stood = simTick - progress.since;
            const auto wait = [&](int more) { return kStrikeStallTicks << std::min(progress.stops + more, kMaxDoublings); };
            if (stops < kMaxStops && stood >= wait(0) && stood >= wait(atFight(u) ? kFightDoublings : 0)) {
                tak::net::Command stop; stop.kind = tak::net::Cmd::Stop; stop.player = uint8_t(player_); stop.unitId = u.id;
                sink(stop);
                ++stops;
                progress.since = simTick;
                if (progress.stops < UINT8_MAX) ++progress.stops;
            }
        }
        next.emplace(u.id, progress);
    }
    strikeProgress_.swap(next);
    strikeProgressAt_ = simTick;
}

// Pool idle (non-builder) fighters; once a strike force has gathered (waveSize, per
// difficulty), send the whole group at the best site on the board (scanTargets,
// pickSite) so the wave arrives together rather than trickling in; with nothing
// known it can reach and hurt, to explore (exploreTarget): an unchecked enemy
// start first, else the map sector longest out of sight. Fighters already sent
// (strikers_) that are idle again go on to the next site at once, or explore on
// from where they stand. Scouting parties go out besides (sendScouts).
void Controller::sendWaves(const tak::sim::World& world, uint32_t simTick,
                           const CommandSink& sink, const std::vector<int>& assigned) {
    std::erase_if(strikers_, [&](int id) { const auto* u = world.unit(id); return !u || !u->alive() || u->player != player_; });
    std::erase_if(pursuits_, [&](const auto& p) {
        return !knowledge_.count(p.first) || (p.second.until <= simTick && simTick + 1 - p.second.last > 60 * 30);
    });
    std::vector<Fighter> pool, loose;   // loose: strikers free for their next site
    for (const auto& u : world.units()) {
        if (!u.alive() || u.player!=player_ || !u.type || u.underConstruction || u.embarked() ||
            u.type->isStructure() || u.type->isBuilder || u.type->weapon.damage<=0 || !waveFree(u) ||
            defenders_.count(u.id) || scouts_.count(u.id) ||
            std::find(assigned.begin(), assigned.end(), u.id) != assigned.end()) continue;
        (strikers_.count(u.id) ? loose : pool).push_back({u.id, u.x.toFloat(), u.z.toFloat(), strength(u)});
    }
    if (pool.empty() && loose.empty() && scouts_.empty()) return;
    const StrikeBoard board = scanTargets(world);
    const auto home = homeOf(world);
    sendScouts(world, simTick, sink, board, pool);
    // A defensive level goes out only to scout and clean up; one that no longer
    // clearly is ahead calls its strikers home.
    if (!dp_.attack && !board.ahead) {
        for (const auto& f : loose) { emit(sink, tak::net::Cmd::Move, f.id, "", home.first, home.second); strikers_.erase(f.id); }
        return;
    }
    const bool split = diff_ == Difficulty::Hard || diff_ == Difficulty::Absurd;
    std::vector<float> taken(board.sites.size(), 0.0f);
    // The best site for `group`, tried for each kind of mover in it (ground first)
    // from where the one of that kind nearest the kind's centre stands, until one
    // has a site it can reach and hurt: a fleet or the flyers go on where the army
    // cannot.
    auto pickFor = [&](const std::vector<Fighter>& group, float force, bool raid) {
        struct Kind { const tak::sim::UnitType* type; float x = 0, z = 0; int n = 0; const Fighter* at = nullptr; float d = 0; };
        std::vector<Kind> kinds;
        auto kindOf = [&](const tak::sim::UnitType* t) {
            return std::find_if(kinds.begin(), kinds.end(), [&](const Kind& k) {
                return k.type->canFly == t->canFly && k.type->domain == t->domain; });
        };
        for (const auto& f : group) {
            const auto* t = world.unit(f.id)->type;
            auto k = kindOf(t);
            if (k == kinds.end()) { kinds.push_back({t}); k = kinds.end() - 1; }
            k->x += f.x; k->z += f.z; ++k->n;
        }
        for (auto& k : kinds) { k.x /= float(k.n); k.z /= float(k.n); }
        for (const auto& f : group) {
            auto k = kindOf(world.unit(f.id)->type);
            const float dx = f.x - k->x, dz = f.z - k->z, d = dx * dx + dz * dz;
            if (!k->at || d < k->d) { k->at = &f; k->d = d; }
        }
        std::stable_sort(kinds.begin(), kinds.end(), [](const Kind& a, const Kind& b) { return !a.type->canFly && b.type->canFly; });
        for (const auto& k : kinds)
            if (const int i = pickSite(world, board, k.at->x, k.at->z, force, k.type, raid, taken); i >= 0) return i;
        return -1;
    };
    // Send `group` (from the front, at most `limit` units) at the best site from
    // where it stands. Hard and Absurd split a force that outweighs a site: each
    // site gets its need with half again (at least a quarter of the force) and the
    // rest goes on to the next, up to four. A group of strikers that cannot match
    // even a third of the best site's need, while not clearly ahead, comes home and
    // rejoins the pool; with `needMet` (the defensive levels' clean-up) a group that
    // does not match the best site's need stays home, or comes home. Returns the
    // units ordered, -1 when the group was outmatched (and, striking, sent home).
    auto launch = [&](const std::vector<Fighter>& group, bool raid, int limit, bool striking, bool needMet) {
        if (group.empty()) return 0;
        float force = 0;
        for (const auto& f : group) force += f.s;
        const int maxSites = split ? 4 : 1;
        int ordered = 0, sites = 0;
        size_t next = 0;
        float remaining = force;
        while (next < group.size() && ordered < limit && sites < maxSites) {
            const int i = pickFor(group, remaining, raid);
            if (i < 0) break;
            const auto& site = board.sites[size_t(i)];
            const bool outmatched = sites == 0 && ((needMet && siteNeed(site) > force) ||
                (striking && diff_ != Difficulty::Easy && !board.winning && siteNeed(site) > 3 * force));
            if (outmatched && striking)
                for (const auto& f : group) { emit(sink, tak::net::Cmd::Move, f.id, "", home.first, home.second); strikers_.erase(f.id); }
            if (outmatched) return -1;
            const float want = sites + 1 == maxSites ? 1e30f : std::max(siteNeed(site) * 1.5f - taken[size_t(i)], force / 4);
            float sent = 0;
            while (next < group.size() && ordered < limit && (sent == 0 || sent < want)) {
                const auto& f = group[next++];
                if (!strikeOrder(world, *world.unit(f.id), site, simTick, sink)) continue;
                strikers_.insert(f.id);
                sent += f.s;
                ++ordered;
            }
            taken[size_t(i)] += sent;
            remaining -= sent;
            ++sites;
        }
        return ordered;
    };
    // Explore with `group` (at most `limit` of it); its units stay strikers, so
    // each goes on from where it gets to.
    auto exploreWith = [&](const std::vector<Fighter>& group, size_t limit) {
        std::vector<int> ids;
        for (size_t i = 0; i < group.size() && i < limit; ++i) ids.push_back(group[i].id);
        const int ordered = explore(world, ids, sink);
        if (ordered) {
            ++explorations_;
            for (int id : ids) strikers_.insert(id);
        }
        return ordered;
    };
    // Strikers that are idle again go straight for the next site, a bunch at a time
    // (a kGroupCell grid), without waiting for the next push: the army keeps working
    // down the board until every enemy it knows of is gone, then explores on.
    if (!loose.empty()) {
        constexpr float kGroupCell = 1024;
        std::map<std::pair<int, int>, std::vector<Fighter>> groups;
        for (const auto& f : loose) groups[{int(std::floor(f.x / kGroupCell)), int(std::floor(f.z / kGroupCell))}].push_back(f);
        int budget = 256;
        for (const auto& [cell, group] : groups) {
            if (budget <= 0) break;
            int ordered = board.sites.empty() ? 0 : launch(group, false, budget, true, !dp_.attack);
            // Nothing known it can reach or hurt: explore from here, else back to
            // the pool for the next push.
            if (!ordered) ordered = exploreWith(group, size_t(budget));
            if (!ordered) for (const auto& f : group) strikers_.erase(f.id);
            budget -= std::max(ordered, 0);
        }
    }
    if (pool.empty()) return;
    // A defensive level cleaning up keeps at least half its fighters home; with
    // nothing known that half explores.
    if (!dp_.attack) {
        const size_t half = (strikers_.size() + pool.size()) / 2;
        const size_t out = std::min(pool.size(), half > strikers_.size() ? half - strikers_.size() : 0);
        const std::vector<Fighter> party(pool.end() - std::ptrdiff_t(out), pool.end());
        if (!party.empty() && !launch(party, false, 256, false, true)) exploreWith(party, 256);
        return;
    }
    if (diff_ == Difficulty::Easy &&
        (simTick < 4*60*30 || (scouted_ && simTick-lastRaidTick_ < 2*60*30 && !board.winning))) return;
    // The pool's objective: the best site for it, or with nothing known it can
    // reach and hurt, the spot to explore that its first non-flyer (else its
    // first flyer) of some kind can reach.
    float poolForce = 0;
    for (const auto& f : pool) poolForce += f.s;
    struct Ready { Fighter f; float x, z; };
    std::vector<Ready> idle;
    // Ships the pool's objective lies inland of -- the army's site, or the spot it
    // explores, or none it can find -- sail as a fleet of their own once there are
    // enough: at a site they can reach, else exploring. Knowing of an enemy base
    // the army can walk to (in sight or on radar from the start), or with nothing
    // left the army can reach to explore, they otherwise idled in port all game.
    auto sailFleet = [&] {
        std::vector<Fighter> fleet;
        for (const auto& f : pool) {
            const auto* t = world.unit(f.id)->type;
            if (!t->canFly && t->domain == tak::sim::UnitType::Domain::Water &&
                std::none_of(idle.begin(), idle.end(), [&](const Ready& r) { return r.f.id == f.id; }))
                fleet.push_back(f);
        }
        if (int(fleet.size()) < (diff_ == Difficulty::Easy ? 2 : 3) || (scouted_ && simTick - lastRaidTick_ < 25 * 30)) return;
        if (launch(fleet, false, 256, false, false) <= 0 && !exploreWith(fleet, 256)) return;
        lastRaidTick_ = simTick; scouted_ = true;
    };
    StrikeSite objective;
    const int pick = board.sites.empty() ? -1 : pickFor(pool, poolForce, false);
    const bool exploring = pick < 0;
    int exploreSlot = 0;
    if (!exploring) objective = board.sites[size_t(pick)];
    else {
        // The first unit of each kind of mover (non-flyers first) in turn, until one
        // has somewhere to explore: a fleet or the flyers go on where the army cannot.
        std::vector<const Fighter*> leads;
        for (bool flyers : {false, true})
            for (const auto& f : pool) {
                const auto* t = world.unit(f.id)->type;
                if (t->canFly != flyers) continue;
                if (std::none_of(leads.begin(), leads.end(), [&](const Fighter* l) {
                        const auto* lt = world.unit(l->id)->type;
                        return lt->canFly == t->canFly && lt->domain == t->domain;
                    }))
                    leads.push_back(&f);
            }
        if (std::none_of(leads.begin(), leads.end(), [&](const Fighter* f) {
                return exploreTarget(world, world.unit(f->id)->type, f->x, f->z, objective.x, objective.z, exploreSlot);
            }))
        { sailFleet(); return; }
    }
    for (const auto& f : pool) {
        const auto* u = world.unit(f.id);
        float tx=0,tz=0;
        if (reachablePoint(world,u->type,f.x,f.z,objective.x,objective.z,tx,tz,sitePad(objective,u->type)))
            idle.push_back({f,tx,tz});
    }
    sailFleet();
    if (idle.empty()) return;
    // Send idle[from, from+n): at sites from the board, or at the spot explored.
    auto send = [&](size_t from, size_t n, bool raid, bool seaborneOnly, auto&& seaborne) {
        std::vector<Fighter> group;
        for (size_t i = from; i < idle.size() && group.size() < n; ++i)
            if (!seaborneOnly || seaborne(world.unit(idle[i].f.id))) group.push_back(idle[i].f);
        if (!exploring) return launch(group, raid, int(n), false, false);
        int sent = 0;
        for (size_t i = from; i < idle.size() && size_t(sent) < n; ++i) {
            if (seaborneOnly && !seaborne(world.unit(idle[i].f.id))) continue;
            emit(sink, tak::net::Cmd::AttackMove, idle[i].f.id, "", idle[i].x, idle[i].z);
            strikers_.insert(idle[i].f.id);
            ++sent;
        }
        if (sent) { claimSlot(exploreSlot, 3 * 60 * 30); ++explorations_; }
        return sent;
    };

    // The "big push" army scales with mana INCOME: a rich economy masses a large army
    // before it commits, a lean one strikes with less. So a strong AI stops trickling
    // its units into the enemy and instead builds an overwhelming force. A tapped-out
    // economy still waits for the minimum wave instead of feeding in single units.
    // Clearly ahead, it commits whatever is idle once that outweighs the defence
    // where it is going, to finish the enemy (not one unit at a time at a Monarch).
    const auto& me = world.player(player_);
    float income = me.income;   // scale the force with actual income
    int bigPush = std::clamp(dp_.waveSize + int(income * 0.25f), dp_.waveSize, 60);

    if (diff_ == Difficulty::Easy) bigPush = std::min(bigPush, 8);
    if (board.winning && !exploring) {
        float force = 0;
        for (const auto& r : idle) force += r.f.s;
        if (force >= siteNeed(objective)) bigPush = 1;
    }
    // A faction may have only a few permitted ships (Easy Aramon has two),
    // while its inland army inflates the economy-based wave target. Let a
    // small fleet sail without waiting for troops that cannot cross the sea.
    // Amphibious (hover) units are that fleet too when no ground unit can reach
    // the objective -- Taros has no ships, only the Rictus and Lich. Where the
    // ground army can walk there, they stay with it for the big push.
    const bool groundReaches=std::any_of(idle.begin(),idle.end(),[&](const auto& r) {
        const auto* u=world.unit(r.f.id);
        return u && !u->type->canFly && u->type->domain==tak::sim::UnitType::Domain::Ground;
    });
    auto seaborne=[&](const tak::sim::Unit* u) {
        return u && !u->type->canFly && (u->type->domain==tak::sim::UnitType::Domain::Water ||
            (!groundReaches && u->type->domain==tak::sim::UnitType::Domain::Hover));
    };
    const int navalCount=int(std::count_if(idle.begin(),idle.end(),[&](const auto& r) {
        return seaborne(world.unit(r.f.id));
    }));
    if (int(idle.size())<bigPush && navalCount>=(diff_==Difficulty::Easy ? 2 : 3) &&
        (!scouted_ || simTick-lastRaidTick_>=25*30)) {
        send(0, 256, false, true, seaborne);
        lastRaidTick_=simTick;scouted_=true;
        return;
    }
    if (int(idle.size()) >= bigPush) {
        // Commit the army -- but cap commands per think so a huge force (a near-cap
        // game, or a stress test with thousands of units) doesn't emit one giant tick
        // bundle that blows the wire frame limit. The rest stay idle and deploy over
        // the next few thinks. kMaxWaveCmds keeps even several coincident AIs well
        // under the cap.
        constexpr int kMaxWaveCmds = 256;
        send(0, size_t(std::min(int(idle.size()), diff_ == Difficulty::Easy ? 8 : kMaxWaveCmds)), false, false, seaborne);
        lastRaidTick_ = simTick;      // let the freshly-built stragglers regroup, don't raid next
        scouted_ = true;
        raidersSincePush_=0;
        return;
    }

    // Still mustering the big army -- but keep HARASSING so the enemy is pressured and
    // revealed instead of the AI turtling behind a wall. Peel off a small raiding party
    // (leaving a home core to keep growing toward the big push), rate-limited so the
    // army isn't bled away piecemeal. A raid picks its site for its own small force,
    // so it goes for an exposed lodestone or builder rather than into the defences;
    // with nothing known, it explores.
    constexpr uint32_t kRaidCooldown = 25 * 30;   // ~25s between raids
    int homeCore = std::max(dp_.waveSize, bigPush / 3);   // never raid below this reserve
    bool canRaid = dp_.raidSize > 0 &&
                   raidersSincePush_ + dp_.raidSize <= std::max(dp_.raidSize,bigPush/3) &&
                   int(idle.size()) >= homeCore + dp_.raidSize &&
                   simTick - lastRaidTick_ >= kRaidCooldown;
    if (canRaid) {
        send(0, size_t(dp_.raidSize), true, false, seaborne);
        raidersSincePush_+=dp_.raidSize;
        lastRaidTick_ = simTick;
    }
}

// Debug builds, TAK_AI_EXPLORE: once a game minute, how much of the map this
// AI's player has explored, how many enemies it has left, what it knows and how
// many scouting parties and exploring groups it has sent and how many scouts it
// has lost, and when each beaten enemy's start was last in sight; once, when no
// enemy is left. A measuring hook only: it changes
// nothing the AI does.
void Controller::logExploration(const tak::sim::World& world, uint32_t simTick) {
    int left = 0;
    for (int p = 0; p < world.numPlayers(); ++p)
        if (p != player_ && !world.isNeutralPlayer(p) && !world.allied(player_, p) && !world.player(p).defeated) ++left;
    if (!left && !loggedWin_) {
        loggedWin_ = true;
        std::fprintf(stderr, "AIWIN p=%d lvl=%d t=%u\n", player_, int(diff_), simTick);
    }
    const uint32_t minute = simTick / 1800;
    if (minute + 1 == loggedMinute_) return;
    loggedMinute_ = minute + 1;
    const auto& explored = world.navigationExploration();
    size_t n = 0;
    for (const auto m : explored) n += (m >> player_) & 1;
    int units = 0;
    for (const auto& u : world.units()) if (u.alive() && u.player == player_) ++units;
    // Each beaten player's base: start index ("a" an ally's):minute found
    // beaten:minute last in sight, "+" while an enemy building is known near it;
    // then the scouts sent to such bases (each of a party) and those lost on the way.
    std::string vacated;
    for (size_t i = 0; i < startVacated_.size(); ++i)
        if (startVacated_[i])
            vacated += (vacated.empty() ? "" : ",") + std::string(i >= allyStartsFrom_ ? "a" : "") + std::to_string(i) +
                       ":" + std::to_string((startVacated_[i] - 1) / 1800) + ":" +
                       (startSeen_[i] ? std::to_string((startSeen_[i] - 1) / 1800) : std::string("-")) +
                       (startBase_[i] ? "+" : "");
    std::fprintf(stderr, "AIEXPLORE t=%u p=%d lvl=%d explored=%.4f left=%d units=%d scouts=%d explorations=%d known=%zu "
                 "scoutsLost=%d vacated=%s vacatedTrips=%d vacatedLost=%d\n",
                 minute, player_, int(diff_), explored.empty() ? 0.0 : double(n) / double(explored.size()), left, units,
                 scoutsSent_, explorations_, knowledge_.size(), scoutsLost_, vacated.empty() ? "-" : vacated.c_str(),
                 vacatedTrips_, vacatedLost_);
}

namespace {
// Debug hook, TAK_AI_REVIVE_STATS: reviveIdle's work and time over the whole
// process (every AI), printed at exit as one AIREVIVE line. A measuring hook
// only: it changes nothing the AI does. Each running room thinks on its own
// worker thread, so a call counts locally and merges under the lock.
struct ReviveStats {
    bool on = tak::devEnv("TAK_AI_REVIVE_STATS") != nullptr;
    std::mutex m;
    uint64_t calls = 0, casters = 0, records = 0, bodies = 0, visits = 0, commands = 0;
    std::vector<double> us;
    ~ReviveStats() {
        if (!on) return;
        std::sort(us.begin(), us.end());
        double total = 0;
        for (double v : us) total += v;
        auto q = [&](double f) { return us.empty() ? 0.0 : us[std::min(us.size() - 1, size_t(f * double(us.size())))]; };
        std::fprintf(stderr, "AIREVIVE calls=%llu casters=%llu records=%llu bodies=%llu visits=%llu commands=%llu "
                     "total_ms=%.3f mean_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f\n",
                     (unsigned long long)calls, (unsigned long long)casters, (unsigned long long)records,
                     (unsigned long long)bodies, (unsigned long long)visits, (unsigned long long)commands,
                     total / 1000, us.empty() ? 0.0 : total / double(us.size()), q(0.5), q(0.95), q(0.99),
                     us.empty() ? 0.0 : us.back());
    }
};
ReviveStats reviveStats;
struct ReviveCall {
    uint64_t casters = 0, records = 0, bodies = 0, visits = 0, commands = 0;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~ReviveCall() {
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard lock(reviveStats.m);
        ++reviveStats.calls; reviveStats.casters += casters; reviveStats.records += records;
        reviveStats.bodies += bodies; reviveStats.visits += visits; reviveStats.commands += commands;
        reviveStats.us.push_back(us);
    }
};
} // namespace

// Retail raises bodies only by order, so the sim never raises one by itself;
// our levels' priests, Chief Engineers, Spirit Wolves and kings are given the
// order: an idle caster (not one given anything else this think) and a body
// within 56 px that it can raise (World::reviveKind), the nearest, one caster
// per body. The search starts none the end would refuse or abandon -- the player at
// its unit limit, the output at its totalallowed, an allocation under 0.7 (a
// computer player's revive then ends unraised, 0x406e79). The Retail AI never
// gets here (Classic returns first): retail's AI orders no revive -- its group
// order 0x50b5d0 takes only modes 2 and 9.
void Controller::reviveIdle(const tak::sim::World& world, std::vector<int>& assigned,
                            const CommandSink& sink) const {
    std::optional<ReviveCall> stats;
    if (reviveStats.on) stats.emplace();
    if (double(world.player(player_).allocation()) < 0.7 || world.atUnitCap(player_)) return;
    constexpr float kReach = 56.0f;
    // The bodies a caster can take up, in world.units() order (the order every
    // caster's search walks them, so an equal distance still goes to the
    // earlier body). Gathered once, at the first idle caster, rather than
    // every caster scanning every unit record: nothing this think changes
    // them (a Resurrect only gives the caster its order). No body, no raise.
    struct Body { float x, z; int id; const tak::sim::UnitType* type; bool claimed; };
    std::vector<Body> bodies;
    bool gathered = false;
    for (const auto& u : world.units()) {
        // The type's caster flags before idle(), which walks the unit's orders:
        // most units are no caster (every test here is a pure read).
        if (!u.alive() || u.player != player_ || !u.type ||
            !(u.type->canResurrect || (u.type->canAnimate && u.type->animateType)) ||
            u.underConstruction || u.embarked() || u.incapacitated() || u.buildSiteId || !idle(u) ||
            std::find(assigned.begin(), assigned.end(), u.id) != assigned.end()) continue;
        if (stats) ++stats->casters;
        if (!gathered) {
            gathered = true;
            if (stats) stats->records += world.units().size();
            for (const auto& c : world.units())
                if (!c.alive() && c.type && !c.corpseFading && c.deadFor >= c.corpseAnimationTicks() &&
                    c.deadFor < c.corpseUntil)
                    bodies.push_back({c.x.toFloat(), c.z.toFloat(), c.id, c.type, false});
            if (stats) stats->bodies += bodies.size();
            if (bodies.empty()) return;
        }
        if (stats) stats->visits += bodies.size();
        const float ux = u.x.toFloat(), uz = u.z.toFloat();
        Body* best = nullptr;
        float bestD = kReach * kReach;
        for (auto& c : bodies) {
            const float dx = c.x - ux, dz = c.z - uz;
            const float d = dx * dx + dz * dz;
            if (d > bestD || (d == bestD && best)) continue;
            const uint8_t kind = world.reviveKind(u.id, c.id);
            if (!kind || world.atTypeCap(player_, kind == 1 ? c.type : u.type->animateType) || c.claimed) continue;
            best = &c; bestD = d;
        }
        if (!best) continue;
        tak::net::Command c; c.kind = tak::net::Cmd::Resurrect; c.player = uint8_t(player_);
        c.unitId = u.id; c.targetId = best->id;
        sink(c);
        if (stats) ++stats->commands;
        best->claimed = true;
        assigned.push_back(u.id);
    }
}

void Controller::tick(const tak::sim::World& world, uint32_t simTick,
                      const CommandSink& sink) {
    // Classic runs retail's own clocks and bookkeeping: hand it every tick before
    // any of the other levels' cadence, home or ally setup runs.
    if (classic_) { classic_->tick(world, simTick, sink); return; }
    // Think cadence (per difficulty), staggered by player so several AIs don't all
    // fire on the same tick. Sim-tick driven so a --mpai run is repeatable. Hard
    // reacts more often (thinkPeriod=20) than Easy (60).
    const bool due = (simTick % uint32_t(dp_.thinkPeriod)) == uint32_t(player_ % dp_.thinkPeriod);
    if (!due && !firstThinkWaits_) return;
    firstThinkWaits_ = false;
    // Player 0's think at tick 0 comes before the sim's first view and exploration
    // pass. Under NOT EXPLORED it would know no ground at all (knowsGround), while
    // a player sees the ground round their start from the first frame: it waits
    // for tick 1, as every other seat's first think comes after that pass.
    if (due && simTick == 0 && world.viewCounts().empty() && world.tickCount() == 0 &&
        !world.mapRevealedFor(player_) && !world.roomLosOff()) {
        firstThinkWaits_ = true;
        return;
    }
    if (world.player(player_).defeated) return;

    now_ = simTick;
    updateKnowledge(world);
    static const bool kExploreLog = tak::devEnv("TAK_AI_EXPLORE") != nullptr;
    if (kExploreLog) logExploration(world, simTick);
    // Door clearance reads the live obstacle overlay, which new buildings change:
    // keep it for one think only.
    navalDoorSteps_.clear();
    if (!home_) {
        home_=homeOf(world);
        // The seat's faction, once: the Monarch it has (a commander is never
        // captured or given away), else the god the match setup gave the seat
        // for its Monarch's side.
        for (const auto& u : world.units())
            if (u.alive() && u.player == player_ && u.type && u.type->commander) { side_ = u.type->side; break; }
        if (const auto* god = world.player(player_).godType; side_.empty() && god) side_ = god->side;
    }
    if (!allyHomesKnown_) {
        allyHomesKnown_=true;
        for (int p=0;p<world.numPlayers();++p)
            if (p!=player_ && world.allied(player_,p) &&
                std::any_of(world.units().begin(),world.units().end(),[p](const auto& u){return u.alive() && u.player==p;})) {
                const auto [x,z]=anchorOf(world,p);
                allyHomes_.push_back({p,x,z});
            }
    }
    if (world.hasManaSpots()) updateBuilderProgress(world,simTick,sink);
    if (world.hasManaSpots() && (diff_==Difficulty::Hard || diff_==Difficulty::Absurd)) updateLostSites(world);
    // A builder of ours bound for a deposit this side now holds taken -- an enemy
    // lodestone seen there on the way, or learned -- is stopped and given another
    // site from the next think, as a player who sees the spot taken would.
    if (world.hasManaSpots())
        for (const auto& u : world.units()) {
            if (!u.alive() || u.player != player_ || !u.type || u.type->isStructure() || !u.type->isBuilder ||
                u.buildSiteId) continue;
            bool taken = false;
            for (const auto& order : u.orders)
                if (order.buildType && order.buildType->onMana && (order.buildX.v || order.buildZ.v))
                    forDepositsNear(world, order.buildX.toFloat(), order.buildZ.toFloat(), 24,
                                    [&](size_t i) { taken = taken || (i < depositTaken_.size() && depositTaken_[i]); });
            if (!taken) continue;
            tak::net::Command stop; stop.kind = tak::net::Cmd::Stop; stop.player = uint8_t(player_); stop.unitId = u.id;
            sink(stop);
        }
    if (!allyHomes_.empty() && world.hasManaSpots()) updateAllyClaims(world,simTick);
    plannedManaSites_.clear();
    plannedSites_.clear();
    if (dp_.unbounded) updateIssuedBuilds(world, simTick);
    if (dp_.unbounded) unqueueStalled(world, simTick, sink);
    Needs needs = assessNeeds(world);   // one empire assessment drives every producer
    planUpgrade(world, needs);
    // Snapshot the idle producers, then act on up to producersPerThink of them -- the
    // per-think cap is what paces the economy across difficulties (Easy builds one
    // thing per think, Hard many).
    std::vector<int> producers;
    for (auto& u : world.units()) {
        if (!u.alive() || u.player != player_ || !u.type) continue;
        // A building can carry canMove=1 in its FBI (the Keep, the Taros Hell), so
        // classify by isStructure() (maxVel), not canMove -- otherwise a canMove
        // building is mistaken for a mobile builder and issued Build orders it can't
        // honour. And never drive a producer that is itself still under construction.
        if (!u.type->isStructure() && u.type->isBuilder && !u.underConstruction &&
            (idle(u) || raising(u)) && u.buildSiteId == 0)
            producers.push_back(u.id);                        // idle mobile builder
        else if (u.type->isStructure() && !u.underConstruction && u.buildQueue.empty() &&
                 !registry_.buildable(u.type->id).empty())
            producers.push_back(u.id);                        // idle factory
    }
    // Round-robin who acts when the per-think cap is smaller than the producer count
    // (Easy caps at 1): otherwise the lowest-id producer -- the Monarch -- takes the
    // only slot every think, so it keeps building economy and the factories never get
    // to train an army. Rotating by the think index gives each producer its turn.
    if (!producers.empty() && dp_.producersPerThink < int(producers.size())) {
        uint32_t rr = simTick / uint32_t(std::max(dp_.thinkPeriod, 1));
        std::rotate(producers.begin(),
                    producers.begin() + rr % uint32_t(producers.size()), producers.end());
    }
    // Over-commit throttle: while broke with construction already in progress, don't
    // start ANOTHER build. Concurrent builds share the one mana pool, so piling on more
    // while the treasury is empty starves them all and leaves half-built units that
    // never finish -- the "produces units then gets stuck" failure. Let the in-flight
    // ones finish (income flows into them) before starting the next. Retail's planner
    // likewise skips its ordinary pick below 70% allocation (0x40c890) but still
    // builds the lodestone it draws separately, so the starved AI may start that.
    bool throttle = false;
    if (world.player(player_).mana < 50.0f)
        for (const auto& u : world.units())
            if (u.player == player_ && u.alive() && u.underConstruction) { throttle = true; break; }
    int acted = 0;
    bool commanderActed = false;
    std::vector<int> assigned;
    // The Monarch fights back. The sim never lets a unit pick its own target while a
    // build order leads its queue, so a Monarch walking to a site -- or waiting there
    // for a converted unit to clear its spot -- stood and died under fire (a replay:
    // five converted units shot Elsin for a minute on a stalled Lodestone approach).
    // When it is building, moving or idle and something hits it or stands within its
    // reach in view (sees), order it to attack that unit -- by id only when it is in
    // view; one only heard hitting it is attack-moved on, to where it stands.
    // Production leaves it alone this think.
    for (const auto& u : world.units()) {
        if (!u.alive() || u.player != player_ || !u.type || !u.type->commander ||
            u.underConstruction || u.embarked() || u.incapacitated()) continue;
        const float reach = std::max(float(u.type->weapon.range), u.type->maxRange());
        const bool armed = u.type->weapon.damage > 0 ||
            std::any_of(u.type->weapons.begin(), u.type->weapons.end(), [](const auto& w) { return w.damage > 0; });
        if (!armed || reach <= 0) break;
        // The whole queue, as waveFree reads it: a flyer's step-out (retail's air
        // traffic) or the sim's walk to a firing spot leads for a moment, and
        // the order behind it is the task. A Creon Sage backing out of its
        // Mortar's minimum range was sent the Attack again on every think.
        if (std::any_of(u.orders.begin(), u.orders.end(), [](const tak::sim::Order& o) {
                return o.targetId || o.attackMove || o.patrol || o.guard || o.load || o.unload;
            })) break;   // already fighting, or a deliberate task
        int target = 0;
        float best = 1e18f;
        auto consider = [&](const tak::sim::Unit* e, bool heard) {
            if (!e || !e->alive() || !e->type || e->embarked() || e->underConstruction ||
                world.isNeutralPlayer(e->player) || world.allied(player_, e->player)) return;
            const float dx = e->x.toFloat() - u.x.toFloat(), dz = e->z.toFloat() - u.z.toFloat();
            const float d = dx * dx + dz * dz, limit = reach + 90;
            if (d <= limit * limit && d < best && (heard || sees(world, *e))) { best = d; target = e->id; }
        };
        consider(world.unit(u.lastHitBy), true);   // whoever is hitting it, seen or not
        if (!target)
            for (const auto& e : world.units()) {
                if (e.type && (e.type->weapon.damage > 0 ||
                    std::any_of(e.type->weapons.begin(), e.type->weapons.end(),
                                [](const auto& w) { return w.damage > 0 || w.mindControl; })))
                    consider(&e, false);
            }
        if (target) {
            const auto& e = *world.unit(target);
            if (sees(world, e)) {
                tak::net::Command c; c.kind = tak::net::Cmd::Attack; c.player = uint8_t(player_);
                c.unitId = u.id; c.targetId = target;
                sink(c);
            } else emit(sink, tak::net::Cmd::AttackMove, u.id, "", e.x.toFloat(), e.z.toFloat());
            assigned.push_back(u.id);
            commanderActed = true;   // the home leash below must not override it
        }
        break;
    }
    if (!throttle || manaShort(world, needs))
        for (int pid : producers) {
            if (acted >= dp_.producersPerThink) break;
            const auto* p = world.unit(pid);
            if (!p || !p->alive()) continue;
            if (std::find(assigned.begin(), assigned.end(), pid) != assigned.end()) continue;
            // Retry in the next-best category when a pick cannot be acted on.
            // Without this a producer whose top category is unproducible burns its
            // turn silently, every think, for ever: measured on Zhon, all three
            // producers picked a lodestone 153 times in 240s with every mana
            // deposit already taken, emitted nothing, and banked 6875 mana while
            // building no army at all. One category per attempt, so the worst case
            // is one pass over the five.
            int exclude = 0;
            for (int attempt = 0; attempt < 5; ++attempt) {
                float siteLimit = 0;
                const auto* pick = weightedPick(world, *p, needs, exclude, throttle, &siteLimit);
                if (!pick) break;
                const bool planned = (upgrade_.stage == 3 && pick == upgrade_.divine) ||
                                     (upgrade_.stage == 1 && pick == upgrade_.priest);
                if (produce(world, *p, pick, sink, siteLimit)) {
                    if (p->type && p->type->commander) commanderActed = true;
                    if (planned) {
                        // Bought: the store is free again, and the next stage
                        // begins with the next think's plan.
                        needs.reserve = 0;
                        upgrade_.stage = 0;
                        upgrade_.target = 0;
                        if (pick == upgrade_.priest) ++needs.priests;
                        else { ++needs.upgrades; ++needs.upgradeProjects; }
                    }
                    ++acted;
                    assigned.push_back(pid);
                    needs.committed += pick->buildCost;
                    ++needs.population;
                    ++needs.counts[pick];
                    if (navalFactory(pick)) ++needs.navalFactories;
                    if (navalProducer(pick)) ++needs.navalProducers;
                    if (pick->onMana && pick->isStructure()) ++needs.lodestoneProjects;
                    if (incomeBuilding(pick) && tak::sim::World::lodestoneUpgradeBase(*pick).empty()) ++needs.lodestones;
                    if (mobileBuilder(pick)) ++needs.mobileBuilders;
                    if (!pick->isStructure() && !pick->canFly && !pick->isBuilder &&
                        pick->domain==tak::sim::UnitType::Domain::Water) ++needs.navalArmy;
                    switch (categoryOf(pick)) {
                        case BuildCat::Army: ++needs.army; break;
                        case BuildCat::Factory: ++needs.factories; break;
                        case BuildCat::Builder: ++needs.builders; break;
                        case BuildCat::Economy: ++needs.economy; ++needs.economyProjects; break;
                        case BuildCat::Defense: ++needs.defenses; break;
                    }
                    break;
                }
                exclude |= 1 << int(categoryOf(pick));
            }
        }
    // The unbounded levels first walk idle fighters out of the base; door clearing
    // below then sees them assigned and handles the rest (ships, builders).
    // A caster raising a body beside it is not stationed or cleared away.
    reviveIdle(world, assigned, sink);
    if (dp_.unbounded) stationArmy(world, assigned, sink);
    // Most units are not factories. Build the ordered factory list once per
    // think, rather than scan the whole empire for every idle army member.
    // Keep IDs so the command sink cannot invalidate stored unit pointers.
    std::vector<int> exitFactories;
    for (const auto& factory:world.units())
        if (factory.alive() && factory.player==player_ && factory.type && !factory.underConstruction &&
            categoryOf(factory.type)==BuildCat::Factory)
            exitFactories.push_back(factory.id);
    // Idle newborns and constructors must clear factory doors even when this AI
    // never attacks. Issue ordinary local moves; do not relax body occupancy or
    // change the movement controller to make production succeed.
    // A ring point can fall in a neighbour's open yard or door (canPlace allows
    // its open cells): Smithy 12's first ring point lay in Smithy 27's door, and
    // the units sent there jammed it for minutes. Skip any own building's
    // footprint and every door strip (doorStrips).
    const BaseLayout layout = exitFactories.empty() ? BaseLayout{} : baseLayout(world);
    int cleared = 0;
    for (const auto& u : world.units()) {
        if (cleared >= std::min(dp_.producersPerThink * 2, 64)) break;
        if (!u.alive() || u.player!=player_ || !u.type || u.type->isStructure() ||
            u.underConstruction || u.embarked() || !idle(u) || u.buildSiteId ||
            std::find(assigned.begin(),assigned.end(),u.id)!=assigned.end()) continue;
        for (int factoryId:exitFactories) {
            const auto* found=world.unit(factoryId);
            if (!found) continue;
            const auto& factory=*found;
            const bool mobile = !factory.type->isStructure();
            if (mobile && categoryOf(u.type) != BuildCat::Army) continue;
            const float clearance=mobile ? 180.0f : float(std::max(factory.type->footX,factory.type->footZ))*8+64;
            const float dx=u.x.toFloat()-factory.x.toFloat(),dz=u.z.toFloat()-factory.z.toFloat();
            if (dx*dx+dz*dz>clearance*clearance) continue;
            bool moved=false;
            for (float r : {clearance+48,clearance+96,clearance+160,clearance+240,clearance+320}) {
                for (int i=0;i<16;++i) {
                    const float a=float((u.id+i)%16)*0.392699082f;
                    const float x=factory.x.toFloat()+detmath::cos(a)*r;
                    const float z=factory.z.toFloat()+detmath::sin(a)*r;
                    const auto home=u.type->domain==tak::sim::UnitType::Domain::Water ?
                        std::pair{factory.x.toFloat(),factory.z.toFloat()} : homeOf(world);
                    if (!dp_.attack && (x-home.first)*(x-home.first)+(z-home.second)*(z-home.second)>600*600) continue;
                    if (!world.canPlace(u.type,x,z) || (!u.type->canFly &&
                        (layout.blocks(footprintRect(u.type,x,z)) ||
                         !world.pathExists(u.type,x,z,u.x.toFloat(),u.z.toFloat())))) continue;
                    emit(sink,tak::net::Cmd::Move,u.id,"",x,z);
                    assigned.push_back(u.id);++cleared;moved=true;break;
                }
                if (moved) break;
            }
            if (moved) break;
        }
    }
    // Keep the Monarch safe: when it's idle (no build this think, no order, no site)
    // and has strayed beyond a leash of home, walk it back to the base. Losing the
    // Monarch can lose the game (Monarch Expendable), so it must not sit exposed out
    // in the field. A plain Move (not AttackMove) -- it retreats, it doesn't hunt.
    for (const auto& u : world.units()) {
        if (!u.alive() || u.player != player_ || !u.type || !u.type->commander) continue;
        if (commanderActed || std::find(assigned.begin(),assigned.end(),u.id)!=assigned.end() || !u.orders.empty() || u.buildSiteId != 0 || u.underConstruction)
            break;
        auto h = homeOf(world);
        float dx = u.x.toFloat() - h.first, dz = u.z.toFloat() - h.second;
        constexpr float kHomeLeash = 520.0f;   // ~a third of a small map's span
        if (dx * dx + dz * dz > kHomeLeash * kHomeLeash)
            emit(sink, tak::net::Cmd::Move, u.id, "", h.first, h.second);
        break;
    }
    if (!dp_.attack) {
        for (const auto& u : world.units()) {
            if (!u.alive() || u.player!=player_ || u.underConstruction || !u.type ||
                !u.type->canSetStance || (u.moveState==0 && u.fireState==2)) continue;
            tak::net::Command c; c.kind=tak::net::Cmd::Stance;
            c.player=uint8_t(player_);c.unitId=u.id;c.targetId=1;
            sink(c); // explicit interception moves; no autonomous long-distance chase
        }
        sendDefenders(world,sink);
    }
    updateStrikeProgress(world, simTick, sink);
    sendWaves(world, simTick, sink, assigned);
}

}  // namespace tak::ai
