#include "client/modelmath.h"
#include "client/retaildebrismodel.h"
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

static std::array<float,3> point(std::span<const std::array<int32_t,15>> chain) {
    std::vector<ModelEmissionPose> poses;
    for(const auto& v:chain) {
        ModelEmissionPose pose;
        for(size_t axis=0;axis<3;++axis) {
            pose.offset[axis]=v[axis];
            pose.move[axis]=v[3+axis];
            pose.turn[axis]=uint16_t(v[6+axis]);
        }
        poses.push_back(pose);
    }
    const auto& root=chain.front();
    const auto& leaf=chain.back();
    return modelEmissionPoint(poses,{leaf[9],leaf[10],leaf[11]},
        uint16_t(root[13]),uint16_t(root[12]),uint16_t(root[14]));
}
int main(int argc,char** argv) {
    {
        tak::tdo::Object source;
        source.name="arm";source.x=3;source.y=4;source.z=5;
        source.offsetRaw={3*65536,4*65536,5*65536};
        source.vertices={1,2,3};source.verticesRaw={{65536,131072,196608}};
        source.primitives.push_back({7,"metal",{0}});
        source.children.resize(2);source.children[0].x=17;
        source.children[0].children.resize(1);
        auto single=tak::retailDebrisModel(source,false);
        auto tree=tak::retailDebrisModel(source,true);
        source.vertices[0]=99;source.primitives[0].texture="changed";
        source.children[0].x=99;source.children.clear();
        if(single.name!="arm" || single.x || single.y || single.z ||
           single.offsetRaw!=std::array<int32_t,3>{} || !single.children.empty() ||
           single.vertices[0]!=1 || single.verticesRaw[0][0]!=65536 ||
           single.primitives[0].texture!="metal" || tree.children.size()!=2 ||
           tree.children[0].x!=17 || tree.children[0].children.size()!=1)return 1;
    }
    if(modelEmissionFixed(32768.f)!=INT32_MIN || modelEmissionFixed(65536.f)!=0 ||
       modelEmissionFixed(-32769.f)!=2147418112 ||
       modelEmissionFixed(0.5f/65536.f)!=0 || modelEmissionFixed(1.5f/65536.f)!=2 ||
       modelEmissionFixed(INFINITY)!=0 || modelEmissionFixed(NAN)!=0)return 1;
    const bool chainMode=argc==2 && std::strcmp(argv[1],"--chain")==0;
    if(chainMode || (argc==2 && std::strcmp(argv[1],"--points")==0)) {
        for(;;) {
            unsigned count=1;
            if(chainMode && std::scanf("%u",&count)!=1)break;
            if(!count || count>256)return 2;
            std::vector<std::array<int32_t,15>> values(count);
            if(std::scanf("%d",&values[0][0])!=1)break;
            for(size_t n=0;n<count;++n)
                for(size_t i=n ? 0:1;i<15;++i)if(std::scanf("%d",&values[n][i])!=1)return 2;
            const auto result=point(values);
            std::printf("%.9g %.9g %.9g\n",result[0],result[1],result[2]);
        }
        return 0;
    }
    auto check=[](int field,int value,std::array<float,3> expected) {
        std::array<int32_t,15> values{};values[13]=32768;values[9]=65536;values[size_t(field)]=value;
        const auto result=point(std::span(&values,1));
        for(size_t axis=0;axis<3;++axis)if(std::abs(result[axis]-expected[axis])>0.0001f)return false;
        return true;
    };
    if(!check(3,65536,{0,0,0}) || !check(5,65536,{1,0,-1}) ||
       !check(8,16384,{0,-1,0}) || !check(14,16384,{0,-1,0}) ||
       !check(7,16384,{0,0,1}) || !check(13,0,{-1,0,0}))return 1;
    std::puts("PASS: native model translation, piece rotation and body roll directions");
}
