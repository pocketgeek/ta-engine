#pragma once
// TAK_PACELOG=<1|path> -- a debug-only per-frame pacing record of the lockstep drain
// (src/client/dev.h; nothing of it exists in a release build, and nothing it does touches
// the sim or the wire). One record per rendered frame:
//   wall ms since the previous frame, ticks the sim finished in it (what the player sees: the
//   worker's simProcessedTick_ delta with the sim thread on, the drained count inline), the
//   sim worker's inbox depth, the jitter buffer's depth, the servo rate, and whether the
//   512-tick fast-forward fired.
// At exit it prints one summary line: ticks-per-frame histogram, zero-tick frames, frames at
// or over twice the nominal tick count (the median), the longest burst and where, p99 frame
// ms, inbox p99 and the fast-forward count. TAK_PACELOG=<path> also writes the records, one
// line each, to <path>. Report-only: tools/pace_check.sh runs it, no gate reads it.
//
// The presentation and worker columns (WE E0.1) follow the original seven on each record line:
//   shown (the tick of the snapshot the frame displays), td (the display time: shown - 1 plus the
//   interpolation fraction the render uses), render-thread simMutex_ wait in us, and the sim
//   worker's (or the inline drain's) time over the ticks it finished since the previous frame:
//   world ms (World::tick + transport-effect capture), capture ms (captureFrame), hash ms (the
//   reported state hash), and the wall and thread-CPU ms of those jobs (wall/CPU > 1.1 is
//   preemption). At exit a second line, PACEWORK, gives the per-tick means and the lock waits.
//
// The presentation columns (WE E3.2) follow: dtd (display time advanced this frame, ticks), nominal (the
// frame's wall time at the game speed, ticks), freeze (1 = the display time was held at the newest
// snapshot: nothing newer to show), gate (1 = the start gate holds the display), snap (1 = the display
// time jumped to stay inside the snapshot ring) and the lag target (ticks). A third exit line, PRESENT,
// gives what the WE synthesis gates on: the coefficient of variation of the displayed time per frame,
// the freeze share, and the frames that advanced more than 1.3x their nominal step (snaps excluded).
// cv is over the raw per-frame advance, so it also carries the frame clock's own jitter (a frame that
// misses a vsync shows twice the game time, which is correct motion); rate_cv is over dtd / nominal, the
// display clock's rate against the wall clock, which is what the presentation itself controls.
// The last column (WE E3.3) is the backlog: ticks delivered but not yet simulated (the jitter buffer
// plus, with the sim worker, the ticks pushed to it and not finished), sampled after the frame's
// drain. The PACE line gives its p99 and max: the command latency a client-bound game builds up.
// TAK_SIM_SLOW_US=N (debug) sleeps N us per simulated tick, to play a slow client.
// TAK_VSYNC_HZ=N paces the headless --mpai frame loop to an N Hz deadline (a fake vsync).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace tak {

class PaceLog {
public:
    struct Frame {
        double wallMs = 0;
        int ticks = 0, inbox = 0, buffered = 0;
        float rate = 0;
        bool fastForward = false;
        uint32_t tick = 0;   // sim tick reached at the end of the frame
        uint32_t shown = 0;  // tick of the displayed snapshot
        double td = 0;       // display time in ticks (shown - 1 + interpolation fraction)
        double lockWaitUs = 0;
        double worldMs = 0, captureMs = 0, hashMs = 0, jobWallMs = 0, jobCpuMs = 0;
        double dtd = 0, nominal = 0;   // display time advanced / the frame's nominal step, in ticks
        bool freeze = false, gate = false, snap = false;
        float lagTarget = 0;
        int backlog = 0;     // delivered, not yet simulated (jitter buffer + worker queue), after the drain
    };

    // "1" (or any value that is not a path): summary only. Anything with a '/' or '.' is a file.
    void open(const char* spec) {
        if (!spec || !*spec || (spec[0] == '0' && !spec[1])) return;
        on_ = true;
        const std::string s = spec;
        if (s.find('/') != std::string::npos || s.find('.') != std::string::npos) f_ = std::fopen(s.c_str(), "w");
    }
    bool on() const { return on_; }

