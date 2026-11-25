#include "cob/retailvm.h"
#include "sim/sim.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

int main() {
    using namespace ta::sim;
    const auto require=[](bool ok,const char* message) {
        if (!ok) throw std::runtime_error(message);
    };

    // TotalA.exe call-ins: AimPrimary sleeps 100 ms and returns 1 (ready,
    // docs/retail-weapon-scripts.md 2.3); FirePrimary and TargetCleared return.
    auto script=std::make_shared<ta::cob::File>();
    script->scripts={{"AimPrimary",0},{"FirePrimary",6},{"TargetCleared",9}};
    script->code={0x10021001,100,0x10013000,0x10021001,1,0x10065000,
                  0x10021001,0,0x10065000,
                  0x10022000,0x10021001,0,0x10065000};

    World world;
    world.setPlayerCount(3);
    world.setSerialThreads(true);
    world.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    UnitType shooter,targetType;
    shooter.canMove=true;shooter.maxVel=Fixed::fromInt(1);
    shooter.turnInPlaceRate=0;
    targetType.maxVel=Fixed::fromInt(1);
    shooter.simulationScript=script;
    // No model, so the shooter stands at y 0: a 30 px body keeps it above the
    // sea for the fire gate's surfaced test (TotalA.exe 0x49ab4d).
    shooter.modelTop=30<<16;
    shooter.maxHp=targetType.maxHp=1000;
    Weapon weapon;weapon.range=400;weapon.reload=10;weapon.damage=1;
    weapon.projVel=300;weapon.energyPerShot=3;   // paid from the player's store (econPayShot)
    weapon.velocityFx=300*65536/30;   // weaponvelocity 300, fx16 per tick (taweapons.cpp)
    weapon.taFlags=1u|1u<<19;weapon.taAim=Weapon::TaAim::Turret;   // lineofsight turret
    shooter.weapons.push_back(weapon);shooter.weapon=weapon;

    const int shooterId=world.spawn(&shooter,200,200,{},0);
    const int targetId=world.spawn(&targetType,300,200,{},1);
    world.setStartResources(0,20,0);
    world.tick(1.f/30.f);
    require(world.playerSees(1,*world.unit(targetId)),"fixture begins with the target visible to its owner");
    world.attack(shooterId,targetId,false);

    // The aim starts in range; the target steps out before AimPrimary
    // returns, so the slot is ready (aim requested and returned) but the
    // range gate holds the shot.
    bool aimed=false,ready=false;
    for(int tick=0;tick<20 && !ready;++tick) {
        if(aimed) {
            const auto* archer=world.unit(shooterId);
            auto* target=world.unit(targetId);
            target->x=archer->x+Fixed::fromInt(401);
            target->z=archer->z;
        }
        world.tick(1.f/30.f);
        const auto& aim=world.unit(shooterId)->weaponAim[0];
        aimed=aimed || aim.requested();
        ready=aim.requested() && aim.ready();
    }
    require(aimed && ready && world.projectiles().empty() && world.books(0).stored[0]==20,
            "AimPrimary's return readies the slot while the target is outside weapon range");
    require(world.playerSees(1,*world.unit(targetId)),
            "the pending target is still visible to the current viewer before switching views");

    // View from an uninvolved third player while the release is held out of
    // range: that player has no sight of the target (World::playerSees, the
    // plane the client's display fog is built from) before it is back in range.
    for(int tick=0;tick<10;++tick) {
        auto* target=world.unit(targetId);
        const auto* archer=world.unit(shooterId);
        target->x=archer->x+Fixed::fromInt(401);
        target->z=archer->z;
        world.tick(1.f/30.f);
        require(world.unit(shooterId)->weaponAim[0].ready() &&
                world.projectiles().empty() && world.books(0).stored[0]==20,
                "an out-of-range ready aim holds its shot while visibility changes");
    }
    require(!world.playerSees(2,*world.unit(targetId)),
            "the pending target is hidden from the selected viewer before re-entering range");

    // Back at the inclusive range boundary. The chase may have turned the
    // body, which moves AimPrimary's body-relative heading out of tolerance:
    // then the slot aims again first (0x49d68a). Either way exactly one shot
    // leaves, paid once, and it consumes the aim.
    bool fired=false;
    for(int tick=0;tick<15 && !fired;++tick) {
        auto* target=world.unit(targetId);
        const auto* archer=world.unit(shooterId);
        target->x=archer->x+Fixed::fromInt(400);
        target->z=archer->z;
        world.tick(1.f/30.f);
        require(!world.playerSees(2,*world.unit(targetId)),"the target remains hidden up to the release tick");
        fired=world.unit(shooterId)->justFired;
    }
    // One shot, paid once (20 - 3). A TA line-of-sight shot takes its first
    // step in its firing tick's projectile pass (0x495513), where this
    // model-less shooter's ground-level muzzle may already have burst it.
    require(fired && world.projectiles().size()<=1 && world.books(0).stored[0]==17 &&
            !world.unit(shooterId)->weaponAim[0].requested(),
            "the hidden live target receives exactly one shot, which consumes the aim");

    std::cout<<"PASS: a ready aim fires once when a live target re-enters range while hidden\n";
}
