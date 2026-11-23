// Lockstep checkpoints for each navigation mode: a fixed mixed-order cohort,
// hashed every 300 ticks. tools/navigation_determinism_test.py compares the
// output with tools/navigation_checkpoints.json, serial and with workers.
#include "navigation_test_common.h"
using namespace navigation_test;
int main(int argc,char** argv) {
    if(argc<2||argc>3){std::fprintf(stderr,"usage: navigation_checkpoint_test MODE [serial|workers]\n");return 2;}
    try {
        const auto selected=mode(argv[1]);const bool serial=argc==2||std::string_view(argv[2])=="serial";
        for(bool boat:{false,true}) {
            World world;setup(world,selected,serial,boat);
            std::array<UnitType,3> types{mover(0,boat),mover(1,boat),mover(2,boat)};
            std::vector<int> ids;
            for(int i=0;i<24;++i){const int id=world.spawn(&types[size_t(i%3)],float(512+i%4*64),float(1504+i/4*64),std::nullopt,0);
                ids.push_back(id);if(i%3==0)world.order(id,3104,2048,false);else if(i%3==1)world.attackMove(id,3104,2048,false);else world.patrol(id,3104,2048);}
            known(world);
            for(int tick=1;tick<=3600;++tick) {
                if(tick==901)for(int id:ids)world.order(id,800,3008,true);
                if(tick==1801){world.stop(ids[0]);world.guard(ids[1],ids[0],false);}
                if(tick==2401)world.order(ids[0],2400,2800,false);
                world.tick(1.f/30);
                if(tick%300==0){legal(world,ids);std::printf("mode=%u boat=%d tick=%d hash=%016llx\n",unsigned(selected),boat,tick,(unsigned long long)world.stateHash());}
            }
        }
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
