#pragma once

// Per-tick render SNAPSHOT of the sim (sim/render decouple): UnitR / PlayerR mirror sim::Unit / sim::Player
// field names so render code reads them unchanged, and Frame is the whole triple-
// buffered snapshot the render/HUD reads instead of live world_. Extracted from
// client/main.cpp; kept at global scope so its unqualified use sites there are
// unchanged.

#include "client/swapshow.h"
#include "client/unitindex.h"
#include "sim/retailanimationqueries.h"
#include "sim/sim.h"   // ta::sim::UnitType / Order / Projectile / World::HitFx

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// Orders exposed to rendering and the HUD. Navigator/transport controllers and
// mission scratch belong to the simulation, not to each of three frame buffers.
// Keep every waypoint and every field consumed by overlays/status/cursors.
struct RenderOrder {
    ta::sim::Fixed x,z,clickX,clickZ,buildX,buildZ;
    const ta::sim::UnitType* buildType=nullptr;
    int targetId=0,reclaimFeat=0,repairTarget=0;
    uint32_t issuedTick=0;
    uint32_t areaNextSpot=0;
    bool load=false,unload=false,attackMove=false,patrol=false,guard=false,goal=false;
    bool reclaimArea=false,buildRectangle=false,manaBuildArea=false;
    // Bit-fields, so the record stays within its 64 bytes. revive: a
    // RESURRECT leg (Order::reviveKind; reclaimFeat is the body).
    bool areaExploring:1=false,areaApproached:1=false,trailPreview:1=false,revive:1=false;
    // A factory output's PARK and a flyer's landing (VTOL_LandIfCan): neither is work, so
    // the IDLE BUILDERS strip counts a unit holding only these as idle (as ai::idle does).
    bool park:1=false,landing:1=false;
    RenderOrder()=default;
    RenderOrder(const ta::sim::Order& o)
        :x(o.x),z(o.z),clickX(o.clickX),clickZ(o.clickZ),buildX(o.buildX),buildZ(o.buildZ),
         buildType(o.buildType),targetId(o.targetId),reclaimFeat(o.reclaimFeat),repairTarget(o.repairTarget),
         issuedTick(o.issuedTick),load(o.load),unload(o.unload),attackMove(o.attackMove),patrol(o.patrol),
         guard(o.guard),goal(o.goal),reclaimArea(bool(o.reclaimArea)),buildRectangle(bool(o.buildRectangle)) {
        revive=o.reviveKind!=0;
        park=bool(o.park);landing=o.landing;
        if (o.reclaimArea) {
            x=ta::sim::Fixed::raw(o.reclaimArea->minX);z=ta::sim::Fixed::raw(o.reclaimArea->minZ);
            buildX=ta::sim::Fixed::raw(o.reclaimArea->maxX);buildZ=ta::sim::Fixed::raw(o.reclaimArea->maxZ);
            areaApproached=o.reclaimArea->stage>=2;   // its approach leg is queued (or done)
        }
    }
};

