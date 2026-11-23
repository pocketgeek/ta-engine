// biktool: decode a Bink (.bik) video through the engine's FFmpeg-backed
// BinkVideo and report its size/fps/frame count; optionally dump one frame as PNG.
//
//   biktool <file.bik>                 -> print dimensions, fps, frame count
//   biktool <file.bik> <n> <out.png>   -> also write frame n as a PNG
//
// A quick standalone check that the door videos decode, independent of the viewer.

#include "util/png.h"
#include "video/bink.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: biktool <file.bik> [frameIndex out.png]\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());

    tak::video::BinkVideo v;
    if (!v.open(std::move(data))) {
        std::fprintf(stderr, "open/decode failed (not a Bink stream, or no FFmpeg)\n");
        return 1;
    }

    const int want = argc >= 4 ? std::atoi(argv[2]) : -1;
    const char* out = argc >= 4 ? argv[3] : nullptr;

    std::vector<uint8_t> rgba;
    int n = 0;
    while (v.nextFrame(rgba)) {
        if (n == want && out) {
            tak::png::write(out, v.width(), v.height(), rgba);
            std::fprintf(stderr, "wrote frame %d -> %s\n", n, out);
        }
        ++n;
    }
    std::printf("bik: %dx%d  fps=%.2f  frames=%d\n", v.width(), v.height(), v.fps(), n);
    return 0;
}
