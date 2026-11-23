// Battle-status music logic (client/battlemusic.h) against the retail thresholds in
// docs/retail-music.md: entry needs > 50 points in 30 s (or > 30 in the last 5 s) AND
// more than 30 own units, nothing happens before the 11th sample, exit needs a quiet
// window and > 60 samples, a change fades 18x2 ticks and Building waits 120 more.
#include "client/battlemusic.h"

#include <cstdio>
#include <cstdlib>

using namespace ta::battlemusic;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static Category catOf(int l) { return defaultCategories()[size_t(l)]; }

int main() {
    // Default table: 1..7 battle, 8..16 building.
    for (int l = 1; l <= 16; ++l) CHECK(catOf(l) == (l <= 7 ? Category::Battle : Category::Building));

    BattleMusic m;
    uint32_t t = 1000;
    Action a = m.startGame(t);
    CHECK(a.kind == Action::Kind::Play && catOf(a.track) == Category::Building);
    CHECK(m.requested() == Category::Building);

    // Heavy combat but only 30 units: no switch (needs > 30).
    auto second = [&](int units, int damage) {
        for (int i = 0; i < damage; ++i) m.onDamage();
        t += 31;
        return m.tick(t, units, false);
    };
    for (int s = 0; s < 20; ++s) second(30, 10);
    CHECK(m.requested() == Category::Building);

    // 31 units and the same combat: switches once the counter is past 10.
    Action sw{};
    for (int s = 0; s < 20 && m.requested() == Category::Building; ++s) sw = second(31, 10);
    CHECK(m.requested() == Category::Battle);
    CHECK(m.counter() == 0);
    // The fade runs 18 steps of 2 ticks, then a Battle track starts.
    bool started = false;
    for (int f = 0; f < 60 && !started; ++f) {
        t += 1;
        Action x = m.tick(t, 31, false);
        if (x.kind == Action::Kind::Play) { started = true; CHECK(catOf(x.track) == Category::Battle); }
        else CHECK(x.gain <= 1.0f && x.gain >= 0.0f);
    }
    CHECK(started);

    // Quiet: back to Building only after the window empties and counter > 60.
    int quietSeconds = 0;
    while (m.requested() == Category::Battle && quietSeconds < 200) { second(31, 0); ++quietSeconds; }
    CHECK(m.requested() == Category::Building);
    CHECK(quietSeconds >= 59);   // > 60 samples since the switch (one fell in the fade)
    // Building waits 36 fade ticks + 120 before the next track.
    int waited = 0;
    started = false;
    while (!started && waited < 400) {
        t += 1; ++waited;
        if (m.tick(t, 31, false).kind == Action::Kind::Play) started = true;
    }
    CHECK(started);
    CHECK(waited >= 18 * 2 + 120);

    // Track end picks another track of the same category.
    t += 1;
    a = m.tick(t, 31, true);
    CHECK(a.kind == Action::Kind::Play && catOf(a.track) == Category::Building);

    // Kills weigh 5: 11 kills in a burst satisfy "last 5 > 30" with enough units.
    BattleMusic k;
    t = 0;
    k.startGame(t);
    for (int s = 0; s < 11; ++s) { t += 31; k.tick(t, 40, false); }
    for (int i = 0; i < 7; ++i) k.onKill();
    t += 31; k.tick(t, 40, false);
    t += 31; k.tick(t, 40, false);
    CHECK(k.requested() == Category::Battle);

    // A new game resets the monitor (retail does not -- spec fix).
    k.startGame(t);
    CHECK(k.requested() == Category::Building && k.counter() == 0);

    // Non-Custom modes ignore battle status.
    BattleMusic p;
    p.setMode(Mode::PlayAll);
    t = 0;
    a = p.startGame(t);
    CHECK(a.kind == Action::Kind::Play && a.track == 1);
    a = p.tick(++t, 0, true);
    CHECK(a.track == 2);

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("battle_music_test: ok");
    return 0;
}
