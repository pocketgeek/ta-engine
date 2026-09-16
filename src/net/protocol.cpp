#include "net/protocol.h"

namespace ta::net {

// The 35-byte Command wire format, matching the classic lockstep layout.
void Writer::cmd(const Command& c) {
    u8(uint8_t(c.kind));
    u8(c.player);
    u32(uint32_t(c.unitId));
    u32(uint32_t(c.targetId));
    f32(c.x);
    f32(c.z);
    u8(c.queue);
    b.insert(b.end(), c.type, c.type + 16);
}

Command Reader::cmd() {
    Command c;
    c.kind = Cmd(u8());
    c.player = u8();
    c.unitId = int32_t(u32());
    c.targetId = int32_t(u32());
    c.x = f32();
    c.z = f32();
    c.queue = u8();
    if (avail(16)) { std::memcpy(c.type, p, 16); p += 16; }
    else ok = false;
    c.type[15] = 0;
    return c;
}

}  // namespace ta::net
