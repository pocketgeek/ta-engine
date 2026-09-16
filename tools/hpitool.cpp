// hpitool — command-line tool for TAK HPI archives.
//
//   hpitool inspect <archive.hpi>...          show header info
//   hpitool list <archive.hpi>                list contents
//   hpitool cat <archive.hpi> <path>          write one file to stdout
//   hpitool extract <archive.hpi> <outdir>    extract everything

#include "hpi/hpi.h"

#include <fstream>
#include <cctype>
#include <iostream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

static int usage() {
    std::cerr << "usage: hpitool inspect <archive.hpi>...\n"
                 "       hpitool list <archive.hpi>\n"
                 "       hpitool cat <archive.hpi> <internal-path>\n"
                 "       hpitool extract <archive.hpi> <outdir>\n"
                 "       hpitool merge <hpi-dir> <outdir>   "
                 "# all *.hpi/*.ufo, retail precedence (newest wins)\n"
                 "       hpitool where <hpi-dir> <internal-path>  "
                 "# show which archive wins a path\n"
                 "       hpitool pack <indir> <out.hpi>     "
                 "# pack a dir into an HPI (round-trip checked)\n";
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1];

    try {
        if (cmd == "inspect") {
            for (int i = 2; i < argc; ++i) {
                std::cout << "== " << argv[i] << " ==\n"
                          << ta::hpi::describe(ta::hpi::inspect(argv[i])) << "\n";
            }
        } else if (cmd == "list") {
            ta::hpi::Archive ar(argv[2]);
            size_t files = 0;
            uint64_t total = 0;
            for (const auto& e : ar.entries()) {
                if (e.isDirectory) {
                    std::cout << "         [" << e.path << "]\n";
                } else {
                    std::printf("%9u %s\n", e.decompressedSize, e.path.c_str());
                    ++files;
                    total += e.decompressedSize;
                }
            }
            std::cout << files << " files, " << total << " bytes\n";
        } else if (cmd == "cat" && argc >= 4) {
            ta::hpi::Archive ar(argv[2]);
            const auto* e = ar.find(argv[3]);
            if (!e) {
                std::cerr << "not found: " << argv[3] << "\n";
                return 1;
            }
            auto data = ar.read(*e);
            std::cout.write(reinterpret_cast<const char*>(data.data()),
                            static_cast<std::streamsize>(data.size()));
        } else if (cmd == "extract" && argc >= 4) {
            ta::hpi::Archive ar(argv[2]);
            fs::path outDir = argv[3];
            size_t files = 0;
            for (const auto& e : ar.entries()) {
                fs::path out = outDir / e.path;
                if (e.isDirectory) {
                    fs::create_directories(out);
                    continue;
                }
                fs::create_directories(out.parent_path());
                auto data = ar.read(e);
                std::ofstream f(out, std::ios::binary);
                f.write(reinterpret_cast<const char*>(data.data()),
                        static_cast<std::streamsize>(data.size()));
                if (!f) throw std::runtime_error("write failed: " + out.string());
                ++files;
            }
            std::cout << "extracted " << files << " files to " << outDir.string() << "\n";
        } else if (cmd == "merge" && argc >= 4) {
            // Layer every *.hpi/*.ufo in a directory with the retail precedence
            // (newest file date wins; loose files on disk override) and extract
            // the resolved result -- what the retail engine would actually see.
            ta::hpi::MountSet ms(argv[2]);
            fs::path outDir = argv[3];
            std::cerr << "mounting " << ms.archiveFiles().size() << " archive(s):\n";
            for (const auto& a : ms.archiveFiles())
                std::cerr << "  " << a.filename().string() << "\n";
            size_t files = 0;
            for (const auto& p : ms.paths()) {
                // Lowercase the output path. The retail engine is case-insensitive
                // (Windows), but archives disagree on case for the same logical file
                // (data.hpi "gamedata/" vs V3Rocket "GameData/"); on a case-sensitive
                // filesystem a lowercase tree is what the engine's lookups expect.
                std::string lp = p;
                for (char& c : lp) c = char(std::tolower((unsigned char)c));
                fs::path out = outDir / lp;
                fs::create_directories(out.parent_path());
                auto data = ms.read(p);
                std::ofstream f(out, std::ios::binary);
                f.write(reinterpret_cast<const char*>(data.data()),
                        static_cast<std::streamsize>(data.size()));
                if (!f) throw std::runtime_error("write failed: " + out.string());
                ++files;
            }
            std::cout << "merged " << files << " files to " << outDir.string() << "\n";
        } else if (cmd == "where" && argc >= 4) {
            ta::hpi::MountSet ms(argv[2]);
            std::cout << argv[3] << " -> " << ms.sourceOf(argv[3]) << "\n";
        } else if (cmd == "pack" && argc >= 4) {
            // hpitool pack <indir> <out.hpi> -- pack every file under <indir>
            // into an uncompressed HPI (internal path = path relative to indir),
            // then reparse it and verify the contents round-trip byte-exact.
            fs::path indir = argv[2];
            std::vector<ta::hpi::PackFile> files;
            for (auto& de : fs::recursive_directory_iterator(indir)) {
                if (!de.is_regular_file()) continue;
                std::string rel = fs::relative(de.path(), indir).generic_string();
                std::ifstream f(de.path(), std::ios::binary);
                std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                          std::istreambuf_iterator<char>());
                files.push_back({rel, std::move(data)});
            }
            auto bytes = ta::hpi::pack(files);
            { std::ofstream o(argv[3], std::ios::binary);
              o.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())); }
            ta::hpi::Archive ar(argv[3]);
            bool ok = true;
            for (const auto& pf : files) {
                const ta::hpi::Entry* e = ar.find(pf.path);
                if (!e || ar.read(*e) != pf.data) { ok = false;
                    std::cerr << "  MISMATCH " << pf.path << "\n"; }
            }
            std::cout << "packed " << files.size() << " files -> " << argv[3] << " ("
                      << bytes.size() << " bytes); " << (ok ? "ROUNDTRIP OK" : "ROUNDTRIP FAILED") << "\n";
            return ok ? 0 : 1;
        } else {
            return usage();
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
