#include "server/commands.h"
#include <cstdio>
#include <limits>

int main() {
    using namespace tak::net;
    int failures=0;
    auto check=[&](bool ok,const char* name) {
        if (!ok) {std::fprintf(stderr,"FAIL: %s\n",name);++failures;}
    };
    auto batch=[](Command c) {Writer w;w.u32(1);w.cmd(c);return w.b;};
    for (unsigned kind=0;kind<=unsigned(Cmd::BuildManaArea);++kind) {
        Command c;c.kind=Cmd(kind);c.x=16384;c.z=-100;c.x2=200;c.z2=300;
        check(tak::srv::validPlayerCommands(batch(c)),"valid command kind");
    }
    for (float v:{std::numeric_limits<float>::quiet_NaN(),
                  std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),
                  32768.0f,-32769.0f,1e30f}) {
        for (int axis=0;axis<4;++axis) {
            for(Cmd kind:{Cmd::ReclaimArea,Cmd::BuildManaArea}) {
                Command c;c.kind=kind;
                (axis==0?c.x:axis==1?c.z:axis==2?c.x2:c.z2)=v;
                check(!tak::srv::validPlayerCommands(batch(c)),"unsafe coordinate");
            }
        }
    }
    Command c;c.kind=Cmd(255);
    check(!tak::srv::validPlayerCommands(batch(c)),"unknown command");
    for (Cmd kind:{Cmd::Train,Cmd::Unqueue}) {
        c.kind=kind;
        for (int n:{0,1,5,10}) {
            c.targetId=n;
            check(tak::srv::validPlayerCommands(batch(c)),"normal production count");
        }
        for (int n:{-1,11,std::numeric_limits<int>::max()}) {
            c.targetId=n;
            check(!tak::srv::validPlayerCommands(batch(c)),"unsafe production count");
        }
    }
    std::vector<Command> parsed;
    c=Command{};
    Writer large;large.u32(512);
    for(int i=0;i<512;++i){c.unitId=i;large.cmd(c);}
    check(tak::srv::parsePlayerCommands(large.b,parsed) && parsed.size()==512 && parsed.back().unitId==511,"large batch decoded once in order");
    c=Command{};
    c.kind=Cmd::Move;auto bytes=batch(c);
    for(size_t n=0;n<bytes.size();++n)
        check(!tak::srv::validPlayerCommands(std::vector<uint8_t>(bytes.begin(),bytes.begin()+n)),"truncated batch");
    bytes.push_back(0);
    check(!tak::srv::validPlayerCommands(bytes),"trailing bytes");
    Writer w;w.u32(0);
    check(tak::srv::validPlayerCommands(w.b),"empty batch");
    w.b[0]=255;
    check(!tak::srv::validPlayerCommands(w.b),"impossible count");
    return failures?1:0;
}
