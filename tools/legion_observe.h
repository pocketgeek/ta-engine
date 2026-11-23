#pragma once
// legion_observe.h -- one const observer for group-motion scenarios (Legion
// and Retail alike), header-only. It reads `const World&` and nothing else:
// it never mutates the world, never calls a lazy cache that could, and uses
// integer math only (positions are 16.16 Fixed, distances integer square
// roots), so every compiler reports the same numbers. Members are iterated in
// the order the groups list them; the pair counts that need two members run
// once, at report time.
//
//   Observer obs(config);                 // groups, gates, lines, probes
//   obs.setStaticBlocked(mask,W,H);       // optional: wall/clearance metrics
//   for(t...) {world.tick(...); obs.work("fieldWork",v); obs.sample(world,t);}
//   auto keys=obs.report();               // ordered key -> int64
//   std::puts(Observer::json(keys).c_str());
//
// Call sample() once per tick, after the tick. The cheap parts (progress,
// age classes, motion, gates, lines, flyers) run every call; the decision-
// stability and spacing parts run when tick % decisionEvery == 0.
//
// Metric definitions (docs: the observer section of the Legion plan, T8):
// - Progress. A member has ARRIVED when its orders are empty, its speed is 0
//   and its centre lies inside the packed-disc limit of its group's click
//   point: 16*(foot+1)*sqrt(N/pi)*1.25 px (foot: the group's largest
//   footprint side, N: the bodies of the command its click belongs to --
//   the group's own size unless several groups' clicks join one convoy,
//   Group::discN). t50/t90/done are the first ticks
//   half/90%/all of the group had arrived; left_behind is the rest at the end.
//   orders_done is the first tick every live member's orders were empty.
//   Click level (lead ruling W3 final exit (f)): groups that name one
//   Group::click (one-body groups whose clicks one convoy merged: aware-*,
//   motion-*) are also read as one command: click.<name>.n/arrived/t50/t90/
//   done over all their bodies (each against its own group's disc), and each
//   such group reports g.<name>.click_n, the click's bodies. A one-body
//   group's own arrived/t90 is then report-only (tools/legion_check.py).
// - Completion (AR-08). When a member's orders empty, its distance from the
//   click (whole cells) is recorded: complete_outside_radius counts those
//   beyond the packed-disc limit; complete_dist_median/max summarise them.
// - Retail age classes. A member with orders that has not moved for 10-150
//   ticks is WAITING, longer is PARKED (retailgrade.h's recent/stale stamps).
//   Each splits into held_by_design (Legion's member state is Holding,
//   Waiting for its field, Arrived or Trapped, or its leg completed and not
//   yet retired: a deliberate stand) and no_progress (Legion says Moving, or
//   the unit is not a Legion member).
// - Motion. spins: heading changed without the position changing.
//   reversals: return to a 16 px cell left less than 90 ticks earlier
//   (A -> B -> A over the last 6 cells). stop_go: a start after a stop while
//   the orders last. sideways/backward: a real step (> 1/4 px) more than
//   45/90 degrees off the facing. walk_in_place: ticks a ground member with
//   orders keeps its walk (speed > 0, not legionStill) after 16 or more ticks
//   without headway (a step under baseSpeed/4 and no turn): two of Legion's
//   8-tick headway windows (World::updateLegionStill), within which such a
//   body always stands (6 in Retail, whose count is a comparison). In Retail it counts the
//   internal speed a refused mover keeps, not its gait: GET 29 reads 0 after
//   two refusals, so the gait stands (walk_net reads the gait).
//   walk_net /
//   walk_net_runs: the user replay's walk-in-place rule (2026-10-13): ticks a
//   ground body's GET 29 reads above 5 (its gait walks) while it is under 8
//   px from where it was 30 ticks earlier, in runs of 30 ticks or more (runs
//   15 ticks apart or less are one), and how many such runs. statue_ticks: a
//   ground member with orders steps at no more than cap/8 (maxVel/8, 1/8
//   slack for rounding) without turning. back: a decision sample whose last
//   step opposes the member's 10-sample mean direction (cos < -0.3; the
//   pinwheel2/lanes2 'back').
// - Progress clocks (Legion only: the key is left out of a run without a Legion
//   navigator). stall_stale_ticks: member updates that reached a new best
//   potential on the group's field (the headway clock) without restarting the
//   stall clock, the progress clock the contact arrival, B1's blocked re-request
//   and the settle rule read (LegionNavigator::staleStall). The two clocks
//   measure one walk; a body whose progress mixed squared cells with potential
//   x64 walked on with a stale stall clock.
// - Decision stability (every decisionEvery ticks, from observed state only).
//   flips: a member's moving/still class changed between two samples while it
//   has orders; flip_rate_per30_permille = flips per 30 ordered member-ticks,
//   x1000. aim_reversals: the side of the member's step relative to the way
//   to its goal (sin > 1/6) flips. engagement_on_flowing: after 30 or more
//   free samples (a step of at least half its base speed) the member stops
//   (under a quarter) or deviates by more than 45 degrees. crawl_samples: a
//   sample's step is positive but no more than cap/8 per tick.
//   transit_samples: ordered ground samples more than 10 cells from the final goal; transit_held_permille: those
//   at no speed; the _far_ pair counts only samples more than 100 cells out (W8 step 0, the MV-12 spacing probe's
//   far held); g.NAME.stops: a transit sample held right after a moving one (the probe's stops by fast / slow type).
//   follow_chain_max/mean: the longest run of same-group movers each within
//   two body widths behind the next (inside 45 degrees of its own motion),
//   at exactly the same speed.
// - Lanes. A Gate is a strip of centre cells: files = distinct lateral bands
//   (band cells wide) among members inside it, averaged over ticks with at
//   least minCount inside; spread = lateral extent. crossings (pw.py): the
//   first lateral cell at each end window of the strip (edge cells deep) is
//   recorded per member; two same-group members that traversed it the same
//   way, entering within pairWindow ticks, cross when their lateral order
//   flips by at least flipCells (default: the group's body width) at both
//   ends. LaneOrder (the MV-13 S-bend probe): the same flip test between the
//   first entries into a line before a vertex and a line after it (pairs of
//   one group unless sameGroup is off). Hug: a
//   member is hugging a vertex when its lateral cell on entering the vertex's
//   line is within `cells` of the tip; carry is the members that hug both
//   vertices (kept separate from crossings).
//   W9 step 0 (observation only): a crossing is also attributed to a flip SITE, the steering source that moved
//   the pair apart laterally: each member's lateral displacement between its two end-window entries is summed
//   per site (LegionNavigator::steerSite: 0 none, 1 field descent, 2 passage gate, 3 pinwheel, 4 lane pass,
//   5 blocked branch, 6 straight walk), and a flipped pair goes to the site whose displacement DIFFERENCE
//   between the two bodies is largest in the direction the order changed (none when no site moved them that
//   way): gate.NAME.cross_{none,field,gate,pivot,pass,blocked,direct} sum to crossings.
// - Spacing (every decisionEvery ticks, ordered ground members). Every live
//   ground body (landed flyers included) is stamped into an epoch-stamped id
//   grid; the ring of cells round a member's footprint is walked:
//   contact_own (a same-group member with orders), contact_other (a body
//   with orders outside the group): permille of member samples. No pair loop.
//   contact_settled (user decision W3-1, 2026-10-09) is judged PER ARRIVED
//   UNIT and leaves out the unit's own command: the member samples touching
//   a body without orders that another command (selection) issued, or no
//   command at all, x1000 / the members arrived at the end (at least 1). A
//   command is one convoy (lead ruling W3 round 4 (1), user decision 2: the
//   clicks one convoy merges under A1 are one order): noteSelection() labels
//   its units with the convoy the caller found for the click (a click of 450
//   lands in 64-unit parts and stays one command; four selections clicked to
//   one point on ticks 1-4 are one command), or as a command of its own when
//   the click joins none; a unit never labelled counts its group as its
//   command. The runner applies the same keying in both modes. Bodies settling behind their own army
//   are what a one-order click does by design, so they are not counted, and
//   a mode that delivers more units is not charged for the bodies it parks.
// - Walls (when a static mask or legality provider is set). Per footprint
//   class, a two-pass chessboard chamfer distance transform over the legal
//   origins; clearance = cells of free ground between the footprint and the
//   nearest wall (distance - 1), capped at 11. wall_touch_permille: moving
//   ordered ground samples with clearance 0; the _near variant and the
//   clearance mean/p10 count only samples within 10 cells of a wall (the
//   lanes2 'touch'/'wallgap'). W9 step 0 splits: wall_still_samples /
//   wall_touch_still_permille (ordered ground samples that did not move, the
//   bodies the moving key leaves out) and the moving touches by place, in
//   permille of wall_samples (wall_touch_{gate,ring,corner,flat}_permille, summing
//   to wall_touch_permille up to rounding) and of the near samples
//   (wall_touch_near_{gate,ring,corner,flat}_permille): gate = within 6 cells of a
//   configured gate region, ring = the body's current leg is an attack / guard
//   (the reach ring round its target), corner = an illegal origin on both axes
//   beside it, else flat wall.
// - Pair proximity (the aware case's contacts_permille). Every decision
//   sample, pairs = live ordered members of A x those of B; contact when
//   their centre cells are within `cells` on both axes (a count grid, no
//   pair loop). permille_x100.
// - Flyers (canFly group members). away_max/mean: px from the group's ground
//   centroid while its ground members have orders, from 150 ticks in.
//   takeoffs (landed -> airborne), relifts (a takeoff after landing during
//   the run), go_arounds (a landing abandoned while airborne), hover_max
//   (consecutive airborne ticks under 1/4 px/tick, not landing, from 60 ticks
//   in), land_delay_max (orders empty while airborne to the start of the
//   descent; 9999 if it never began), illegal_overlap_ticks (a landed flyer's
//   footprint on a non-flyer ground body).
// - Side lines: members whose centre cell lies at or beyond `at` on the axis;
//   t90 is the first tick 90% of the observed members were there.
// - Work: any per-tick counters the caller passes (work() for per-tick
//   values, workCumulative() for running totals); max, p99 and total.
#include "sim/sim.h"
#include "sim/footprint.h"
#include "sim/retailmotion.h"
#include "sim/retailanimationqueries.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tak::legion_observe {

using Keys=std::vector<std::pair<std::string,int64_t>>;