    void frame(const Frame& fr) {
        if (!on_) return;
        frames_.push_back(fr);
        if (f_)
            std::fprintf(f_, "%.3f %d %u %d %d %.3f %d %u %.3f %.1f %.3f %.3f %.3f %.3f %.3f %.4f %.4f %d %d %d %.2f %d\n",
                         fr.wallMs, fr.ticks, fr.tick, fr.inbox, fr.buffered, double(fr.rate), fr.fastForward ? 1 : 0,
                         fr.shown, fr.td, fr.lockWaitUs, fr.worldMs, fr.captureMs, fr.hashMs, fr.jobWallMs, fr.jobCpuMs,
                         fr.dtd, fr.nominal, fr.freeze ? 1 : 0, fr.gate ? 1 : 0, fr.snap ? 1 : 0, double(fr.lagTarget), fr.backlog);
    }

    struct Summary {
        size_t frames = 0, zeroTick = 0, overTwice = 0, fastForward = 0;
        int nominal = 0, maxBurst = 0, inboxP99 = 0, backlogP99 = 0, backlogMax = 0;
        uint32_t burstTick = 0;
        double wallP99 = 0, wallMax = 0;
        std::map<int, size_t> histogram;
    };

    // Frames before `skip` are ignored (the load frames and the initial buffer fill).
    Summary summarize(size_t skip = 0) const {
        Summary s;
        std::vector<int> ticks, inbox, backlog;
        std::vector<double> wall;
        for (size_t i = skip; i < frames_.size(); ++i) {
            ticks.push_back(frames_[i].ticks);
            inbox.push_back(frames_[i].inbox);
            backlog.push_back(frames_[i].backlog);
            wall.push_back(frames_[i].wallMs);
        }
        s.frames = ticks.size();
        if (ticks.empty()) return s;
        auto sorted = ticks;
        std::sort(sorted.begin(), sorted.end());
        s.nominal = sorted[sorted.size() / 2];
        std::sort(inbox.begin(), inbox.end());
        s.inboxP99 = inbox[std::min(inbox.size() - 1, inbox.size() * 99 / 100)];
        std::sort(backlog.begin(), backlog.end());
        s.backlogP99 = backlog[std::min(backlog.size() - 1, backlog.size() * 99 / 100)];
        s.backlogMax = backlog.back();
        std::sort(wall.begin(), wall.end());
        s.wallP99 = wall[std::min(wall.size() - 1, wall.size() * 99 / 100)];
        s.wallMax = wall.back();
        for (size_t i = skip; i < frames_.size(); ++i) {
            const auto& fr = frames_[i];
            ++s.histogram[fr.ticks];
            if (fr.ticks == 0) ++s.zeroTick;
            if (s.nominal > 0 && fr.ticks >= 2 * s.nominal) ++s.overTwice;
            if (fr.fastForward) ++s.fastForward;
            if (fr.ticks > s.maxBurst) { s.maxBurst = fr.ticks; s.burstTick = fr.tick; }
        }
        return s;
    }

    static std::string format(const Summary& s) {
        std::string out = "PACE frames=" + std::to_string(s.frames) + " nominal=" + std::to_string(s.nominal) +
                          " zero_tick=" + std::to_string(s.zeroTick) + " over_2x=" + std::to_string(s.overTwice) +
                          " max_burst=" + std::to_string(s.maxBurst) + "@" + std::to_string(s.burstTick) +
                          " ff512=" + std::to_string(s.fastForward) + " inbox_p99=" + std::to_string(s.inboxP99) +
                          " backlog_p99=" + std::to_string(s.backlogP99) + " backlog_max=" + std::to_string(s.backlogMax);
        char b[64];
        std::snprintf(b, sizeof b, " wall_p99=%.2fms wall_max=%.2fms", s.wallP99, s.wallMax);
        out += b;
        out += " hist={";
        bool first = true;
        for (const auto& [k, n] : s.histogram) {
            out += (first ? "" : ", ") + std::to_string(k) + ":" + std::to_string(n);
            first = false;
        }
        return out + "}";
    }

