// makeicons -- render the app icons (src/util/appicon) to PNGs.
//   makeicons <outdir>
// Writes taclient.png, cartographer.png, taserver.png at 256px into <outdir>.

#include "util/appicon.h"
#include "util/png.h"

#include <cstdio>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    std::filesystem::create_directories(dir);
    struct { ta::appicon::Kind kind; const char* name; } apps[] = {
        {ta::appicon::Kind::Client, "taclient"},
        {ta::appicon::Kind::Cartographer, "cartographer"},
        {ta::appicon::Kind::Server, "taserver"},
    };
    const int sz = 256;
    for (auto& a : apps) {
        auto px = ta::appicon::render(a.kind, sz);
        std::string p = dir + "/" + a.name + ".png";
        ta::png::write(p, sz, sz, px);
        std::printf("wrote %s (%dx%d)\n", p.c_str(), sz, sz);
    }
    return 0;
}
