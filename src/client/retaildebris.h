#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <algorithm>

namespace ta {

// TotalA.exe 0x485140: the height under a point is the mean of its 16 px map
// cell's two height bytes (+5, +6: highest and lowest corner), -1 off the map.
template<class Height>
int retailDebrisTerrainHeight(int32_t x,int32_t z,int width,int height,Height sample) {
    const int cx=int(int16_t(x>>16))/16,cz=int(int16_t(z>>16))/16;
    if(cx<0 || cz<0 || cx>=width || cz>=height)return -1;
    const int nx=std::min(cx+1,width-1),nz=std::min(cz+1,height-1);
    const std::array<int,4> values={sample(cx,cz),sample(nx,cz),sample(cx,nz),sample(nx,nz)};
    return (*std::min_element(values.begin(),values.end())+
            *std::max_element(values.begin(),values.end()))/2;
}

// The fx.gaf sequences TotalA.exe loads at game start (0x429952..0x429ae6) and
// hands to its effect list 0x420a30: BITMAP1..BITMAP5 and BITMAPNUKE of a COB
// EXPLODE (game +0x147f7..+0x1480b), the sea and lava splashes (+0x147eb,
// +0x147ef). "explosion" doubles as the debris impact burst.
enum class RetailFx : int8_t {
    None=-1, Explosion, Explode2, Explode3, Explode4, Explode5, Nuke1, WaterSplash, LavaSplash,
};
inline const char* retailFxSequence(RetailFx fx) {
    switch(fx) {
    case RetailFx::Explosion: return "explosion";
    case RetailFx::Explode2: return "explode2";
    case RetailFx::Explode3: return "explode3";
    case RetailFx::Explode4: return "explode4";
    case RetailFx::Explode5: return "explode5";
    case RetailFx::Nuke1: return "nuke1";
    case RetailFx::WaterSplash: return "h2oboom2";
    case RetailFx::LavaSplash: return "lavasplash";
    default: return nullptr;
    }
}

// Display-only flying debris of a COB EXPLODE (docs/retail-weapon-scripts.md
// "Debris"). Geometry ownership belongs to the caller; this never enters sim.
//
// Two kinds, both from TotalA.exe:
//  - a whole piece (0x421620 -> pool 0x511df0, 100 slots; tick 0x4213b0),
//  - a SHATTER fragment, one per quad of the piece (0x421700, sharing the
//    300-entry effect list game +0x1491b; tick inline in 0x420f30).
struct RetailDebrisMotion {
    // The EXPLODE descriptor's flag word (+0x28), as 0x481140 builds it from
    // the script's flags.
    enum : uint32_t {
        kFire=1, kSmoke=2, kShatter=4, kFall=8, kExplodeOnHit=0x10, kShatterExplodeOnHit=0x20,
    };
    std::array<int32_t,3> position{}, velocity{};
    // A fragment's constant per-tick drift: half the dying unit's velocity
    // (0x421830..0x421849). The display has no unit velocity: zero.
    std::array<int32_t,3> drift{};
    std::array<uint16_t,3> rotation{}, spin{};
    uint32_t remaining=900, flags=0;
    bool shatter=false;
    struct Result { bool alive; RetailFx effect=RetailFx::None; int light=-1; };

    // 0x4811d0..0x481238: SHATTER 1 -> 4 (and EXPLODE_ON_HIT 2 -> 0x20 with it,
    // else 0x10), FALL 4 -> 8, SMOKE 8 -> 2, FIRE 16 -> 1.
    static uint32_t descriptorFlags(uint32_t scriptFlags) {
        uint32_t d=0;
        if(scriptFlags&1) d|=kShatter|((scriptFlags&2)?kShatterExplodeOnHit:0u);
        else if(scriptFlags&2) d|=kExplodeOnHit;
        if(scriptFlags&4) d|=kFall;
        if(scriptFlags&8) d|=kSmoke;
        if(scriptFlags&16) d|=kFire;
        return d;
    }

    // COB EXPLODE, TotalA.exe 0x481140: unless BITMAPONLY (0x20), six draws of
    // the game random (0x4b6c30, rand % n): three spin rates rand(3000), then
    // vx = (20 - rand(40)) << 14, vy = rand(10) << 16, vz = (20 - rand(40)) << 14;
    // lifetime 900 (0x4811c3). The spins turn piece +0x10 / +0x12 / +0x14 by
    // the second / third / first draw (0x421505..0x421519).
    template<class Random>
    static bool launch(uint32_t scriptFlags, Random random, RetailDebrisMotion& out) {
        if(scriptFlags&0x20)return false;
        out=RetailDebrisMotion{};
        const uint16_t s0=uint16_t(random(3000)),s1=uint16_t(random(3000)),s2=uint16_t(random(3000));
        out.spin={s1,s2,s0};
        out.velocity[0]=(20-int32_t(random(40)))*16384;
        out.velocity[1]=int32_t(random(10))*65536;
        out.velocity[2]=(20-int32_t(random(40)))*16384;
        out.remaining=900;
        out.flags=descriptorFlags(scriptFlags);
        return true;
    }

