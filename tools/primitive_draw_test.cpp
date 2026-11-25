// client/primitivedraw.h: TotalA.exe's primitive order (0x4cb370) and flat swatches.
#include "client/primitivedraw.h"

#include <cstdio>
#include <vector>

namespace {

bool check(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", what);
    return condition;
}

// One primitive per entry of `heights`: a triangle whose vertices all sit at that y.
ta::tdo::Object piece(const std::vector<int32_t>& heights, int32_t selection) {
    ta::tdo::Object object;
    object.selectionPrimitive = selection;
    for (int32_t y : heights) {
        ta::tdo::Primitive primitive;
        for (int k = 0; k < 3; ++k) {
            primitive.indices.push_back(uint16_t(object.verticesRaw.size()));
            object.verticesRaw.push_back({k * 65536, y, 0});
        }
        object.primitives.push_back(primitive);
    }
    return object;
}

} // namespace

int main() {
    using namespace ta::primitivedraw;
    bool ok = true;
    // No selection primitive: primitive 0 stays first, the rest sort by height.
    ok &= check(drawOrder(piece({50, 30, 10, 20}, -1)) == std::vector<uint16_t>{0, 2, 3, 1},
                "no selection: 0 first, 1..n-1 ascending");
    // The selection primitive swaps into slot 0 and is left out; primitive 0 sorts in.
    ok &= check(drawOrder(piece({5, 30, 10, 0, 20}, 3)) == std::vector<uint16_t>{0, 2, 4, 1},
                "selection 3: dropped, old 0 sorted with the rest");
    // Ties keep their order after the swap (strict-greater bubble sort).
    ok &= check(drawOrder(piece({7, 7, 7, 7}, 1)) == std::vector<uint16_t>{0, 2, 3},
                "ties: swapped order kept");
    ok &= check(drawOrder(piece({7, 9, 7, 7}, 2)) == std::vector<uint16_t>{0, 3, 1},
                "selection 2: sequence 1,0,3 sorted by height (7,7,9 -> 0,3,1)");
    // Averages truncate toward zero like idiv: -1/3 is 0, not -1.
    {
        auto object = piece({0, 0}, -1);
        object.verticesRaw[3][1] = -1;
        ok &= check(averageY(object, object.primitives[1]) == 0, "idiv truncates toward zero");
    }
    ok &= check(drawOrder(piece({}, 0)).empty(), "an empty piece draws nothing");
    ok &= check(flatColourOf(flatTextureName(0)) == 0 && flatColourOf(flatTextureName(209)) == 209,
                "swatch names round-trip");
    ok &= check(flatColourOf("noise6d") == -1 && flatColourOf("#pal") == -1 && flatColourOf("#pal256") == -1 &&
                flatColourOf("#pal1x") == -1, "real textures are not swatches");
    if (ok) std::puts("PASS: primitive draw order and flat swatches");
    return ok ? 0 : 1;
}
