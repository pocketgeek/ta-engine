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
            ok &= eq("tiles", r.tiles == m.tiles);
            ok &= eq("tileGfx", r.numTiles == m.numTiles && r.tileGfx == m.tileGfx);
            ok &= eq("featureNames", r.featureNames == m.featureNames);
            ok &= eq("minimap", r.minimapW == m.minimapW && r.minimapH == m.minimapH &&
                                r.minimap == m.minimap);
            // Byte identity is the real bar: field equality would still pass if
            // save() laid the sections out somewhere retail never would.
            std::ifstream orig(argv[2], std::ios::binary);
            std::vector<uint8_t> raw((std::istreambuf_iterator<char>(orig)),
                                      std::istreambuf_iterator<char>());
            // Byte identity is the real bar -- field equality would still pass if
            // save() laid the sections out somewhere retail never would. The one
            // exception is the tile plane's 16-byte alignment gap: retail leaves
            // it UNINITIALISED. Most shipped maps happen to have zeros there, but
            // Coast To Coast carries stale bytes, so reproducing it exactly would
            // mean reproducing the contents of Cavedog's write buffer. Nothing
            // reads those bytes, so they are excluded and everything else must
            // match to the byte.
            auto hw = [&](const std::vector<uint8_t>& v, int word) {
                size_t o = size_t(word) * 4;
                return uint32_t(v[o] | (v[o+1] << 8) | (v[o+2] << 16) | (uint32_t(v[o+3]) << 24));
            };
            std::vector<uint8_t> cmp = bytes;
            auto ignore = [&](size_t beg, size_t end) {
                if (end > beg && end <= cmp.size() && end <= raw.size())
                    std::copy(raw.begin() + beg, raw.begin() + end, cmp.begin() + beg);
            };
            ignore(hw(cmp, 3) + size_t(m.blocksX) * m.blocksY * 2, hw(cmp, 4));
            // The other uninitialised region: each feature-name record is a u32
            // index plus a 128-byte NAME BUFFER, and retail writes the whole
            // buffer after strcpy-ing the name into it. Every record in a map
            // carries the same trailing bytes -- values like 0xbff7xxxx, i.e.
            // leaked stack addresses -- so what is past each NUL is a snapshot of
            // Cavedog's stack, not data. Compared up to the NUL only.
            for (size_t i = 0; i < m.featureNames.size(); ++i) {
                size_t rec = hw(cmp, 8) + i * 132;
                ignore(rec + 4 + m.featureNames[i].size() + 1, rec + 132);
            }
            bool same = raw == cmp;
            if (!same) {
                size_t i = 0, n = std::min(raw.size(), cmp.size());
                while (i < n && raw[i] == cmp[i]) ++i;
                std::cout << "  first difference at offset " << i << " (0x" << std::hex
                          << i << std::dec << "): orig=" << int(i < raw.size() ? raw[i] : 0)
                          << " ours=" << int(i < cmp.size() ? cmp[i] : 0)
                          << "; sizes " << raw.size() << " vs " << cmp.size() << "\n";
            }
            ok &= eq("byte-identical (bar the alignment gap)", same);
            std::cout << (ok ? "ROUNDTRIP OK (" : "ROUNDTRIP FAILED (")
                      << bytes.size() << " bytes)\n";
            return ok ? 0 : 1;
        } else if (cmd == "info") {
            std::cout << m.width << "x" << m.height << " cells ("
                      << m.width * 16 << "x" << m.height * 16 << " px)\n";
            std::set<uint16_t> used(m.tiles.begin(), m.tiles.end());
            std::cout << "tiles: " << m.numTiles << " in library, " << used.size()
                      << " referenced\n";
            std::cout << "sea level: " << m.seaLevel << "\n";
            size_t feats = 0, covered = 0;
            for (auto f : m.features) {
                if (f == ta::tnt::kFeatureCovered) ++covered;
                else if (f != ta::tnt::kNoFeature) ++feats;
            }
            std::cout << "features: " << feats << " placed (" << covered
                      << " covered cells), " << m.featureNames.size() << " named\n";
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
