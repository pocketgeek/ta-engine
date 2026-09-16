// tnttool — inspect TAK TNT maps.
//
//   tnttool info <map.tnt>            header + layer summary
//   tnttool heightmap <map.tnt> <out.png>
//   tnttool minimap <map.tnt> <out.png>   (grayscale; palette applied later)

#include "crt/crt.h"
#include "hpi/hpi.h"
#include "terrain/terrain.h"
#include "tnt/ota.h"
#include "tnt/tnt.h"
#include "util/png.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: tnttool info|heightmap|minimap <map.tnt> [out.png]\n"
                     "       tnttool render <map.tnt> <terrain-dir> <out.png>\n"
                     "       tnttool roundtrip <map.tnt>\n"
                     "       tnttool ota <map.ota>\n"
                     "       tnttool crt <map.crt>\n";
        return 2;
    }
    std::string cmd = argv[1];
    try {
        if (cmd == "crt") {
            // tnttool crt <file.crt> -- parse + re-serialize. Verifies the
            // parsed fields survive parse->write->parse and that write is
            // byte-stable (write(parse(write)) == write). Retail files carry
            // in-memory residue in unused record bytes, so a clean write is
            // not byte-identical to the source, but is semantically exact.
            std::ifstream in(argv[2], std::ios::binary);
            std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            auto s = ta::crt::parse(d);
            auto w = ta::crt::write(s);
            auto s2 = ta::crt::parse(w);
            auto w2 = ta::crt::write(s2);
            bool sem = s.units.size() == s2.units.size() &&
                       s.customTypes.size() == s2.customTypes.size() &&
                       s.regions.size() == s2.regions.size() &&
                       s.players.size() == s2.players.size();
            for (size_t i = 0; sem && i < s.units.size(); ++i) {
                const auto &a = s.units[i], &b = s2.units[i];
                sem = a.objectName == b.objectName && a.x == b.x && a.z == b.z &&
                      a.player == b.player && a.health == b.health &&
                      a.armor == b.armor && a.weapon == b.weapon &&
                      a.angle == b.angle && a.veteran == b.veteran;
            }
            bool stable = (w == w2);
            size_t rules = 0;
            for (const auto& groups : s.players)
                for (const auto& g : groups) rules += g.conditions.size() + g.actions.size();
            std::cout << "CRT " << argv[2] << ": " << s.units.size() << " units, "
                      << s.customTypes.size() << " custom types, " << rules
                      << " rules, " << s.regions.size() << " regions\n";
            std::cout << (sem && stable ? "ROUNDTRIP OK (" : "ROUNDTRIP FAILED (")
                      << w.size() << " bytes; semantic=" << (sem ? "ok" : "FAIL")
                      << " stable=" << (stable ? "ok" : "FAIL") << ")\n";
            return sem && stable ? 0 : 1;
        }
        if (cmd == "ota") {
            // tnttool ota <file.ota> -- parse + re-serialize, byte-compare.
            std::ifstream in(argv[2], std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            auto sc = ta::tnt::Scenario::parse(text);
            auto out = sc.write();
            std::cout << "OTA " << argv[2] << ": " << sc.starts.size()
                      << " start pos, size " << sc.sizeW << "x" << sc.sizeH
                      << ", kingdom=" << sc.kingdom << "\n";
            std::cout << (out == text ? "BYTE-IDENTICAL (" : "DIFFERS (")
                      << out.size() << " vs " << text.size() << " bytes)\n";
            return out == text ? 0 : 1;
        }

        auto m = ta::tnt::Map::load(argv[2]);

        if (cmd == "roundtrip") {
            // Load, re-serialize with Map::save(), reload, and compare every
            // field -- proves the writer reproduces the retail TNT layout.
            auto bytes = m.save();
            auto r = ta::tnt::Map::load(bytes, "<roundtrip>");
            auto eq = [](const char* n, bool ok) {
                std::cout << "  " << (ok ? "OK  " : "FAIL") << " " << n << "\n";
                return ok;
            };
            bool ok = true;
            ok &= eq("dims/sea", r.width == m.width && r.height == m.height &&
                                 r.seaLevel == m.seaLevel);
            ok &= eq("heights", r.heights == m.heights);
            ok &= eq("features", r.features == m.features);
            ok &= eq("tileKeys", r.tileKeys == m.tileKeys);
            ok &= eq("tileCols", r.tileCols == m.tileCols);
            ok &= eq("tileRows", r.tileRows == m.tileRows);
            ok &= eq("featureNames", r.featureNames == m.featureNames);
            ok &= eq("minimap", r.minimapW == m.minimapW && r.minimapH == m.minimapH &&
                                r.minimap == m.minimap);
            ok &= eq("overview", r.overviewW == m.overviewW && r.overviewH == m.overviewH &&
                                 r.overview == m.overview);
            std::cout << (ok ? "ROUNDTRIP OK (" : "ROUNDTRIP FAILED (")
                      << bytes.size() << " bytes)\n";
            return ok ? 0 : 1;
        } else if (cmd == "info") {
            std::cout << m.width << "x" << m.height << " cells ("
                      << m.width * 16 << "x" << m.height * 16 << " px)\n";
            std::set<uint32_t> keys(m.tileKeys.begin(), m.tileKeys.end());
            std::cout << "terrain JPGs: " << keys.size() << " distinct\n";
            size_t feats = 0;
            for (auto f : m.features)
                if (f != 0xFFFF) ++feats;
            std::cout << "feature cells: " << feats << "\n";
            std::cout << "minimap: " << m.minimapW << "x" << m.minimapH << "\n";
        } else if (cmd == "render" && argc >= 5) {
            // render <map.tnt> <retail-install-dir> <out.png>
            ta::hpi::Vfs vfs = ta::hpi::mountRetailRoot(argv[3]);
            ta::terrain::Compositor comp(vfs);
            auto img = comp.renderMap(m);
            ta::png::write(argv[4], img.width, img.height, img.rgba);
            std::cout << "wrote " << argv[4] << " (" << img.width << "x" << img.height
                      << ")\n";
        } else if ((cmd == "heightmap" || cmd == "minimap") && argc >= 4) {
            int w, h;
            const std::vector<uint8_t>* src;
            if (cmd == "heightmap") {
                w = m.width; h = m.height; src = &m.heights;
            } else {
                w = m.minimapW; h = m.minimapH; src = &m.minimap;
            }
            std::vector<uint8_t> rgba(size_t(w) * h * 4);
            for (size_t i = 0; i < src->size(); ++i) {
                uint8_t v = (*src)[i];
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = v;
                rgba[i * 4 + 3] = 255;
            }
            ta::png::write(argv[3], w, h, rgba);
            std::cout << "wrote " << argv[3] << " (" << w << "x" << h << ")\n";
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
