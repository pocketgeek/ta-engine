#pragma once
// legion_scale.h -- the Legion 10,000 scale probe (phase 0).
// Header-only, tools only. legion_scenario reports its keys for a file that says `probe scale`
// (or with --scale). Observation only: it reads `const World&` and the navigator's read-only test
// hooks after each tick, keeps integer state of its own and writes nothing back, so a run with it
// on hashes the same as one without (legion_scenario --neutral checks the observer the same way).
//
// Per body, against the LAST order click that selected it (a non-queued move, fight, patrol, attack
// or guard; a Stop cancels it), with every time a tick count FROM THAT CLICK:
//   deliver   the tick its command landed in the sim less the click tick (the uplink: in Legion
//             one explicit order per click, landing whole; in Retail 64 commands a tick per client
//             and the 512-command window over the file's round trip)
//   start     the first tick it stood 4 px or more from where it was when its command landed
//             (first useful movement)
//   arrive    the first tick its centre was inside the click's packed disc (the observer's
//             16*(foot+1)*sqrt(N/pi)*1.25 px, foot the selection's largest footprint side, N the
//             bodies of every click one convoy merged): PHYSICAL arrival, orders or not
//   complete  the first tick its orders were empty after its command landed (ORDER completion)
// Keys (prefix scale.; times in ticks, -1 "never" or fewer bodies than the quantile needs):
//   n, point_n, cancelled_n, dead_n            bodies clicked; the live, uncancelled ones with a point
//                                               (move/fight/patrol: the arrival set); Stop after the
//                                               click; dead at the end. start/arrive/complete quantiles
//                                               are over the live, uncancelled bodies
//   idle_n                                     the file's bodies never clicked (idle obstacles)
//   deliver.p50/.p99/.max, deliver.missing     command latency; clicked bodies whose command never landed
//   start.t50/.t90/.t99/.last, start.never     first useful movement ("last" = the last member to start)
//   arrive.n, arrive.t50/.t90/.t99/.last       physical arrival of the live, uncancelled point bodies
//   complete.n, complete.t50/.t90/.t99/.last   order completion of the same bodies
//   complete_outside_n                         completions while outside the disc
//   open_inside_end                            inside the disc at the end with the order still open
//   path_x1000.p50/.p90/.max                   travelled since the click / the straight line from the
//                                              click position to the disc's edge, x1000, for arrived bodies
//                                              that started 4+ cells outside their disc
//   moving.peak, moving.unit_ticks             bodies that changed position on a tick: the most on one
//                                              tick, and the sum over the run (moving vs idle)
//   stuck_end                                  live point bodies with the order open, outside the disc,
//                                              that moved under 2 cells net over the last 600 ticks
//   livelock_end                               of those, the ones that travelled 4+ cells in that window
//                                              (walking without getting anywhere)
//   throughput.peak300                         most arrivals inside one 300-tick window
//   wait.max, wait.p99, wait.member_ticks      Legion only: the longest run of ticks a body spent in the
//                                              navigator's Waiting state (no usable field: navigation
//                                              service delay), its 99th percentile over bodies, and the
//                                              total; `wait.max` is the queue age the slowest body saw
//   nav.groups_peak, nav.fields_peak, nav.members_peak, nav.bytes_peak
//                                              Legion only, sampled every 30 ticks: live navigator
//                                              containers and Stats::bytes (the navigator's own capacity
//                                              estimate: planes, seeds, potentials, buckets, members)
//   navwork.<class>.total / .tick_max, navwork.tick_all_max
//                                              Legion only (stage 4, LegionNavigator::accounting): work per
//                                              NavWork class (sim/navbudget.h), cumulative and the most one
//                                              navigator tick window spent; the most of all classes at once
//   navbytes.peak, navbytes.<category>_at_peak  deterministic NavBytes: the largest total sampled (every
//                                              30 ticks and at the end) and its categories then
//   sched.gap_max.<class>, sched.gap_max.any, sched.age_max.<class>, sched.promotions, sched.fit_skips, sched.reservations,
//   sched.request_promotions, sched.requests_max, sched.stopped_shares, sched.admissions,
//   sched.shared_evictions, sched.inflight_max.first, sched.inflight_max.refresh, sched.request_cells_max,
//   sched.admit_gap_max,
//   sched.reg_deferred, sched.reg_wait_max, sched.plane_waits, sched.sync_plane_calls, sched.sync_plane_max,
//   sched.init_max                             the scheduler's service record (NavAccounting)
//   route.demand_peak, route.demand_dropped    stage 5: the most demand entries one paused build held, and
//                                              demands a full list could not represent (NavAccounting)
//   route.needs.<consumer>, route.run_on.<consumer>
//                                              stage 5: regions consumers asked a paused build for, and
//                                              routes a consumer's uncertain read made run on to done
//   route.corridors, route.corridor_widens, route.fallbacks
//                                              stage 6: corridor fields built, corridors and chains rebuilt round a
//                                              member outside them, plans the search could not complete (windows)
//   route.chains, route.tiles_built, route.tile_hits, route.tile_evictions, route.tile_bytes_peak,
//   route.chain_skips, route.chain_abandons, route.chain_repairs, route.tile_cross_hits
//                                              stage 6: tile chains built; shared tiles built, found, evicted;
//                                              the tile cache's peak; regions left off (nothing to lead into /
//                                              relabelled); halo repairs; hits by routes to other goals
//   route.full_plans, route.seed_plans, route.seed_bound, route.seed_bound_restores
//                                              plans made for a group whose bounded window had failed (a large
//                                              component); plans over the seeds' own cells (every member cut off);
//                                              windows charged for their buried seeds, and charged in full after all
//   static.cut_traps, static.trapped_resumed, sched.awaited_ticks
//                                              members trapped by their component; trapped members that went on in
//                                              place once their goals rejoined it; member-ticks standing off a
//                                              touched corridor or chain (their refresh a Waiting job)
// Everything is integer and in body order, so every compiler reports the same numbers.
#include "legion_observe.h"
#include "sim/legion.h"
#include "sim/sim.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace ta::legion_scale {