// Inclusive rectangle of body centre cells (position >> 20: 16 px cells).
struct Rect {
    int x0=0,z0=0,x1=-1,z1=-1;
    bool contains(int x,int z) const {return x>=x0&&x<=x1&&z>=z0&&z<=z1;}
};
struct Group {
    std::string name;
    std::vector<int> ids;
    int clickX=0,clickZ=0;   // px
    // Bodies of the command (convoy) the group's click joined, its own among
    // them; 0: the group alone. Sizes the packed disc (lead ruling W3 round 4 (1)).
    int discN=0;
    // The click (named by its first group) this group is read at, beside its own keys; empty:
    // none. Set for one-body groups whose clicks one convoy merged (ruling W3 final exit (f)).
    std::string click;
};
struct Gate {
    std::string name;
    Rect region;
    int lateral=1;           // lateral axis: 0 x, 1 z (motion runs along the other)
    int band=2;              // file width in cells
    int minCount=3;          // members inside for a files sample
    int edge=2;              // depth in cells of each end window (crossings)
    int pairWindow=600;      // ticks between two members' entries for a pair
    int flipCells=0;         // 0: the group's body width
};
struct Line {
    Rect region;
    int lateral=1;           // 0 x, 1 z
    int sign=1;              // orients the lateral cell (sign*cell is recorded)
};
struct LaneOrder {
    std::string name;
    Line before,after;
    int window=300;
    int minCells=2;
    bool sameGroup=true;     // false: pairs across groups count too (the audit's wall-4x50 probe)
};
struct Hug {
    std::string name;
    Line first,second;            // regions and lateral axes (sign unused here)
    int tipFirst=0,tipSecond=0;   // lateral cell of each vertex's tip (unsigned)
    int cells=2;
};
struct Side {
    std::string name;
    int axis=0;              // 0 x, 1 z
    int at=0;                // centre cell; >= at is the far side
};
struct Pair {
    std::string a,b;         // group names; a comma list is the union of its groups
    int cells=2;
    std::string name;        // key stem "pair.<name>."; empty: "pair.<a>.<b>."
};

struct Config {
    std::vector<Group> groups;
    std::vector<Gate> gates;
    std::vector<LaneOrder> laneOrders;
    std::vector<Hug> hugs;
    std::vector<Side> sides;
    std::vector<Pair> pairs;
    int decisionEvery=10;
    // Selftest only: 1 makes the crossing flip test strict (> instead of >=
    // one body width), which legion_observe_selftest must detect.
    int mutation=0;
};

namespace detail {
inline int64_t isqrt(int64_t v) {return v<=0?0:int64_t(sim::isqrt64(uint64_t(v)));}
inline int64_t permille(int64_t n,int64_t d) {return d>0?(n*1000+d/2)/d:0;}
inline int centreCell(sim::Fixed p) {return int(p.v>>20);}
// Packed-disc radius squared, px^2: (16*(f+1)*1.25)^2*N/pi with pi = 355/113.
inline int64_t discRadius2(int foot,int n) {return int64_t(400)*(foot+1)*(foot+1)*n*113/355;}
}

class Observer {
public:
    explicit Observer(Config c):cfg_(std::move(c)) {
        for(size_t g=0;g<cfg_.groups.size();++g) {
            for(int id:cfg_.groups[g].ids) {
                bool dup=false;for(const auto& m:m_)dup|=m.id==id;
                if(dup)continue;
                Member m;m.id=id;m.group=int(g);m_.push_back(m);
            }
        }
        groupStats_.resize(cfg_.groups.size());
        for(size_t g=0;g<cfg_.groups.size();++g) {
            const auto& name=cfg_.groups[g].click;
            if(name.empty())continue;
            auto it=std::find_if(clicks_.begin(),clicks_.end(),[&](const ClickStats& c) {return c.name==name;});
            if(it==clicks_.end()) {clicks_.push_back(ClickStats{});clicks_.back().name=name;it=clicks_.end()-1;}
            it->groups.push_back(int(g));
        }
        gateStats_.resize(cfg_.gates.size());
        sideStats_.resize(cfg_.sides.size());
        pairStats_.resize(cfg_.pairs.size());
        for(size_t p=0;p<cfg_.pairs.size();++p) {
            pairStats_[p].a=groupSet(cfg_.pairs[p].a);pairStats_[p].b=groupSet(cfg_.pairs[p].b);
        }
        for(auto& m:m_) {
            m.gate.assign(cfg_.gates.size()*2,Mark{});
            m.gateLat.assign(cfg_.gates.size(),std::array<int64_t,kSites>{});
            m.lane.assign(cfg_.laneOrders.size()*2,Mark{});
            m.hug.assign(cfg_.hugs.size()*2,Mark{});
        }
    }

    // Walls: 1x1 blocked cells (nonzero = blocked), the map's cell size. A
    // footprint origin is legal when every cell under it is inside the map
    // and open. Use setStaticLegal for terrain/plane legality instead.
    void setStaticBlocked(std::vector<uint8_t> blocked,int w,int h) {
        blocked_=std::move(blocked);bw_=w;bh_=h;legalFn_=nullptr;staticChanged();
    }
    // Any legality source (e.g. a test holding a mutable World may pass
    // LegionNavigator::staticLegal). Called only from staticChanged() rebuilds.
    void setStaticLegal(std::function<bool(const sim::Unit&,int,int)> fn,int w,int h) {
        legalFn_=std::move(fn);bw_=w;bh_=h;blocked_.clear();staticChanged();
    }
    void staticChanged() {dt_.clear();}

    // One command issued to `ids`: they are one another's own command for
    // contact_settled until a later selection relabels them. `convoy` (> 0)
    // is the convoy the click joined: every selection noted with the same
    // convoy is one command (ruling W3 round 4 (1)); 0 makes the selection a
    // command of its own. A shift-queued order keeps its units' current command.
    void noteSelection(const std::vector<int>& ids,uint32_t convoy=0) {
        const int64_t cmd=convoy?kConvoyCommand+int64_t(convoy):++selections_;
        for(int id:ids) {
            auto it=std::lower_bound(selection_.begin(),selection_.end(),std::pair<int,int64_t>{id,INT64_MIN});
            if(it!=selection_.end()&&it->first==id)it->second=cmd;
            else selection_.insert(it,{id,cmd});
        }
    }

    void work(std::string_view name,uint64_t value) {workSeries(name).pending+=value;}
    void workCumulative(std::string_view name,uint64_t total) {
        auto& s=workSeries(name);
        s.pending+=total>=s.lastTotal?total-s.lastTotal:0;s.lastTotal=total;
    }

    void sample(const sim::World& w,int64_t tick) {
        if(start_<0)start_=tick;
        ++samples_;
        const bool decide=cfg_.decisionEvery>0&&tick%cfg_.decisionEvery==0;
        // Read-only: legionNavigator() is a plain accessor; unitState is const.
        const auto* nav=const_cast<sim::World&>(w).legionNavigator();
        legion_=legion_||nav;
        const bool anyFlyer=hasFlyers(w);
        if(anyFlyer||decide)buildGrid(w,anyFlyer);
        for(auto& m:m_)perTick(w,m,tick,nav);
        progress(w,tick);
        gates();
        sides(tick);
        if(anyFlyer)flyerAway(w,tick);
        if(decide) {
            for(auto& m:m_)decision(w,m);
            followChains();
            spacing(w);
            pairProximity(w);
        }
        for(auto& s:work_) {s.values.push_back(s.pending);s.pending=0;}
    }

    // W9 step 0: the live members of group g that have not arrived (the progress() test: no orders, standing
    // still, inside the group's disc) at this moment -- the tails a decision trace follows.
    std::vector<int> leftBehind(const sim::World& w,size_t g) {
        std::vector<int> ids;
        const int64_t r2=groupRadius2(g);
        const auto& grp=cfg_.groups[g];
        for(const auto& m:m_) {
            if(m.group!=int(g))continue;
            const auto* u=w.unit(m.id);
            if(!u||!u->alive())continue;
            const int64_t dx=(int64_t(u->x.v)>>16)-grp.clickX,dz=(int64_t(u->z.v)>>16)-grp.clickZ;
            if(!u->orders.empty()||u->speed.v!=0||dx*dx+dz*dz>r2)ids.push_back(m.id);
        }
        return ids;
    }
    size_t groupCount() const {return cfg_.groups.size();}
    const std::string& groupName(size_t g) const {return cfg_.groups[g].name;}

