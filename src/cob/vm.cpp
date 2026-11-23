#include "cob/vm.h"
#include "cob/retailstate.h"
#include "sim/retailrng.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace tak::cob {

namespace {

constexpr float kPi = 3.14159265358979f;
// COB angles: 65536 units per full circle. Linear: 65536 per world unit.
constexpr float kAngle = 2 * kPi / 65536.0f;
constexpr float kLinear = 1.0f / 65536.0f;
// COB move/turn/spin speeds are per SECOND. (Retail stores speed/30 as a per-frame
// step at its fixed 30Hz update -- i.e. `speed` angle-or-linear units per second --
// so we step by speed*dt in real time and they integrate to the same rate.)

float towards(float cur, float target, float step) {
    if (cur < target) return std::min(cur + step, target);
    return std::max(cur - step, target);
}

// Angular step toward a target along the SHORTEST path, like the retail COB
// interpreter (which compares abs(target-cur) to a half-circle and flips the
// turn direction). Without this, a turn from e.g. 350deg to 10deg sweeps the
// long way through 180deg instead of the short way through 0.
float angTowards(float cur, float target, float step) {
    float d = std::remainder(target - cur, 2 * kPi);   // shortest signed delta in (-pi,pi]
    if (std::abs(d) <= step) return target;
    return cur + (d > 0 ? step : -step);
}

} // namespace

struct Vm::Native {
    RetailScriptState state;
    uint32_t seed=12345;
    double ticks=0;
    std::vector<uint8_t> dirty;
    std::vector<size_t> changed;
    // Gait pacing: the shared per-type map, the rate and its tick accumulator,
    // the pieces (0-63) whose motion a gait thread started last, and the slot
    // that is running now (-1 outside the thread loop, e.g. a query call).
    std::span<const uint8_t> gait;
    float gaitRate=1;
    double gaitTicks=0;
    uint64_t gaitPieces=0;
    int slot=-1;
    bool gaitThread(unsigned i) const {
        const uint32_t pc=state.vm.threads[i].words[1];
        return pc<gait.size() && gait[pc];
    }
    template<class Host> void tick(const File& file,int32_t elapsed,Host& host) {
        if(gait.empty()) {state.tick(file,elapsed,host);return;}
        bool full=true;
        if(elapsed && gaitRate<1) {
            gaitTicks+=gaitRate;
            full=gaitTicks>=1;
            if(full)gaitTicks-=1;
        }
        state.tickGated(file,elapsed,host,[&](unsigned i) {
            slot=int(i);
            return full || !gaitThread(i);
        },full ? 0 : gaitPieces);
        slot=-1;
    }
    explicit Native(const File& file):state(file),dirty(file.pieces.size(),1) {
        changed.reserve(file.pieces.size());
        for(size_t i=0;i<file.pieces.size();++i) changed.push_back(i);
    }
    void pieceChanged(size_t i) {
        if (!dirty[i]) {dirty[i]=1;changed.push_back(i);}
    }
    struct Host {
        Vm& vm;Native& native;
        uint32_t random(int32_t bound) { return tak::sim::retailRandom(native.seed,bound); }
        void pieceChanged(size_t i) { native.pieceChanged(i); }
        void pieceCommanded(size_t i) {
            if(native.gait.empty() || i>=64)return;
            const uint64_t bit=uint64_t(1)<<i;
            if(native.slot>=0 && native.gaitThread(unsigned(native.slot)))native.gaitPieces|=bit;
            else native.gaitPieces&=~bit;
        }
        uint32_t get(int id,const std::array<uint32_t,4>& args) {
            if (!vm.onGet) return 0;
            std::vector<int32_t> values;
            if (std::any_of(args.begin(),args.end(),[](uint32_t value){return value!=0;}))
                for(auto value:args) values.push_back(std::bit_cast<int32_t>(value));
            return uint32_t(vm.onGet(id,values));
        }
        void set(int id,int value) { if(vm.onSetUnitValue) vm.onSetUnitValue(id,value); }
        uint32_t sound(int name,int32_t priority) {
            if(vm.onPlaySound) vm.onPlaySound(name,priority);
            // Native unit sound host 50def0 always returns zero.
            return 0;
        }
        void effect(uint32_t op,int piece,int32_t value) {
            if(op==0x1000f000 && vm.onEmitSfx) vm.onEmitSfx(piece,value);
            if(op==0x10071000) {
                if(!vm.onExplode || vm.onExplode(piece,value))
                    vm.hideExplodedPiece(piece,value);
            }
        }
    };
};