namespace obs = ta::legion_observe;

// Machine-dependent memory gauges for the wall line (stderr only, never keys): the process's peak resident
// set (Linux VmHWM; -1 elsewhere) and the bytes malloc holds in use now (glibc; -1 elsewhere).
inline long long peakRssKb() {
#if defined(__linux__)
    if (FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        long long kb = -1;
        while (std::fgets(line, sizeof line, f))
            if (std::strncmp(line, "VmHWM:", 6) == 0) { kb = std::atoll(line + 6); break; }
        std::fclose(f);
        return kb;
    }
#endif
    return -1;
}
inline long long heapKb() {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    const struct mallinfo2 m = mallinfo2();
    return (long long)((m.uordblks + m.hblkhd) / 1024);
#else
    return -1;
#endif
}

class ScaleProbe {
public:
    // Every body the file spawns, in file order; bodies never clicked are the idle ones.
    explicit ScaleProbe(const std::vector<int>& all) : ids_(all), b_(all.size()) {
        int top = 0;
        for (int id : all) top = std::max(top, id);
        index_.assign(size_t(top) + 1, -1);
        for (size_t i = 0; i < all.size(); ++i) index_[size_t(all[i])] = int(i);
    }

    // A non-queued click selected `id` at `tick`. point: a ground point (px, raw Fixed >> 16) and its
    // packed disc radius squared (px^2); otherwise a unit target (attack/guard: no arrival).
    void click(const ta::sim::World& w, int id, int tick, bool point, int64_t px, int64_t pz, int64_t r2) {
        Body* b = body(id);
        if (!b) return;
        const auto* u = w.unit(id);
        Body fresh;
        fresh.clicked = true;
        fresh.click = tick;
        fresh.point = point;
        fresh.tx = px; fresh.tz = pz; fresh.r2 = r2;
        if (u) { fresh.cx = u->x.v >> 16; fresh.cz = u->z.v >> 16; }
        fresh.travelled = b->travelled;   // the odometer runs across clicks
        fresh.travelledAtClick = b->travelled;
        fresh.px = b->px; fresh.pz = b->pz; fresh.seen = b->seen;
        fresh.ring = b->ring; fresh.ringN = b->ringN;
        fresh.landedN = b->landedN;
        fresh.expect = ~0u;   // set by expect() once the click's commands are issued
        *b = fresh;
    }
    void cancel(int id) {
        if (Body* b = body(id); b && b->clicked) b->cancelled = true;
    }
    // The click just made for `id` is delivered once `n` commands for it have landed in all (its own last
    // command's position in the unit's command stream; call after the click's commands were issued).
    void expect(int id, uint32_t n) {
        if (Body* b = body(id); b && b->clicked) b->expect = n;
    }
    // A command for `id` landed in the sim on `tick` (call before World::tick of that tick).
    void landed(const ta::sim::World& w, int id, int tick) {
        Body* b = body(id);
        if (!b) return;
        ++b->landedN;
        if (!b->clicked || b->land >= 0 || b->landedN < b->expect) return;
        b->land = tick;
        if (const auto* u = w.unit(id)) { b->lx = u->x.v >> 16; b->lz = u->z.v >> 16; }
    }

