#include "client/streamkey.h"

#include "client/dev.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace tak {
namespace {

// Notices that the field changed since the last write without keeping another copy of
// the key around. A collision only skips one redundant write.
uint64_t keyHash(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h ^ s.size();
}

// Debug-only stand-ins (TAK_CREDSTORE): the greyed-out state, and a store that lives in
// this process only.
class Unavailable final : public credstore::Store {
public:
    std::string name() const override { return "PASSWORD STORE"; }
    std::string probe() override { return credstore::kNoStore; }
    credstore::Result load(std::string&, std::string& e) override { e = credstore::kNoStore; return credstore::Result::Failed; }
    bool store(const std::string&, std::string& e) override { e = credstore::kNoStore; return false; }
    bool erase(std::string& e) override { e = credstore::kNoStore; return false; }
};
class Memory final : public credstore::Store {
public:
    ~Memory() override { credstore::wipe(secret_); }
    std::string name() const override { return "MEMORY"; }
    std::string probe() override { return {}; }
    credstore::Result load(std::string& s, std::string&) override {
        if (secret_.empty()) return credstore::Result::Missing;
        s = secret_;
        return credstore::Result::Ok;
    }
    bool store(const std::string& s, std::string&) override { credstore::wipe(secret_); secret_ = s; return true; }
    bool erase(std::string&) override { credstore::wipe(secret_); return true; }
private:
    std::string secret_;
};

}  // namespace

struct StreamKeyMemory::Op {
    enum Kind { Probe, Store, Erase } kind = Probe;
    bool load = false;          // Probe: then load the remembered key
    std::string secret;         // Store
    uint64_t generation = 0;
};

struct StreamKeyMemory::Shared {
    struct Done {
        Op::Kind kind = Op::Probe;
        bool ok = true, loaded = false;
        credstore::Result result = credstore::Result::Missing;
        std::string reason, error, secret;
        uint64_t generation = 0;
    };
    std::shared_ptr<credstore::Store> store;
    std::mutex m;
    std::condition_variable wake, settled;
    std::deque<Op> ops;
    std::vector<Done> done;
    bool running = false, stop = false, exited = false;
    std::thread thread;
    // The worker owns a reference, so detaching it at shutdown is safe.
    static void run(std::shared_ptr<Shared> sh) {
        std::unique_lock lk(sh->m);
        for (;;) {
            sh->wake.wait(lk, [&] { return sh->stop || !sh->ops.empty(); });
            if (sh->ops.empty()) break;   // stopping, and every write is done
            Op op = std::move(sh->ops.front());
            sh->ops.pop_front();
            if (sh->stop && op.kind == Op::Probe) continue;   // nobody is waiting for it
            sh->running = true;
            lk.unlock();
            Done d;
            d.kind = op.kind;
            d.generation = op.generation;
            if (op.kind == Op::Probe) {
                d.reason = sh->store->probe();
                if (d.reason.empty() && op.load) { d.loaded = true; d.result = sh->store->load(d.secret, d.error); }
            } else if (op.kind == Op::Store) {
                d.ok = sh->store->store(op.secret, d.error);
            } else {
                d.ok = sh->store->erase(d.error);
            }
            credstore::wipe(op.secret);
            lk.lock();
            sh->running = false;
            sh->done.push_back(std::move(d));
            sh->settled.notify_all();
        }
        sh->exited = true;
        sh->settled.notify_all();
    }
};

StreamKeyMemory::StreamKeyMemory(std::shared_ptr<credstore::Store> store, bool remember)
    : sh_(std::make_shared<Shared>()), remember_(remember) {
    sh_->store = std::move(store);
}

StreamKeyMemory::~StreamKeyMemory() {
    using namespace std::chrono_literals;
    if (sh_->thread.joinable()) {
        std::unique_lock lk(sh_->m);
        sh_->stop = true;
        sh_->wake.notify_all();
        if (!sh_->settled.wait_for(lk, 3s, [&] { return sh_->exited; })) {
            lk.unlock();
            sh_->store->cancel();   // a call stuck on an unlock prompt
            lk.lock();
            sh_->settled.wait_for(lk, 1s, [&] { return sh_->exited; });
        }
        const bool exited = sh_->exited;
        lk.unlock();
        if (exited) sh_->thread.join(); else sh_->thread.detach();
    }
    std::lock_guard lk(sh_->m);
    for (auto& d : sh_->done) credstore::wipe(d.secret);
}