Vm::~Vm()=default;
Vm::Vm(const Vm&)=default;
void Vm::hideExplodedPiece(int piece,int32_t flags) {
    if(flags&0x20)return;
    const auto hide=[&](int index) {
        if(index<0 || size_t(index)>=pieces_.size())return;
        pieces_[size_t(index)].visible=false;
        if(native_)native_->state.pieces[size_t(index)].visible=false;
    };
    hide(piece);
    if((flags&64) && piece>=0 && size_t(piece)<explosionDescendants_.size())
        for(int child:explosionDescendants_[size_t(piece)])hide(child);
}
void Vm::enableRetailAnimation() {
    native_=std::make_unique<Native>(*file_);
    exportNativePieces();
}
void Vm::seedRandom(uint32_t seed) {
    // The retail generator's domain is 1..2^31-2 (0 and 2^31-1 are fixed points).
    seed=1+seed%0x7ffffffeu;
    if(native_) native_->seed=seed;
    rng_=seed;
}
void Vm::setGaitCode(std::span<const uint8_t> code) {
    if(native_) native_->gait=code;
}
void Vm::setGaitRate(float rate) {
    if(!native_) return;
    rate=std::clamp(rate,0.0f,1.0f);
    if(rate>=1) native_->gaitTicks=0;
    native_->gaitRate=rate;
}
float Vm::gaitRate() const { return native_ ? native_->gaitRate : 1.0f; }
void Vm::reset() {
    threads_.clear();
    if(native_) {
        native_->state.vm.clearThreads();
    }
}
std::span<const RetailPiece> Vm::retailPieces() const {
    return native_ ? std::span<const RetailPiece>(native_->state.pieces) : std::span<const RetailPiece>{};
}
int32_t Vm::getStatic(size_t i) const {
    if(native_) return i<native_->state.vm.statics.size() ? int32_t(native_->state.vm.statics[i]) : 0;
    return i<statics_.size() ? statics_[i] : 0;
}
std::vector<uint32_t> Vm::threadPcs() const {
    std::vector<uint32_t> out;
    if(native_) {
        for(uint32_t pending=native_->state.vm.activeThreadMask();pending;pending&=pending-1)
            out.push_back(native_->state.vm.threads[std::countr_zero(pending)].words[1]);
    } else for(const auto& t:threads_) out.push_back(t.pc);
    return out;
}
bool Vm::mayReachExplosion(std::span<const uint8_t> reachability) const {
    const auto unsafe=[&](size_t pc) {return pc>=reachability.size() || reachability[pc];};
    if(native_) {
        for(uint32_t pending=native_->state.vm.activeThreadMask();pending;pending&=pending-1)
            if(unsafe(native_->state.vm.threads[std::countr_zero(pending)].words[1]))return true;
    } else {
        const auto reachable=[&](const auto& thread) {
            if(thread.dead)return false;
            if(unsafe(thread.pc))return true;
            for(auto pc:thread.callStack)if(unsafe(pc))return true;
            return false;
        };
        for(const auto& thread:threads_)if(reachable(thread))return true;
        for(const auto& thread:pending_)if(reachable(thread))return true;
    }
    return false;
}
void Vm::exportNativePieces() {
    const auto convert=[](const RetailPiece& source,PieceState& target) {
        target.visible=source.visible;
        target.cached=source.cached;
        target.shaded=source.shaded;
        target.rendered=source.rendered;
        for(int axis=0;axis<3;++axis) {
            target.move[axis]=float(source.move[axis])*kLinear;
            target.rot[axis]=float(source.turn[axis])*kAngle;
            target.moveTarget[axis]=float(source.moveTarget[axis])*kLinear;
            target.moveSpeed[axis]=std::abs(float(source.moveSpeed[axis]))*30*kLinear;
            target.moving[axis]=source.moveSpeed[axis]!=0;
            target.rotTarget[axis]=float(source.turnTarget[axis])*kAngle;
            target.rotSpeed[axis]=std::abs(float(source.turnSpeed[axis]))*30*kAngle;
            target.turning[axis]=source.turnTarget[axis]!=-1 && source.turnSpeed[axis]!=0;
            target.spin[axis]=source.turnTarget[axis]==-1 ? float(source.turnSpeed[axis])*30*kAngle : 0;
            target.spinTarget[axis]=float(source.spinTarget[axis])*30*kAngle;
            target.spinAccel[axis]=float(source.spinAcceleration[axis])*900*kAngle;
        }
    };
    // Preserve the old export boundaries: callbacks inside a script tick still
    // observe the preceding exported pose. Only the conversion work is sparse.
    for (size_t i:native_->changed) {
        convert(native_->state.pieces[i],pieces_[i]);
        native_->dirty[i]=0;
    }
    native_->changed.clear();
#ifndef NDEBUG
    static const bool verify=std::getenv("TAK_VERIFY_NATIVE_EXPORT")!=nullptr;
    if (verify) for (size_t i=0;i<pieces_.size();++i) {
        PieceState expected;
        convert(native_->state.pieces[i],expected);
        if (!(expected==pieces_[i]))
            throw std::runtime_error("sparse native piece export differs from full conversion");
    }
#endif
}

