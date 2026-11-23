// The lobby's full-window map view (BigMapView) against a change of the data set it
// reads. Its MapView decodes and composites terrain on two workers that read the game's
// VFS through a view borrowing it, so GameView closes it before the data set is remounted
// or destroyed (stopPreviewReaders). Here a read gate holds those workers at a read:
// closing the view must wait for them, and once it has, the data set can be replaced with
// no reader left in it -- the released workers read on through it until they stop. Both
// ways openBigMap opens a view (an installed map by path; a generated one, rolled on the
// main thread through a map-resource-free view), each drawn once so the chunk worker runs
// too, and twice over: the second time on the remounted data set. Build it with
// -fsanitize=thread to check the hand-over (tools/preview_lifetime_test.py covers the
// GameView side end to end).
#include "client/bigmapview.h"
#include "client/gpuvram.h"
#include "client/vfsreadgate.h"
#include "hpi/hpi.h"
#include "tnt/mapgen.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
static void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv) try {
    if(argc!=2)return 2;
    using namespace std::chrono_literals;
    SDL_SetMainReady();
    SDL_SetHint("SDL_SHUTDOWN_DBUS_ON_QUIT","1"); // release SDL's process-global DBus allocations for leak checks
    check(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
    auto* window=SDL_CreateWindow("big map",0,0,640,480,SDL_WINDOW_HIDDEN);
    auto* renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_TARGETTEXTURE);
    if(!renderer)renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
    check(renderer,SDL_GetError());
    const auto baseline=gpuvram::bytes();
    auto vfs=tak::hpi::mountRetailRoot(argv[1]);
    for(int round=0;round<2;++round) {
        std::vector<std::unique_ptr<BigMapView>> views;
        tak::ReadGate gate(vfs);   // after `views`: on a failure it lets the workers go first
        gate.arm();
        views.push_back(std::make_unique<BigMapView>(renderer,std::make_unique<tak::hpi::Vfs>(&vfs),
                                                     tak::hpi::findMap(vfs,"Inner Circle"),"aramon"));
        {
            tak::mapgen::Params params;
            params.seed=uint64_t(7+round);
            auto view=std::make_unique<tak::hpi::Vfs>(&vfs,true);
            auto map=tak::mapgen::generate(params,*view).map;
            views.push_back(std::make_unique<BigMapView>(renderer,std::move(view),std::move(map),"aramon"));
        }
        for(auto& view:views)view->draw({0,0,640,480});
        check(gate.waitHeld(views.size(),60s),"each view's section worker reached a read");
        std::fprintf(stderr,"round %d: %zu reads held\n",round,gate.held());
        std::atomic<bool> closed{false};
        bool closedWhileHeld=false;
        std::thread releaser([&] {
            std::this_thread::sleep_for(300ms);
            closedWhileHeld=closed.load();
            gate.release();
        });
        views.clear();   // what stopPreviewReaders does to bigMap_
        closed=true;
        releaser.join();
        check(!closedWhileHeld,"closing a view waits for its workers held at a read");
        check(gate.held()==0 && !gate.gone(),"the data set is intact once the views are closed");
        vfs=tak::hpi::mountRetailRoot(argv[1]);   // the remount
        check(gate.gone(),"the remount drops the old data set");
        const auto at=gate.goneState();
        check(at.held==0 && at.released,"no reader was left in the old data set");
        check(at.lateReads>0,"the released workers read on through the old data set");
        std::fprintf(stderr,"round %d: data set replaced after %zu late reads\n",round,at.lateReads);
    }
    check(gpuvram::bytes()==baseline,"the views free their textures");
    SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    std::puts("PASS: full-window views hold the data set until their workers stop; remounts after");return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