    Keys report() const {
        Keys k;
        auto put=[&](std::string key,int64_t v) {k.emplace_back(std::move(key),v);};
        put("members",int64_t(m_.size()));put("samples",samples_);
        for(size_t g=0;g<cfg_.groups.size();++g) {
            const auto& gs=groupStats_[g];const std::string p="g."+cfg_.groups[g].name+".";
            int n=0,flyers=0,landed=0;std::vector<int64_t> dists;int64_t outside=0;
            int64_t takeoffs=0,takeoffsMax=0,relifts=0,reliftsMax=0,goArounds=0,hoverMax=0,landDelayMax=0;
            for(const auto& m:m_) {
                if(m.group!=int(g))continue;
                ++n;
                if(m.completeDist>=0) {dists.push_back(m.completeDist);outside+=m.completeOutside;}
                if(!m.flyer)continue;
                ++flyers;landed+=m.mode==1;
                takeoffs+=m.takeoffs;takeoffsMax=std::max<int64_t>(takeoffsMax,m.takeoffs);
                relifts+=m.relifts;reliftsMax=std::max<int64_t>(reliftsMax,m.relifts);
                goArounds+=m.goArounds;hoverMax=std::max<int64_t>(hoverMax,m.hoverMax);
                landDelayMax=std::max<int64_t>(landDelayMax,m.idleAt>=0?9999:m.landDelayMax);
            }
            std::sort(dists.begin(),dists.end());
            put(p+"n",n);put(p+"radius_px",detail::isqrt(gs.radius2));
            put(p+"arrived",gs.arrived);put(p+"t50",gs.t50);put(p+"t90",gs.t90);put(p+"done",gs.done);
            put(p+"stops",gs.stops);
            put(p+"left_behind",n-gs.arrived-gs.dead);put(p+"dead",gs.dead);put(p+"orders_done",gs.ordersDone);
            for(const auto& c:clicks_)if(c.name==cfg_.groups[g].click)put(p+"click_n",c.n);
            put(p+"complete_n",int64_t(dists.size()));put(p+"complete_outside_radius",outside);
            put(p+"complete_dist_median",dists.empty()?-1:dists[(dists.size()-1)/2]);
            put(p+"complete_dist_max",dists.empty()?-1:dists.back());
            if(flyers) {
                put(p+"flyers",flyers);put(p+"flyers_landed",landed);
                put(p+"away_max",gs.awayMax);put(p+"away_mean",gs.awaySamples?gs.awaySum/gs.awaySamples:0);
                put(p+"takeoffs",takeoffs);put(p+"takeoffs_max",takeoffsMax);
                put(p+"relifts",relifts);put(p+"relifts_max",reliftsMax);put(p+"go_arounds",goArounds);
                put(p+"hover_max",hoverMax);put(p+"land_delay_max",landDelayMax);
                put(p+"illegal_overlap_ticks",gs.illegalOverlap);
            }
        }
        for(const auto& c:clicks_) {
            const std::string p="click."+c.name+".";
            put(p+"n",c.n);put(p+"arrived",c.arrived);put(p+"t50",c.t50);put(p+"t90",c.t90);put(p+"done",c.done);
        }
        put("spins",spins_);put("reversals",reversals_);put("stop_go",stopGo_);
        put("sideways",sideways_);put("backward",backward_);put("back",back_);
        put("walk_in_place",walkInPlace_);put("statue_ticks",statue_);put("crawl_samples",crawl_);
        {
            int64_t ticks=walkNet_,runs=walkNetRuns_;
            for(const auto& m:m_)if(m.walkFrom>=0&&m.walkTo-m.walkFrom+1>=30) {ticks+=m.walkTo-m.walkFrom+1;++runs;}
            put("walk_net",ticks);put("walk_net_runs",runs);
        }
        {   // AR-01/AR-02 tails (W3 tail fixture): bodies whose order is open while they stood still 900+ ticks.
            int64_t openStill=0,stillEver=0;
            for(const auto& m:m_) {
                if(!m.seen)continue;
                openStill+=m.stillRun>=900;stillEver+=m.maxStillRun>=900;
            }
            put("open_still900",openStill);put("still900_ever",stillEver);
        }
        put("waiting_held",waitingHeld_);put("waiting_no_progress",waitingNoProgress_);
        put("parked_held",parkedHeld_);put("parked_no_progress",parkedNoProgress_);
        if(legion_)put("stall_stale_ticks",stallStale_);
        put("transit_samples",transit_);put("transit_held_permille",detail::permille(transitHeld_,transit_));
        put("transit_far_samples",transitFar_);put("transit_far_held_permille",detail::permille(transitFarHeld_,transitFar_));
        put("decision_samples",ordered_);put("stopped_permille",detail::permille(stopped_,ordered_));
        // Pathfinding deep dive: the motion keys by the mechanism of the body's last update (LegionNavigator::Why).
        // Report-only; Retail bodies are no members, so everything of theirs is "none".
        for(int k=0;k<kWhys;++k) {
            const auto& wa=why_[size_t(k)];
            if(!wa.ticks&&!wa.samples)continue;
            const std::string p=std::string("why.")+sim::LegionNavigator::whyName(k)+".";
            put(p+"ticks",wa.ticks);put(p+"moved_ticks",wa.movedTicks);put(p+"samples",wa.samples);put(p+"stopped",wa.stopped);
            put(p+"statue",wa.statue);put(p+"sideways",wa.sideways);put(p+"backward",wa.backward);put(p+"reversals",wa.reversals);
        }
        put("flips",flips_);
        put("flip_rate_per30_permille",flipSamples_?(flips_*30*1000+flipSamples_*cfg_.decisionEvery/2)/(flipSamples_*cfg_.decisionEvery):0);
        put("aim_reversals",aimReversals_);put("engagement_on_flowing",engagement_);
        put("follow_chain_max",followMax_);
        put("follow_chain_mean_x100",followSamples_?(followSum_*100+followSamples_/2)/followSamples_:0);
        int64_t detourSum=0,detourN=0;
        for(const auto& m:m_) {
            if(!m.seen)continue;
            const int64_t dx=(int64_t(m.x)-m.startX)>>8,dz=(int64_t(m.z)-m.startZ)>>8;
            const int64_t straight=detail::isqrt(dx*dx+dz*dz);   // 1/256 px
            if(straight>64*256) {detourSum+=(m.travelled>>8)*1000/straight;++detourN;}
        }
        put("detour_permille",detourN?detourSum/detourN-1000:0);
        put("spacing_samples",spacingSamples_);
        put("contact_own_permille",detail::permille(contactOwn_,spacingSamples_));
        put("contact_other_permille",detail::permille(contactOther_,spacingSamples_));
        {   // W3-1: per arrived unit (see the header).
            int64_t arrived=0;for(const auto& gs:groupStats_)arrived+=gs.arrived;
            put("contact_settled_permille",detail::permille(contactSettled_,std::max<int64_t>(arrived,1)));
        }
        if(bw_>0) {
            put("wall_samples",wallSamples_);put("wall_touch_permille",detail::permille(wallTouch_,wallSamples_));
            put("wall_near_samples",int64_t(near_.size()));
            int64_t touching=0,sum=0;for(int g:near_) {touching+=g<=0;sum+=g;}
            put("wall_touch_near_permille",detail::permille(touching,int64_t(near_.size())));
            {   // W9 step 0 splits (see the header)
                static constexpr const char* kNames[4]={"flat","gate","ring","corner"};
                std::array<int64_t,4> nearBy{};
                for(size_t i=0;i<near_.size();++i)nearBy[nearPlace_[i]&3]+=near_[i]<=0;
                put("wall_still_samples",wallStill_);put("wall_touch_still_permille",detail::permille(wallStillTouch_,wallStill_));
                for(int c=0;c<4;++c) {
                    put(std::string("wall_touch_")+kNames[c]+"_permille",detail::permille(wallTouchBy_[size_t(c)],wallSamples_));
                    put(std::string("wall_touch_near_")+kNames[c]+"_permille",detail::permille(nearBy[size_t(c)],int64_t(near_.size())));
                }
            }
            put("clearance_mean_x100",near_.empty()?-1:(sum*100+int64_t(near_.size())/2)/int64_t(near_.size()));
            auto sorted=near_;std::sort(sorted.begin(),sorted.end());
            put("clearance_p10_x100",sorted.empty()?-1:int64_t(sorted[sorted.size()/10])*100);
        }
        for(size_t i=0;i<cfg_.gates.size();++i) {
            const auto& s=gateStats_[i];const std::string p="gate."+cfg_.gates[i].name+".";
            int64_t pairs=0,cross=0;std::array<int64_t,kSites> bySite{};gateCrossings(i,pairs,cross,&bySite);
            put(p+"samples",s.samples);
            put(p+"files_x100",s.samples?(s.files*100+s.samples/2)/s.samples:0);
            put(p+"spread_x100",s.samples?(s.spread*100+s.samples/2)/s.samples:0);
            put(p+"pairs",pairs);put(p+"crossings",cross);
            static constexpr const char* kSiteNames[kSites]={"none","field","gate","pivot","pass","blocked","direct"};
            for(int k=0;k<kSites;++k)put(p+"cross_"+kSiteNames[k],bySite[size_t(k)]);
        }
        for(size_t i=0;i<cfg_.laneOrders.size();++i) {
            const std::string p="lane."+cfg_.laneOrders[i].name+".";
            int64_t pairs=0,swaps=0;laneSwaps(i,pairs,swaps);
            put(p+"pairs",pairs);put(p+"swaps",swaps);put(p+"swaps_permille",detail::permille(swaps,pairs));
        }
        for(size_t i=0;i<cfg_.hugs.size();++i) {
            const auto& h=cfg_.hugs[i];const std::string p="hug."+h.name+".";
            int64_t first=0,second=0,carry=0;
            for(const auto& m:m_) {
                const auto& a=m.hug[i*2];const auto& b=m.hug[i*2+1];
                const bool ha=a.tick>=0&&std::abs(a.lat-h.tipFirst)<=h.cells;
                const bool hb=b.tick>=0&&std::abs(b.lat-h.tipSecond)<=h.cells;
                first+=ha;second+=hb;carry+=ha&&hb;
            }
            put(p+"first",first);put(p+"second",second);put(p+"carry",carry);
        }
        for(size_t i=0;i<cfg_.sides.size();++i) {
            const auto& s=sideStats_[i];const std::string p="side."+cfg_.sides[i].name+".";
            put(p+"high",s.high);put(p+"low",s.low);put(p+"t90",s.t90);
        }
        for(size_t i=0;i<cfg_.pairs.size();++i) {
            const auto& s=pairStats_[i];const std::string p="pair."+(cfg_.pairs[i].name.empty()?cfg_.pairs[i].a+"."+cfg_.pairs[i].b:cfg_.pairs[i].name)+".";
            put(p+"pairs",s.pairs);put(p+"contacts",s.contacts);
            put(p+"permille_x100",s.pairs?(s.contacts*100000+s.pairs/2)/s.pairs:0);
        }
        for(const auto& s:work_) {
            const std::string p="work."+s.name+".";
            auto v=s.values;uint64_t total=0,max=0;for(auto x:v) {total+=x;max=std::max(max,x);}
            uint64_t p99=0;
            if(!v.empty()) {
                const size_t at=(v.size()*99+99)/100-1;
                std::nth_element(v.begin(),v.begin()+ptrdiff_t(at),v.end());p99=v[at];
            }
            put(p+"max",int64_t(max));put(p+"p99",int64_t(p99));put(p+"total",int64_t(total));
        }
        return k;
    }

    static std::string json(const Keys& keys) {
        std::string s="{";
        for(size_t i=0;i<keys.size();++i) {
            if(i)s+=",";
            s+="\""+keys[i].first+"\":"+std::to_string(keys[i].second);
        }
        return s+"}";
    }
    static int64_t get(const Keys& keys,std::string_view key,int64_t fallback=INT64_MIN) {
        for(const auto& [k,v]:keys)if(k==key)return v;
        return fallback;
    }

    static constexpr int kSites=sim::LegionNavigator::kSites;
    static constexpr int kWhys=sim::LegionNavigator::kWhys;
private:
    struct Mark {int lat=0;int64_t tick=-1;};
    struct Member {
        int id=0,group=0;
        bool seen=false,flyer=false,alive=false;
        int foot=1,fx=1,fz=1;
        int32_t x=0,z=0,startX=0,startZ=0;uint16_t heading=0;
        int64_t travelled=0;   // raw px (1/65536) along the per-tick steps
        // legacy Motion: the last cells visited and when.
        std::vector<std::pair<int64_t,int64_t>> cells;
        bool moved=false,everMoved=false,hadOrders=false;
        int stillRun=0,noHeadRun=0,maxStillRun=0;
        // walk_net: the last 31 positions (a ring) and the open run (-1 none).
        std::array<std::array<int32_t,2>,31> netAt{};int netN=0,netHead=0;int64_t walkFrom=-1,walkTo=-1;
        int completeDist=-1;bool completeOutside=false;
        // decision samples: positions at the last 11 samples (10 steps).
        std::vector<std::pair<int32_t,int32_t>> ring;
        int lastClass=-1,aimSign=0,freeRun=0;
        int wasHeld=-1;          // transit: the last sample's held class (1 held, 0 moving), -1 none yet
        uint8_t whyLast=0;       // LegionNavigator::steerWhy at the last per-tick sample (the why.* keys)
        int64_t speed=0;bool chainMover=false;int64_t stepX=0,stepZ=0;
        // flyers
        int mode=-1;bool landing=false,landedOnce=false;
        int takeoffs=0,relifts=0,goArounds=0,hoverRun=0,hoverMax=0;
        int64_t idleAt=-1;int64_t landDelayMax=0;
        std::vector<Mark> gate,lane,hug;   // [probe*2 + end]
        // W9 step 0: per gate, the signed lateral displacement (1/256 px) walked between the two end-window
        // entries, summed per steering site (LegionNavigator::steerSite 0..5).
        std::vector<std::array<int64_t,kSites>> gateLat;
    };
    struct GroupStats {
        int64_t radius2=0;int foot=0;
        int n=0,arrived=0,dead=0;int64_t t50=-1,t90=-1,done=-1,ordersDone=-1;
        int64_t awayMax=0,awaySum=0,awaySamples=0,illegalOverlap=0;
        int64_t stops=0;         // transit samples: held right after a moving one (W8 step 0, the MV-12 fast/slow stops)
    };
    // A click read as one command (ruling W3 final exit (f)): its groups' bodies together.
    struct ClickStats {std::string name;std::vector<int> groups;int n=0,arrived=0;int64_t t50=-1,t90=-1,done=-1;};
    struct GateStats {int64_t samples=0,files=0,spread=0;};
    struct SideStats {int64_t high=0,low=0,t90=-1;};
    struct PairStats {std::vector<char> a,b;int64_t pairs=0,contacts=0;};   // a, b: per group index
    struct WorkSeries {std::string name;uint64_t pending=0,lastTotal=0;std::vector<uint64_t> values;};