// Per-tick render SNAPSHOT of a sim Unit (sim/render decouple). Field names + methods MIRROR sim::Unit so render code
// reads them unchanged; captured each tick by GameView::captureFrame() so the render never
// dereferences world_. Also carries the motion-interpolation prev pose (px/pz/ph); the curr
// pose is x/z/heading (mirroring Unit). Indexed by unit id in Frame::units.
struct UnitR {
    float px = 0, pz = 0, ph = 0;   // previous-tick pose (for interpolation)
    bool  seeded = false;           // has a valid prev pose to interpolate from
    ta::SwapShow swap;             // a Legion swap still playing out (swapshow.h); swap.ox/oz is drawn at x/z
    float pox = 0, poz = 0;         // ... and the offset that was drawn at px/pz
    uint32_t gen = 0;               // captureFrame generation this record was last written
                                    // (== frame gen means the unit is live THIS tick)
    // --- snapshot of Unit's render-read surface (same NAMES as sim::Unit, so render code
    //     that reads u.<field> works unchanged once its parameter is a UnitR) ---
    int id = 0;
    const ta::sim::UnitType* type = nullptr;
    int player = 0;
    float x = 0, z = 0, heading = 0;   // current-tick pose
    uint16_t headingWord = 0;
    uint16_t bodyPitch=0,bodyRoll=0;
    std::array<int32_t,3> aimVelocity{};
    std::array<int32_t,3> worldPosition{}; // exact, unprojected position for script queries
    int turnSpeedPercent = 0;
    int turnReqBam = 0;                // tick's requested turn (BAM) for TurnDirection anim
    float hp = 0, mana = 0;
    int veteran = 0;
    int kills = 0;                     // its own kill count (Unit::kills, the KillCount gadget)
    float deadFor = -1;
    int inTransport = 0;
    // The pad piece a flyer sits on (sim Unit::padPiece): one on a repair
    // pad is on show and can be picked, unlike a transport's passenger.
    int8_t padPiece = -1;
    int8_t squad = 0;
    int stance = 1;
    int8_t outputStance = -1;   // a producer's stance for its output (-1: none chosen)
    uint8_t standingOrder = 3;
    int weaponSlot = 0;
    std::array<int32_t,3> weaponReloads{};
    bool underConstruction = false, buildBegun = false;
    bool selectable = true;     // retail +0x130 & 0x20: false while a mission script runs it
    bool cloaked = false, cloakOn = true, active = true;
    float frozenFor = 0, stonedFor = 0, paralyzedFor = 0;
    float selfDestructT = -1;   // >=0 = self-destruct countdown (seconds) armed
    int buildSiteId = 0, productionSiteId = 0, reclaimId = 0, repairId = 0;
    bool conjuring = false;
    bool hasConstructionEmitter = false;
    std::array<uint32_t,2> constructionEmissions{};
    std::vector<ta::sim::RetailConstructionParticle> constructionParticles;
    float buildProgress = 0;
    int constructionPercentLeft = 0;
    float buildRemaining = 0;       // retail unit+0x104: 1 at placement, 0 complete
    bool moverMoving = false;       // retail mover+0x20 (speed) nonzero

