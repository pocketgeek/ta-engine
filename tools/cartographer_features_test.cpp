#include "cartographer/features.h"
#include "hpi/hpi.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

static void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
int main() try {
    using namespace tak;
    auto files=std::make_shared<hpi::Vfs::Files>();
    std::vector<uint8_t> gaf(92);
    auto u16=[&](size_t at,uint16_t value) {gaf[at]=uint8_t(value);gaf[at+1]=uint8_t(value>>8);};
    auto u32=[&](size_t at,uint32_t value) {u16(at,uint16_t(value));u16(at+2,uint16_t(value>>16));};
    u32(0,0x00010100);u32(4,1);u32(12,16);u16(16,1);
    std::memcpy(gaf.data()+24,"test_sprite",11);u32(56,64);u32(60,2);
    u16(64,2);u16(66,2);u16(68,uint16_t(-1));u16(70,3);u32(80,88);
    gaf[88]=gaf[89]=gaf[90]=1;gaf[91]=0; // Last pixel is transparent.
    (*files)["anims/test.gaf"]=gaf;
    const std::string definition="[test] {\ncategory=trees;\nfilename=test;\nseqname=test_sprite;\nworld=aramon;\n}";
    (*files)["features/aramon/test.tdf"]={definition.begin(),definition.end()};
    auto& palette=(*files)["palettes/aramon_features.pcx"];palette.resize(1024);
    palette[4]=210;palette[5]=45;palette[6]=90;
    hpi::Vfs vfs;vfs.setMapFiles(files);
    cart::FeatureRef ref{"test","trees","test","test_sprite","aramon"};
    cart::FeatureLibrary sync,async;
    const auto expected=*sync.sprite(vfs,ref);
    check(expected.w==2 && expected.h==2 && expected.xoff==-1 && expected.yoff==3 &&
          expected.rgba[0]==210 && expected.rgba[15]==0,"synthetic frame/palette/anchor decode");
    auto wait=[&](cart::FeatureLibrary& library,const cart::FeatureRef& feature) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        for(;;) {
            if(const auto* result=library.requestSprite(vfs,feature))return result;
            check(std::chrono::steady_clock::now()<deadline,"feature worker did not finish");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    check(!async.requestSprite(vfs,ref) && async.loading(),"first request schedules work without decoding inline");
    const auto* actual=wait(async,ref);
    check(actual->rgba==expected.rgba && actual->w==expected.w && actual->h==expected.h &&
          actual->xoff==expected.xoff && actual->yoff==expected.yoff,"worker pixels/anchor differ from synchronous decode");
    check(async.requestSprite(vfs,ref)==actual && !async.loading(),"completed sprite is reused without restarting work");
    auto missing=ref;missing.name="missing";missing.filename="absent";
    check(wait(async,missing)->w==0 && !async.loading(),"missing art becomes a completed empty sprite");
    check(async.requestSprite(vfs,missing)->w==0 && !async.loading(),"missing art failure is cached");
    auto copied=ref;copied.name="copied";
    check(!async.requestSprite(vfs,copied),"copied request starts asynchronously");
    copied.filename="absent"; // The worker must own the original descriptor.
    check(wait(async,copied)->rgba==expected.rgba,"worker borrowed mutable feature descriptor");
    auto pending=ref;pending.name="pending";async.requestSprite(vfs,pending);
    async.quiesce();check(!async.loading(),"quiesce must join before VFS replacement");
    auto replacement=std::make_shared<hpi::Vfs::Files>(*files);
    (*replacement)["palettes/aramon_features.pcx"][4]=31;
    vfs.setMapFiles(replacement);async.scan(vfs,"aramon");
    check(wait(async,pending)->rgba[0]==31,"old worker result leaked after changing map assets");
    check(async.byName("TEST") && async.byName("TEST")->filename=="test","indexed feature lookup preserves case-insensitive names");
    async.scan(vfs,"zhon");check(!async.byName("test"),"old world feature index leaked after rescan");
    pending.name="destruction";
    {cart::FeatureLibrary closing;closing.requestSprite(vfs,pending);} // Joins before borrowed VFS dies.
    std::cout<<"PASS: asynchronous feature pixels, anchors, failures, copied requests and source replacement\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
