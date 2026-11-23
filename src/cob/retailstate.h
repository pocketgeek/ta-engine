#pragma once

#include "cob/retailvm.h"
#include "cob/retailpieces.h"
#include <algorithm>
#include <cstdlib>
#include <optional>

namespace tak::cob {

struct RetailScriptState {
    RetailVm vm;
    std::vector<RetailPiece> pieces;
    uint32_t signature=0;
    explicit RetailScriptState(const File& file):vm(file.numStatics),pieces(file.pieces.size()) {}

    // Rebuild after importing or directly editing piece records. Normal COB
    // commands maintain this derived index; pieces beyond 64 use the full scan.
    void rebuildPieceIndex() {
        activePieces_=0;
        for (size_t i=0;i<std::min(size_t(64),pieces.size());++i)
            if (pieces[i].active) activePieces_|=uint64_t(1)<<i;
    }

    void restore(const File& file,std::span<const uint8_t> bytes,
                 std::optional<uint32_t> expectedSignature={}) {
        if (bytes.size()!=0xa48+file.numStatics*4+file.pieces.size()*0x6c)
            throw std::runtime_error("saved COB state does not fit its script");
        auto word=[&](size_t offset) {
            return uint32_t(bytes[offset]) | uint32_t(bytes[offset+1])<<8 |
                   uint32_t(bytes[offset+2])<<16 | uint32_t(bytes[offset+3])<<24;
        };
        if (expectedSignature && word(0)!=*expectedSignature)
            throw std::runtime_error("saved COB signature mismatch");
        RetailScriptState restored(file);
        restored.signature=word(0);
        for (size_t i=0;i<16;++i) {
            auto& t=restored.vm.threads[i];
            for (size_t n=0;n<41;++n) t.words[n]=word(4+i*0xa4+n*4);
            t.words[8]=0;
            if (t.flags() && (t.pc()>=file.code.size() || t.top()<-1 || t.top()>=32))
                throw std::runtime_error("invalid saved COB thread");
        }
        restored.vm.active=word(0xa44);
        if (restored.vm.active!=size_t(std::count_if(restored.vm.threads.begin(),restored.vm.threads.end(),
                [](const auto& t){return t.words[0]!=0;})))
            throw std::runtime_error("invalid saved COB active count");
        restored.vm.rebuildThreadIndex();
        for (size_t n=0;n<file.numStatics;++n) restored.vm.statics[n]=word(0xa48+n*4);
        for (size_t i=0;i<pieces.size();++i) {
            auto& p=restored.pieces[i]; size_t offset=0xa48+file.numStatics*4+i*0x6c;
            for (auto* values:{&p.moveTarget,&p.moveSpeed,&p.turnTarget,&p.turnSpeed,
                              &p.spinTarget,&p.spinAcceleration,&p.move,&p.turn})
                for (auto& value:*values) { value=std::bit_cast<int32_t>(word(offset)); offset+=4; }
            p.visible=word(offset)!=0; p.cached=word(offset+4)!=0; p.shaded=word(offset+8)!=0;
            p.active=true;
        }
        restored.rebuildPieceIndex();
        *this=std::move(restored);
    }

    template<class Host> struct Adapter {
        RetailScriptState& state; Host& host;
        bool waiting(bool turn,int piece,int axis) {
            if (axis<0 || axis>2) throw std::runtime_error("invalid COB wait axis");
            const auto& p=state.pieces.at(size_t(piece));
            return (turn ? p.turnSpeed : p.moveSpeed)[size_t(axis)]!=0;
        }
        uint32_t random(int32_t bound) { return host.random(bound); }
        uint32_t get(int id,const std::array<uint32_t,4>& args) { return host.get(id,args); }
        void set(int id,int value) { host.set(id,value); }
        uint32_t sound(int name,int32_t priority) { return host.sound(name,priority); }
        // Display hosts may track converted poses. Gameplay hosts have no hook;
        // their instantiation keeps the same piece update and callback ordering.
        void changed(size_t piece) {
            if constexpr(requires { host.pieceChanged(piece); }) host.pieceChanged(piece);
        }
        void piece(uint32_t op,int piece,int axis,int32_t target,int32_t speed) {
            auto& p=state.pieces.at(size_t(piece));
            p.command(op,axis,target,speed,state.vm.ticksPerSecond);
            if constexpr(requires { host.pieceCommanded(size_t(piece)); }) host.pieceCommanded(size_t(piece));
            if (unsigned(piece)<64) {
                const uint64_t bit=uint64_t(1)<<piece;
                if (p.active) state.activePieces_|=bit;
                else state.activePieces_&=~bit;
            }
            changed(size_t(piece));
        }
        void effect(uint32_t op,int piece,int32_t arg) {
            auto& p=state.pieces.at(size_t(piece));
            switch (op) {
            case 0x10005000: p.visible=true; break;
            case 0x10006000: p.visible=false; break;
            case 0x10007000: p.cached=true; break;
            case 0x10008000: p.cached=false; break;
            case 0x10009000: p.rendered=true; break;
            case 0x1000a000: p.rendered=false; break;
            case 0x1000d000: p.shaded=true; break;
            case 0x1000e000: p.shaded=false; break;
            default: host.effect(op,piece,arg); return;
            }
            changed(size_t(piece));
        }
    };

