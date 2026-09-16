// gaftool — inspect and export TAK GAF/TAF sprite banks.
//
//   gaftool list <file.gaf|taf> <palette.pal>
//   gaftool export <file.gaf|taf> <palette.pal> <outdir> [entry-name]

#include "gaf/gaf.h"
#include "util/png.h"

#include <iostream>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: gaftool list <file.gaf> <palette.pal>\n"
                     "       gaftool export <file.gaf> <palette.pal> <outdir> [entry]\n";
        return 2;
    }
    std::string cmd = argv[1];
    try {
        auto pal = ta::gaf::Palette::load(argv[3]);
        auto seqs = ta::gaf::load(argv[2], pal);

        if (cmd == "list") {
            for (const auto& s : seqs) {
                std::cout << s.name << ": " << s.frames.size() << " frame(s)";
                if (!s.frames.empty())
                    std::cout << ", " << s.frames[0].width << "x" << s.frames[0].height
                              << " anchor (" << s.frames[0].xoff << "," << s.frames[0].yoff << ")";
                std::cout << "\n";
            }
        } else if (cmd == "export" && argc >= 5) {
            fs::path outDir = argv[4];
            std::string only = argc >= 6 ? argv[5] : "";
            fs::create_directories(outDir);
            size_t count = 0;
            for (const auto& s : seqs) {
                if (!only.empty() && s.name != only) continue;
                for (size_t f = 0; f < s.frames.size(); ++f) {
                    const auto& fr = s.frames[f];
                    if (fr.width == 0 || fr.height == 0) continue;
                    auto out = outDir / (s.name + "_" + std::to_string(f) + ".png");
                    ta::png::write(out, fr.width, fr.height, fr.rgba);
                    ++count;
                }
            }
            std::cout << "wrote " << count << " PNGs to " << outDir.string() << "\n";
        } else {
            std::cerr << "unknown command\n";
            return 2;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