    // One SHATTER fragment, TotalA.exe 0x42184c..0x4218e9 and 0x421b20..0x421b88:
    // vx, vz, vy = (80 - rand(160)) << 9, vy plus 30 * gravity; three spin rates
    // 800 - rand(1600) (rotation starts at 0); then the face normal n (unit
    // length, (v2 - v1) x (v0 - v1) of the quad's first three vertices) kicks
    // vx += rand(200) * (short)(n.x * 512) and vz -= rand(200) * (short)(n.z * 512).
    // The fragment explodes on hit when the script asked EXPLODE_ON_HIT (0x421815).
    template<class Random>
    static RetailDebrisMotion fragment(uint32_t descriptor, int32_t gravity,
                                       const std::array<float,3>& normal, Random random) {
        RetailDebrisMotion out;
        out.shatter=true;
        out.flags=descriptor;
        out.velocity[0]=(80-int32_t(random(160)))*512;
        out.velocity[2]=(80-int32_t(random(160)))*512;
        out.velocity[1]=std::bit_cast<int32_t>(uint32_t((80-int32_t(random(160)))*512)+uint32_t(gravity)*30u);
        for(auto& s:out.spin)s=uint16_t(800-int32_t(random(1600)));
        const int32_t kx=int16_t(int32_t(normal[0]*512.0f));
        out.velocity[0]=std::bit_cast<int32_t>(uint32_t(out.velocity[0])+uint32_t(int32_t(random(200))*kx));
        const int32_t kz=int16_t(int32_t(normal[2]*512.0f));
        out.velocity[2]=std::bit_cast<int32_t>(uint32_t(out.velocity[2])-uint32_t(int32_t(random(200))*kz));
        return out;
    }

    // `gravity` is the game's +0x14263 (the map's, 16.16 per tick^2), `sea`
    // its sea level byte +0x1427f; `lava` the map's lavaworld (rules +0xd44),
    // `suppressSplash` nosealeveltrigger (rules +0xd48).
    template<class Terrain>
    Result tick(uint8_t sea, int32_t gravity, bool lava, bool suppressSplash, Terrain terrain) {
        const auto add=[](int32_t a,int32_t b) {return std::bit_cast<int32_t>(uint32_t(a)+uint32_t(b));};
        const int32_t seaLine=int32_t(uint32_t(sea)<<16);
        const RetailFx splash=lava ? RetailFx::LavaSplash : RetailFx::WaterSplash;
        if(shatter) {
            // 0x420f81..0x4210ac: move (drift + velocity), fall, turn, then test
            // the new point. In water (at or under the sea, over a cell lower
            // than it) it ends, splashing when it explodes on hit. On the
            // ground it goes back, bounces at half speed, and ends once the
            // bounce is under 1 px a tick, bursting when it explodes on hit.
            // A fragment has no lifetime.
            const auto old=position;
            for(unsigned i=0;i<3;++i)position[i]=add(position[i],add(drift[i],velocity[i]));
            velocity[1]=std::bit_cast<int32_t>(uint32_t(velocity[1])-uint32_t(gravity));
            for(unsigned i=0;i<3;++i)rotation[i]=uint16_t(rotation[i]+spin[i]);
            const int h=terrain(position);
            const bool hits=(flags&kShatterExplodeOnHit)!=0;
            if(position[1]<=seaLine && h<int(sea))
                return hits && !suppressSplash ? Result{false,splash,-1} : Result{false};
            if(int(int16_t(uint32_t(position[1])>>16))>h)return {true};
            position=old;
            velocity[1]=-(velocity[1]/2);
            if(int16_t(uint32_t(velocity[1])>>16)>0)return {true};
            return hits ? Result{false,RetailFx::Explosion,0} : Result{false};
        }
        // 0x4213b0. The lifetime runs out after 900 updates (the 901st ends it).
        if(remaining--==0)return {false};
        // At or under the sea line: gone, splashing when it explodes on hit
        // (0x4213d9..0x421440, light none).
        if(position[1]<=seaLine)
            return (flags&kExplodeOnHit) && !suppressSplash ? Result{false,splash,-1} : Result{false};
        // The ground under the present point, against this tick's step: a
        // bounce halves all three speeds and turns the fall upward; under
        // 2 px a tick it ends there, bursting (light 0) when it explodes on hit
        // (0x421450..0x4214d4).
        const int h=terrain(position);
        if(add(position[1],velocity[1])<=int32_t(uint32_t(h)<<16)) {
            velocity[1]=-(velocity[1]>>1);
            velocity[0]>>=1;velocity[2]>>=1;
            if(velocity[1]<0x20000)
                return (flags&kExplodeOnHit) ? Result{false,RetailFx::Explosion,0} : Result{false};
        }
        // 0x4214e4..0x421538: move, turn, and fall only with FALL.
        for(unsigned i=0;i<3;++i) {
            position[i]=add(position[i],velocity[i]);
            rotation[i]=uint16_t(rotation[i]+spin[i]);
        }
        if(flags&kFall)
            velocity[1]=std::bit_cast<int32_t>(uint32_t(velocity[1])-uint32_t(gravity));
        return {true};
    }
};

} // namespace ta
