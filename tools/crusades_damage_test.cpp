// Synthetic loader regressions derived from official3.0 KINGDOMS.icd.
#include "sim/sim.h"
#include "hpi/hpi.h"
#include "client/cursorrange.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    namespace fs=std::filesystem;
    const auto dir=fs::temp_directory_path()/("tak-damage-"+std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {fs::path p;~Cleanup(){std::error_code e;fs::remove_all(p,e);}} cleanup{dir};
    fs::create_directories(dir/"units");
    struct Case {const char* base;const char* multiplier;int expected;int defaultDamage;};
    const Case cases[]={{"301","0.5",150,301},{"10","0.3",3,10},
        {"100","0.29",28,100},{"65535","1.1",72088,65535},
        {"91","-0.5",-45,91},{"180","0.04",7,180},
        {"65537","2",2,1},{"301.9","0.5",150,301}};
    for(size_t i=0;i<std::size(cases);++i) {
        const auto& c=cases[i];
        std::ofstream f(dir/"units"/("case"+std::to_string(i)+".fbi"));
        f<<"[UNITINFO]\n{\nObjectName=case"<<i<<";\nDamageCategory=fort;\nCategory=monster;\nTEDClass=factory;\n}\n"
         <<"[WEAPON1]\n{\n[DAMAGE]\n{\ndefault="<<c.base<<";\nfort="<<c.multiplier<<";\nmonster=0;\nfactory=0;\n}\n}\n";
    }
    tak::hpi::Vfs vfs;vfs.addLayer(tak::hpi::MountSet(dir));
    tak::sim::TypeRegistry registry;registry.loadDir(vfs,"units/");
    int failures=0;
    auto check=[&](bool ok,const char* text){if(!ok){std::cerr<<text<<'\n';++failures;}};
    for(size_t i=0;i<std::size(cases);++i) {
        const auto* t=registry.find("case"+std::to_string(i));
        check(t && !t->weapons.empty(),"synthetic weapon loaded");
        if(!t || t->weapons.empty())continue;
        auto w=t->weapons.front();
        check(w.damage==cases[i].defaultDamage,"default is atoi wrapped uint16");
        check(w.damageVs(t)==cases[i].expected,"native category damage after interning");
        w.dmgVsIds.clear();
        check(w.damageVs(t)==cases[i].expected,"uninterned category damage agrees");
        tak::sim::UnitType generic;generic.categories={"monster","factory"};
        check(w.damageVs(&generic)==w.damage,"generic CATEGORY/TEDClass must not match");
        generic.damageCategory="monster";
        check(w.damageVs(&generic)==0,"explicit category immunity");
        check(tak::client::cursorDamageVs(w,generic)==0,"cursor and combat share category immunity");
    }
    return failures?1:0;
}
