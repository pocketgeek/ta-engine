#pragma once

// A test seam at the VFS read boundary. A ReadGate pushes a layer on top of a Vfs that
// holds no files but is asked first on every lookup (the Vfs tries its top layer first,
// and a MountSet runs its keep filter before it searches), so it sees every read that
// reaches the Vfs, through any view whose base it is. Armed, it holds each read made on
// another thread than the owner (the main thread): releaseLatest() lets the reader held
// last go on (its later reads pass), release() lets every reader go. The layer reports
// when the Vfs drops it -- a remount replacing the data set, or the Vfs going away --
// with how many reads it was still holding then. Nothing mounts one in a game: the debug
// probe TAK_PREVIEW_LIFETIME_TEST (gameview_devharness.cpp) and
// tools/bigmap_lifetime_test.cpp hold preview workers at a read with it.

#include "hpi/hpi.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tak {

class ReadGate {
public:
    // The layer's state when the Vfs dropped it: reads still held in it (let go but not
    // yet out of it included), whether every reader had been let go, and the reads that
    // went through it after their release.
    struct Gone { size_t held = 0; bool released = false; size_t lateReads = 0; };

    explicit ReadGate(hpi::Vfs& vfs, std::thread::id owner = std::this_thread::get_id())
        : s_(std::make_shared<State>()) {
        s_->owner = owner;
        hpi::MountConfig cfg;
        cfg.includeLoose = false;
        cfg.archiveExts.clear();
        // The layer alone owns `drop`: it ends when the Vfs drops the layer.
        cfg.keep = [s = s_, drop = std::make_shared<Drop>(s_)](const std::string& path) {
            State& st = *s;   // the gate owns the state too: valid even if the layer goes
            const auto self = std::this_thread::get_id();
            std::unique_lock lk(st.m);
            if (!st.armed || self == st.owner) return false;
            if (!st.released && !st.let.count(self)) {
                const size_t ticket = st.next++;
                st.held.push_back({ticket, self, path});
                st.cv.notify_all();
                st.cv.wait(lk, [&] { return st.released || st.let.count(self); });
                std::erase_if(st.held, [&](const Held& h) { return h.ticket == ticket; });
            }
            ++st.lateReads;
            return false;   // the layer has no files: the read goes on to the real ones
        };
        vfs.addLayer(hpi::MountSet({}, std::move(cfg)));
    }
    ~ReadGate() { release(); }   // never leave a reader waiting
    ReadGate(const ReadGate&) = delete;
    ReadGate& operator=(const ReadGate&) = delete;

    void arm() { std::lock_guard lk(s_->m); s_->armed = true; }
    void release() {
        { std::lock_guard lk(s_->m); s_->released = true; }
        s_->cv.notify_all();
    }
    // Let the reader held last go on; false when none is waiting.
    bool releaseLatest() {
        {
            std::lock_guard lk(s_->m);
            const Held* latest = nullptr;
            for (const Held& h : s_->held)
                if (!s_->let.count(h.thread) && (!latest || h.ticket > latest->ticket)) latest = &h;
            if (!latest || s_->released) return false;
            s_->let.insert(latest->thread);
        }
        s_->cv.notify_all();
        return true;
    }
    // Block until at least `n` reads are held, waiting, at once; false on timeout.
    bool waitHeld(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lk(s_->m);
        return s_->cv.wait_for(lk, timeout, [&] { return waitingLocked() >= n; });
    }
    size_t held() const { std::lock_guard lk(s_->m); return waitingLocked(); }
    bool gone() const { std::lock_guard lk(s_->m); return s_->gone; }
    Gone goneState() const { std::lock_guard lk(s_->m); return s_->at; }
    // Called on the thread that drops the layer, as it does.
    void onGone(std::function<void(const Gone&)> report) {
        std::lock_guard lk(s_->m);
        s_->onGone = std::move(report);
    }

private:
    struct Held { size_t ticket; std::thread::id thread; std::string path; };
    size_t waitingLocked() const {
        if (s_->released) return 0;
        return size_t(std::count_if(s_->held.begin(), s_->held.end(),
                                    [&](const Held& h) { return !s_->let.count(h.thread); }));
    }
    struct State {
        mutable std::mutex m;
        std::condition_variable cv;
        std::thread::id owner;
        bool armed = false, released = false, gone = false;
        size_t next = 0, lateReads = 0;
        std::vector<Held> held;
        std::set<std::thread::id> let;   // readers let go one by one: their reads pass
        Gone at;
        std::function<void(const Gone&)> onGone;
    };
    struct Drop {
        std::shared_ptr<State> s;
        explicit Drop(std::shared_ptr<State> state) : s(std::move(state)) {}
        ~Drop() {
            std::function<void(const Gone&)> report;
            Gone at;
            {
                std::lock_guard lk(s->m);
                s->gone = true;
                at = s->at = {s->held.size(), s->released, s->lateReads};
                report = s->onGone;
            }
            s->cv.notify_all();
            if (report) report(at);
        }
    };
    std::shared_ptr<State> s_;
};

}  // namespace tak
