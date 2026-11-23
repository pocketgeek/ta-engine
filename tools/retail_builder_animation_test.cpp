#include "client/retailbuilderanimation.h"
#include "cob/cob.h"
#include "cob/vm.h"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
struct Event { bool start; int workId; };

bool run(bool exerciseDisplayVm,const char* scriptPath) {
    bool building=false;
    int workId=0;
    std::vector<Event> events;
    std::vector<std::pair<int,int>> unitValueWrites;
    std::unique_ptr<tak::cob::Vm> vm;
    if(exerciseDisplayVm) {
        auto file=std::make_shared<tak::cob::File>(tak::cob::load(scriptPath));
        vm=std::make_unique<tak::cob::Vm>(std::move(file));
        vm->enableRetailAnimation();
        vm->onSetUnitValue=[&](int id,int value) { unitValueWrites.emplace_back(id,value); };
    }
    const auto update=[&](bool productionQueued,int productionSiteId,bool siteActive,
                          bool walking=false,bool flying=false) {
        tak::updateRetailBuilderAnimation(building,workId,
            0,0,0,productionQueued,productionSiteId,siteActive,walking,flying,
            [&](bool start) {
                const int eventWorkId=workId;
                events.push_back({start,eventWorkId});
                if(vm) {
                    const char* name=start ? "StartBuilding" : "StopBuilding";
                    if(!vm->start(name)) {
                        std::fprintf(stderr,"display COB is missing %s\n",name);
                        return;
                    }
                    // The real display VM delivers SET_UNIT_VALUE at its next
                    // animation step, like GameView's worker-frame advancement.
                    vm->tick(1.0f/30.0f);
                }
            });
    };

    // A non-empty queue at the unit cap has no active site and must not animate.
    update(true,0,false);
    if(building || workId || !events.empty()) {
        std::fprintf(stderr,"queued production without an output site started a build pose\n");
        return false;
    }

    // The active site starts one build pose. Repeated snapshots do not replay it.
    update(true,41,true);
    update(true,41,true);
    if(events.size()!=1 || !events[0].start || events[0].workId!=41) {
        std::fprintf(stderr,"first production site did not create exactly one start edge\n");
        return false;
    }

    // A second output site is a new job even while the queue never becomes empty.
    update(true,42,true);
    if(events.size()!=2 || !events[1].start || events[1].workId!=42) {
        std::fprintf(stderr,"consecutive production site did not restart the build pose\n");
        return false;
    }

    // The queue can remain non-empty while waiting at the unit cap. Stop the
    // active pose as soon as the current site retires; repeated idle ticks stay quiet.
    update(true,0,false);
    update(true,0,false);
    if(events.size()!=3 || events[2].start || events[2].workId!=0 || building) {
        std::fprintf(stderr,"waiting queue failed to stop its stale build pose\n");
        return false;
    }

    if(exerciseDisplayVm && unitValueWrites!=std::vector<std::pair<int,int>>{
            {5,1},{5,1},{5,0}}) {
        std::fprintf(stderr,"real display COB did not receive start/start/stop unit-state call-ins\n");
        return false;
    }

    // Placed construction remains keyed to its actual site id, and a hovering
    // flyer may animate while moving once the site is active.
    update(false,0,false,true,false);
    if(events.size()!=3) {
        std::fprintf(stderr,"ground builder started its pose while still walking\n");
        return false;
    }
    bool placedBuilding=false;int placedWorkId=0;
    tak::updateRetailBuilderAnimation(placedBuilding,placedWorkId,
        73,0,0,false,0,false,false,false,
        [&](bool start) { events.push_back({start,placedWorkId}); });
    if(!placedBuilding || placedWorkId!=73 || events.back().workId!=73) {
        std::fprintf(stderr,"placed build stopped following its site identity\n");
        return false;
    }
    bool flyingBuilding=false;int flyingWorkId=0;
    tak::updateRetailBuilderAnimation(flyingBuilding,flyingWorkId,
        0,0,0,true,44,true,true,true,
        [&](bool start) { events.push_back({start,flyingWorkId}); });
    if(!flyingBuilding || flyingWorkId!=44 || events.back().workId!=44) {
        std::fprintf(stderr,"active flying conjure did not animate while hovering\n");
        return false;
    }

    std::puts(exerciseDisplayVm
        ? "PASS: queued builder animations follow active output-site edges through the real display COB VM"
        : "PASS: queued builder animations start per output site and stop while the queue waits");
    return true;
}
}

int main(int argc,char** argv) {
    {
        using Call=tak::RetailFactoryAnimationCall;
        bool active=false;int site=0;
        std::vector<Call> calls;
        const auto update=[&](bool constructing,bool queued,int output) {
            tak::updateRetailFactoryAnimation(active,site,constructing,queued,output,
                [&](Call call){calls.push_back(call);});
        };
        update(true,true,0); // a queued order cannot open an unfinished factory
        update(false,true,0); // open the yard before an output is ready
        update(false,true,41);
        update(false,true,41); // repeated render snapshot
        update(false,true,42); // consecutive product without an empty queue
        update(false,false,0); // completion then yard closure
        update(false,false,0);
        update(false,true,0); // cancelled before creating an output
        update(false,false,0);
        const std::vector<Call> expected{Call::Activate,Call::StartBuilding,
            Call::StopBuilding,Call::StartBuilding,Call::StopBuilding,Call::Deactivate,
            Call::Activate,Call::Deactivate};
        if(calls!=expected || active || site) {
            std::fprintf(stderr,"factory output/yard callback sequence mismatch\n");return 1;
        }
    }

    if(argc==1)return run(false,nullptr) ? 0 : 1;
    if(argc==2)return run(true,argv[1]) ? 0 : 1;
    std::fprintf(stderr,"usage: retail_builder_animation_test [unit-script.cob]\n");
    return 2;
}
