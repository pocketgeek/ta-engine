#include "cob/retailvm.h"
#include "sim/sim.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

int main() {
    using namespace tak::sim;
    const auto require=[](bool ok,const char* message) {
        if (!ok) throw std::runtime_error(message);
    };

    auto script=std::make_shared<tak::cob::File>();
    script->scripts={{"AimWeapon",0},{"FireWeapon",9},{"TargetCleared",18}};
    script->code={0x10021001,100,0x10013000,0x10021001,22,0x10021002,2,0x10082000,0x10065000,
                  0x10021001,100,0x10013000,0x10021001,23,0x10021002,0,0x10082000,0x10065000,
                  0x10022000,0x10021001,21,0x10021002,0,0x10082000,0x10065000};

    World world;
    world.setPlayerCount(3);
    world.setSerialThreads(true);
    world.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
    UnitType shooter,targetType;
    shooter.canMove=true;shooter.maxVel=Fixed::fromInt(1);
    shooter.turnInPlaceRate=0;
    targetType.maxVel=Fixed::fromInt(1);
    shooter.simulationScript=script;
    shooter.maxHp=targetType.maxHp=1000;
    shooter.maxMana=20;
    Weapon weapon;weapon.range=400;weapon.reload=10;weapon.damage=1;
    weapon.projVel=300;weapon.manaCost=3;
    shooter.weapons.push_back(weapon);shooter.weapon=weapon;

    const int shooterId=world.spawn(&shooter,200,200,{},0);
    const int targetId=world.spawn(&targetType,300,200,{},1);
    world.unit(shooterId)->mana=20;   // a new unit's pool starts empty (511990)
    world.tick(1.f/30.f);
    require(world.playerSees(1,*world.unit(targetId)),"fixture begins with the target visible to its owner");
    world.attack(shooterId,targetId,false);

    bool fireCallback=false,pendingRelease=false;
    for(int tick=0;tick<20 && !pendingRelease;++tick) {
        if(fireCallback) {
            const auto* archer=world.unit(shooterId);
            auto* target=world.unit(targetId);
            target->x=archer->x+Fixed::fromInt(401);
            target->z=archer->z;
        }
        world.tick(1.f/30.f);
        const auto* archer=world.unit(shooterId);
        fireCallback=fireCallback || archer->fireAnimations!=0;
        pendingRelease=(archer->weaponAim[0].flags&16)!=0;
    }
    require(fireCallback && pendingRelease,
            "FireWeapon SET 23 becomes pending while the target is outside weapon range");
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
        require((world.unit(shooterId)->weaponAim[0].flags&16) &&
                world.projectiles().empty() && world.unit(shooterId)->mana==20,
                "an out-of-range SET 23 remains pending while visibility changes");
    }
    require(!world.playerSees(2,*world.unit(targetId)),
            "the pending target is hidden from the selected viewer before re-entering range");

    auto* target=world.unit(targetId);
    const auto* archer=world.unit(shooterId);
    target->x=archer->x+Fixed::fromInt(400);
    target->z=archer->z;
    world.tick(1.f/30.f);
    require(!world.playerSees(2,*world.unit(targetId)),
            "the target remains hidden on the release tick");
    require(world.unit(shooterId)->justFired && world.projectiles().size()==1 &&
            world.unit(shooterId)->mana==17 &&
            !(world.unit(shooterId)->weaponAim[0].flags&16),
            "the hidden live target receives exactly one pending projectile release");

    std::cout<<"PASS: pending SET 23 releases once when a live target re-enters range while hidden\n";
}