Vm::Vm(std::shared_ptr<const File> file, bool deterministicRand)
    : file_(std::move(file)), detRand_(deterministicRand) {
    statics_.assign(file_->numStatics + 8, 0);
    pieces_.assign(file_->pieces.size(), PieceState{});
}

bool Vm::start(const std::string& script, const std::vector<int32_t>& args) {
    int idx = file_->scriptIndex(script);
    if (idx < 0) return false;
    if(native_) {
        if(args.size()>4) throw std::invalid_argument("too many animation callback arguments");
        std::array<uint32_t,4> values{};
        for(size_t i=0;i<args.size();++i) values[i]=uint32_t(args[i]);
        const bool started=native_->state.startArguments(*file_,idx,values,unsigned(args.size()));
        Native::Host host{*this,*native_};
        if(started) native_->tick(*file_,0,host);
        exportNativePieces();
        return started;
    }
    Thread t;
    t.pc = file_->scripts[size_t(idx)].entry;
    t.locals = args;
    t.locals.resize(std::max<size_t>(args.size(), 16), 0);
    // Starting from inside a hook while tick() iterates threads_ must not
    // invalidate the iteration: defer to end of tick.
    if (ticking_) pending_.push_back(std::move(t));
    else threads_.push_back(std::move(t));
    return true;
}

bool Vm::startNow(const std::string& script, const std::vector<int32_t>& args) {
    if (native_ || ticking_) return start(script, args);
    if (!start(script, args)) return false;
    ticking_ = true;
    run(threads_.back());
    ticking_ = false;
    for (auto& t : pending_) threads_.push_back(std::move(t));
    pending_.clear();
    std::erase_if(threads_, [](const Thread& t) { return t.dead; });
    return true;
}

int32_t Vm::call(const std::string& script, const std::vector<int32_t>& args) {
    lastLocals_.clear();
    lastReturn_ = 0;
    int idx = file_->scriptIndex(script);
    if (idx < 0) return 0;
    if(native_) {
        std::array<uint32_t,4> values{};
        for(size_t i=0;i<std::min(args.size(),values.size());++i) values[i]=uint32_t(args[i]);
        Native::Host host{*this,*native_};
        native_->state.query(*file_,idx,values,host);
        for(auto value:values) lastLocals_.push_back(std::bit_cast<int32_t>(value));
        exportNativePieces();
        return 0; // retail unit queries return through their argument words
    }
    Thread t;
    t.pc = file_->scripts[size_t(idx)].entry;
    t.locals = args;
    t.locals.resize(std::max<size_t>(args.size(), 16), 0);
    // Query call-ins are straight-line (no SLEEP/WAIT); run() executes to RETURN
    // in one slice. Bound the loop and stop if a script yields (pc unchanged after
    // a slice = SLEEP/WAIT), so a caller can never hang on a non-query script.
    for (int guard = 0; guard < 64 && !t.dead; ++guard) {
        uint32_t before = t.pc;
        run(t);
        if (!t.dead && t.pc == before) break;
    }
    lastLocals_ = t.locals;
    return lastReturn_;
}

void Vm::setStatic(size_t i, int32_t v) {
    if(native_) {
        if(i<native_->state.vm.statics.size()) native_->state.vm.statics[i]=uint32_t(v);
        return;
    }
    if (i >= statics_.size()) statics_.resize(i + 1, 0);
    statics_[i] = v;
}

