#pragma once
#include "net/protocol.h"
#include "sim/powercodes.h"
#include <cmath>

namespace ta::srv {
inline bool parsePlayerCommands(const std::vector<uint8_t>& payload, std::vector<ta::net::Command>& parsed) {
    ta::net::Reader reader(payload.data(),payload.size());
    const uint32_t count=reader.u32();
    if (!reader.ok || count>payload.size()/35) return false;
    // Simulation destinations use signed 16.16. Reject values that cannot
    // be converted safely, including finite but enormous coordinates.
    const auto coordinate=[](float v) { return ta::net::commandCoordinate(v); };
    parsed.clear();
    parsed.reserve(count);
    for (uint32_t i=0;i<count && reader.ok;++i) {
        const auto c=reader.cmd();
        // Every ordinary verb, Resurrect, and the power codes (an unknown code or
        // argument is forged: the client sends only the seven the sim applies).
        const bool power=c.kind==ta::net::Cmd::PowerCode;
        const bool resurrect=c.kind==ta::net::Cmd::Resurrect;
        const bool ground=c.kind==ta::net::Cmd::AttackGround;
        // The D-gun (339), RECLAIMUNIT (340), Land (341) and the standing orders
        // (342) are ordinary verbs too.
        const bool late=c.kind==ta::net::Cmd::DGun || c.kind==ta::net::Cmd::ReclaimUnit || c.kind==ta::net::Cmd::Land ||
                        c.kind==ta::net::Cmd::StandingOrder;
        // Kingdoms' lodestone area build and weapon switch are retired; the
        // area reclaim is an engine feature (TA has no RECLAIMAREA,
        // docs/ta-port-plan.md).
        const bool retired=c.kind==ta::net::Cmd::RetiredAreaBuild || c.kind==ta::net::Cmd::SetWeapon;
        if (retired || (uint8_t(c.kind)>uint8_t(ta::net::Cmd::RetiredAreaBuild) && !power && !resurrect && !ground && !late) ||
            !coordinate(c.x) || !coordinate(c.z) ||
            !coordinate(c.x2) || !coordinate(c.z2)) return false;
        if (power && (!ta::sim::simPowerCode(c.targetId) || c.queue>uint8_t(ta::sim::PowerArg::Invalid)))
            return false;
        // Ctrl+Shift requests ten units, the largest client production batch.
        // A forged INT_MAX count would otherwise allocate billions of entries
        // in World::train before another network/resource check could run.
        if ((c.kind==ta::net::Cmd::Train || c.kind==ta::net::Cmd::Unqueue) &&
            (c.targetId<0 || c.targetId>10)) return false;
        parsed.push_back(c);
    }
    return reader.ok && reader.p==reader.end;
}
inline bool validPlayerCommands(const std::vector<uint8_t>& payload) {
    std::vector<ta::net::Command> parsed;
    return parsePlayerCommands(payload,parsed);
}
}