void StreamKeyMemory::enqueue(Op op) {
    std::lock_guard lk(sh_->m);
    sh_->ops.push_back(std::move(op));
    if (!sh_->thread.joinable()) sh_->thread = std::thread(&Shared::run, sh_);
    sh_->wake.notify_all();
}

void StreamKeyMemory::check() {
    if (state_ == State::Checking) return;
    if (state_ == State::Available && (loaded_ || !remember_)) return;
    state_ = State::Checking;
    Op op;
    op.load = remember_ && !loaded_;
    op.generation = generation_;
    enqueue(std::move(op));
}

void StreamKeyMemory::poll(std::string& key) {
    std::vector<Shared::Done> done;
    { std::lock_guard lk(sh_->m); done.swap(sh_->done); }
    for (auto& d : done) {
        if (d.kind == Op::Probe) {
            state_ = d.reason.empty() ? State::Available : State::Unavailable;
            reason_ = d.reason;
            // A failed load (locked, denied) is retried when the panel next opens; until
            // one succeeds nothing is written, so a stored key is never erased unseen.
            if (d.loaded && d.result == credstore::Result::Failed) error_ = d.error;
            else if (d.loaded) {
                loaded_ = true;
                // Not if the box changed meanwhile, or the player already entered a key.
                if (d.generation == generation_ && remember_ && key.empty()) {
                    // assign, not swap: the field keeps its reserved buffer (no unwiped copy
                    // left behind when typing would outgrow a swapped-in one).
                    if (d.result == credstore::Result::Ok) key.assign(d.secret);
                    syncedHash_ = keyHash(key);
                    syncedValid_ = true;
                }
            }
        } else if (!d.ok) {
            error_ = d.error;
            syncedValid_ = false;   // retry at the next write point
            // The untick's delete failed (a cancelled unlock prompt, access refused): the key
            // is still stored, so the box ticks itself again instead of claiming it is gone.
            if (d.kind == Op::Erase && !remember_ && d.generation == generation_) remember_ = true;
        }
        credstore::wipe(d.secret);
    }
}

bool StreamKeyMemory::setRemember(bool on, const std::string& key) {
    if (state_ != State::Available) return false;
    if (on == remember_) return true;
    remember_ = on;
    ++generation_;
    Op op;
    op.generation = generation_;
    if (on && !key.empty()) { op.kind = Op::Store; op.secret = key; }
    else op.kind = Op::Erase;
    enqueue(std::move(op));
    syncedHash_ = keyHash(key);
    syncedValid_ = on;
    loaded_ = true;   // a tick replaces whatever was stored; nothing left to load
    return true;
}

void StreamKeyMemory::sync(const std::string& key) {
    // Not before the startup load lands: an empty field then means "not loaded yet", and
    // erasing would lose the stored key. A key entered meanwhile is written at the next
    // write point (poll() leaves it unsynced).
    if (!remember_ || !loaded_ || state_ == State::Unavailable) return;
    const uint64_t h = keyHash(key);
    if (syncedValid_ && h == syncedHash_) return;
    Op op;
    op.generation = generation_;
    if (key.empty()) op.kind = Op::Erase;
    else { op.kind = Op::Store; op.secret = key; }
    enqueue(std::move(op));
    syncedHash_ = h;
    syncedValid_ = true;
}

std::string StreamKeyMemory::storeName() const { return sh_->store->name(); }

bool StreamKeyMemory::busy() const {
    std::lock_guard lk(sh_->m);
    return !sh_->ops.empty() || sh_->running || !sh_->done.empty();
}

std::string StreamKeyMemory::takeError() { std::string e; e.swap(error_); return e; }

bool StreamKeyMemory::waitIdle(int ms) {
    std::unique_lock lk(sh_->m);
    return sh_->settled.wait_for(lk, std::chrono::milliseconds(ms),
                                 [&] { return sh_->ops.empty() && !sh_->running; });
}

std::shared_ptr<credstore::Store> streamKeyStore() {
    if (const char* v = devEnv("TAK_CREDSTORE")) {
        if (!std::strcmp(v, "none")) return std::make_shared<Unavailable>();
        if (!std::strcmp(v, "memory")) return std::make_shared<Memory>();
        if (!std::strcmp(v, "test")) return credstore::system(credstore::testSlot());
    }
    return credstore::system(credstore::streamKeySlot());
}

}  // namespace tak