    int groupIndex(const std::string& name) const {
        for(size_t g=0;g<cfg_.groups.size();++g)if(cfg_.groups[g].name==name)return int(g);
        return -1;
    }
    // Membership flags over the groups of a comma list; empty when any name is unknown.
    std::vector<char> groupSet(const std::string& list) const {
        std::vector<char> in(cfg_.groups.size(),0);
        size_t at=0;
        while(at<=list.size()) {
            size_t e=list.find(',',at);if(e==std::string::npos)e=list.size();
            const int g=groupIndex(list.substr(at,e-at));
            if(g<0)return {};
            in[size_t(g)]=1;at=e+1;
        }
        return in;
    }
    WorkSeries& workSeries(std::string_view name) {
        for(auto& s:work_)if(s.name==name)return s;
        WorkSeries s;s.name=std::string(name);s.values.assign(size_t(samples_),0);
        work_.push_back(std::move(s));return work_.back();
    }
    bool hasFlyers(const sim::World& w) const {
        for(const auto& m:m_)if(const auto* u=w.unit(m.id);u&&u->type&&u->type->canFly)return true;
        return false;
    }
    static bool groundBody(const sim::Unit& u) {
        return u.alive()&&!u.embarked()&&u.type&&!u.type->isStructure()&&!(u.type->canFly&&u.flightGroundMode!=1);
    }

    // ---- id grid: every live ground body's footprint, epoch-stamped ----
    void buildGrid(const sim::World& w,bool flyerCheck) {
        gw_=w.mapW();gh_=w.mapH();
        const size_t cells=size_t(std::max(gw_,0))*size_t(std::max(gh_,0));
        if(gridEpoch_.size()!=cells) {gridEpoch_.assign(cells,0);gridId_.assign(cells,0);epoch_=0;}
        ++epoch_;
        auto stamp=[&](const sim::Unit& u) {
            const int ox=sim::footprintOrigin(u.x,u.type->footX),oz=sim::footprintOrigin(u.z,u.type->footZ);
            for(int j=0;j<u.type->footZ;++j)for(int i=0;i<u.type->footX;++i) {
                const int x=ox+i,z=oz+j;
                if(x<0||z<0||x>=gw_||z>=gh_)continue;
                gridEpoch_[size_t(z)*size_t(gw_)+size_t(x)]=epoch_;gridId_[size_t(z)*size_t(gw_)+size_t(x)]=u.id;
            }
        };
        // Non-flyer bodies first, so a landed flyer can be checked against them.
        for(const auto& u:w.units())if(groundBody(u)&&!u.type->canFly)stamp(u);
        if(flyerCheck) {
            for(const auto& m:m_) {
                const auto* u=w.unit(m.id);
                if(!u||!u->alive()||!u->type||!u->type->canFly||u->flightGroundMode!=1)continue;
                if(overlapsBody(*u))++groupStats_[size_t(m.group)].illegalOverlap;
            }
        }
        for(const auto& u:w.units())if(groundBody(u)&&u.type->canFly)stamp(u);
    }
    int bodyAt(int x,int z) const {
        if(x<0||z<0||x>=gw_||z>=gh_)return 0;
        const size_t i=size_t(z)*size_t(gw_)+size_t(x);
        return gridEpoch_[i]==epoch_?gridId_[i]:0;
    }
    bool overlapsBody(const sim::Unit& u) const {
        const int ox=sim::footprintOrigin(u.x,u.type->footX),oz=sim::footprintOrigin(u.z,u.type->footZ);
        for(int j=0;j<u.type->footZ;++j)for(int i=0;i<u.type->footX;++i) {
            const int id=bodyAt(ox+i,oz+j);
            if(id&&id!=u.id)return true;
        }
        return false;
    }

    // ---- per tick, per member ----
    void perTick(const sim::World& w,Member& m,int64_t tick,const sim::LegionNavigator* nav) {
        const auto* u=w.unit(m.id);
        if(!u||!u->alive()||!u->type) {m.moved=false;m.alive=false;return;}
        m.alive=true;
        const bool fresh=!m.seen;
        if(fresh) {
            m.seen=true;m.flyer=u->type->canFly;m.fx=u->type->footX;m.fz=u->type->footZ;m.foot=std::max(m.fx,m.fz);
            m.x=m.startX=u->x.v;m.z=m.startZ=u->z.v;m.heading=uint16_t(u->heading.v);m.mode=u->flightGroundMode;
            m.landing=u->landing.has_value();
        }
        if(nav&&nav->staleStall(m.id))++stallStale_;
        const int why=nav?std::clamp(nav->steerWhy(m.id),0,kWhys-1):0;m.whyLast=uint8_t(why);
        const bool orders=!u->orders.empty();
        const int64_t dx=int64_t(u->x.v)-m.x,dz=int64_t(u->z.v)-m.z;
        const bool moved=!fresh&&(dx||dz);
        const bool turned=!fresh&&uint16_t(u->heading.v)!=m.heading;
        const int64_t step2=dx*dx+dz*dz;
        if(moved)m.travelled+=detail::isqrt(step2);
        // spins and cell reversals (the legacy Motion observer)
        if(!fresh&&!moved&&turned)++spins_;
        const int64_t cell=(int64_t(u->z.v>>20)<<32)|uint32_t(u->x.v>>20);
        if(m.cells.empty()||m.cells.back().first!=cell) {
            for(size_t i=0;i+1<m.cells.size();++i)
                if(m.cells[i].first==cell&&tick-m.cells[i].second<90) {++reversals_;++why_[size_t(why)].reversals;break;}
            m.cells.push_back({cell,tick});
            if(m.cells.size()>6)m.cells.erase(m.cells.begin());
        }
        if(orders) {
            if(moved&&!m.moved&&m.everMoved)++stopGo_;
            // sideways / backward against the facing (legion_group_motion)
            if(moved&&step2>int64_t(16384)*16384) {
                const auto way=sim::retailHeadingToPort(uint16_t(sim::retailDirection(sim::Fixed::raw(int32_t(-dx)),sim::Fixed::raw(int32_t(-dz))).v));
                const int32_t off=std::abs(sim::retailTurnRequest(way,u->heading));
                sideways_+=off>8192;backward_+=off>16384;
                why_[size_t(why)].sideways+=off>8192;why_[size_t(why)].backward+=off>16384;
            }
            // age classes
            m.stillRun=moved?0:m.stillRun+1;
            m.maxStillRun=std::max(m.maxStillRun,m.stillRun);
            if(m.stillRun>=10) {
                const int state=nav?nav->unitState(m.id):0;
                // A leg already completed (its arrival event set, 0x500) and
                // waiting for World to retire it is a deliberate stand, not
                // a body without progress: Legion drops the member on the
                // completing tick, so it read as no_progress for that tick.
                const auto& leg=u->orders[sim::World::currentLeg(u->orders)];
                const bool held=state>=2||(leg.mission.pending&0x500)==0x500;
                if(m.stillRun<=150)(held?waitingHeld_:waitingNoProgress_)++;
                else (held?parkedHeld_:parkedNoProgress_)++;
            }
            if(!m.flyer) {
                const int64_t pace=std::max<int64_t>(1,u->baseSpeed.v/4);
                m.noHeadRun=(step2>=pace*pace||turned)?0:m.noHeadRun+1;
                // (Retail keeps the 6 ticks of Legion's old per-tick rule: its count is a comparison only.)
                if(m.noHeadRun>=(nav?2*sim::LegionHeadway::kWindow:6)&&u->speed.v>0&&!u->legionStill)++walkInPlace_;
                m.netAt[size_t(m.netHead)]={u->x.v,u->z.v};m.netHead=(m.netHead+1)%31;m.netN=std::min(m.netN+1,31);
                if(m.netN==31) {
                    // GET 29 (World::ScriptHost, 4dc100): the step at the body's speed against its
                    // terrain-scaled base speed, 0 while it stands in its scripts.
                    const auto step=sim::retailGroundStep(u->heading,u->speed);
                    const int32_t get29=sim::retailHorizontalAnimationPercent(step.s.v,step.c.v,(u->baseSpeed*w.groundTerrainMultiplier(*u)).v,
                        u->bodyBlockStreak>=2||u->legionStill,u->embarked());
                    const auto& old=m.netAt[size_t(m.netHead)];
                    const int64_t nx=(int64_t(u->x.v)-old[0])>>8,nz=(int64_t(u->z.v)-old[1])>>8;
                    if(get29>5&&nx*nx+nz*nz<int64_t(8*256)*(8*256)) {
                        if(m.walkFrom>=0&&tick-m.walkTo<=15)m.walkTo=tick;
                        else {closeWalk(m);m.walkFrom=m.walkTo=tick;}
                    } else if(m.walkFrom>=0&&tick-m.walkTo>15)closeWalk(m);
                }
                const int64_t cap=int64_t(u->type->maxVel.v)*9/64;   // cap/8 plus 1/8 slack
                ++why_[size_t(why)].ticks;why_[size_t(why)].movedTicks+=moved;
                if(moved&&!turned&&step2<=cap*cap) {++statue_;++why_[size_t(why)].statue;}
            }
        } else {
            m.stillRun=0;m.noHeadRun=0;
        }
        // completion (AR-08)
        if(orders)m.hadOrders=true;
        else if(m.hadOrders&&m.completeDist<0) {
            const auto& g=cfg_.groups[size_t(m.group)];
            const int64_t cx=(int64_t(u->x.v)>>16)-g.clickX,cz=(int64_t(u->z.v)>>16)-g.clickZ;
            m.completeDist=int(detail::isqrt(cx*cx+cz*cz)/16);
            m.completeOutside=cx*cx+cz*cz>groupRadius2(size_t(m.group));
        }
        if(moved)m.everMoved=true;
        m.moved=moved;
        // gates, lane-order lines, hug lines: first entries
        const int cx=detail::centreCell(u->x),cz=detail::centreCell(u->z);
        {   // W9 step 0: the lateral step of a member between its two end-window entries, by steering site
            const int site=nav?std::clamp(nav->steerSite(m.id),0,kSites-1):0;
            const int64_t ddx=int64_t(u->x.v)-m.x,ddz=int64_t(u->z.v)-m.z;
            for(size_t i=0;i<cfg_.gates.size();++i)
                if((m.gate[i*2].tick>=0)!=(m.gate[i*2+1].tick>=0))
                    m.gateLat[i][size_t(site)]+=(cfg_.gates[i].lateral?ddz:ddx)>>8;
        }
        for(size_t i=0;i<cfg_.gates.size();++i) {
            const auto& g=cfg_.gates[i];
            if(!g.region.contains(cx,cz))continue;
            const int along=g.lateral?cx:cz,lat=g.lateral?cz:cx;
            const int lo=g.lateral?g.region.x0:g.region.z0,hi=g.lateral?g.region.x1:g.region.z1;
            if(along<lo+g.edge&&m.gate[i*2].tick<0)m.gate[i*2]={lat,tick};
            if(along>hi-g.edge&&m.gate[i*2+1].tick<0)m.gate[i*2+1]={lat,tick};
        }
        auto line=[&](const Line& l,Mark& mark) {
            if(mark.tick<0&&l.region.contains(cx,cz))mark={l.sign*(l.lateral?cz:cx),tick};
        };
        for(size_t i=0;i<cfg_.laneOrders.size();++i) {line(cfg_.laneOrders[i].before,m.lane[i*2]);line(cfg_.laneOrders[i].after,m.lane[i*2+1]);}
        for(size_t i=0;i<cfg_.hugs.size();++i) {
            const auto& h=cfg_.hugs[i];
            if(m.hug[i*2].tick<0&&h.first.region.contains(cx,cz))m.hug[i*2]={h.first.lateral?cz:cx,tick};
            if(m.hug[i*2+1].tick<0&&h.second.region.contains(cx,cz))m.hug[i*2+1]={h.second.lateral?cz:cx,tick};
        }
        // flyers
        if(m.flyer) {
            const int mode=u->flightGroundMode;
            if(m.mode==1&&mode==2) {++m.takeoffs;if(m.landedOnce)++m.relifts;}
            if(m.mode==2&&mode==1)m.landedOnce=true;
            const bool landing=u->landing.has_value();
            if(m.landing&&!landing&&mode==2)++m.goArounds;
            const bool hover=mode==2&&u->speed.v<16384&&!landing&&tick-start_>=60;
            m.hoverRun=hover?m.hoverRun+1:0;m.hoverMax=std::max(m.hoverMax,m.hoverRun);
            if(!orders&&mode==2&&m.idleAt<0)m.idleAt=tick;
            if(orders)m.idleAt=-1;
            if(m.idleAt>=0&&(landing||mode==1)) {m.landDelayMax=std::max(m.landDelayMax,tick-m.idleAt);m.idleAt=-1;}
            m.mode=mode;m.landing=landing;
        }
        m.x=u->x.v;m.z=u->z.v;m.heading=uint16_t(u->heading.v);
    }

