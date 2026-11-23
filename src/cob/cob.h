#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tak::cob {

// COB unit-script bytecode (TAK version 6; same opcode set as TA's v4 with
// a 13-word header). Scripts animate 3DO pieces by name.

struct Script {
    std::string name;
    uint32_t entry = 0;      // word offset into code
};

// The code an engine-started Activate waits out (see createHold). Empty when
// nothing waits.
struct CreateHold {
    uint32_t begin = 0, end = 0;
    bool contains(uint32_t pc) const { return pc >= begin && pc < end; }
};

struct File {
    std::vector<Script> scripts;
    std::vector<std::string> pieces;   // index = piece number
    // Per-cob string/name table (COB v6 header 0x2c/0x30). PLAY_SOUND and the mission
    // MAP_COMMAND opcode index into this: for unit cobs it holds sound names, for
    // mission "god" cobs it holds the command strings ("create NPCEMEN", "SetTrigger",
    // "SetMission m 133 68", "<mission>.wav", ...). See docs/campaign-design.md.
    std::vector<std::string> names;
    std::vector<uint32_t> code;        // 32-bit word stream
    uint32_t numStatics = 0;
    CreateHold createHold;             // createHold(*this), filled by load()

    int scriptIndex(const std::string& name) const;   // -1 if absent
    // Resolve a name-table index (PLAY_SOUND / MAP_COMMAND operand); "" if out of range.
    const std::string& name(uint32_t idx) const {
        static const std::string empty;
        return idx < names.size() ? names[idx] : empty;
    }
};

File load(const std::filesystem::path& path);
// Parse from an in-memory buffer (a VFS-resolved archive entry).
File load(const std::vector<uint8_t>& d, const std::string& origin = "<memory>");

// Where Create still holds back an Activate the engine starts on a finished
// unit's completion or production (World::switchScript). A factory's Create
// waits in a sleep loop for BUILD_PERCENT_LEFT to reach 0 and only then runs
// InitState, which sets the RequestState bookkeeping that Activate's
// RequestState(0) relies on; started before that, Activate finds the yard
// "already on" and never opens it. Every shipped Create (204 scripts) can
// return, so the hold is the whole of Create: a thread whose pc lies in it,
// including Create waiting on a script it CALLed. A Create that cannot return
// (no RETURN reachable from its entry, an endless loop) would hold forever,
// so for it the hold ends where its build-wait does: the exit of the first
// JUMP_IF_FALSE after a BUILD_PERCENT_LEFT read (no build-wait: no hold).
CreateHold createHold(const File& f);

// Disassemble one script (by index) to text.
std::string disassemble(const File& f, int script);
// Conservative control-flow reachability, indexed by code word. Unknown or
// malformed instructions are marked unsafe along with EXPLODE itself.
std::vector<uint8_t> explosionReachability(const File& f);
// Display-only gait map, indexed by code word: 1 inside a script that plays a
// movement cycle (walk*, def_walk, swim*, stumble*, row/slowrow/fastrow, drive,
// wheel_*, SuperDynamicWheelSpinner), never a *Control poller, flight or idle
// script. A CALL gives the callee its own thread slot, so a thread's pc names
// the cycle it is playing. Empty when the script has no such cycle.
std::vector<uint8_t> gaitCode(const File& f);

} // namespace tak::cob