int32_t Vm::pop(Thread& t) {
    if (t.stack.empty()) return 0;
    int32_t v = t.stack.back();
    t.stack.pop_back();
    return v;
}

void Vm::push(Thread& t, int32_t v) { t.stack.push_back(v); }

void Vm::tick(float dt) {
    if(native_) {
        native_->ticks+=std::max(0.0,double(dt))*30.0;
        if(native_->ticks+1e-7<1) return;
        Native::Host host{*this,*native_};
        while(native_->ticks+1e-7>=1) {
            native_->tick(*file_,1,host);
            native_->ticks-=1;
        }
        exportNativePieces();
        return;
    }
    now_ += dt;

    // Progress piece animations. The sweep is gated on anyMotion_ -- the branchy
    // pieces x 3-axes walk was ~80% of an IDLE unit's tick cost, and a standing
    // army/garrisoned building animates nothing. Ops that start a move/turn/spin
    // set the flag; the sweep clears it once every animation has settled.
    if (anyMotion_) {
        bool still = false;
        for (auto& p : pieces_) {
            for (int a = 0; a < 3; ++a) {
                if (p.moving[a]) {
                    p.move[a] = towards(p.move[a], p.moveTarget[a], p.moveSpeed[a] * dt);
                    if (p.move[a] == p.moveTarget[a]) p.moving[a] = false;
                }
                if (p.turning[a]) {
                    p.rot[a] = angTowards(p.rot[a], p.rotTarget[a], p.rotSpeed[a] * dt);
                    if (p.rot[a] == p.rotTarget[a]) p.turning[a] = false;
                }
                if (p.spinAccel[a] > 0 && p.spin[a] != p.spinTarget[a])
                    p.spin[a] = towards(p.spin[a], p.spinTarget[a], p.spinAccel[a] * dt);
                p.rot[a] += p.spin[a] * dt;
                if (p.moving[a] || p.turning[a] || p.spin[a] != 0 ||
                    (p.spinAccel[a] > 0 && p.spin[a] != p.spinTarget[a]))
                    still = true;
            }
        }
        anyMotion_ = still;
    }

    ticking_ = true;
    for (size_t i = 0; i < threads_.size(); ++i)
        if (!threads_[i].dead) run(threads_[i]);
    ticking_ = false;
    for (auto& t : pending_) threads_.push_back(std::move(t));
    pending_.clear();
    std::erase_if(threads_, [](const Thread& t) { return t.dead; });
}

