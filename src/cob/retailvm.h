#pragma once

#include "cob/cob.h"
#include <array>
#include <bit>
#include <cstdlib>
#include <span>
#include <stdexcept>

namespace tak::cob {

// Integer thread interpreter observed at 56c540 / 56c870 / 56c8b0.
// Piece operations and engine queries belong to the host. Unlike the display
// VM, a CALL occupies another one of the sixteen slots and suspends its caller.
class RetailVm {
public:
    struct Thread {
        std::array<uint32_t,41> words{};
        uint32_t& flags() { return words[0]; }
        uint32_t& pc() { return words[1]; }
        int32_t top() const { return std::bit_cast<int32_t>(words[2]); }
        uint32_t& local(int index) {
            if (index<0 || index>=32) throw std::runtime_error("COB stack index out of range");
            return words[size_t(index)+9];
        }
        void push(uint32_t value) { const int index=top()+1; local(index)=value; words[2]=uint32_t(index); }
        uint32_t pop() { const auto value=local(top()); --words[2]; return value; }
    };
    std::array<Thread,16> threads{};
    std::vector<uint32_t> statics;
    uint32_t active=0;
    int32_t ticksPerSecond=30;

    explicit RetailVm(size_t staticCount=0):statics(staticCount) {}

    // Derived from flags, never serialized or hashed. Rebuild after importing
    // raw thread records; ordinary start/finish operations maintain it.
    void rebuildThreadIndex() {
        threadMask_=0;
        for(size_t i=0;i<threads.size();++i)
            if(threads[i].flags())threadMask_|=uint32_t(1)<<i;
    }
    void clearThreads() {
        for(auto& thread:threads)thread.flags()=0;
        active=0;threadMask_=0;
    }
    uint32_t activeThreadMask() const { return threadMask_; }

    int start(const File& file,int script,std::span<const uint32_t> args={}) {
        if (script<0 || size_t(script)>=file.scripts.size()) return -1;
        const uint32_t free=~threadMask_&0xffffu;
        if(free) {
            const size_t i=std::countr_zero(free);
            auto& t=threads[i];
            t.flags()=0x1000000; t.pc()=file.scripts[size_t(script)].entry;
            t.words[2]=0xffffffffu; t.words[8]=0; t.words[7]=1;
            // Arguments occupy locals, but CREATE_LOCAL owns the stack top.
            for (size_t n=0;n<args.size();++n) t.local(int(n))=args[n];
            ++active;
            threadMask_|=uint32_t(1)<<i;
            return int(i);
        }
        return -1;
    }

    void finish(size_t slot) {
        auto& t=threads.at(slot);
        if (!t.flags()) return;
        t.flags()=0; --active;
        threadMask_&=~(uint32_t(1)<<slot);
        for(uint32_t pending=threadMask_;pending;pending&=pending-1) {
            auto& parent=threads[std::countr_zero(pending)];
            if ((parent.flags()&0xfff00000u)==0x2800000 && parent.words[6]==slot)
                parent.flags()=0x1000000;
        }
    }

