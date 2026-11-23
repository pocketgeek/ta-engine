#pragma once
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace tak::srv {
struct Limits {
    uint64_t rooms=16,running=4,accounts=10000;
    uint64_t mapMemory=512ull<<20,mapDisk=4ull<<30;
    uint64_t replayMemory=256ull<<20,replayDisk=4ull<<30;
    bool set(std::string_view name,std::string_view value) {
        uint64_t* target=nullptr;uint64_t scale=1,maximum=10000;
        if(name=="--max-games") {target=&rooms;maximum=64;}
        else if(name=="--max-running-games") {target=&running;maximum=64;}
        else if(name=="--max-accounts") {target=&accounts;maximum=100000;}
        else {
            scale=1ull<<20;maximum=65536;
            if(name=="--map-memory-mib")target=&mapMemory;
            else if(name=="--map-storage-mib")target=&mapDisk;
            else if(name=="--replay-memory-mib")target=&replayMemory;
            else if(name=="--replay-storage-mib")target=&replayDisk;
        }
        if(!target)return false;
        uint64_t n=0;auto parsed=std::from_chars(value.data(),value.data()+value.size(),n);
        if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || !n || n>maximum)
            throw std::runtime_error("invalid server resource limit");
        *target=n*scale;return true;
    }
};
}
