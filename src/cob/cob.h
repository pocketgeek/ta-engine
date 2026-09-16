#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ta::cob {

// COB unit-script bytecode (TAK version 6; same opcode set as TA's v4 with
// a 13-word header). Scripts animate 3DO pieces by name.

struct Script {
    std::string name;
    uint32_t entry = 0;      // word offset into code
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

// Disassemble one script (by index) to text.
std::string disassemble(const File& f, int script);

} // namespace ta::cob