    // Host: waiting(turn,piece,axis), random(bound), get(value,args),
    // set(value,amount), piece(op,piece,axis,target,speed), effect(op,piece,arg).
    template<class Host>
    void run(const File& file,size_t slot,int32_t elapsed,Host& host) {
        auto& t=threads.at(slot);
        if ((t.flags()&0xff000000u)==0x2000000) {
            const auto wait=t.flags()&0xf00000u;
            if (wait==0x400000) {
                t.words[3]-=uint32_t(elapsed);
                if (std::bit_cast<int32_t>(t.words[3])<=0) t.flags()=0x1000000;
            } else if ((wait==0x100000 || wait==0x200000) &&
                       !host.waiting(wait==0x100000,int(t.words[4]),int(t.words[5])))
                t.flags()=0x1000000;
        }
        if ((t.flags()&0xff000000u)!=0x1000000) return;
        auto signedWord=[](uint32_t x) { return std::bit_cast<int32_t>(x); };
        for (unsigned steps=0;steps<100000;++steps) {
            const auto pc=t.pc();
            const auto op=file.code.at(pc);
            auto arg=[&](unsigned n) { return file.code.at(size_t(pc)+1+n); };
            auto binary=[&](auto fn) { auto b=t.pop(),a=t.pop(); t.push(uint32_t(fn(a,b))); };
            switch (op) {
            case 0x10021001: t.push(arg(0)); t.pc()+=2; break;
            case 0x10021002: t.push(t.local(int(arg(0)))); t.pc()+=2; break;
            case 0x10021004: t.push(statics.at(arg(0))); t.pc()+=2; break;
            case 0x10022000: t.local(t.top()+1); ++t.words[2]; ++t.pc(); break;
            case 0x10023002: { auto v=t.pop(); t.local(int(arg(0)))=v; t.pc()+=2; break; }
            case 0x10023004: statics.at(arg(0))=t.pop(); t.pc()+=2; break;
            case 0x10024000: t.pop(); ++t.pc(); break;
            case 0x10031000: binary([](auto a,auto b){return a+b;}); ++t.pc(); break;
            case 0x10032000: binary([](auto a,auto b){return a-b;}); ++t.pc(); break;
            case 0x10033000: binary([](auto a,auto b){return a*b;}); ++t.pc(); break;
            case 0x10034000: case 0x1003b000: {
                const int64_t b=signedWord(t.pop()),a=signedWord(t.pop());
                if (!b || (a==INT32_MIN && b==-1)) throw std::runtime_error("COB division fault");
                t.push(uint32_t(op==0x10034000 ? a/b : a%b)); ++t.pc(); break;
            }
            case 0x10035000: binary([](auto a,auto b){return a&b;}); ++t.pc(); break;
            case 0x10036000: binary([](auto a,auto b){return a|b;}); ++t.pc(); break;
            case 0x10037000: case 0x10059000: binary([](auto a,auto b){return a^b;}); ++t.pc(); break;
            case 0x10038000: t.push(~t.pop()); ++t.pc(); break;
            case 0x10039000: binary([](auto a,auto b){return a<<(b&31);}); ++t.pc(); break;
            case 0x1003a000: binary([&](auto a,auto b){return signedWord(a)>>(b&31);}); ++t.pc(); break;
            case 0x10041000: {
                auto hi=t.pop(),lo=t.pop(); t.push(lo+host.random(signedWord(hi-lo+1))); ++t.pc(); break;
            }
            case 0x10042000: { auto id=t.pop(); t.push(host.get(signedWord(id),std::array<uint32_t,4>{})); ++t.pc(); break; }
            case 0x10043000: {
                std::array<uint32_t,4> args;
                for (int i=3;i>=0;--i) args[size_t(i)]=t.pop();
                auto id=t.pop(); t.push(host.get(signedWord(id),args)); ++t.pc(); break;
            }
            case 0x10051000: binary([&](auto a,auto b){return signedWord(a)<signedWord(b);}); ++t.pc(); break;
            case 0x10052000: binary([&](auto a,auto b){return signedWord(a)<=signedWord(b);}); ++t.pc(); break;
            case 0x10053000: binary([&](auto a,auto b){return signedWord(a)>signedWord(b);}); ++t.pc(); break;
            case 0x10054000: binary([&](auto a,auto b){return signedWord(a)>=signedWord(b);}); ++t.pc(); break;
            case 0x10055000: binary([](auto a,auto b){return a==b;}); ++t.pc(); break;
            case 0x10056000: binary([](auto a,auto b){return a!=b;}); ++t.pc(); break;
            case 0x10057000: binary([](auto a,auto b){return a&&b;}); ++t.pc(); break;
            case 0x10058000: binary([](auto a,auto b){return a||b;}); ++t.pc(); break;
            case 0x1005a000: t.push(!t.pop()); ++t.pc(); break;
            case 0x10061000: case 0x10062000: {
                const int child=start(file,int(arg(0)));
                if (child>=0) {
                    for (int i=int(arg(1))-1;i>=0;--i) threads[size_t(child)].local(i)=t.pop();
                    threads[size_t(child)].words[7]=t.words[7];
                }
                t.pc()+=3;
                if (op==0x10062000) { t.words[6]=uint32_t(child); t.flags()=0x2800000; return; }
                break;
            }
            case 0x10064000: t.pc()=arg(0); break;
            case 0x10065000: finish(slot); return;
            case 0x10066000: t.pc()=t.pop() ? pc+2 : arg(0); break;
            case 0x10067000: {
                const auto mask=t.pop();
                for (size_t i=0;i<threads.size();++i)
                    if (threads[i].flags() && (threads[i].words[7]&mask)) finish(i);
                ++t.pc(); if (!t.flags()) return; break;
            }
            case 0x10068000: t.words[7]=t.pop(); ++t.pc(); break;
            case 0x10013000: {
                const auto milliseconds=t.pop();
                const int32_t product=signedWord(milliseconds*uint32_t(ticksPerSecond));
                t.words[3]=uint32_t(product/1000); t.flags()=0x2400000; ++t.pc(); return;
            }
            case 0x10011000: case 0x10012000:
                t.words[4]=arg(0); t.words[5]=arg(1); t.pc()+=3;
                t.flags()=op==0x10011000 ? 0x2100000 : 0x2200000; return;
            case 0x10082000: { auto value=t.pop(),id=t.pop(); host.set(signedWord(id),signedWord(value)); ++t.pc(); break; }
            case 0x10001000: case 0x10002000: case 0x10003000: case 0x10004000:
            case 0x1000b000: case 0x1000c000: {
                auto target=t.pop();
                auto speed=(op==0x10001000 || op==0x10002000 || op==0x10003000) ? t.pop() : 0;
                host.piece(op,int(arg(0)),int(arg(1)),signedWord(target),signedWord(speed)); t.pc()+=3; break;
            }
            case 0x10005000: case 0x10006000: case 0x10007000: case 0x10008000:
            case 0x10009000: case 0x1000a000: case 0x1000d000: case 0x1000e000:
                host.effect(op,int(arg(0)),0); t.pc()+=2; break;
            case 0x1000f000: case 0x10071000:
                host.effect(op,int(arg(0)),signedWord(t.pop())); t.pc()+=2; break;
            case 0x10072000: {
                const auto priority=signedWord(t.pop());
                t.push(host.sound(int(arg(0)),priority)); t.pc()+=2; break;
            }
            default: throw std::runtime_error("unsupported simulation COB opcode "+std::to_string(op));
            }
        }
        throw std::runtime_error("simulation COB instruction budget exceeded");
    }

    template<class Host> void tick(const File& file,int32_t elapsed,Host& host,bool reference=false) {
#ifndef NDEBUG
        static const bool verify=std::getenv("TAK_VERIFY_THREAD_INDEX")!=nullptr;
        if(verify) {
            uint32_t expected=0;
            for(size_t i=0;i<threads.size();++i)if(threads[i].flags())expected|=uint32_t(1)<<i;
            if(expected!=threadMask_)throw std::runtime_error("COB thread index differs from active flags");
        }
#endif
        if(!active)return;
        if(reference) {
            for(size_t i=0;i<threads.size();++i)if(threads[i].flags())run(file,i,elapsed,host);
            return;
        }
        // Re-read after each run: a child started in a later slot executes this
        // tick, but a newly reused earlier slot waits until the next tick.
        uint32_t remaining=0xffffu;
        while(const uint32_t pending=threadMask_&remaining) {
            const unsigned slot=std::countr_zero(pending);
            run(file,slot,elapsed,host);
            remaining=0xffffu<<(slot+1);
        }
    }
private:
    uint32_t threadMask_=0;
};

} // namespace tak::cob