    // After World::tick of tick t.
    void sample(const ta::sim::World& w, const ta::sim::LegionNavigator* nav, int t) {
        int64_t moving = 0;
        for (size_t i = 0; i < b_.size(); ++i) {
            Body& b = b_[i];
            const auto* u = w.unit(ids_[i]);
            if (!u || !u->alive()) { b.dead = true; continue; }
            const int64_t x = u->x.v, z = u->z.v;
            if (b.seen) {
                const int64_t dx = (x - b.px) >> 12, dz = (z - b.pz) >> 12;   // 1/16 px
                if (x != b.px || z != b.pz) ++moving;
                b.travelled += obs::detail::isqrt(dx * dx + dz * dz);
            }
            b.px = x; b.pz = z; b.seen = true;
            if (t % kRingEvery == 0) {
                b.ring[size_t(b.ringN % kRing)] = {x >> 16, z >> 16, b.travelled};
                ++b.ringN;
            }
            if (!b.clicked) continue;
            const int64_t ux = x >> 16, uz = z >> 16;
            if (b.land >= 0) {
                if (!u->orders.empty()) b.had = true;
                if (b.start < 0 && (ux - b.lx) * (ux - b.lx) + (uz - b.lz) * (uz - b.lz) >= 16) b.start = t;
                if (b.had && b.complete < 0 && u->orders.empty()) {
                    b.complete = t;
                    b.completeOutside = b.point && !inside(b, ux, uz);
                }
            }
            if (b.point && b.arrive < 0 && inside(b, ux, uz)) { b.arrive = t; b.travelledAtArrive = b.travelled; }
            if (nav) {
                if (nav->unitState(ids_[i]) == 3) {
                    ++b.waitRun; ++waitTicks_;
                    b.waitMax = std::max(b.waitMax, b.waitRun);
                } else b.waitRun = 0;
            }
        }
        movingPeak_ = std::max(movingPeak_, moving);
        movingTicks_ += moving;
        if (nav && t % 30 == 0) {
            const auto s = nav->stats();
            groupsPeak_ = std::max<int64_t>(groupsPeak_, int64_t(s.liveGroups));
            fieldsPeak_ = std::max<int64_t>(fieldsPeak_, int64_t(s.liveFields));
            membersPeak_ = std::max<int64_t>(membersPeak_, int64_t(s.liveMembers));
            bytesPeak_ = std::max<int64_t>(bytesPeak_, int64_t(s.bytes));
            account(*nav);
        }
        last_ = t;
    }
    // The navigator's accounting (stage 4): the latest record, and the NavBytes at the largest total.
    void account(const ta::sim::LegionNavigator& nav) {
        acct_ = nav.accounting();
        hasAcct_ = true;
        if (acct_.bytes.total() >= peakBytes_.total()) peakBytes_ = acct_.bytes;
    }