    template<class Host> void tick(const File& file,int32_t elapsed,Host& host) {
        Adapter<Host> adapter{*this,host};
        vm.tick(file,elapsed,adapter);
#ifndef NDEBUG
        static const bool verify=std::getenv("TAK_VERIFY_PIECE_INDEX")!=nullptr;
        if (verify) {
            uint64_t expected=0;
            for (size_t i=0;i<std::min(size_t(64),pieces.size());++i)
                if (pieces[i].active) expected|=uint64_t(1)<<i;
            if (expected!=activePieces_) throw std::runtime_error("COB piece index differs from active flags");
        }
#endif
        if (!elapsed) return;
        uint64_t pending=activePieces_;
        while (pending) {
            const unsigned i=std::countr_zero(pending);
            const uint64_t bit=uint64_t(1)<<i;
            pending&=~bit;
            adapter.changed(i);
            pieces[i].tick(elapsed);
            if (!pieces[i].active) activePieces_&=~bit;
        }
        for (size_t i=64;i<pieces.size();++i) {
            if constexpr(requires { host.pieceChanged(i); })
                if (pieces[i].active) adapter.changed(i);
            pieces[i].tick(elapsed);
        }
    }

    // Display only, never the simulation: tick() in the same order, except
    // that `admit(slot)` is asked before each thread runs (false holds it for
    // this step, its sleep or wait unadvanced) and the `held` pieces (0-63)
    // keep their motion for a later step. Holding nothing is exactly tick().
    template<class Host,class Admit> void tickGated(const File& file,int32_t elapsed,Host& host,
                                                   Admit&& admit,uint64_t held) {
        Adapter<Host> adapter{*this,host};
        uint32_t remaining=0xffffu;
        while(const uint32_t pending=vm.activeThreadMask()&remaining) {
            const unsigned slot=std::countr_zero(pending);
            if(admit(slot))vm.run(file,slot,elapsed,adapter);
            remaining=0xffffu<<(slot+1);
        }
        if (!elapsed) return;
        uint64_t pending=activePieces_&~held;
        while (pending) {
            const unsigned i=std::countr_zero(pending);
            const uint64_t bit=uint64_t(1)<<i;
            pending&=~bit;
            adapter.changed(i);
            pieces[i].tick(elapsed);
            if (!pieces[i].active) activePieces_&=~bit;
        }
        for (size_t i=64;i<pieces.size();++i) {
            if constexpr(requires { host.pieceChanged(i); })
                if (pieces[i].active) adapter.changed(i);
            pieces[i].tick(elapsed);
        }
    }

    // 56c5f0 with immediate=1 starts a notification and runs ALL threads
    // with zero elapsed time, including the piece update. Queries below run
    // only their own thread and therefore cannot substitute for this path.
    template<class Host> bool notify(const File& file,int script,Host& host) {
        if (vm.start(file,script)<0) return false;
        tick(file,0,host);
        return true;
    }

    // 56c680 installs four argument words and exposes only count stack slots.
    bool startArguments(const File& file,int script,const std::array<uint32_t,4>& args,unsigned count) {
        if (count>4) throw std::invalid_argument("too many script arguments");
        const int slot=vm.start(file,script,args);
        if (slot<0) return false;
        vm.threads[size_t(slot)].words[2]=count-1u;
        return true;
    }

    // The derived active-piece index (pieces 0-63), for instruments.
    uint64_t activePieceMask() const { return activePieces_; }

    template<class Host> bool query(const File& file,int script,std::array<uint32_t,4>& args,Host& host) {
        const int slot=vm.start(file,script,args);
        if (slot<0) return false;
        auto& t=vm.threads[size_t(slot)]; t.words[2]=3;
        Adapter<Host> adapter{*this,host};
        vm.run(file,size_t(slot),0,adapter);
        for (int i=0;i<4;++i) args[size_t(i)]=t.local(i);
        return true;
    }
private:
    uint64_t activePieces_=0;
};

} // namespace tak::cob
