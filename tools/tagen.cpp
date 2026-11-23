// tagen <install-dir> <world 0-5> <out.png> [seed] [cells]: build a TA random map
// (tnt/mapgen_ta.cpp) and render it through the terrain compositor; prints the
// map id the game accepts as a map name.
#include "hpi/hpi.h"
#include "terrain/terrain.h"
#include "tnt/mapgen.h"
#include "util/png.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: tagen <install-dir> <world 0-5> <out.png> [seed] [cells]\n"); return 2; }
    auto vfs = ta::hpi::mountRetailRoot(argv[1]);
    ta::mapgen::Params p;
    p.mapType = uint8_t(std::atoi(argv[2]));
    p.seed = argc > 4 ? uint64_t(std::atoll(argv[4])) : 1;
    const int cells = argc > 5 ? std::atoi(argv[5]) : 256;
    p.widthCells = p.heightCells = uint16_t(cells);
    p.players = 4;
    const auto r = ta::mapgen::generate(p, vfs);
    std::printf("map %dx%d cells, %d tiles, %zu ore feature types, %zu starts\nid %s\n", r.map.width, r.map.height,
                r.map.tileCount, r.map.featureNames.size(), r.starts.size(), ta::mapgen::encodeMapId(p).c_str());
    ta::terrain::Compositor comp(vfs);
    auto img = comp.renderMap(r.map);
    ta::png::write(argv[3], img.width, img.height, img.rgba);
    return 0;
}