    const ta::sim::UnitType* replacementModel = nullptr;
    float replacementX=0,replacementZ=0,replacementHeading=0;
    float replacementOpacity=-1; // negative: ordinary construction presentation
    int scriptHealthPercent = 100;
    bool yardOpen = false;
    std::vector<const ta::sim::UnitType*> buildQueue;
    std::vector<RenderOrder> orders, rally;
    // Its own Legion spot on its current move-like leg (World::legionDestination), for
    // the destination markers; captured with the orders, so only for the units whose
    // orders are (never another side's destination for a player). hasSpot false: none.
    bool hasSpot = false;
    float spotX = 0, spotZ = 0;
    // Deposits the first lodestone area job has already tried (sorted spot
    // indices); the order-line preview plans the remaining ones from it.
    std::vector<uint32_t> areaVisited;
    void captureOrders(const ta::sim::Unit& unit) {
        orders.assign(unit.orders.begin(),unit.orders.end());
        rally.assign(unit.rally.begin(),unit.rally.end());
        areaVisited.clear();
    }
    void clearOrders() {orders.clear();rally.clear();areaVisited.clear();hasSpot=false;}
    // Orders given to this unit set its output's rally instead (World::setRally):
    // a production building, or a mobile builder while it repeats production.
    bool holdsRally() const { return type && (type->producesUnits() || repeatType); }
    // Its stance buttons set the stance its output starts with (outputStance):
    // a finished production building, or a mobile builder on an infinite queue.
    bool showsOutputStance() const {
        return type && ((type->producesUnits() && !underConstruction) || repeatType);
    }
    const std::vector<RenderOrder>& displayedOrders() const {
        return holdsRally() ? rally : orders;
    }
    // Construction still pending anywhere in the queue (builds are ordinary
    // orders now, so this is just "is one of them a build").
    bool hasQueuedBuild() const {
        for (const auto& o : orders) if (o.buildType) return true;
        return false;
    }
    // Construction OR reclaim still queued.
    bool hasQueuedWork() const {
        for (const auto& o : orders)
            if (o.buildType || o.reclaimFeat || o.reclaimArea || o.repairTarget) return true;
        return false;
    }
    std::vector<int> cargo;
    const ta::sim::UnitType* repeatType = nullptr;
    bool moving_ = false, walking_ = false;   // cached u.moving()/u.walking()
    float speed = 0;                           // px/s (diagnostic use)
    int horizontalSpeedPercent = 0;
    int verticalSpeedPercent = 0;
    float flightY = 0;                        // absolute height from the sim flight controller
    uint8_t flightGroundMode = 1;
    uint16_t movementTerrainFlags = 0; // native mover flags used by GET 28/34
    uint32_t animationOccupancy = 0;
    uint32_t animationMoveRate = 0;
    float gaitRate = 1;                       // Legion display gait pace (gaitpace.h)
    uint32_t flightBeginCallbackSerial = 0;
    uint32_t flightLandingCallbackSerial = 0;
    void captureMoveRate(const ta::sim::Unit& u,int16_t turn) {
        const auto multiplier=u.groundTerrainFlags&0x800 ? u.type->roadMult :
            u.groundTerrainFlags&0x1000 ? u.type->waterMult : ta::sim::Fixed::fromInt(1);
        const auto& v=u.flightVelocity;
        const int32_t horizontal=u.type->canFly ? int32_t(ta::sim::isqrt64(
            uint64_t(int64_t(v.x)*v.x)+uint64_t(int64_t(v.z)*v.z))) : u.speed.v;
        animationMoveRate=ta::sim::retailAnimationMoveRate(u.speed.v,turn,horizontal,
            (u.type->animationMoveRate1*multiplier).v,(u.type->animationMoveRate2*multiplier).v,
            movementRefused,u.embarked());
    }
    void captureOccupancy(const ta::sim::Unit& u,int sea,uint32_t previous) {
        animationOccupancy=u.type->maxVel>ta::sim::Fixed() ?
            ta::sim::retailAnimationOccupancy(previous,u.flightGroundMode,
                int16_t((u.type->canFly ? u.flightY : u.groundY).floorInt()),
                uint8_t(sea),uint8_t(u.type->waterline),int16_t(u.type->modelTop>>16)) : 0;
    }
    bool movementRefused = false;             // retail mover's repeated-refusal bit
    void captureMovement(const ta::sim::Unit& u) {
        moving_ = u.moving(); walking_ = u.walking();
        speed = u.speed.toFloat() * 30.0f;
        bodyPitch=u.groundPitch;bodyRoll=u.groundRoll;
        worldPosition={u.x.v,(u.type->canFly ? u.flightY : u.groundY).v,u.z.v};
        flightY = u.flightY.toFloat();
        flightGroundMode = u.flightGroundMode;
        flightBeginCallbackSerial = u.flightBeginCallbackSerial;
        flightLandingCallbackSerial = u.flightLandingCallbackSerial;
        movementTerrainFlags=u.groundTerrainFlags;
        movementRefused = !u.type->canFly && (u.bodyBlockStreak >= 2 || u.legionStill);
        const auto multiplier=u.groundTerrainFlags&0x800 ? u.type->roadMult :
            u.groundTerrainFlags&0x1000 ? u.type->waterMult : ta::sim::Fixed::fromInt(1);
        const auto maximum=u.baseSpeed*multiplier;
        const auto step=ta::sim::retailGroundStep(u.heading,u.speed);
        aimVelocity=u.type->canFly ? std::array<int32_t,3>{u.flightVelocity.x,u.flightVelocity.y,u.flightVelocity.z}
                                  : std::array<int32_t,3>{step.s.v,0,step.c.v};
        horizontalSpeedPercent=ta::sim::retailHorizontalAnimationPercent(
            u.type->canFly ? u.flightVelocity.x : step.s.v,
            u.type->canFly ? u.flightVelocity.z : step.c.v,
            maximum.v,movementRefused,u.embarked());
        verticalSpeedPercent = u.type->canFly ? ta::sim::retailFlightVerticalPercent(
            u.flightVelocity.y,maximum.v,false,u.embarked()) : 0;
    }
    // Split the authoritative height for the existing body/shadow/effect paths.
    // While taking off below a nearby hill's clearance datum, that datum must
    // not lift the rendered body above the actual flight position.
    // Both are absolute heights: the body draws flightY/2 up, as retail's z - y/2.
    std::pair<float,float> flightRenderHeight(float datum) const {
        const float height=std::max(0.0f,flightY);
        const float ground=std::min(std::max(0.0f,datum),height);
        return {ground,height-ground};
    }
    // Retail GET 29 (4dc100) hides retained controller speed after repeated
    // refusal. A unit waiting behind another body must not walk in place.
    float animationSpeed() const { return movementRefused || embarked() ? 0.0f : speed; }
    int32_t animationSpeedPercent() const {
        return movementRefused || embarked() ? 0 : horizontalSpeedPercent;
    }
    int corpseAnimationTicks = 0;
    bool corpsePhase = false;                  // dead, death anim done, body still lies
    uint8_t deathType = 1;                     // killing blow damagetype (3 = gib)
    uint8_t severity = 0;                      // retail Killed severity (1..100)
    int corpseFeat = -1;                       // resolved corpse/statue FeatType index
    bool corpseReclaimable = false;
    bool corpseFading = false;                 // a reclaim finished it: neither raised nor reclaimed
    bool corpseSinking = false;                // a water grave: the body is drawn at corpseSinkY
    float corpseSinkY = 0;                     // its feature height (4959c0), world px
    uint8_t corpseSinkAlpha = 255;            // its fade, ftol(alpha * 255.0f) (4ed056): 255 = none
    uint8_t corpseDecayAlpha = 255;           // a decaying body's last 60 updates fade it out: 255 = none
    float corpseDecaySink = 0;                 // ...and sink it into the ground (495af6), world px
    uint8_t corpseFade() const {
        return corpsePhase ? std::min(corpseSinking ? corpseSinkAlpha : uint8_t(255),corpseDecayAlpha) : uint8_t(255);
    }
    int corpseCellX=0,corpseCellZ=0,corpseFootX=0,corpseFootZ=0;
    bool corpseStatue = false;                 // petrified/frozen: the body stays UPRIGHT,
                                               // unlike a normal corpse which lies flat
    int justBuilt = 0;                         // one-tick: unit id produced this tick, else 0
    bool disco = false, headbang = false;      // cached world_.disco/headbangTarget(player)
    bool sightShared = false;                  // cached sharesSightToLocal(player)
    // The local display's sight of this unit (client/retailfog.h), for another
    // side's unit: losSeen is the main view's LosOn (0x4f7210) on the local plane,
    // mapSeen the minimap's dot rule (0x4f9970: radar or its centre cell in view).
    // Spectators (noFog_) and sight-sharing allies ignore both.
    bool losSeen = false, mapSeen = false;
    std::string scenarioName;
    const std::string& displayName() const { return scenarioName.empty() ? type->name : scenarioName; }
    bool alive() const { return deadFor < 0; }
    bool embarked() const { return inTransport != 0 && padPiece < 0; }
    bool moving() const { return moving_; }
    bool walking() const { return walking_ && animationSpeed() > 0.0f; }
};