    // Per-tick means of the worker columns and the render-thread lock waits, over frames from `skip`.
    std::string work(size_t skip = 0) const {
        double ticks = 0, world = 0, capture = 0, hash = 0, wall = 0, cpu = 0, waitMax = 0, waitSum = 0;
        size_t waited = 0, frames = 0;
        for (size_t i = skip; i < frames_.size(); ++i) {
            const auto& fr = frames_[i];
            ++frames;
            ticks += fr.ticks; world += fr.worldMs; capture += fr.captureMs; hash += fr.hashMs;
            wall += fr.jobWallMs; cpu += fr.jobCpuMs;
            waitSum += fr.lockWaitUs;
            waitMax = std::max(waitMax, fr.lockWaitUs);
            if (fr.lockWaitUs > 0) ++waited;
        }
        char b[320];
        const double t = std::max(1.0, ticks);
        std::snprintf(b, sizeof b,
                      "PACEWORK ticks=%.0f world_ms=%.3f capture_ms=%.3f hash_ms=%.3f job_wall_ms=%.3f wall_cpu=%.3f"
                      " lock_wait_frames=%zu/%zu lock_wait_us_sum=%.0f lock_wait_us_max=%.0f",
                      ticks, world / t, capture / t, hash / t, wall / t, cpu > 0 ? wall / cpu : 0.0, waited, frames,
                      waitSum, waitMax);
        return b;
    }

    // Presentation smoothness over frames from `skip`, gate frames and paused frames (nominal 0) left out:
    // dtd mean and CV, freeze frames, frames over 1.3x their nominal step (snaps excluded), snaps.
    struct Present {
        size_t frames = 0, freeze = 0, over13 = 0, snaps = 0;
        double dtdMean = 0, cv = 0, rateCv = 0, maxRatio = 0, lagTarget = 0;
    };
    Present present(size_t skip = 0) const {
        Present p;
        double sum = 0, sum2 = 0, lag = 0, rsum = 0, rsum2 = 0;
        size_t rated = 0;
        for (size_t i = skip; i < frames_.size(); ++i) {
            const auto& fr = frames_[i];
            if (fr.gate || fr.nominal <= 0) continue;
            ++p.frames;
            sum += fr.dtd; sum2 += fr.dtd * fr.dtd; lag += fr.lagTarget;
            if (fr.freeze) ++p.freeze;
            if (fr.snap) { ++p.snaps; continue; }
            const double ratio = fr.dtd / fr.nominal;
            ++rated; rsum += ratio; rsum2 += ratio * ratio;
            p.maxRatio = std::max(p.maxRatio, ratio);
            if (ratio > 1.3) ++p.over13;
        }
        if (!p.frames) return p;
        const double n = double(p.frames);
        p.dtdMean = sum / n;
        p.cv = p.dtdMean > 0 ? std::sqrt(std::max(0.0, sum2 / n - p.dtdMean * p.dtdMean)) / p.dtdMean : 0;
        p.lagTarget = lag / n;
        if (rated) {
            const double rm = rsum / double(rated);
            p.rateCv = rm > 0 ? std::sqrt(std::max(0.0, rsum2 / double(rated) - rm * rm)) / rm : 0;
        }
        return p;
    }
    static std::string format(const Present& p) {
        char b[256];
        std::snprintf(b, sizeof b,
                      "PRESENT frames=%zu dtd_mean=%.4f cv=%.2f%% rate_cv=%.2f%% freeze=%zu (%.2f%%) over_1.3x=%zu max_ratio=%.2f"
                      " snaps=%zu lag_target_mean=%.2f",
                      p.frames, p.dtdMean, 100.0 * p.cv, 100.0 * p.rateCv, p.freeze, p.frames ? 100.0 * double(p.freeze) / double(p.frames) : 0.0,
                      p.over13, p.maxRatio, p.snaps, p.lagTarget);
        return b;
    }

    ~PaceLog() {
        if (f_) std::fclose(f_);
        // The first second of frames is the initial buffer fill and the load: not pacing.
        if (on_ && !frames_.empty()) {
            size_t skip = 0;
            double t = 0;
            while (skip < frames_.size() && t < 1000.0) t += frames_[skip++].wallMs;
            std::fprintf(stderr, "%s\n", format(summarize(skip)).c_str());
            std::fprintf(stderr, "%s\n", work(skip).c_str());
            std::fprintf(stderr, "%s\n", format(present(skip)).c_str());
        }
    }

private:
    bool on_ = false;
    std::FILE* f_ = nullptr;
    std::vector<Frame> frames_;
};

}  // namespace tak