    int64_t groupRadius2(size_t g) {
        auto& gs=groupStats_[g];
        if(!gs.radius2) {
            int foot=1,n=0;
            for(const auto& m:m_)if(m.group==int(g)) {++n;foot=std::max(foot,m.foot);}
            gs.foot=foot;gs.radius2=detail::discRadius2(foot,std::max({n,cfg_.groups[g].discN,1}));
        }
        return gs.radius2;
    }

    void progress(const sim::World& w,int64_t tick) {
        for(size_t g=0;g<cfg_.groups.size();++g) {
            auto& gs=groupStats_[g];
            const auto& grp=cfg_.groups[g];
            const int64_t r2=groupRadius2(g);
            int n=0,arrived=0,dead=0;bool ordersLeft=false;
            for(const auto& m:m_) {
                if(m.group!=int(g))continue;
                ++n;
                const auto* u=w.unit(m.id);
                if(!u||!u->alive()) {++dead;continue;}
                if(!u->orders.empty()) {ordersLeft=true;continue;}
                if(u->speed.v!=0)continue;
                const int64_t dx=(int64_t(u->x.v)>>16)-grp.clickX,dz=(int64_t(u->z.v)>>16)-grp.clickZ;
                arrived+=dx*dx+dz*dz<=r2;
            }
            gs.arrived=arrived;gs.dead=dead;
            const int64_t at=tick-start_;
            if(gs.t50<0&&n&&arrived*2>=n)gs.t50=at;
            if(gs.t90<0&&n&&arrived*10>=n*9)gs.t90=at;
            if(gs.done<0&&n&&arrived==n)gs.done=at;
            if(gs.ordersDone<0&&n&&!ordersLeft)gs.ordersDone=at;
            gs.n=n;
        }
        for(auto& c:clicks_) {
            c.n=0;c.arrived=0;
            for(int g:c.groups) {c.n+=groupStats_[size_t(g)].n;c.arrived+=groupStats_[size_t(g)].arrived;}
            const int64_t at=tick-start_;
            if(c.t50<0&&c.n&&c.arrived*2>=c.n)c.t50=at;
            if(c.t90<0&&c.n&&c.arrived*10>=c.n*9)c.t90=at;
            if(c.done<0&&c.n&&c.arrived==c.n)c.done=at;
        }
    }

    void gates() {
        for(size_t i=0;i<cfg_.gates.size();++i) {
            const auto& g=cfg_.gates[i];
            bands_.clear();int count=0,lo=INT_MAX,hi=INT_MIN;
            for(const auto& m:m_) {
                if(!m.seen||!m.alive)continue;
                const int cx=int(m.x>>20),cz=int(m.z>>20);
                if(!g.region.contains(cx,cz))continue;
                const int lat=g.lateral?cz:cx;
                bands_.push_back(lat>=0?lat/std::max(g.band,1):-1-(-1-lat)/std::max(g.band,1));
                lo=std::min(lo,lat);hi=std::max(hi,lat);++count;
            }
            if(count<g.minCount)continue;
            std::sort(bands_.begin(),bands_.end());
            auto& s=gateStats_[i];
            ++s.samples;s.files+=std::unique(bands_.begin(),bands_.end())-bands_.begin();s.spread+=hi-lo;
        }
    }
    void gateCrossings(size_t i,int64_t& pairs,int64_t& cross,std::array<int64_t,kSites>* bySite=nullptr) const {
        const auto& g=cfg_.gates[i];
        struct Pass {int group,dir,entry,exit,foot;int64_t tick;const Member* m;};
        std::vector<Pass> passes;
        for(const auto& m:m_) {
            const auto& a=m.gate[i*2];const auto& b=m.gate[i*2+1];
            if(a.tick<0||b.tick<0)continue;
            const bool up=a.tick<b.tick;
            passes.push_back({m.group,up?1:-1,up?a.lat:b.lat,up?b.lat:a.lat,m.foot,std::min(a.tick,b.tick),&m});
        }
        for(size_t a=0;a<passes.size();++a)for(size_t b=a+1;b<passes.size();++b) {
            const auto& p=passes[a];const auto& q=passes[b];
            if(p.group!=q.group||p.dir!=q.dir)continue;
            if(std::abs(p.tick-q.tick)>g.pairWindow)continue;
            ++pairs;
            const int flip=g.flipCells>0?g.flipCells:std::max(p.foot,q.foot);
            const int da=p.entry-q.entry,db=p.exit-q.exit;
            const bool far=cfg_.mutation==1?std::abs(da)>flip&&std::abs(db)>flip:std::abs(da)>=flip&&std::abs(db)>=flip;
            if(int64_t(da)*db<0&&far) {
                ++cross;
                if(bySite) {   // W9 step 0: the site whose lateral displacement difference most favours the new order
                    const int sign=db>0?1:-1;
                    int best=0;int64_t bestV=0;
                    for(int k=0;k<kSites;++k) {
                        const int64_t v=(p.m->gateLat[i][size_t(k)]-q.m->gateLat[i][size_t(k)])*sign;
                        if(v>bestV) {bestV=v;best=k;}
                    }
                    ++(*bySite)[size_t(best)];
                }
            }
        }
    }
    void laneSwaps(size_t i,int64_t& pairs,int64_t& swaps) const {
        const auto& l=cfg_.laneOrders[i];
        std::vector<const Member*> both;
        for(const auto& m:m_)if(m.lane[i*2].tick>=0&&m.lane[i*2+1].tick>=0)both.push_back(&m);
        for(size_t a=0;a<both.size();++a)for(size_t b=a+1;b<both.size();++b) {
            const auto& p=*both[a];const auto& q=*both[b];
            if(l.sameGroup&&p.group!=q.group)continue;
            if(std::abs(p.lane[i*2].tick-q.lane[i*2].tick)>l.window)continue;
            ++pairs;
            const int da=p.lane[i*2].lat-q.lane[i*2].lat,db=p.lane[i*2+1].lat-q.lane[i*2+1].lat;
            if(int64_t(da)*db<0&&std::abs(da)>=l.minCells&&std::abs(db)>=l.minCells)++swaps;
        }
    }
    void sides(int64_t tick) {
        for(size_t i=0;i<cfg_.sides.size();++i) {
            const auto& s=cfg_.sides[i];auto& st=sideStats_[i];
            int64_t high=0,low=0;
            for(const auto& m:m_) {
                if(!m.seen||!m.alive)continue;
                const int c=s.axis?int(m.z>>20):int(m.x>>20);
                (c>=s.at?high:low)++;
            }
            st.high=high;st.low=low;
            if(st.t90<0&&!m_.empty()&&high*10>=int64_t(m_.size())*9)st.t90=tick-start_;
        }
    }
    void flyerAway(const sim::World& w,int64_t tick) {
        for(size_t g=0;g<cfg_.groups.size();++g) {
            int64_t sx=0,sz=0,n=0;bool busy=false;
            for(const auto& m:m_) {
                if(m.group!=int(g)||m.flyer)continue;
                const auto* u=w.unit(m.id);
                if(!u||!u->alive())continue;
                sx+=u->x.v;sz+=u->z.v;++n;busy|=!u->orders.empty();
            }
            if(!n||!busy||tick-start_<150)continue;
            auto& gs=groupStats_[g];
            for(const auto& m:m_) {
                if(m.group!=int(g)||!m.flyer)continue;
                const auto* u=w.unit(m.id);
                if(!u||!u->alive())continue;
                const int64_t dx=(int64_t(u->x.v)-sx/n)>>16,dz=(int64_t(u->z.v)-sz/n)>>16;
                const int64_t d=detail::isqrt(dx*dx+dz*dz);
                gs.awayMax=std::max(gs.awayMax,d);gs.awaySum+=d;++gs.awaySamples;
            }
        }
    }

