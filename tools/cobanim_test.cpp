// Script-driven animation contracts, run on the real shipped COBs.
//
// Retail's engine barely drives animation: it calls Create, and the SCRIPT
// starts control threads that poll GET_UNIT_VALUE and pick the animation. So
// what has to be right is the engine's ANSWERS. Each case here pins one answer
// by running the real script and observing what it does with it.
//
// A castle/factory opens its yard through a BLOCKING protocol (see aracastl
// OpenYard): it sets unit value 18 (YARD_OPEN), then loops -- reading 18 and
// leaving only when the engine answers NONZERO, setting 19 (BUGGER_OFF) and
// sleeping 1.5s each time round. Answer 0 and the thread spins for ever; and
// since Go CALLs startbuild and then OpenYard, the state machine never reaches
// Stop/stopbuild, so the yard doors open and never close.
//
// This pins the engine's side of that contract: with the answer the engine
// gives, the build sequence must terminate.
#include "cob/cob.h"
#include "cob/vm.h"
#include "client/renderframe.h"
#include "client/threadpool.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace tak;

static int fails = 0;
static int ran = 0;   // units actually exercised; 0 means the run proved nothing
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : (" -- " + detail).c_str());
    if (!ok) ++fails;
    ++ran;
}

// How many times the unit re-issues BUGGER_OFF (unit value 19) over ~40s of
// simulated time, having primed it the way the engine does and then activated
// it.
//
// This reads the protocol directly rather than counting threads. Thread counts
// prove nothing here: a factory legitimately holds one open while it is active
// (Creon's Go ends with START_SCRIPT FactoryFun, the machinery loop), and the
// close path has a handshake of its own. But BUGGER_OFF is unambiguous -- it is
// the "get out of my yard" retry, and it is issued ONLY by the refused branch
// of the OpenYard loop, once every 1.5s, for ever.
static int buggerOffs(const std::string& path, int32_t yardGrant) {
    cob::File f = cob::load(path);
    cob::Vm vm(std::move(f), true);
    vm.enableRetailAnimation();
    int count = 0;
    vm.onGet = [&](int32_t id, const std::vector<int32_t>&) -> int32_t {
        return id == 18 ? (yardGrant > 0 ? 1 : 0) : 0;
    };
    vm.onSetUnitValue = [&](int32_t id, int32_t v) { if (id == 19 && v) ++count; };
    vm.start("Create");
    for (int i = 0; i < 30; ++i) vm.tick(1.0f / 30.0f);
    vm.start("InitState");
    for (int i = 0; i < 5; ++i) vm.tick(1.0f / 30.0f);
    vm.start("Activate");
    for (int i = 0; i < 1200; ++i) vm.tick(1.0f / 30.0f);   // 40s
    return count;
}

// The shipped wheel script compares GET 33 against +/-910, but native GET 33
// clamps its result to +/-100. Preserve retail's unreachable branch rather than
// feeding fabricated angle units to make the differential engage.
static int wheelSides(const std::string& path, int32_t turnRate) {
    cob::File f = cob::load(path);
    cob::Vm vm(std::move(f), true);
    vm.enableRetailAnimation();
    vm.onGet = [&](int32_t id, const std::vector<int32_t>&) -> int32_t {
        if (id == 33) return turnRate;
        if (id == 29) return 100;          // at full speed, so the wheels turn
        return 0;
    };
    vm.start("Create");
    for (int i = 0; i < 120; ++i) vm.tick(1.0f / 30.0f);
    // Distinct spin rates across the pieces = the differential engaged. Wheels
    // are driven with SPIN, so the target rate is what carries it.
    std::vector<float> rates;
    for (const auto& ps : vm.pieces())
        for (int ax = 0; ax < 3; ++ax)
            if (std::fabs(ps.spinTarget[ax]) > 1e-4f) rates.push_back(ps.spinTarget[ax]);
    int distinct = 0;
    for (size_t i = 0; i < rates.size(); ++i) {
        bool seen = false;
        for (size_t j = 0; j < i; ++j)
            if (std::fabs(rates[i] - rates[j]) < 1e-3f) { seen = true; break; }
        if (!seen) ++distinct;
    }
    return distinct;
}

