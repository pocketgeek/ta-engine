// PaceLog (src/client/pacelog.h): the ticks-per-frame summary behind TAK_PACELOG / tools/pace_check.sh.
// A run shaped like the audited 8x one (12 ticks a frame, a few 11s and 13s, one 93-tick burst) must report the
// same histogram, the burst and its tick, and count the zero-tick and double-nominal frames.
#include "client/pacelog.h"

#include <cmath>
#include <cstdio>
#include <string>

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

int main() {
    tak::PaceLog log;   // not opened: records nothing
    log.frame({50, 12, 0, 5, 1, false, 12});
    check(log.summarize().frames == 0, "a log that was not opened records nothing");

    tak::PaceLog on;
    on.open("1");
    uint32_t tick = 0;
    auto frame = [&](int ticks, int inbox, bool ff = false) {
        tick += uint32_t(ticks);
        on.frame({50.0, ticks, inbox, 5, 1.0f, ff, tick});
    };
    for (int i = 0; i < 100; ++i) frame(12, 4);
    for (int i = 0; i < 10; ++i) frame(11, 4);
    for (int i = 0; i < 5; ++i) frame(13, 4);
    frame(0, 30);          // a stalled frame
    frame(93, 40, true);   // the burst that follows it, with the 512-tick fast-forward flag
    const auto s = on.summarize();
    check(s.frames == 117, "117 frames counted");
    check(s.nominal == 12, "the nominal tick count is the median (12)");
    check(s.zeroTick == 1 && s.overTwice == 1 && s.fastForward == 1, "one zero-tick frame, one at or over 2x, one fast-forward");
    check(s.maxBurst == 93 && s.burstTick == tick, "the longest burst is 93 ticks, at the tick it ended");
    check(s.histogram.at(12) == 100 && s.histogram.at(11) == 10 && s.histogram.at(13) == 5 && s.histogram.at(0) == 1 &&
          s.histogram.at(93) == 1, "the histogram has every bin");
    check(s.inboxP99 >= 30, "inbox p99 sees the deep frames");
    const std::string text = tak::PaceLog::format(s);
    check(text.find("max_burst=93@") != std::string::npos && text.find("zero_tick=1") != std::string::npos &&
          text.find("hist={0:1, 11:10, 12:100, 13:5, 93:1}") != std::string::npos, "the summary line carries them");
    check(on.summarize(10).frames == 107, "frames before the skip point are ignored");
    // PRESENT (WE E3.2): the displayed-time CV, freeze share and over-1.3x frames; gate and paused frames are
    // left out, a snap is counted but not judged as an over-step.
    tak::PaceLog pr;
    pr.open("1");
    auto present = [&](double dtd, double nominal, bool freeze = false, bool gate = false, bool snap = false) {
        tak::PaceLog::Frame fr;
        fr.wallMs = 4.17; fr.dtd = dtd; fr.nominal = nominal; fr.freeze = freeze; fr.gate = gate; fr.snap = snap;
        fr.lagTarget = 2;
        pr.frame(fr);
    };
    for (int i = 0; i < 5; ++i) present(0, 1, false, true);   // start gate: ignored
    for (int i = 0; i < 96; ++i) present(1.0, 1.0);
    present(2.0, 1.0);                       // one frame that jumped 2x
    present(0.0, 1.0, true);                 // one freeze
    present(6.0, 1.0, false, false, true);   // one snap
    present(0.0, 0.0);                       // paused: ignored
    const auto p = pr.present();
    check(p.frames == 99, "gate and paused frames are left out (99 judged)");
    check(p.freeze == 1 && p.over13 == 1 && p.snaps == 1, "one freeze, one over 1.3x, one snap");
    check(p.maxRatio == 2.0, "the snap is not the max ratio");
    check(p.cv > 0.4 && p.cv < 0.6, "the CV reflects the jump, freeze and snap");
    check(tak::PaceLog::format(p).find("freeze=1 (1.01%)") != std::string::npos, "the PRESENT line carries the freeze share");
    tak::PaceLog smooth;
    smooth.open("1");
    for (int i = 0; i < 100; ++i) { tak::PaceLog::Frame fr; fr.dtd = i % 2 ? 1.02 : 0.98; fr.nominal = 1; smooth.frame(fr); }
    check(std::abs(smooth.present().cv - 0.02) < 1e-6, "a +-2% alternation is a 2% CV");
    tak::PaceLog jitter;   // a frame clock that alternates 0.8 and 1.2 ticks, the display time following it exactly
    jitter.open("1");
    for (int i = 0; i < 100; ++i) { tak::PaceLog::Frame fr; fr.nominal = i % 2 ? 1.2 : 0.8; fr.dtd = fr.nominal; jitter.frame(fr); }
    check(jitter.present().cv > 0.19 && jitter.present().rateCv < 1e-6, "frame-clock jitter shows in cv, not in rate_cv");
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("pacelog_test: all passed\n");
    return 0;
}