    // ---- decision samples ----
    void decision(const sim::World& w,Member& m) {
        m.chainMover=false;
        const auto* u=w.unit(m.id);
        if(!u||!u->alive()||!u->type||!m.seen) {m.alive=false;return;}
        m.alive=true;
        const bool orders=!u->orders.empty();
        const int every=std::max(cfg_.decisionEvery,1);
        int64_t sx=0,sz=0;bool moved=false;
        if(!m.ring.empty()) {sx=int64_t(u->x.v)-m.ring.back().first;sz=int64_t(u->z.v)-m.ring.back().second;moved=sx||sz;}
        const bool hasPrev=!m.ring.empty();
        const int64_t step=detail::isqrt(sx*sx+sz*sz);   // raw px over `every` ticks
        const int64_t base=std::max<int64_t>(1,u->baseSpeed.v);
        if(orders) {++ordered_;stopped_+=u->speed.v<=0;auto& wa=why_[size_t(m.whyLast)];++wa.samples;wa.stopped+=u->speed.v<=0;}
        if(orders&&!m.flyer) {
            // W8 step 0 (MV-12 spacing probe): a sample more than 10 cells from the final goal is in transit;
            // more than 100 is far. Held = no speed. A stop is a held sample right after a moving one.
            const auto& goal=u->orders.back();
            const int64_t gx=(int64_t(goal.x.v)-u->x.v)>>16,gz=(int64_t(goal.z.v)-u->z.v)>>16;
            const int64_t cells=detail::isqrt(gx*gx+gz*gz)/16;
            if(cells>10) {
                const int held=u->speed.v<=0;
                ++transit_;transitHeld_+=held;
                if(cells>100) {++transitFar_;transitFarHeld_+=held;}
                if(held&&m.wasHeld==0&&m.group>=0&&size_t(m.group)<groupStats_.size())++groupStats_[size_t(m.group)].stops;
                m.wasHeld=held;
            } else m.wasHeld=-1;
        } else m.wasHeld=-1;
        if(orders&&hasPrev) {
            ++flipSamples_;
            const int cls=moved?1:0;
            if(m.lastClass>=0&&cls!=m.lastClass)++flips_;
            m.lastClass=cls;
            // crawl: moving, at no more than cap/8 per tick on average
            const int64_t cap=int64_t(u->type->maxVel.v)*9/64*every;
            if(moved&&!m.flyer&&step<=cap)++crawl_;
            // back: the last step against the 10-sample mean direction
            if(moved&&m.ring.size()>=10) {
                const auto& old=m.ring[m.ring.size()-10];
                const int64_t mx=(int64_t(u->x.v)-old.first)>>12,mz=(int64_t(u->z.v)-old.second)>>12;
                const int64_t bx=sx>>12,bz=sz>>12;
                const int64_t lm2=mx*mx+mz*mz,lb2=bx*bx+bz*bz,dot=mx*bx+mz*bz;
                if(lm2>128*128&&lb2>32*32&&dot<0&&100*dot*dot>9*lm2*lb2)++back_;
            }
            // aim reversals: the side of the step relative to the way to the goal
            if(moved) {
                const auto& goal=u->orders.back();
                const int64_t gx=(int64_t(goal.x.v)-u->x.v)>>16,gz=(int64_t(goal.z.v)-u->z.v)>>16;
                const int64_t ax=sx>>8,az=sz>>8;
                const int64_t g2=gx*gx+gz*gz,a2=ax*ax+az*az,crs=ax*gz-az*gx;
                if(g2>=32*32&&a2>0&&crs*crs*36>a2*g2) {
                    const int sign=crs>0?1:-1;
                    if(m.aimSign&&sign!=m.aimSign)++aimReversals_;
                    m.aimSign=sign;
                }
            }
            // engagement on flowing: a long free run, then a stop or a >45 degree turn
            const bool freeStep=moved&&step*2>=base*every;
            bool engaged=false;
            if(m.freeRun>=30) {
                const bool stop=step*4<base*every;
                bool bend=false;
                if(!stop&&(m.stepX||m.stepZ)) {
                    const int64_t ax=sx>>8,az=sz>>8,bx=m.stepX>>8,bz=m.stepZ>>8;
                    const int64_t dot=ax*bx+az*bz,a2=ax*ax+az*az,b2=bx*bx+bz*bz;
                    bend=dot<=0||2*dot*dot<a2*b2;
                }
                if(stop||bend) {++engagement_;engaged=true;}
            }
            m.freeRun=engaged?0:freeStep?m.freeRun+1:0;
            if(moved&&!m.flyer) {m.chainMover=true;m.speed=u->speed.v;}
        } else if(!orders) {
            m.lastClass=-1;m.aimSign=0;m.freeRun=0;
        }
        m.stepX=sx;m.stepZ=sz;
        m.ring.push_back({u->x.v,u->z.v});
        if(m.ring.size()>11)m.ring.erase(m.ring.begin());
        // walls
        if(orders&&moved&&!m.flyer&&bw_>0) {
            const int g=clearance(*u);
            if(g>=0) {
                ++wallSamples_;wallTouch_+=g==0;
                const int place=g==0||g<kClearanceCap?touchPlace(*u,orders):0;
                if(g==0)++wallTouchBy_[size_t(place)];
                if(g<kClearanceCap) {near_.push_back(g);nearPlace_.push_back(uint8_t(place));}
            }
        } else if(orders&&!moved&&hasPrev&&!m.flyer&&bw_>0) {   // W9 step 0: the ordered bodies standing still
            const int g=clearance(*u);
            if(g>=0) {++wallStill_;wallStillTouch_+=g==0;}
        }
    }

    // Follow chains: a mover's leader is the nearest same-group mover ahead of
    // it (inside 45 degrees of its motion) within two body widths, at exactly
    // its speed. Candidates come from a sorted cell index, not a pair loop.
    void followChains() {
        std::vector<std::pair<int64_t,int>> index;   // (cell key, member)
        for(size_t i=0;i<m_.size();++i) {
            const auto& m=m_[i];
            if(!m.chainMover)continue;
            index.push_back({cellKey(m.group,int(m.x>>20),int(m.z>>20)),int(i)});
        }
        if(index.empty())return;
        std::sort(index.begin(),index.end());
        std::vector<int> leader(m_.size(),-1);
        for(const auto& [key,i]:index) {
            const auto& m=m_[size_t(i)];
            const int reach=2*m.foot;   // cells: two body widths
            const int64_t reachPx=int64_t(reach)*16;
            const int cx=int(m.x>>20),cz=int(m.z>>20);
            int best=-1;int64_t bestD=INT64_MAX;
            for(int z=cz-reach;z<=cz+reach;++z) {
                const int64_t k0=cellKey(m.group,cx-reach,z),k1=cellKey(m.group,cx+reach,z);
                for(auto it=std::lower_bound(index.begin(),index.end(),std::pair<int64_t,int>{k0,INT_MIN});it!=index.end()&&it->first<=k1;++it) {
                    const int j=it->second;if(j==i)continue;
                    const auto& o=m_[size_t(j)];
                    if(o.speed!=m.speed)continue;
                    const int64_t dx=(int64_t(o.x)-m.x)>>12,dz=(int64_t(o.z)-m.z)>>12;   // 1/16 px
                    const int64_t d2=dx*dx+dz*dz;
                    if(d2>reachPx*reachPx*256)continue;
                    const int64_t ax=m.stepX>>8,az=m.stepZ>>8;
                    const int64_t dot=ax*dx+az*dz;
                    if(dot<=0||2*dot*dot<(ax*ax+az*az)*d2)continue;
                    if(d2<bestD||(d2==bestD&&o.id<m_[size_t(best)].id)) {bestD=d2;best=j;}
                }
            }
            leader[size_t(i)]=best;
        }
        std::vector<int> depth(m_.size(),0);
        int sampleMax=0;
        for(const auto& [key,i]:index) {
            // Walk to the head (or a node already measured), then fill back.
            std::vector<int> path;int at=i;
            while(at>=0&&depth[size_t(at)]==0&&int(path.size())<=int(m_.size())) {
                depth[size_t(at)]=-1;path.push_back(at);at=leader[size_t(at)];
            }
            int d=at>=0&&depth[size_t(at)]>0?depth[size_t(at)]:0;   // -1: a cycle, counted from here
            for(auto it=path.rbegin();it!=path.rend();++it)depth[size_t(*it)]=++d;
            sampleMax=std::max(sampleMax,depth[size_t(i)]);
        }
        followMax_=std::max<int64_t>(followMax_,sampleMax);followSum_+=sampleMax;++followSamples_;
    }
    static int64_t cellKey(int group,int x,int z) {
        return (int64_t(group)<<40)|(int64_t(uint32_t(z+0x8000)&0xfffff)<<20)|int64_t(uint32_t(x+0x8000)&0xfffff);
    }

    void spacing(const sim::World& w) {
        for(const auto& m:m_) {
            const auto* u=w.unit(m.id);
            if(!u||!u->alive()||!u->type||u->orders.empty()||m.flyer)continue;
            ++spacingSamples_;
            const int ox=sim::footprintOrigin(u->x,m.fx),oz=sim::footprintOrigin(u->z,m.fz);
            bool own=false,other=false,settled=false;
            const int64_t mine=commandOf(m.id);
            auto look=[&](int x,int z) {
                const int id=bodyAt(x,z);
                if(!id||id==m.id)return;
                const auto* v=w.unit(id);
                if(!v)return;
                if(v->orders.empty()) {if(commandOf(id)!=mine)settled=true;}
                else if(memberGroup(id)==m.group)own=true;
                else other=true;
            };
            for(int i=-1;i<=m.fx;++i) {look(ox+i,oz-1);look(ox+i,oz+m.fz);}
            for(int j=0;j<m.fz;++j) {look(ox-1,oz+j);look(ox+m.fx,oz+j);}
            contactOwn_+=own;contactOther_+=other;contactSettled_+=settled;
        }
    }
    // The command a unit's order came from: its last noted selection, else
    // its group (negative), else none (a unit of no group: its own id).
    int64_t commandOf(int id) const {
        const auto it=std::lower_bound(selection_.begin(),selection_.end(),std::pair<int,int64_t>{id,INT64_MIN});
        if(it!=selection_.end()&&it->first==id)return it->second;
        const int g=memberGroup(id);
        return g>=0?-1-int64_t(g):INT64_MIN+id;
    }
    int memberGroup(int id) const {
        // Members are few next to the map; a sorted id index keeps this O(log n).
        if(memberIndex_.size()!=m_.size()) {
            memberIndex_.clear();
            for(const auto& m:m_)memberIndex_.push_back({m.id,m.group});
            std::sort(memberIndex_.begin(),memberIndex_.end());
        }
        const auto it=std::lower_bound(memberIndex_.begin(),memberIndex_.end(),std::pair<int,int>{id,INT_MIN});
        return it!=memberIndex_.end()&&it->first==id?it->second:-1;
    }

