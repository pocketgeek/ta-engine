// modeltool — inspect TAK 3DO unit models.
//
//   modeltool info <model.3do>              object tree + textures
//   modeltool obj <model.3do> <out.obj>     export to Wavefront OBJ
//   modeltool wire <model.3do> <out.png>    orthographic wireframe render

#include "tdo/tdo.h"
#include "util/png.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <fstream>

using ta::tdo::Object;

static void printTree(const Object& o, int depth) {
    std::cout << std::string(size_t(depth) * 2, ' ') << (o.name.empty() ? "(unnamed)" : o.name)
              << ": " << o.vertices.size() / 3 << " verts, " << o.primitives.size()
              << " prims\n";
    for (const auto& c : o.children) printTree(c, depth + 1);
}

struct Vec3 { float x, y, z; };

// Flatten the object tree into world-space polygons.
static void flatten(const Object& o, Vec3 base, std::vector<std::vector<Vec3>>& polys) {
    base.x += o.x; base.y += o.y; base.z += o.z;
    for (const auto& p : o.primitives) {
        if (p.indices.size() < 2) continue;
        std::vector<Vec3> poly;
        for (auto idx : p.indices) {
            size_t i = size_t(idx) * 3;
            if (i + 2 >= o.vertices.size()) continue;
            poly.push_back({base.x + o.vertices[i], base.y + o.vertices[i + 1],
                            base.z + o.vertices[i + 2]});
        }
        if (poly.size() >= 2) polys.push_back(std::move(poly));
    }
    for (const auto& c : o.children) flatten(c, base, polys);
}

static void writeObj(const Object& o, Vec3 base, std::ofstream& out, size_t& vbase) {
    base.x += o.x; base.y += o.y; base.z += o.z;
    out << "o " << (o.name.empty() ? "unnamed" : o.name) << "\n";
    size_t nv = o.vertices.size() / 3;
    for (size_t i = 0; i < nv; ++i)
        out << "v " << base.x + o.vertices[i * 3] << " " << base.y + o.vertices[i * 3 + 1]
            << " " << base.z + o.vertices[i * 3 + 2] << "\n";
    for (const auto& p : o.primitives) {
        if (p.indices.size() < 3) continue;
        out << "f";
        for (auto idx : p.indices) out << " " << vbase + idx + 1;
        out << "\n";
    }
    vbase += nv;
    for (const auto& c : o.children) writeObj(c, base, out, vbase);
}

static void drawLine(std::vector<uint8_t>& img, int W, int H, int x0, int y0, int x1, int y1) {
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    while (true) {
        if (x0 >= 0 && x0 < W && y0 >= 0 && y0 < H) {
            size_t i = (size_t(y0) * W + x0) * 4;
            img[i] = 120; img[i + 1] = 220; img[i + 2] = 160; img[i + 3] = 255;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: modeltool info|obj|wire <model.3do> [out]\n";
        return 2;
    }
    std::string cmd = argv[1];
    try {
        auto m = ta::tdo::load(argv[2]);

        if (cmd == "info") {
            printTree(m.root, 0);
            std::cout << "textures:";
            for (const auto& t : m.textures()) std::cout << " " << t;
            std::cout << "\n";
        } else if (cmd == "obj" && argc >= 4) {
            std::ofstream out(argv[3]);
            size_t vbase = 0;
            writeObj(m.root, {0, 0, 0}, out, vbase);
            std::cout << "wrote " << argv[3] << "\n";
        } else if (cmd == "wire" && argc >= 4) {
            std::vector<std::vector<Vec3>> polys;
            flatten(m.root, {0, 0, 0}, polys);

            // 3/4 view: rotate around Y then tilt around X, orthographic.
            const float ay = 0.7f, ax = 0.5f;
            auto proj = [&](Vec3 v) {
                float x = v.x * std::cos(ay) + v.z * std::sin(ay);
                float z = -v.x * std::sin(ay) + v.z * std::cos(ay);
                float y = v.y * std::cos(ax) - z * std::sin(ax);
                return Vec3{x, y, 0};
            };
            float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
            for (auto& poly : polys)
                for (auto& v : poly) {
                    Vec3 p = proj(v);
                    minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
                    miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
                }
            const int W = 640, H = 640, pad = 40;
            float scale = std::min((W - 2 * pad) / std::max(maxx - minx, 1e-6f),
                                   (H - 2 * pad) / std::max(maxy - miny, 1e-6f));
            std::vector<uint8_t> img(size_t(W) * H * 4);
            for (size_t i = 0; i < img.size(); i += 4) {
                img[i] = 24; img[i + 1] = 24; img[i + 2] = 32; img[i + 3] = 255;
            }
            auto toPx = [&](Vec3 v) {
                Vec3 p = proj(v);
                return std::pair<int, int>{int((p.x - minx) * scale) + pad,
                                           H - (int((p.y - miny) * scale) + pad)};
            };
            for (auto& poly : polys)
                for (size_t i = 0; i < poly.size(); ++i) {
                    auto [x0, y0] = toPx(poly[i]);
                    auto [x1, y1] = toPx(poly[(i + 1) % poly.size()]);
                    drawLine(img, W, H, x0, y0, x1, y1);
                }
            ta::png::write(argv[3], W, H, img);
            std::cout << "wrote " << argv[3] << " (" << polys.size() << " polys)\n";
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