    void report(obs::Keys& k, const ta::sim::World& w, bool legion) const {
        const std::string p = "scale.";
        int64_t n = 0, liveN = 0, pointN = 0, cancelled = 0, dead = 0, idle = 0, missing = 0, never = 0;
        int64_t outside = 0, openInside = 0, stuck = 0, livelock = 0;
        std::vector<int64_t> deliver, start, arrive, complete, path, wait, arriveAt;
        for (size_t i = 0; i < b_.size(); ++i) {
            const Body& b = b_[i];
            if (!b.clicked) { idle += 1; continue; }
            ++n;
            if (b.dead) ++dead;
            if (b.cancelled) { ++cancelled; continue; }
            if (b.land >= 0) deliver.push_back(b.land - b.click); else ++missing;
            if (legion) wait.push_back(b.waitMax);
            if (b.dead) continue;   // the quantiles below are over the bodies alive at the end
            ++liveN;
            if (b.start >= 0) start.push_back(b.start - b.click); else ++never;
            if (!b.point) continue;
            ++pointN;
            if (b.arrive >= 0) {
                arrive.push_back(b.arrive - b.click);
                arriveAt.push_back(b.arrive);
                // Against the straight line to the disc's edge, for bodies that started 4+ cells outside it.
                const int64_t dx = b.tx - b.cx, dz = b.tz - b.cz;
                const int64_t edge = obs::detail::isqrt(dx * dx + dz * dz) - obs::detail::isqrt(b.r2);   // px
                if (edge >= 64) path.push_back((b.travelledAtArrive - b.travelledAtClick) * 1000 / 16 / edge);
            }
            if (b.complete >= 0) {
                complete.push_back(b.complete - b.click);
                outside += b.completeOutside;
            }
            const auto* u = w.unit(ids_[i]);
            if (!u) continue;
            const int64_t ux = u->x.v >> 16, uz = u->z.v >> 16;
            const bool in = inside(b, ux, uz);
            const bool open = !u->orders.empty();
            if (in && open) ++openInside;
            if (!in && open && b.ringN > kWindow / kRingEvery) {
                const auto& was = b.ring[size_t((b.ringN - 1 - kWindow / kRingEvery) % kRing)];
                const int64_t dx = ux - was.x, dz = uz - was.z;
                if (dx * dx + dz * dz < 32 * 32) {
                    ++stuck;
                    if ((b.travelled - was.travelled) / 16 >= 64) ++livelock;
                }
            }
        }
        k.emplace_back(p + "n", n);
        k.emplace_back(p + "point_n", pointN);
        k.emplace_back(p + "cancelled_n", cancelled);
        k.emplace_back(p + "dead_n", dead);
        k.emplace_back(p + "idle_n", idle);
        quantiles(k, p + "deliver", deliver, {{"p50", 50}, {"p99", 99}}, int64_t(deliver.size()), true);
        k.emplace_back(p + "deliver.missing", missing);
        milestones(k, p + "start", start, liveN);
        k.emplace_back(p + "start.never", never);
        k.emplace_back(p + "arrive.n", int64_t(arrive.size()));
        milestones(k, p + "arrive", arrive, pointN);
        k.emplace_back(p + "complete.n", int64_t(complete.size()));
        milestones(k, p + "complete", complete, pointN);
        k.emplace_back(p + "complete_outside_n", outside);
        k.emplace_back(p + "open_inside_end", openInside);
        quantiles(k, p + "path_x1000", path, {{"p50", 50}, {"p90", 90}}, int64_t(path.size()), true);
        k.emplace_back(p + "moving.peak", movingPeak_);
        k.emplace_back(p + "moving.unit_ticks", movingTicks_);
        k.emplace_back(p + "stuck_end", stuck);
        k.emplace_back(p + "livelock_end", livelock);
        // The most arrivals inside one 300-tick window (arrival ticks sorted; two pointers).
        std::sort(arriveAt.begin(), arriveAt.end());
        int64_t peak = 0;
        for (size_t a = 0, z = 0; z < arriveAt.size(); ++z) {
            while (arriveAt[z] - arriveAt[a] >= 300) ++a;
            peak = std::max<int64_t>(peak, int64_t(z - a + 1));
        }
        k.emplace_back(p + "throughput.peak300", peak);
        if (legion) {
            std::sort(wait.begin(), wait.end());
            k.emplace_back(p + "wait.max", wait.empty() ? 0 : wait.back());
            k.emplace_back(p + "wait.p99", wait.empty() ? 0 : wait[(wait.size() * 99 + 99) / 100 - 1]);
            k.emplace_back(p + "wait.member_ticks", waitTicks_);
            k.emplace_back(p + "nav.groups_peak", groupsPeak_);
            k.emplace_back(p + "nav.fields_peak", fieldsPeak_);
            k.emplace_back(p + "nav.members_peak", membersPeak_);
            k.emplace_back(p + "nav.bytes_peak", bytesPeak_);
            if (hasAcct_) {
                const auto& a = acct_;
                for (size_t c = 0; c < size_t(ta::sim::NavWork::Count); ++c) {
                    const std::string n = p + "navwork." + ta::sim::kNavWorkNames[c];
                    k.emplace_back(n + ".total", int64_t(a.total[c]));
                    k.emplace_back(n + ".tick_max", int64_t(a.tickMax[c]));
                }
                k.emplace_back(p + "navwork.tick_all_max", int64_t(a.tickAll));
                const auto& b = peakBytes_;
                k.emplace_back(p + "navbytes.peak", int64_t(b.total()));
                const std::pair<const char*, uint64_t> cats[] = {
                    {"planes", b.planes}, {"fields", b.fields}, {"tiles", b.tiles}, {"portals", b.portals},
                    {"queues", b.queues}, {"scratch", b.scratch}, {"metadata", b.metadata}, {"inflight", b.inflight}};
                for (const auto& [name, v] : cats) k.emplace_back(p + "navbytes." + name + "_at_peak", int64_t(v));
                for (size_t c = 0; c < size_t(ta::sim::NavClass::Count); ++c) {
                    k.emplace_back(p + "sched.gap_max." + ta::sim::kNavClassNames[c], int64_t(a.serviceGapMax[c]));
                    k.emplace_back(p + "sched.age_max." + ta::sim::kNavClassNames[c], int64_t(a.requestAgeMax[c]));
                }
                k.emplace_back(p + "sched.gap_max.any", int64_t(a.serviceGapAny));
                k.emplace_back(p + "sched.promotions", int64_t(a.promotions));
                k.emplace_back(p + "sched.fit_skips", int64_t(a.fitSkips));
                k.emplace_back(p + "sched.reservations", int64_t(a.reservations));
                k.emplace_back(p + "sched.request_promotions", int64_t(a.requestPromotions));
                k.emplace_back(p + "sched.requests_max", int64_t(a.requestsMax));
                k.emplace_back(p + "sched.stopped_shares", int64_t(a.stoppedShares));
                k.emplace_back(p + "sched.admissions", int64_t(a.admissions));
                k.emplace_back(p + "sched.shared_evictions", int64_t(a.sharedEvictions));
                k.emplace_back(p + "sched.inflight_max.first", int64_t(a.firstInflightMax));
                k.emplace_back(p + "sched.inflight_max.refresh", int64_t(a.refreshInflightMax));
                k.emplace_back(p + "sched.request_cells_max", int64_t(a.requestCellsMax));
                k.emplace_back(p + "sched.admit_gap_max", int64_t(a.admitGapMax));
                k.emplace_back(p + "sched.reg_deferred", int64_t(a.regDeferred));
                k.emplace_back(p + "sched.reg_wait_max", int64_t(a.regWaitMax));
                k.emplace_back(p + "sched.plane_waits", int64_t(a.planeWaits));
                k.emplace_back(p + "sched.sync_plane_calls", int64_t(a.syncPlaneCalls));
                k.emplace_back(p + "sched.sync_plane_max", int64_t(a.syncPlaneMax));
                k.emplace_back(p + "sched.init_max", int64_t(a.initMax));
                k.emplace_back(p + "route.demand_peak", int64_t(a.demandPeak));
                k.emplace_back(p + "route.demand_dropped", int64_t(a.demandDropped));
                for (size_t c = 0; c < size_t(ta::sim::RouteNeed::Count); ++c) {
                    k.emplace_back(p + "route.needs." + ta::sim::kRouteNeedNames[c], int64_t(a.routeNeeds[c]));
                    k.emplace_back(p + "route.run_on." + ta::sim::kRouteNeedNames[c], int64_t(a.routeRunOns[c]));
                }
                k.emplace_back(p + "route.corridors", int64_t(a.corridors));
                k.emplace_back(p + "route.corridor_widens", int64_t(a.corridorWidens));
                k.emplace_back(p + "route.fallbacks", int64_t(a.routeFallbacks));
                k.emplace_back(p + "route.chains", int64_t(a.chains));
                k.emplace_back(p + "route.tiles_built", int64_t(a.tilesBuilt));
                k.emplace_back(p + "route.tile_hits", int64_t(a.tileHits));
                k.emplace_back(p + "route.tile_evictions", int64_t(a.tileEvictions));
                k.emplace_back(p + "route.tile_bytes_peak", int64_t(a.tileBytesPeak));
                k.emplace_back(p + "route.chain_skips", int64_t(a.chainSkips));
                k.emplace_back(p + "route.chain_abandons", int64_t(a.chainAbandons));
                k.emplace_back(p + "route.chain_repairs", int64_t(a.chainRepairs));
                k.emplace_back(p + "route.tile_cross_hits", int64_t(a.tileCrossHits));
                k.emplace_back(p + "static.routes_closed", int64_t(a.routesClosed));
                k.emplace_back(p + "static.routes_opened", int64_t(a.routesOpened));
                k.emplace_back(p + "static.improve_refreshes", int64_t(a.improveRefreshes));
                k.emplace_back(p + "static.improve_demands", int64_t(a.improveDemands));
                k.emplace_back(p + "static.trapped_kept", int64_t(a.trappedKept));
                k.emplace_back(p + "static.trapped_reopened", int64_t(a.trappedReopened));
                k.emplace_back(p + "static.chain_resolves", int64_t(a.chainResolves));
                k.emplace_back(p + "static.plan_repairs", int64_t(a.planRepairs));
                k.emplace_back(p + "route.full_plans", int64_t(a.fullPlans));
                k.emplace_back(p + "route.seed_plans", int64_t(a.seedPlans));
                k.emplace_back(p + "route.seed_bound", int64_t(a.seedBoundStarts));
                k.emplace_back(p + "route.seed_bound_restores", int64_t(a.seedBoundRestores));
                k.emplace_back(p + "static.cut_traps", int64_t(a.cutTraps));
                k.emplace_back(p + "static.trapped_resumed", int64_t(a.trappedResumed));
                k.emplace_back(p + "sched.awaited_ticks", int64_t(a.awaitedTicks));
                k.emplace_back(p + "traffic.sectors", int64_t(a.trafficSectors));
                k.emplace_back(p + "traffic.charges", int64_t(a.trafficCharges));
                k.emplace_back(p + "traffic.jam_stamps", int64_t(a.jamStamps));
                k.emplace_back(p + "traffic.corrections_built", int64_t(a.correctionsBuilt));
                k.emplace_back(p + "traffic.correction_hits", int64_t(a.correctionHits));
                k.emplace_back(p + "choke.openings", int64_t(a.openings));
                k.emplace_back(p + "choke.scans", int64_t(a.openingScans));
                k.emplace_back(p + "choke.entrants", int64_t(a.openingEntrants));
                k.emplace_back(p + "choke.delay_max", int64_t(a.openingDelayMax));
                k.emplace_back(p + "choke.charged_fields", int64_t(a.chargedFields));
                k.emplace_back(p + "choke.charges", int64_t(a.openingCharges));
                k.emplace_back(p + "choke.replans", int64_t(a.openingReplans));
                k.emplace_back(p + "choke.search_charges", int64_t(a.openingSearchCharges));
                k.emplace_back(p + "choke.splits", int64_t(a.openingSplits));
                k.emplace_back(p + "choke.split_members", int64_t(a.splitMembers));
                k.emplace_back(p + "choke.admissions", int64_t(a.passAdmits));
                k.emplace_back(p + "choke.admit_waits", int64_t(a.admitWaits));
                k.emplace_back(p + "choke.admit_turns", int64_t(a.admitTurns));
                k.emplace_back(p + "choke.admit_closings", int64_t(a.admitClosings));
                k.emplace_back(p + "arrive.shared_areas", int64_t(a.sharedAreas));
                k.emplace_back(p + "arrive.group_entrances", int64_t(a.groupEntrances));
            }
        }
    }

private:
    static constexpr int kRing = 8, kRingEvery = 100, kWindow = 600;
    struct Snap { int64_t x = 0, z = 0, travelled = 0; };
    struct Body {
        bool clicked = false, point = false, cancelled = false, dead = false, had = false, seen = false;
        bool completeOutside = false;
        int click = -1, land = -1, start = -1, arrive = -1, complete = -1;
        int64_t tx = 0, tz = 0, r2 = 0;         // the point (px) and its disc (px^2)
        int64_t cx = 0, cz = 0, lx = 0, lz = 0;  // position at the click and when the command landed (px)
        int64_t px = 0, pz = 0;                  // last sampled position (raw Fixed)
        int64_t travelled = 0, travelledAtClick = 0, travelledAtArrive = 0;   // 1/16 px
        int waitRun = 0, waitMax = 0;
        uint32_t landedN = 0, expect = ~0u;      // commands landed for the body in all; the count delivering the click
        std::array<Snap, kRing> ring{};
        int64_t ringN = 0;
    };
    static bool inside(const Body& b, int64_t x, int64_t z) {
        return (x - b.tx) * (x - b.tx) + (z - b.tz) * (z - b.tz) <= b.r2;
    }
    Body* body(int id) {
        if (id < 0 || size_t(id) >= index_.size() || index_[size_t(id)] < 0) return nullptr;
        return &b_[size_t(index_[size_t(id)])];
    }
    // t50/t90/t99/last over `n` bodies: the need-th smallest value, -1 when fewer reached it.
    static void milestones(obs::Keys& k, const std::string& p, std::vector<int64_t> v, int64_t n) {
        std::sort(v.begin(), v.end());
        const auto at = [&](int64_t need) -> int64_t {
            return need >= 1 && need <= int64_t(v.size()) ? v[size_t(need - 1)] : -1;
        };
        k.emplace_back(p + ".t50", at((n + 1) / 2));
        k.emplace_back(p + ".t90", at((n * 9 + 9) / 10));
        k.emplace_back(p + ".t99", at((n * 99 + 99) / 100));
        k.emplace_back(p + ".last", at(n));
    }
    // Nearest-rank quantiles over the values present, and their max.
    static void quantiles(obs::Keys& k, const std::string& p, std::vector<int64_t> v,
                          std::initializer_list<std::pair<const char*, int>> qs, int64_t n, bool max) {
        std::sort(v.begin(), v.end());
        for (const auto& [name, q] : qs)
            k.emplace_back(p + "." + name, n ? v[size_t((n * q + 99) / 100 - 1)] : -1);
        if (max) k.emplace_back(p + ".max", n ? v.back() : -1);
    }

    std::vector<int> ids_;
    std::vector<Body> b_;
    std::vector<int> index_;
    int64_t movingPeak_ = 0, movingTicks_ = 0, waitTicks_ = 0;
    int64_t groupsPeak_ = 0, fieldsPeak_ = 0, membersPeak_ = 0, bytesPeak_ = 0;
    ta::sim::NavAccounting acct_;
    ta::sim::NavBytes peakBytes_;
    bool hasAcct_ = false;
    int last_ = -1;
};

}  // namespace ta::legion_scale