// Per-tick render snapshot of a sim Player (mirrors sim::Player's read field names, like
// UnitR). Captured each tick so the HUD/scoreboard never reads live world_ players.
struct PlayerR {
    std::string side; // Stable faction identity, including after elimination.
    uint8_t manaShareMask = 0xff;
    float mana = 0, storage = 0, income = 0, expenditure = 0; // expenditure: requested mana/s
    // Total Annihilation metal/energy (sim/taeconomy.h): stored, cap, and the last
    // second's income/expense, the figures the top bar shows.
    float metal = 0, energy = 0, metalCap = 0, energyCap = 0;
    float metalIncome = 0, metalExpense = 0, energyIncome = 0, energyExpense = 0;
    double produced[2] = {0, 0}, wasted[2] = {0, 0};   // lifetime, [0] energy [1] metal (the result plate)
    void captureEconomy(const ta::sim::Player& player) {
        metal = player.books.stored[1]; energy = player.books.stored[0];
        metalCap = player.books.cap[1]; energyCap = player.books.cap[0];
        metalIncome = player.books.income[1]; metalExpense = player.books.expense[1];
        energyIncome = player.books.income[0]; energyExpense = player.books.expense[0];
        for (int r = 0; r < 2; ++r) { produced[r] = player.books.produced[r]; wasted[r] = player.books.wasted[r]; }
        mana=float(player.mana);
        storage=player.manaCapacity();
        // Retail HUD calls 401330/401350: the oldest 29 tick samples of
        // produced and REQUESTED mana, scaled to mana/second. The request is
        // not capped by what the pool could pay, so -N can exceed +N.
        // The current sample enters next tick.
        double produced=0,used=0;
        for (size_t i=0;i<29;++i) {
            produced+=player.displayResources.samples[i][0];
            used+=player.displayResources.samples[i][1];
        }
        income=float(produced*double(1.034482717514038f));
        expenditure=float(used*double(1.034482717514038f));
    }
    int manaBulbFrame(int count) const {
        if (count<=1 || storage<=0) return 0;
        // 4b221e truncates; it does not round up to the next fill level.
        return int(std::clamp(double(mana)/double(storage),0.0,1.0)*double(count-1));
    }
    int32_t score = 0;
    int kills = 0, unitCount = 0, team = 0;
    uint32_t combatEvents = 0;      // battle-music input (Player::combatEvents)
    int built = 0, losses = 0;      // end-of-game scoreboard counters
    float defeatedAt = -1;          // world clock at elimination (-1 = still in)
    bool defeated = false;
    uint64_t emoteSequence = 0;
    bool emoteDisco = false;
};