void Vm::run(Thread& t) {
    if (now_ < t.sleepUntil) return;
    if (t.waitPiece >= 0) {
        const auto& p = pieces_[size_t(t.waitPiece)];
        bool busy = t.waitTurn ? p.turning[t.waitAxis] : p.moving[t.waitAxis];
        if (busy) return;
        t.waitPiece = -1;
    }

    const auto& code = file_->code;
    for (int guard = 0; guard < 5000; ++guard) {
        if (t.pc >= code.size()) { t.dead = true; return; }
        uint32_t op = code[t.pc];
        auto arg = [&](int i) { return int32_t(code[t.pc + 1 + size_t(i)]); };

        switch (op) {
            case 0x10021001: push(t, arg(0)); t.pc += 2; break;               // PUSH_CONST
            case 0x10021002: {                                                // PUSH_LOCAL
                size_t i = size_t(arg(0));
                push(t, i < t.locals.size() ? t.locals[i] : 0);
                t.pc += 2; break;
            }
            case 0x10021004: {                                                // PUSH_STATIC
                size_t i = size_t(arg(0));
                push(t, i < statics_.size() ? statics_[i] : 0);
                t.pc += 2; break;
            }
            case 0x10022000: t.locals.push_back(0); t.pc += 1; break;         // CREATE_LOCAL
            case 0x10023002: {                                                // POP_LOCAL
                size_t i = size_t(arg(0));
                if (i >= t.locals.size()) t.locals.resize(i + 1, 0);
                t.locals[i] = pop(t);
                t.pc += 2; break;
            }
            case 0x10023004: {                                                // POP_STATIC
                size_t i = size_t(arg(0));
                if (i >= statics_.size()) statics_.resize(i + 1, 0);
                statics_[i] = pop(t);
                t.pc += 2; break;
            }
            case 0x10024000: pop(t); t.pc += 1; break;                        // POP_STACK

            case 0x10031000: { auto b = pop(t), a = pop(t); push(t, a + b); t.pc += 1; break; }
            case 0x10032000: { auto b = pop(t), a = pop(t); push(t, a - b); t.pc += 1; break; }
            case 0x10033000: { auto b = pop(t), a = pop(t); push(t, a * b); t.pc += 1; break; }
            case 0x10034000: { auto b = pop(t), a = pop(t); push(t, b ? a / b : 0); t.pc += 1; break; }
            // TAK's opcode numbering here differs from "classic TA" -- verified
            // against the retail COB interpreter (KINGDOMS.icd, dispatch @0x56c966):
            // sub-ops 5..B are AND OR XOR NOT(unary) SHL SHR(arith) MOD. We had these
            // wrong, and OR/XOR/NOT were absent -- hitting a missing one killed the
            // thread, which is why buildings (whose scripts OR-pack emit-sfx codes)
            // never animated. There is no 0x1003C000.
            case 0x10035000: { auto b = pop(t), a = pop(t); push(t, a & b); t.pc += 1; break; }   // AND
            case 0x10036000: { auto b = pop(t), a = pop(t); push(t, a | b); t.pc += 1; break; }   // OR
            case 0x10037000: case 0x10059000:                                                     // XOR
                { auto b = pop(t), a = pop(t); push(t, a ^ b); t.pc += 1; break; }
            case 0x10038000: push(t, ~pop(t)); t.pc += 1; break;                                  // NOT (unary)
            case 0x10039000: { auto b = pop(t), a = pop(t); push(t, a << b); t.pc += 1; break; }  // SHL
            case 0x1003A000: { auto b = pop(t), a = pop(t); push(t, a >> b); t.pc += 1; break; }  // SHR (arithmetic)
            case 0x1003B000: { auto b = pop(t), a = pop(t); push(t, b ? a % b : 0); t.pc += 1; break; }  // MOD

            case 0x10041000: {                                                // RAND
                int32_t hi = pop(t), lo = pop(t);
                if (hi < lo) std::swap(hi, lo);
                push(t, lo + int32_t(nextRand() % uint32_t(hi - lo + 1)));
                t.pc += 1; break;
            }
            case 0x10042000: {                                                // GET_UNIT_VALUE
                int32_t valId = pop(t);
                push(t, onGet ? onGet(valId, {}) : 0);
                t.pc += 1; break;
            }
            case 0x10043000: {                                                // GET (valId + 4)
                int32_t p4 = pop(t), p3 = pop(t), p2 = pop(t), p1 = pop(t);
                int32_t valId = pop(t);
                push(t, onGet ? onGet(valId, {p1, p2, p3, p4}) : 0);
                t.pc += 1; break;
            }
            case 0x10044000: {                                                // engine query (pop1/push1)
                int32_t id = pop(t);
                push(t, onGet ? onGet(id, {}) : 0);
                t.pc += 1; break;
            }
            case 0x10045000: push(t, 0); t.pc += 1; break;                    // engine query (pop0/push1)

            case 0x10051000: { auto b = pop(t), a = pop(t); push(t, a < b); t.pc += 1; break; }
            case 0x10052000: { auto b = pop(t), a = pop(t); push(t, a <= b); t.pc += 1; break; }
            case 0x10053000: { auto b = pop(t), a = pop(t); push(t, a > b); t.pc += 1; break; }
            case 0x10054000: { auto b = pop(t), a = pop(t); push(t, a >= b); t.pc += 1; break; }
            case 0x10055000: { auto b = pop(t), a = pop(t); push(t, a == b); t.pc += 1; break; }
            case 0x10056000: { auto b = pop(t), a = pop(t); push(t, a != b); t.pc += 1; break; }
            case 0x10057000: { auto b = pop(t), a = pop(t); push(t, a && b); t.pc += 1; break; }
            case 0x10058000: { auto b = pop(t), a = pop(t); push(t, a || b); t.pc += 1; break; }
            case 0x1005A000: push(t, !pop(t)); t.pc += 1; break;

            case 0x10061000: {                                                // START_SCRIPT
                int32_t script = arg(0), nparams = arg(1);
                std::vector<int32_t> params(size_t(std::max(nparams, 0)));
                for (int i = nparams - 1; i >= 0; --i) params[size_t(i)] = pop(t);
                t.pc += 3;
                if (script >= 0 && size_t(script) < file_->scripts.size()) {
                    Thread nt;
                    nt.pc = file_->scripts[size_t(script)].entry;
                    nt.locals = params;
                    nt.locals.resize(std::max<size_t>(params.size(), 16), 0);
                    nt.signalMask = t.signalMask;
                    // Defer during tick: pushing to threads_ would invalidate
                    // the reference we are executing from.
                    if (ticking_) pending_.push_back(std::move(nt));
                    else threads_.push_back(std::move(nt));
                    // Retail continues the parent immediately (pc already advanced
                    // by 3), rather than yielding -- so a script that starts several
                    // sub-scripts kicks them all off in the same tick.
                    break;
                }
                break;
            }
            case 0x10062000: {                                                // CALL_SCRIPT
                int32_t script = arg(0), nparams = arg(1);
                for (int i = 0; i < nparams; ++i) pop(t);
                t.callStack.push_back(t.pc + 3);
                if (script >= 0 && size_t(script) < file_->scripts.size())
                    t.pc = file_->scripts[size_t(script)].entry;
                else { t.dead = true; return; }
                break;
            }
            case 0x10063000: {                                                // start/call variant (absent from TAK data; stack-safe)
                int32_t nparams = arg(1);
                for (int i = 0; i < nparams; ++i) pop(t);
                t.pc += 3; break;
            }
            case 0x10064000: t.pc = uint32_t(arg(0)); break;                  // JUMP
            case 0x10065000: {                                                // RETURN
                int32_t rv = pop(t);
                if (t.callStack.empty()) { lastReturn_ = rv; t.dead = true; return; }
                t.pc = t.callStack.back();
                t.callStack.pop_back();
                break;
            }
            case 0x10066000:                                                  // JUMP_IF_FALSE
                if (pop(t) == 0) t.pc = uint32_t(arg(0));
                else t.pc += 2;
                break;
            case 0x10067000: {                                                // SIGNAL
                uint32_t mask = uint32_t(pop(t));
                t.pc += 1;
                for (auto& other : threads_)
                    if (&other != &t && (other.signalMask & mask)) other.dead = true;
                break;
            }
            case 0x10068000: t.signalMask = uint32_t(pop(t)); t.pc += 1; break;

            case 0x10001000: case 0x1000B000: {                               // MOVE[_NOW]
                int piece = arg(0), axis = arg(1);
                bool now = op == 0x1000B000;
                // Push order is [speed, target]: target is on top.
                float target = float(pop(t)) * kLinear;
                float speed = now ? 0 : float(pop(t)) * kLinear;
                if (piece >= 0 && size_t(piece) < pieces_.size() && axis >= 0 && axis < 3) {
                    auto& p = pieces_[size_t(piece)];
                    if (now || speed <= 0) { p.move[axis] = target; p.moving[axis] = false; }
                    else {
                        p.moveTarget[axis] = target;
                        p.moveSpeed[axis] = speed;
                        p.moving[axis] = true;
                        anyMotion_ = true;
                    }
                }
                t.pc += 3; break;
            }
            case 0x10002000: case 0x1000C000: {                               // TURN[_NOW]
                int piece = arg(0), axis = arg(1);
                bool now = op == 0x1000C000;
                // Push order is [speed, target]: target is on top.
                float target = float(pop(t)) * kAngle;
                float speed = now ? 0 : float(pop(t)) * kAngle;
                if (piece >= 0 && size_t(piece) < pieces_.size() && axis >= 0 && axis < 3) {
                    auto& p = pieces_[size_t(piece)];
                    if (now || speed <= 0) { p.rot[axis] = target; p.turning[axis] = false; }
                    else {
                        p.rotTarget[axis] = target;
                        p.rotSpeed[axis] = std::abs(speed);
                        p.turning[axis] = true;
                        anyMotion_ = true;
                    }
                }
                t.pc += 3; break;
            }
            case 0x10003000: {                                                // SPIN
                // Retail pops speed (target rate) first, then acceleration; it
                // ramps the rate up at that accel, or jumps instantly if accel==0.
                int piece = arg(0), axis = arg(1);
                float target = float(pop(t)) * kAngle;   // target rate (rad/sec)
                float accel = float(pop(t)) * kAngle;    // ramp (rad/sec^2)
                if (piece >= 0 && size_t(piece) < pieces_.size() && axis >= 0 && axis < 3) {
                    auto& p = pieces_[size_t(piece)];
                    p.spinTarget[axis] = target;
                    p.spinAccel[axis] = std::abs(accel);
                    if (accel == 0) p.spin[axis] = target;
                    anyMotion_ = true;
                }
                t.pc += 3; break;
            }
            case 0x10004000: {                                                // STOP_SPIN
                // Decelerate the rate toward 0 at the given decel (0 = stop dead).
                int piece = arg(0), axis = arg(1);
                float decel = float(pop(t)) * kAngle;
                if (piece >= 0 && size_t(piece) < pieces_.size() && axis >= 0 && axis < 3) {
                    auto& p = pieces_[size_t(piece)];
                    p.spinTarget[axis] = 0;
                    p.spinAccel[axis] = std::abs(decel);
                    if (decel == 0) p.spin[axis] = 0;
                    anyMotion_ = true;
                }
                t.pc += 3; break;
            }
            case 0x10005000:                                                  // SHOW
                if (size_t(arg(0)) < pieces_.size()) pieces_[size_t(arg(0))].visible = true;
                t.pc += 2; break;
            case 0x10006000:                                                  // HIDE
                if (size_t(arg(0)) < pieces_.size()) pieces_[size_t(arg(0))].visible = false;
                t.pc += 2; break;

            case 0x10007000: case 0x10008000: case 0x10009000: case 0x1000A000:
            case 0x1000D000: case 0x1000E000:
                if (size_t(arg(0)) < pieces_.size()) {
                    auto& piece=pieces_[size_t(arg(0))];
                    if(op==0x10007000 || op==0x10008000)piece.cached=op==0x10007000;
                    if(op==0x10009000 || op==0x1000A000)piece.rendered=op==0x10009000;
                    if(op==0x1000D000 || op==0x1000E000)piece.shaded=op==0x1000D000;
                }
                t.pc += 2; break;
            case 0x10075000:
                t.pc += 2; break;
            case 0x1000F000: {                                                // EMIT_SFX
                int piece = arg(0);
                int32_t sfx = pop(t);           // packed sfx-type code
                if (onEmitSfx) onEmitSfx(piece, sfx);
                t.pc += 2; break;
            }
            case 0x10071000: {                                                // EXPLODE
                // Piece debris (retail icd 0x50dd20): the piece flies off with a
                // random arc unless flag 0x20. Hide only after the host accepts
                // creation, before executing any subsequent visibility command.
                int piece = arg(0);
                int32_t flags = pop(t);
                if (!onExplode || onExplode(piece, flags))
                    hideExplodedPiece(piece,flags);
                t.pc += 2; break;
            }
            case 0x10072000: {                                                // PLAY_SOUND
                // Preserve the authored sound class/flags for the audio host.
                const int32_t flags=pop(t);
                if (onPlaySound) onPlaySound(arg(0),flags);
                push(t, 0);
                t.pc += 2;
                break;
            }
            case 0x10074000: t.pc += 3; break;                                // engine cmd (3-word, stack-neutral)
            case 0x10073000: {                                                // MAP_COMMAND
                int sub = arg(0), argc = arg(1);
                std::vector<int32_t> params(size_t(std::max(argc, 0)));
                for (int i = argc - 1; i >= 0; --i) params[size_t(i)] = pop(t);
                push(t, onMapCommand ? onMapCommand(sub, params) : 0);
                t.pc += 3; break;
            }
            case 0x10082000: {                                                // SET_UNIT_VALUE
                int32_t value = pop(t);
                int32_t valId = pop(t);
                if (onSetUnitValue) onSetUnitValue(valId, value);
                t.pc += 1; break;
            }
            case 0x10083000: pop(t); pop(t); pop(t); t.pc += 1; break;        // ATTACH
            case 0x10084000: pop(t); t.pc += 1; break;                        // DROP

            case 0x10011000: case 0x10012000: {                               // WAIT_TURN/MOVE
                t.waitPiece = arg(0);
                t.waitAxis = arg(1);
                t.waitTurn = op == 0x10011000;
                t.pc += 3;
                return;
            }
            case 0x10013000: {                                                // SLEEP
                int32_t ms = pop(t);
                t.sleepUntil = now_ + float(ms) / 1000.0f;
                t.pc += 1;
                return;
            }
            default:
                // Unknown opcode: stop this thread rather than corrupt state.
                t.dead = true;
                return;
        }
    }
}

} // namespace tak::cob
