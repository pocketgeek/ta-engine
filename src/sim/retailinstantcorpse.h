#pragma once
#include "cob/cob.h"
#include <optional>
#include <vector>

namespace tak::sim {
// Resolve a corpse-1 request from constant operations and, when supplied, the
// death-type argument. Reject other runtime inputs and side effects rather than
// guessing their values. The bounded walk also rejects nonterminating handlers.
inline bool retailConstantKilledCorpse(const cob::File& file,
                                      std::optional<int32_t> deathType,
                                      bool bitmapOnly) {
    const int index=file.scriptIndex("Killed");
    if (index<0) return false;
    size_t pc=file.scripts[size_t(index)].entry;
    std::vector<int32_t> stack;
    int32_t corpse=0;
    for (unsigned steps=0;steps<256 && pc<file.code.size();++steps) {
        const uint32_t op=file.code[pc++];
        switch (op) {
            case 0x10022000: break; // CREATE_LOCAL
            case 0x10021001: // PUSH_CONST
                if (pc>=file.code.size()) return false;
                stack.push_back(int32_t(file.code[pc++]));break;
            case 0x10021002: // Only the supplied death type is a known local input.
                if (!deathType || pc>=file.code.size() || file.code[pc++]!=2) return false;
                stack.push_back(*deathType);break;
            case 0x10023002: // POP_LOCAL: only the corpse out-parameter
                if (pc>=file.code.size() || file.code[pc++]!=1 || stack.empty()) return false;
                corpse=stack.back();stack.pop_back();break;
            case 0x10036000: { // bitwise OR
                if (stack.size()<2) return false;
                const auto value=stack.back();stack.pop_back();stack.back()|=value;break;
            }
            case 0x10055000: { // EQ: authored death-type branches (Veruna towers/factory)
                if (!deathType || stack.size()<2) return false;
                const auto value=stack.back();stack.pop_back();stack.back()=stack.back()==value;break;
            }
            case 0x10064000: // JUMP
                if (!deathType || pc>=file.code.size() || file.code[pc]>=file.code.size()) return false;
                pc=file.code[pc];break;
            case 0x10066000: { // JUMP_IF_FALSE
                if (!deathType || pc>=file.code.size() || stack.empty()) return false;
                const auto target=file.code[pc++];
                const auto value=stack.back();stack.pop_back();
                if (target>=file.code.size()) return false;
                if (!value) pc=target;
                break;
            }
            case 0x10071000: // EXPLODE changes effects, not the corpse out-parameter.
                if (pc>=file.code.size() || stack.empty() || (bitmapOnly && !(stack.back()&32))) return false;
                if (file.code[pc++]>=file.pieces.size()) return false;
                stack.pop_back();break;
            case 0x10065000: return corpse==1;
            default: return false; // unknown state, timers, or asynchronous work
        }
    }
    return false;
}
inline bool retailInstantCorpse(const cob::File& file) {
    // 512860 retires immediately with no Dying callback or owner timer.
    return file.scriptIndex("Dying")<0 && file.scriptIndex("death")<0 &&
           retailConstantKilledCorpse(file,std::nullopt,true);
}
// Independently of owner-animation lifetime, these Killed handlers explicitly
// request corpse 1 after explosion damage. Unknown/stateful scripts retain the
// existing policy; no unit names or speculative GET results are used here.
inline bool retailExplosionCorpse(const cob::File& file) {
    return retailConstantKilledCorpse(file,3,false);
}
}
