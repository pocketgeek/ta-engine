// mapfilter_test -- the CHANGE MAP panel's map list FILTER (client/mapfilter.h): a map stays
// listed when its title or name contains the text, ignoring case; empty lists everything.

#include "client/mapfilter.h"

#include <cstdio>

namespace mf = tak::mapfilter;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

int main() {
    std::printf("mapfilter_test\n");
    const std::string ul = mf::lower("ULA"), arena = mf::lower("arena");
    check(mf::matches("Ulasem Arena", "ulasem arena", ""), "an empty filter lists every map");
    check(mf::matches("Ulasem Arena", "x", ul), "upper-case text finds a mixed-case title");
    check(mf::matches("Ulasem Arena", "x", mf::lower("ulasem a")), "lower-case text, spaces included");
    check(mf::matches("Ulasem Arena", "x", arena), "a match in the middle of the title");
    check(mf::matches("", "Ulasem Arena", arena), "the name is searched when there is no title");
    check(!mf::matches("Two Sins", "two sins", ul), "a map without the text is left out");
    check(mf::lower("VeRuNa 2") == "veruna 2", "lower() folds only letters");
    std::printf(g_fail ? "FAILED (%d)\n" : "PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
