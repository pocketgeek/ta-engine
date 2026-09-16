#pragma once

#include "cob/cob.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <vector>

namespace ta::cob {

// Minimal COB interpreter, sufficient to run unit animation scripts.
// Unit-state queries return 0 and side effects outside piece animation
// (sounds, explosions, attach) are no-ops.

struct PieceState {
    float move[3] = {0, 0, 0};      // world units
    float rot[3] = {0, 0, 0};       // radians
    float spin[3] = {0, 0, 0};      // current rate, radians/sec
    bool visible = true;

    // Spin ramp: SPIN accelerates the rate toward spinTarget, STOP_SPIN
    // decelerates it toward 0, both at spinAccel rad/sec^2 (0 = jump instantly).
    float spinTarget[3] = {0, 0, 0}, spinAccel[3] = {0, 0, 0};

    // In-flight animations (target + speed per axis).
    float moveTarget[3] = {0, 0, 0}, moveSpeed[3] = {0, 0, 0};
    bool moving[3] = {false, false, false};
    float rotTarget[3] = {0, 0, 0}, rotSpeed[3] = {0, 0, 0};
    bool turning[3] = {false, false, false};
};

class Vm {
public:
    // The bytecode is immutable and shared: every unit of a type references ONE
    // parsed File (some are 60-140KB of code words) instead of owning a copy.
    //
    // `deterministicRand`: the RAND opcode's RNG. false (default) = a tiny 8-byte
    // PRNG -- used by the client's per-unit animation VMs, whose RAND is purely
    // cosmetic (never hashed), which is what keeps a Vm from carrying a 5 KB
    // std::mt19937 x tens of thousands of units. true = the exact retail mt19937
    // sequence, for a VM whose RAND CAN feed hashed sim state (the mission
    // god-script runner) so lockstep stays byte-identical.
    explicit Vm(std::shared_ptr<const File> file, bool deterministicRand = false);
    explicit Vm(File file, bool deterministicRand = false)
        : Vm(std::make_shared<const File>(std::move(file)), deterministicRand) {}

    // Start a script by name with integer args; returns false if unknown.
    bool start(const std::string& script, const std::vector<int32_t>& args = {});

    // Run a script SYNCHRONOUSLY to completion and return its top-level RETURN
    // value (0 if the script is absent or yields without returning). This is for
    // the engine "query" call-ins -- QueryWeapon / SweetSpot -- which are
    // straight-line scripts that just compute and return a model piece index.
    // After the call, lastLocals() holds the script's final locals, for call-ins
    // that write an out-param local instead of returning a value. Client-side
    // only (the animation VM is never hashed), so this cannot affect lockstep.
    int32_t call(const std::string& script, const std::vector<int32_t>& args = {});
    const std::vector<int32_t>& lastLocals() const { return lastLocals_; }

    // Engine hooks (mission scripting). Defaults: return 0 / ignore.
    std::function<int32_t(int sub, const std::vector<int32_t>&)> onMapCommand;
    std::function<int32_t(int32_t valId, const std::vector<int32_t>&)> onGet;
    std::function<void(int32_t valId, int32_t value)> onSetUnitValue;
    // emit-sfx (0x1000F000): spawn a visual effect (fire/smoke/...) at `piece`.
    // sfxType is the packed COB code (e.g. 256|6 = large flame, 256|1 = smoke).
    std::function<void(int piece, int32_t sfxType)> onEmitSfx;
    // play-sound (0x10072000): play the wav named by COB name-table index `nameIdx`
    // at the unit. Like onEmitSfx, the hook only STASHES on the (worker) VM thread;
    // the host drains it on the main thread.
    std::function<void(int32_t nameIdx)> onPlaySound;
    // explode (0x10071000): piece flies off as debris (flags = COB explode type;
    // bit 0x20 = no debris entity, high bits add one-shot effects -- icd 0x50dd20).
    // The VM hides the piece when debris spawns; the hook only STASHES (worker
    // thread), the host drains on the main thread like onEmitSfx.
    std::function<void(int piece, int32_t flags)> onExplode;
    void setStatic(size_t i, int32_t v);
    void reset() { threads_.clear(); }   // stop all threads, keep piece poses

    // Advance time by dt seconds: run threads, progress animations.
    void tick(float dt);

    const std::vector<PieceState>& pieces() const { return pieces_; }
    const File& file() const { return *file_; }
    size_t threadCount() const { return threads_.size(); }
    std::vector<uint32_t> threadPcs() const {
        std::vector<uint32_t> out;
        for (const auto& t : threads_) out.push_back(t.pc);
        return out;
    }
    int32_t getStatic(size_t i) const { return i < statics_.size() ? statics_[i] : 0; }

private:
    struct Thread {
        uint32_t pc = 0;
        std::vector<int32_t> stack;
        std::vector<int32_t> locals;
        float sleepUntil = 0;
        int waitPiece = -1, waitAxis = 0;
        bool waitTurn = false;
        uint32_t signalMask = 0;
        bool dead = false;
        std::vector<uint32_t> callStack;
    };

    void run(Thread& t);
    int32_t pop(Thread& t);
    void push(Thread& t, int32_t v);

    std::shared_ptr<const File> file_;
    std::vector<int32_t> statics_;
    int32_t lastReturn_ = 0;              // top-level RETURN value of the last call()
    std::vector<int32_t> lastLocals_;     // final locals of the last call() (out-params)
    std::vector<PieceState> pieces_;
    std::vector<Thread> threads_;
    std::vector<Thread> pending_;
    bool ticking_ = false;
    bool anyMotion_ = false;   // any piece has a live move/turn/spin (gates the sweep)
    float now_ = 0;
    // RAND state. `detRng_` (a lazily-built mt19937 seeded 12345) is only used when
    // deterministicRand was requested -- so an ordinary animation VM carries just the
    // 8-byte xorshift `rng_` and a null pointer, not a 5 KB Mersenne Twister.
    uint64_t rng_ = 12345;
    bool detRand_ = false;
    std::unique_ptr<std::mt19937> detRng_;
    uint32_t nextRand() {
        if (detRand_) {
            if (!detRng_) detRng_ = std::make_unique<std::mt19937>(12345);
            return uint32_t((*detRng_)());
        }
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17;   // xorshift64
        return uint32_t(rng_);
    }
};

} // namespace ta::cob