    void pairProximity(const sim::World& w) {
        for(auto& p:pairStats_) {
            if(p.a.empty()||p.b.empty())continue;
            const int cells=cfg_.pairs[size_t(&p-pairStats_.data())].cells;
            if(countGrid_.size()!=size_t(std::max(gw_,0))*size_t(std::max(gh_,0))) {countGrid_.assign(size_t(gw_)*size_t(gh_),0);countEpoch_.assign(countGrid_.size(),0);cepoch_=0;}
            ++cepoch_;
            int64_t na=0,nb=0;
            auto live=[&](const Member& m)->const sim::Unit* {
                const auto* u=w.unit(m.id);
                return u&&u->alive()&&!u->orders.empty()?u:nullptr;
            };
            for(const auto& m:m_) {
                if(!p.b[size_t(m.group)])continue;
                const auto* u=live(m);if(!u)continue;
                ++nb;
                const int x=detail::centreCell(u->x),z=detail::centreCell(u->z);
                if(x<0||z<0||x>=gw_||z>=gh_)continue;
                const size_t i=size_t(z)*size_t(gw_)+size_t(x);
                if(countEpoch_[i]!=cepoch_) {countEpoch_[i]=cepoch_;countGrid_[i]=0;}
                ++countGrid_[i];
            }
            for(const auto& m:m_) {
                if(!p.a[size_t(m.group)])continue;
                const auto* u=live(m);if(!u)continue;
                ++na;
                const int x=detail::centreCell(u->x),z=detail::centreCell(u->z);
                for(int j=-cells;j<=cells;++j)for(int i=-cells;i<=cells;++i) {
                    const int cx=x+i,cz=z+j;
                    if(cx<0||cz<0||cx>=gw_||cz>=gh_)continue;
                    const size_t c=size_t(cz)*size_t(gw_)+size_t(cx);
                    if(countEpoch_[c]==cepoch_)p.contacts+=countGrid_[c];
                }
            }
            p.pairs+=na*nb;
        }
    }

    // ---- walls: chessboard chamfer distance over legal origins ----
    static constexpr int kClearanceCap=11;
    int clearance(const sim::Unit& u) {
        const int key=u.type->footX*64+u.type->footZ;
        auto it=std::find_if(dt_.begin(),dt_.end(),[&](const auto& e){return e.first==key;});
        if(it==dt_.end()) {dt_.push_back({key,buildDistance(u)});it=dt_.end()-1;}
        const int ox=sim::footprintOrigin(u.x,u.type->footX),oz=sim::footprintOrigin(u.z,u.type->footZ);
        if(ox<0||oz<0||ox>=bw_||oz>=bh_)return -1;
        const int d=it->second[size_t(oz)*size_t(bw_)+size_t(ox)];
        return d<=0?0:std::min(d-1,kClearanceCap);
    }
    // Where a wall sample is: within kGateNear cells of a configured gate region, else the current leg is
    // an attack / guard (the reach ring round its target), else an illegal origin on both axes beside the
    // body (a corner), else a flat wall.
    int touchPlace(const sim::Unit& u,bool orders) {
        const int cx=detail::centreCell(u.x),cz=detail::centreCell(u.z);
        for(const auto& g:cfg_.gates)
            if(cx>=g.region.x0-kGateNear&&cx<=g.region.x1+kGateNear&&cz>=g.region.z0-kGateNear&&cz<=g.region.z1+kGateNear)return kPlaceGate;
        if(orders&&u.orders[sim::World::currentLeg(u.orders)].targetId!=0)return kPlaceRing;
        const int key=u.type->footX*64+u.type->footZ;
        auto it=std::find_if(dt_.begin(),dt_.end(),[&](const auto& e){return e.first==key;});
        if(it==dt_.end())return kPlaceFlat;
        const int ox=sim::footprintOrigin(u.x,u.type->footX),oz=sim::footprintOrigin(u.z,u.type->footZ);
        auto blockedAt=[&](int x,int z) {return x<0||z<0||x>=bw_||z>=bh_||it->second[size_t(z)*size_t(bw_)+size_t(x)]==0;};
        const bool ax=blockedAt(ox-1,oz)||blockedAt(ox+1,oz),az=blockedAt(ox,oz-1)||blockedAt(ox,oz+1);
        return ax&&az?kPlaceCorner:kPlaceFlat;
    }
    std::vector<uint16_t> buildDistance(const sim::Unit& u) const {
        const int fx=u.type->footX,fz=u.type->footZ;
        std::vector<uint16_t> d(size_t(bw_)*size_t(bh_),0);
        for(int z=0;z<bh_;++z)for(int x=0;x<bw_;++x) {
            bool legal=x+fx<=bw_&&z+fz<=bh_;
            if(legal&&legalFn_)legal=legalFn_(u,x,z);
            else if(legal)for(int j=0;j<fz&&legal;++j)for(int i=0;i<fx&&legal;++i)legal=!blocked_[size_t(z+j)*size_t(bw_)+size_t(x+i)];
            d[size_t(z)*size_t(bw_)+size_t(x)]=legal?uint16_t(0xffff):0;
        }
        // Outside the map counts as illegal: distance 1 at the border.
        auto at=[&](int x,int z)->int {return x<0||z<0||x>=bw_||z>=bh_?0:d[size_t(z)*size_t(bw_)+size_t(x)];};
        for(int z=0;z<bh_;++z)for(int x=0;x<bw_;++x) {
            auto& v=d[size_t(z)*size_t(bw_)+size_t(x)];
            if(!v)continue;
            const int m=std::min({at(x-1,z),at(x-1,z-1),at(x,z-1),at(x+1,z-1)});
            v=uint16_t(std::min<int>(v,m+1));
        }
        for(int z=bh_-1;z>=0;--z)for(int x=bw_-1;x>=0;--x) {
            auto& v=d[size_t(z)*size_t(bw_)+size_t(x)];
            if(!v)continue;
            const int m=std::min({at(x+1,z),at(x+1,z+1),at(x,z+1),at(x-1,z+1)});
            v=uint16_t(std::min<int>(v,m+1));
        }
        return d;
    }

    Config cfg_;
    std::vector<Member> m_;
    mutable std::vector<std::pair<int,int>> memberIndex_;
    std::vector<GroupStats> groupStats_;
    std::vector<ClickStats> clicks_;
    std::vector<GateStats> gateStats_;
    std::vector<SideStats> sideStats_;
    std::vector<PairStats> pairStats_;
    std::vector<WorkSeries> work_;
    std::vector<int> bands_;
    int64_t start_=-1,samples_=0;
    int64_t spins_=0,reversals_=0,stopGo_=0,sideways_=0,backward_=0,back_=0,walkInPlace_=0,statue_=0,crawl_=0;
    int64_t walkNet_=0,walkNetRuns_=0;
    void closeWalk(Member& m) {
        if(m.walkFrom>=0&&m.walkTo-m.walkFrom+1>=30) {walkNet_+=m.walkTo-m.walkFrom+1;++walkNetRuns_;}
        m.walkFrom=m.walkTo=-1;
    }
    int64_t waitingHeld_=0,waitingNoProgress_=0,parkedHeld_=0,parkedNoProgress_=0;
    bool legion_=false;int64_t stallStale_=0;   // a Legion navigator was seen; LegionNavigator::staleStall updates
    int64_t transit_=0,transitHeld_=0,transitFar_=0,transitFarHeld_=0;
    int64_t ordered_=0,stopped_=0,flipSamples_=0,flips_=0,aimReversals_=0,engagement_=0,followMax_=0,followSum_=0,followSamples_=0;
    struct WhyAcc {int64_t ticks=0,movedTicks=0,samples=0,stopped=0,statue=0,sideways=0,backward=0,reversals=0;};
    std::array<WhyAcc,kWhys> why_{};
    int64_t spacingSamples_=0,contactOwn_=0,contactOther_=0,contactSettled_=0;
    static constexpr int64_t kConvoyCommand=int64_t(1)<<40;   // convoy commands, apart from plain selections
    std::vector<std::pair<int,int64_t>> selection_;int64_t selections_=0;   // (unit id, command), sorted
    int64_t wallSamples_=0,wallTouch_=0;std::vector<int> near_;
    // W9 step 0: the place of each near-wall sample (parallel to near_), the moving touches by place, and the
    // still ordered samples (kPlaceFlat, kPlaceGate, kPlaceRing, kPlaceCorner).
    enum {kPlaceFlat=0,kPlaceGate=1,kPlaceRing=2,kPlaceCorner=3};
    static constexpr int kGateNear=6;
    std::vector<uint8_t> nearPlace_;std::array<int64_t,4> wallTouchBy_{};int64_t wallStill_=0,wallStillTouch_=0;
    // id grid and the pair count grid
    int gw_=0,gh_=0;uint32_t epoch_=0,cepoch_=0;
    std::vector<uint32_t> gridEpoch_,countEpoch_;std::vector<int> gridId_;std::vector<int64_t> countGrid_;
    // walls
    std::vector<uint8_t> blocked_;int bw_=0,bh_=0;
    std::function<bool(const sim::Unit&,int,int)> legalFn_;
    std::vector<std::pair<int,std::vector<uint16_t>>> dt_;
};

