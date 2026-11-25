// client/padrider.h: a parked flyer's attitude is its pad's plus the pad
// piece's x / z turns (TotalA.exe 0x43e180, axes from 0x43def0 / 0x4b6cc0).
#include "client/padrider.h"
#include "client/modelmath.h"

#include <cmath>
#include <cstdio>

namespace {

bool check(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", what);
    return condition;
}

bool near(const float a[3], const float b[3]) {
    return std::fabs(a[0] - b[0]) < 1e-4f && std::fabs(a[1] - b[1]) < 1e-4f && std::fabs(a[2] - b[2]) < 1e-4f;
}

} // namespace

int main() {
    using namespace ta::padrider;
    bool ok = true;
    constexpr float kQuarter = 1.5707963267948966f;
    ok &= check(angleWord(kQuarter) == 0x4000 && angleWord(-kQuarter) == 0xc000, "radians to words");
    ok &= check(attitude(100, 200, nullptr) == std::pair<uint16_t, uint16_t>{100, 200}, "no piece: the pad's");
    const float rot[3] = {kQuarter, 1.0f, -kQuarter};
    ok &= check(attitude(0x1000, 0x0100, rot) == std::pair<uint16_t, uint16_t>{0x5000, 0xc100},
                "x turn to pitch, z turn to roll, y left to the heading");
    // The body built from those words turns a point exactly as the pad piece's
    // own script turn would (both enter the model as -angle).
    const float xOnly[3] = {kQuarter, 0, 0}, zOnly[3] = {0, 0, kQuarter};
    for (const float* turn : {xOnly, zOnly}) {
        const auto [pitch, roll] = attitude(0, 0, turn);
        float body[3], piece[3];
        modelBodyTransform(pitch, roll).apply(1, 2, 3, body);
        ta::cob::PieceState pose;
        for (int axis = 0; axis < 3; ++axis) pose.rot[axis] = turn[axis];
        scriptTransform(Xform{}, 0, 0, 0, &pose).apply(1, 2, 3, piece);
        ok &= check(near(body, piece), "the body attitude matches the piece's script turn");
    }
    if (ok) std::puts("PASS: pad rider attitude");
    return ok ? 0 : 1;
}