// A complete per-tick render snapshot -- everything the render/HUD reads from the sim.
// Held in a ring (see frameBuf_): the render reads front(), the writer fills the oldest free slot
// and publishes it. `live` points into THIS Frame's `units`, so it swaps consistently.
struct Frame {
    std::vector<UnitR> units;            // compact current records, never indexed by lifetime ID
    // Small ID-to-slot table may retain history; full snapshots only retain current units.
    std::vector<int32_t> unitSlots;
    int slot(int id) const {
        if(id<0 || size_t(id)>=unitSlots.size())return -1;
        const int32_t index=unitSlots[size_t(id)];
        if(index<0 || size_t(index)>=units.size())return -1;
        const auto& u=units[size_t(index)];
        return u.id==id && u.gen==gen && u.type ? index : -1;
    }
    const UnitR* unit(int id) const {
        const int index=slot(id);return index<0 ? nullptr : &units[size_t(index)];
    }
    std::vector<const UnitR*> live;      // compact list of units live this tick (points into units)
    // Broad phase over `live` (workstream L): built by captureFrame from the positions it
    // copied, indices into `live`. Presentation only -- never read by the simulation.
    ta::render::UnitGrid unitGrid;
    uint64_t unitGridBuildNs = 0;        // capture-thread time of that build (telemetry)
    // Indices into `live` of the units whose captured orders hold a build order the
    // queued-build ghosts draw (buildType, not an area job), ascending: every other
    // unit draws no ghost, so the renderer need not walk the whole list for them.
    std::vector<uint32_t> buildGhostUnits;
    // Indices into `live` of the units whose type flies, ascending. Cursor picking
    // (GameView::pickCandidates) takes them from a query of their own, whose band also
    // allows for a flyer's altitude lift.
    std::vector<uint32_t> flyerUnits;
    // The largest corpse footprint side (UnitR::corpseFootX/Z) of any live record: the
    // body pick circle (reclaimPickRadius) of every record is within its radius.
    int maxCorpseFoot = 0;
    // The largest |x - px - pox|, |z - pz - poz|, |swap.ox| or |swap.oz| of any live record:
    // interpPose (px + pox + (x + swap.ox - px - pox) a, a in [0, 1]) never puts a unit further
    // than this from its snapshot position.
    float maxInterpStep = 0;
    std::array<PlayerR, 8> players{};
    int numPlayers = 0;
    // The local display plane (client/retailfog.h): one byte per 32 px projected
    // cell, kUnexplored/kExplored/kInView. Empty for a noFog_ view. fogGen bumps
    // whenever the content changes, so the overlay re-uploads only then.
    std::vector<uint8_t> fog;
    int fogW = 0, fogH = 0;
    uint32_t fogGen = 0;
    std::vector<uint8_t> effectVisibility;
    int effectVisW = 0, effectVisH = 0;

    std::vector<ta::sim::Projectile> projectiles;
    std::vector<ta::sim::FlameShot> flames;
    std::vector<ta::sim::World::HitFx> hits;   // weapon impacts this tick (cosmeticStep FX)
    int winningTeam = -1;                // world_.winningTeam() (victory overlay)
    ta::sim::RetailWind wind;
    uint32_t gameTick = 0;               // world_.tickCount() (benchmark timing)
    bool benchEnded = false;             // world_.benchmarkEnded(): the server ended the run
    bool scenarioClock = false;         // CRT Display gameclock action
    uint64_t tickMs = 0;                 // wall-clock of this tick (for interpolation)
    float tickDurMs = 1000.0f / 30.0f;
    uint64_t pubCounter = 0;             // performance counter at publish (presentation clock gaps)
    uint32_t gen = 0;                    // capture generation (UnitR.gen == this => live this tick)
    // World lifetime the units belong to (GameView::worldEpoch_): unit ids restart
    // at 1 after a replay/rejoin reset, so render caches key identity on (epoch, id).
    // Presentation only; never hashed.
    uint32_t worldEpoch = 0;
    // Shake state is snapshotted; mission audio uses an ordered event queue
    // captured under the worker's lock so skipped snapshots cannot drop lines.
    ta::sim::World::ShakeReq shakeReq;
};