int main(int argc, char** argv) {
    if(argc==3 && std::string(argv[1])=="--schedule-bench") {
        const auto file=std::make_shared<const cob::File>(cob::load(argv[2]));
        ThreadPool pool;
        uint64_t expected=0;
        const auto reachability=cob::explosionReachability(*file);
        for(int trial=0;trial<6;++trial) {
            const int mode=trial%3;
            std::vector<std::unique_ptr<cob::Vm>> units;
            units.reserve(16000);
            for(int i=0;i<16000;++i) {
                auto vm=std::make_unique<cob::Vm>(file,true);vm->enableRetailAnimation();
                vm->onGet=[](int32_t id,const std::vector<int32_t>&) {
                    return id==4 || id==29 || id==33 ? 100 : (id==34 ? 1 : 0);
                };
                vm->start("Create");vm->start("StartMoving");
                units.push_back(std::move(vm));
            }
            std::vector<double> times;
            std::vector<cob::Vm*> parallelUnits,serialUnits;
            parallelUnits.reserve(16000);serialUnits.reserve(16000);
            for(int frame=0;frame<120;++frame) {
                const auto begin=std::chrono::steady_clock::now();
                const auto tick=[&](size_t b,size_t e) {
                    for(size_t i=b;i<e;++i)units[i]->tick(1.f/30.f);
                };
                if(mode==2) {
                    parallelUnits.clear();serialUnits.clear();
                    for(const auto& vm:units)
                        (vm->mayReachExplosion(reachability)?serialUnits:parallelUnits).push_back(vm.get());
                    pool.parallelFor(parallelUnits.size(),[&](size_t b,size_t e) {
                        for(size_t i=b;i<e;++i)parallelUnits[i]->tick(1.f/30.f);
                    },1500);
                    for(auto* vm:serialUnits)vm->tick(1.f/30.f);
                } else if(mode==1)pool.parallelFor(units.size(),tick,1500);
                else tick(0,units.size());
                const double ms=std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now()-begin).count();
                if(frame>=30)times.push_back(ms);
            }
            uint64_t hash=1469598103934665603ull;
            const auto mix=[&](uint32_t value){hash=(hash^value)*1099511628211ull;};
            for(const auto& vm:units) {
                for(const auto& piece:vm->retailPieces()) {
                    mix(piece.visible);
                    for(int axis=0;axis<3;++axis) {mix(piece.move[axis]);mix(piece.turn[axis]);}
                }
                for(auto pc:vm->threadPcs())mix(pc);
            }
            if(trial==0)expected=hash;
            check(hash==expected,"serial/parallel animation benchmark final-state agreement");
            std::sort(times.begin(),times.end());
            std::printf("16000 VMs %s trial=%d p50=%.3fms p95=%.3fms max=%.3fms hash=%016llx\n",
                mode==2?"selective":mode==1?"parallel":"serial",trial,times[times.size()/2],
                times[times.size()*95/100],times.back(),static_cast<unsigned long long>(hash));
            std::fflush(stdout);
        }
        return fails?1:0;
    }
    if(argc==3 && std::string(argv[1])=="--blood-query") {
        cob::Vm vm(cob::load(argv[2]),true);vm.enableRetailAnimation();
        for(int i=0;i<64;++i) {
            vm.call("QueryBlood",{-1});
            const auto& locals=vm.lastLocals();
            check(!locals.empty() && locals[0]>=2 && locals[0]<=6,
                  "Hunter QueryBlood replaces the minus-one argument sentinel");
        }
        return fails?1:0;
    }
    if(argc==2 && std::string(argv[1])=="--explode-capture") {
        {
            cob::File f;f.scripts={{"Idle",0},{"Killed",2},{"Caller",6}};
            f.code={0x10064000,0,0x10021001,0,0x10071000,0,
                    0x10062000,1,0,0x10065000};
            auto reachable=cob::explosionReachability(f);
            check(!reachable[0] && reachable[2] && reachable[6],
                  "dormant death script does not serialize an idle loop; CALL reaches explosion");
            f.code[6]=0x10061000;
            check(cob::explosionReachability(f)[6],"START_SCRIPT follows explosion reachability");
            f.code={0x10066000,4,0x10064000,0,0x10071000,0};
            reachable=cob::explosionReachability(f);
            check(reachable[0] && reachable[2],"conditional explosion propagates through cyclic paths");
            f.code={0x10021001,0x10071000,0x10065000};
            check(!cob::explosionReachability(f)[0],"opcode-shaped constant does not serialize safe code");
            f.code={0x10064000,999};
            check(cob::explosionReachability(f)[0],"invalid branch conservatively requires serial execution");
            f.code={0xdeadbeef};
            check(cob::explosionReachability(f)[0],"unknown opcode conservatively requires serial execution");
        }
        // Flags must survive the integer VM's sparse pose export. RENDER_OFF
        // gates shadows only; DONT_CACHE moves body rendering to the other pass.
        for(bool retail:{false,true})for(unsigned flags=0;flags<16;++flags) {
            cob::File file;file.pieces={"body"};file.scripts={{"Flags",0},{"Reset",11}};
            file.code={flags&1?0x10005000u:0x10006000u,0,
                       flags&2?0x10007000u:0x10008000u,0,
                       flags&4?0x1000d000u:0x1000e000u,0,
                       flags&8?0x10009000u:0x1000a000u,0,
                       0x10021001,0,0x10065000,
                       0x10005000,0,0x10007000,0,0x1000d000,0,0x10009000,0,
                       0x10021001,0,0x10065000};
            cob::Vm vm(std::move(file),true);if(retail)vm.enableRetailAnimation();
            vm.start("Flags");vm.tick(1.0f/30.0f);
            const auto& piece=vm.pieces()[0];
            check(piece.visible==bool(flags&1) && piece.cached==bool(flags&2) &&
                  piece.shaded==bool(flags&4) && piece.rendered==bool(flags&8),
                  "authored piece flags reach display pose");
            check(piece.castsShadow()==((flags&11)==11),"native per-piece shadow admission");
            vm.start("Reset");vm.tick(1.0f/30.0f);
            check(piece.visible && piece.cached && piece.shaded && piece.rendered,
                  "piece flags can be restored after sparse export");
        }
        for(bool retail:{false,true})for(bool accepted:{false,true})
        for(unsigned later:{0u,0x10005000u,0x10006000u})
        for(unsigned flags:{0u,0x20u,0x40u,0x60u}) {
            cob::File file;file.pieces={"body","child","sibling"};file.scripts={{"Test",0}};
            file.code={0x10021001,flags,0x10071000,0};
            if(later)file.code.insert(file.code.end(),{later,0});
            file.code.insert(file.code.end(),{0x10021001,0,0x10065000});
            cob::Vm vm(std::move(file),true);
            if(retail)vm.enableRetailAnimation();
            vm.setExplosionDescendants({{1},{},{}});
            int calls=0;
            vm.onExplode=[&](int piece,int32_t value) {
                ++calls;
                check(piece==0 && unsigned(value)==flags,"EXPLODE callback arguments");
                check(retail?vm.retailPieces()[0].visible:vm.pieces()[0].visible,
                      "detached snapshot sees source visibility before hiding");
                return accepted;
            };
            vm.start("Test");vm.tick(1.0f/30.0f);
            check(calls==1,"EXPLODE callback delivered once");
            const bool visible=later ? later==0x10005000 : (!accepted || bool(flags&0x20));
            check(vm.pieces()[0].visible==visible,"EXPLODE admission preserves later SHOW/HIDE ordering");
            check(vm.pieces()[1].visible==(!accepted || !(flags&64) || bool(flags&32)),
                  "subtree detachment hides descendants only when debris is requested");
            check(vm.pieces()[2].visible,"subtree detachment leaves root siblings attached");
        }
        for(bool retail:{false,true}) {
            cob::File file;file.pieces={"body"};
            file.scripts={{"Idle",0},{"Killed",5}};
            file.code={0x10021001,100,0x10013000,0x10064000,0,
                       0x10021001,100,0x10013000,0x10021001,0,
                       0x10071000,0,0x10021001,0,0x10065000};
            const auto reachability=cob::explosionReachability(file);
            cob::Vm vm(std::move(file),true);if(retail)vm.enableRetailAnimation();
            vm.start("Idle");vm.tick(1.f/30);
            check(!vm.mayReachExplosion(reachability),"active idle loop retains parallel scheduling");
            vm.start("Killed");
            check(vm.mayReachExplosion(reachability),"active death thread requires serial scheduling");
            bool admitted=false;int explosions=0;
            vm.onExplode=[&](int,int32_t) {
                check(admitted,"every executed explosion was classified before the VM tick");
                ++explosions;return true;
            };
            for(int i=0;i<10;++i) {
                admitted=vm.mayReachExplosion(reachability);vm.tick(1.f/30);
            }
            check(explosions==1 && !vm.mayReachExplosion(reachability),
                  "completed death thread restores parallel scheduling for remaining idle loop");
        }
        for(bool retail:{false,true}) {
            cob::File file;file.pieces={"body"};file.scripts={{"Test",0}};
            file.code={0x10021001,0,0x10071000,0, // accepted explosion
                       0x10005000,0,             // SHOW before the next attempt
                       0x10021001,0,0x10071000,0, // rejected explosion
                       0x10021001,0,0x10065000};
            file.scripts.push_back({"QueryBlood",uint32_t(file.code.size())});
            file.code.insert(file.code.end(),{0x10021001,7,0x10023002,0,
                                             0x10021001,0,0x10065000});
            cob::Vm vm(std::move(file),true);
            if(retail)vm.enableRetailAnimation();
            int calls=0;
            vm.onExplode=[&](int,int32_t) {
                ++calls;
                check(retail?vm.retailPieces()[0].visible:vm.pieces()[0].visible,
                      "synchronous creation sees SHOW before the second explosion");
                if(calls!=1)return false;
                vm.call("QueryBlood",{-1});
                check(!vm.lastLocals().empty() && vm.lastLocals()[0]==7,
                      "QueryBlood can reenter the VM from accepted EXPLODE");
                return true;
            };
            vm.start("Test");vm.tick(1.0f/30.0f);
            check(calls==2 && vm.pieces()[0].visible,
                  "nested query preserves the outer script and rejected source visibility");
        }
        {
            cob::File file;file.pieces.resize(70,"piece");file.scripts={{"Test",0}};
            file.code={0x10021001,30*65536,0x10021001,30*65536,
                       0x10001000,69,0,0x10021001,777,0x1000f000,69,
                       0x10021001,0,0x10065000};
            file.scripts.push_back({"Immediate",uint32_t(file.code.size())});
            file.code.insert(file.code.end(),{0x10021001,7*65536,0x1000b000,69,0,
                0x10006000,69,0x10021001,778,0x1000f000,69,
                0x10021001,0,0x10065000});
            cob::Vm vm(std::move(file),true);vm.enableRetailAnimation();
            int callbacks=0;
            vm.onEmitSfx=[&](int piece,int32_t code) {
                ++callbacks;
                check(piece==69,"high-index piece callback preserved");
                check(code==777 ? vm.pieces()[69].moveTarget[0]==0 :
                      vm.pieces()[69].move[0]==30 && vm.pieces()[69].visible,
                      "callbacks retain pose from previous export boundary");
            };
            vm.start("Test");
            for(int i=0;i<31;++i)vm.tick(1.f/30);
            check(vm.pieces()[69].move[0]==30 && !vm.pieces()[69].moving[0] &&
                  vm.pieces()[69].moveSpeed[0]==0,
                  "high-index motion exports its final settling state");
            vm.call("Immediate");
            check(callbacks==2 && vm.pieces()[69].move[0]==7 && !vm.pieces()[69].visible,
                  "query exports immediate movement and visibility before returning");
        }
        return fails?1:0;
    }
    if (argc==3 && std::string(argv[1])=="--hunter-movement") {
        sim::UnitType type;type.maxVel=sim::Fixed::fromInt(5);
        sim::Unit unit;unit.type=&type;unit.baseSpeed=type.maxVel;unit.speed=sim::Fixed::fromInt(1);
        UnitR frame;frame.type=&type;
        cob::Vm vm(cob::load(argv[2]),true);
        vm.enableRetailAnimation();
        vm.onGet=[&](int32_t id,const std::vector<int32_t>&) {
            return id==29 ? frame.animationSpeedPercent() : 0;
        };
        if (!vm.start("MoveWatcher")) return 2;
        for (int blocked : {0,1,2,2,0}) {
            unit.bodyBlockStreak=blocked;frame.captureMovement(unit);
            for (int tick=0;tick<12;++tick) vm.tick(1.0f/30.0f);
            check(vm.getStatic(0)==(blocked<2),
                  "Hunter walk gate follows collision refusal and resumes without restarting its script");
        }
        return fails ? 1 : 0;
    }
    if (argc < 2) { std::printf("usage: cobyard_test <scripts-dir>\n"); return 2; }
    const std::string dir = argv[1];
    // Yard units across three factions, so a faction-specific script template
    // cannot make this pass by accident.
    const char* units[] = {"aracastl", "arakeep", "tarcastl", "creacad", "cresmit"};

    std::printf("[build yard: the engine must grant YARD_OPEN (unit value 18)]\n");
    for (const char* u : units) {
        const std::string path = dir + "/" + u + ".cob";
        std::FILE* probe = std::fopen(path.c_str(), "rb");
        if (!probe) { std::printf("  [SKIP] %s (not in this install)\n", u); continue; }
        std::fclose(probe);

        const int granted = buggerOffs(path, 1);
        const int refused = buggerOffs(path, 0);
        check(granted <= 1,
              std::string(u) + ": granting the yard settles the handshake",
              "BUGGER_OFF issued " + std::to_string(granted) + "x in 40s");
        // The counter-case: what the bug looked like, and what regressing the
        // engine's answer to 0 would look like again.
        check(refused > 10,
              std::string(u) + ": refusing it retries for ever (counter-case)",
              "BUGGER_OFF issued " + std::to_string(refused) + "x in 40s");
    }

    std::printf("\n[wheels: unit value 33 is a SIGNED turn rate]\n");
    {
        const std::string path = dir + "/aracan.cob";
        std::FILE* probe = std::fopen(path.c_str(), "rb");
        if (!probe) std::printf("  [SKIP] aracan (not in this install)\n");
        else {
            std::fclose(probe);
            const int straight = wheelSides(path, 0);
            const int turning = wheelSides(path,sim::retailTurnAnimationPercent(2000,2000,0,65536,false));
            const int reversing = wheelSides(path,sim::retailTurnAnimationPercent(-2000,2000,0,65536,false));
            check(straight>0 && turning==straight && reversing==straight,
                  "aracan: retail turn percentages do not cross its unreachable 910 threshold",
                  "distinct wheel rates straight=" + std::to_string(straight) +
                      " turning=" + std::to_string(turning));
        }
    }

    // A run that skipped everything must NOT report success. The first wiring of
    // this test pointed at a path with no COBs in it, every unit was skipped, and
    // it printed ALL PASS -- a green test that proved nothing is worse than none.
    if (!ran) {
        std::printf("\nFAILED -- no COBs found under \"%s\"; nothing was tested\n",
                    dir.c_str());
        return 2;
    }
    std::printf("\n%s (%d checks)\n", fails ? "FAILED" : "ALL PASS", ran);
    return fails ? 1 : 0;
}