// The Work adaptor: every LegionNavigator::Stats counter, as per-tick deltas,
// into an Observer's Work section (work.<snake_name>.max/p99/total), plus
// work.legion_total, the per-tick sum of the work classes below. Call
// sample() once per tick, after World::tick and before Observer::sample. It
// reads World::legionStats() (a const copy) only. A counter enters the report
// the first tick it moves (earlier ticks count 0), so a counter that never
// moves has no keys: read a missing key as 0 (except midroute_completions, the safety
// key W3 gates on, which is always present). engaged_now is not work either: the
// runner reports it as gauge.engaged_max and engaged.member_ticks. Gauges (bytes, live_*) and the
// running max completion_dist_max are not per-tick work and are skipped.
// Retail worlds have no navigator: nothing is fed.
//
// Total Legion work sums the classes that count cells, iterations or probes,
// each once: subsets are left out of the sum (field_work_* split field_work;
// sched_group_visits and join_iterations are inside group_loop_iters;
// formation_ring_cells and rechoice_bfs_cells inside slot_search_cells;
// line_sweeps are counted by trace_cells, one plus the cells stepped). World's
// convoy_tests (index probes plus candidates per order, sim/convoy.h) live
// outside Stats and are fed and summed here too.
//
// work.legion_all (2026-10-14 Legion review, item 5) is legion_total plus the
// three steering searches it leaves out because they are reported as
// instruments: aware_work (awareScan's chains, members and samples; its pair
// count aware_pairs is already a class of the total), lane_work (the passage
// gate's search and aim, a formation's entrance search, the passage lanes) and
// pivot_work less pivot_sweeps (the pinwheel's descent chain, wall scan,
// narrowing tests and free-width probes; the one unit it adds per line sweep is
// a sweep trace_cells already counts). None counts cells another class counts.
// Report-only: legion_check and legion_cost gate legion_total and the classes,
// not this sum.
class NavWork {
public:
    static bool totalClass(std::string_view n) {
        static constexpr std::string_view k[]={
            "field_work","trace_cells","pass_scan_cells","slot_search_cells","group_loop_iters",
            "share_scan_iters","aware_pairs","held_rechecks","lift_members_walked","still_units_processed",
            "crowd_window_ring_cells","crowd_settle_visits","detour_cells","softowner_lookups",
            "pivot_part_ids"};
        for(auto c:k)if(c==n)return true;
        return false;
    }
    void sample(Observer& obs,const sim::World& w) {
        if(!const_cast<sim::World&>(w).legionNavigator())return;   // a plain accessor
        const auto s=w.legionStats();
        size_t i=0;uint64_t total=0,awareTickWork=0,steering=0,pivotWork=0,pivotSweeps=0;
        sim::LegionNavigator::forEachStat(s,[&](const char* name,uint64_t v) {
            const std::string_view n(name);
            if(i>=last_.size())last_.push_back(0);
            const uint64_t d=v>=last_[i]?v-last_[i]:0;last_[i++]=v;
            if(n=="bytes"||n.substr(0,5)=="live_"||n=="completion_dist_max")return;
            if(n=="still_per_residue_max"||n=="quota_peg_run_max") {gauge(n,v);return;}   // running maxima, not work
            if(n=="engaged_now") {   // W5 step 0: a per-tick population, not work (Stats::engagedNow)
                engagedMax_=std::max(engagedMax_,d);engagedTicks_+=d;return;
            }
            if(isW8Instrument(n)) {   // W8 step 0: the lane counters, reported by w8() and never summed as work
                w8_[std::string(n)]=v;
                if(n=="lane_work")steering+=d;   // ... except into legion_all
                if(n=="pivot_work")pivotWork+=d;
                if(n=="pivot_sweeps")pivotSweeps+=d;
                return;
            }
            if(isW9Instrument(n)) {   // W9 step 0: the mechanism counters, reported by w9() and never summed as work
                w9_[std::string(n)]=v;
                return;
            }
            if(isW7Instrument(n)) {   // W7 step 0: observation counters, reported by w7() and never summed as work
                w7_[std::string(n)]=v;
                if(n=="aware_work") {awareTickWork+=d;steering+=d;}   // ... except into legion_all
                return;
            }
            if(n=="aware_pairs")awareTickWork+=d;
            if(d||n=="midroute_completions")obs.work(n,d);   // a safety key: always present, 0 when clean
            if(totalClass(n))total+=d;
        });
        awareWorkMax_=std::max(awareWorkMax_,awareTickWork);
        // World's convoy lookups (sim/convoy.h), a work class outside Stats.
        const uint64_t convoy=w.convoyStats().tests;
        const uint64_t d=convoy>=lastConvoy_?convoy-lastConvoy_:0;lastConvoy_=convoy;
        if(d)obs.work("convoy_tests",d);
        total+=d;
        obs.work("legion_total",total);
        steering+=pivotWork-std::min(pivotWork,pivotSweeps);   // each sweep added one unit of pivot_work
        obs.work("legion_all",total+steering);
        // Churn bins (W4 step 0): total Legion work per kChurnBin ticks of the run.
        const size_t bin=ticks_++/kChurnBin;
        if(bin>=bins_.size())bins_.resize(bin+1,0);
        bins_[bin]+=total;
    }
    static constexpr uint32_t kChurnBin=1500;
    // Total Legion work per kChurnBin-tick bin (the last bin may be partial).
    const std::vector<uint64_t>& churnBins() const {return bins_;}
    // The running-maximum gauges the Stats carry (their last value): residue
    // load of scanStill and the longest field-quota peg run.
    uint64_t stillPerResidueMax() const {return stillPerResidueMax_;}
    uint64_t quotaPegRunMax() const {return quotaPegRunMax_;}
    // W5 step 0: the most bodies tickCombat braked in reach on one tick (Stats::engagedNow's largest
    // per-tick delta) and the member-ticks summed over the run.
    uint64_t engagedMax() const {return engagedMax_;}
    // W7 step 0: the latest cumulative value of each awareness / parting / give-way counter
    // (Stats::parts, awareEncounters, awareReplans, awareBuilds, awareWork, awareLatency*, giveWay*),
    // and the most awareness work (aware_pairs + aware_work) any one tick spent.
    const std::map<std::string,uint64_t>& w7() const {return w7_;}
    uint64_t awareWorkMax() const {return awareWorkMax_;}
    static bool isW7Instrument(std::string_view n) {
        return n=="parts"||n=="aware_work"||n=="aware_encounters"||n=="aware_replans"||n=="aware_builds"||
            n.substr(0,14)=="aware_latency_"||n.substr(0,8)=="giveway_";
    }
    uint64_t engagedMemberTicks() const {return engagedTicks_;}
    // W8 step 0: the latest cumulative value of each lane counter (Stats::pivotCalls, pivotWork, pivotSweeps,
    // laneWork, laneClipped). pivot_work is the base lane steering work the S1 lanes are gated against (<= 1.5x).
    const std::map<std::string,uint64_t>& w8() const {return w8_;}
    // W9 step 0: the latest cumulative value of each mechanism counter (Stats::sealSettles .. orderInversionsTaken).
    const std::map<std::string,uint64_t>& w9() const {return w9_;}
    static bool isW9Instrument(std::string_view n) {
        static constexpr std::string_view k[]={
            "seal_settles","creep_presses","deep_rechoices","anchor_advances","gate_lane_moves","gate_holds","fold_binds",
            "turn_guard_rejects","clear_aims","meet_latches","order_inversions_avoided","order_inversions_taken"};
        for(auto c:k)if(c==n)return true;
        return false;
    }
    static bool isW8Instrument(std::string_view n) {
        return n=="pivot_calls"||n=="pivot_work"||n=="pivot_sweeps"||n=="lane_work"||n=="lane_clipped"||n.substr(0,7)=="vertex_"||n=="gate_commits"||n=="gate_releases"||n=="pass_filter_holds";
    }
private:
    void gauge(std::string_view n,uint64_t v) {(n=="quota_peg_run_max"?quotaPegRunMax_:stillPerResidueMax_)=v;}
    std::vector<uint64_t> bins_;
    std::map<std::string,uint64_t> w7_,w8_,w9_;
    uint64_t awareWorkMax_=0;
    uint64_t ticks_=0,stillPerResidueMax_=0,quotaPegRunMax_=0,engagedMax_=0,engagedTicks_=0;
    std::vector<uint64_t> last_;
    uint64_t lastConvoy_=0;
};


// `behind NAME LATER EARLIER [cells=N]` (PLAN W7 step 0, user decision 5): the route-behind metric. LATER is the group
// that gives way to the stream EARLIER; under decision 5 it always routes behind that stream and never waits at its
// edge. Per LATER member the path it walked while ordered against the straight line from its first ordered position to
// its click, and the WAIT: a LATER member with orders that stood (a step of no more than an eighth of its speed) with a
// moving EARLIER member within `cells` (default 8) cells; a group wait is a tick on which at least a quarter of the
// LATER members with orders (and 2) do so, a wait is a run of at least 30 such ticks. Read-only.
//   behind.NAME.members          LATER members that were ever ordered
//   behind.NAME.path_cells       mean path walked (cells), straight_cells the mean straight line, extra_cells the
//                                difference, detour_permille path / straight - 1
//   behind.NAME.wait_member_ticks  member-ticks standing at the stream
//   behind.NAME.wait_ticks / .wait_run_max / .waits   group-level wait ticks, the longest run, runs of >= 30
struct BehindProbe {
    std::string name;
    int64_t edge = 8 * 16;
    struct Late { int id; int64_t cx, cz; int32_t px = INT32_MIN, pz = INT32_MIN; int64_t sx = 0, sz = 0; double path = 0; bool started = false; };
    std::vector<Late> late;
    std::vector<int> early;
    std::vector<std::pair<int32_t, int32_t>> earlyPrev, stream;
    int64_t memberWait = 0, groupWait = 0, run = 0, runMax = 0, waits = 0;
    void sample(const sim::World& w) {
        if (earlyPrev.size() != early.size()) earlyPrev.assign(early.size(), {INT32_MIN, INT32_MIN});
        stream.clear();
        for (size_t i = 0; i < early.size(); ++i) {
            const auto* u = w.unit(early[i]);
            if (!u || !u->alive() || u->orders.empty()) { earlyPrev[i] = {INT32_MIN, INT32_MIN}; continue; }
            if (earlyPrev[i].first != INT32_MIN && (u->x.v != earlyPrev[i].first || u->z.v != earlyPrev[i].second))
                stream.push_back({int32_t(int64_t(u->x.v) >> 16), int32_t(int64_t(u->z.v) >> 16)});
            earlyPrev[i] = {u->x.v, u->z.v};
        }
        int64_t active = 0, nearWait = 0;
        for (auto& b : late) {
            const auto* u = w.unit(b.id);
            if (!u || !u->alive() || !u->type) continue;
            const bool ordered = !u->orders.empty();
            if (ordered && !b.started) { b.started = true; b.sx = int64_t(u->x.v) >> 16; b.sz = int64_t(u->z.v) >> 16; }
            if (ordered && b.px != INT32_MIN) {
                const double dx = double(u->x.v) - b.px, dz = double(u->z.v) - b.pz;
                b.path += std::sqrt(dx * dx + dz * dz) / 65536.0;
                ++active;
                const int64_t cap = int64_t(u->baseSpeed.v) / 8;
                if (int64_t(dx) * int64_t(dx) + int64_t(dz) * int64_t(dz) <= cap * cap) {
                    const int64_t x = int64_t(u->x.v) >> 16, z = int64_t(u->z.v) >> 16;
                    for (const auto& e : stream)
                        if (std::abs(e.first - x) <= edge && std::abs(e.second - z) <= edge) { ++nearWait; break; }
                }
            }
            b.px = u->x.v; b.pz = u->z.v;
        }
        memberWait += nearWait;
        if (nearWait >= 2 && nearWait * 4 >= active) { ++groupWait; ++run; }
        else { if (run >= 30) ++waits; runMax = std::max(runMax, run); run = 0; }
    }
    void report(Keys& k) const {
        const std::string p = "behind." + name + ".";
        double path = 0, straight = 0;
        int64_t n = 0;
        for (const auto& b : late) {
            if (!b.started) continue;
            const double dx = double(b.cx - b.sx), dz = double(b.cz - b.sz);
            path += b.path; straight += std::sqrt(dx * dx + dz * dz); ++n;
        }
        k.emplace_back(p + "members", n);
        k.emplace_back(p + "path_cells", n ? int64_t(std::llround(path / 16.0 / double(n))) : 0);
        k.emplace_back(p + "straight_cells", n ? int64_t(std::llround(straight / 16.0 / double(n))) : 0);
        k.emplace_back(p + "extra_cells", n ? int64_t(std::llround((path - straight) / 16.0 / double(n))) : 0);
        k.emplace_back(p + "detour_permille", straight > 0 ? int64_t(std::llround((path / straight - 1.0) * 1000.0)) : 0);
        k.emplace_back(p + "wait_member_ticks", memberWait);
        k.emplace_back(p + "wait_ticks", groupWait);
        k.emplace_back(p + "wait_run_max", std::max(runMax, run));
        k.emplace_back(p + "waits", waits + (run >= 30 ? 1 : 0));
    }
};

}
