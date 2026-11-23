#include "cob/cob.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace tak::cob {

namespace {

uint32_t u32(const std::vector<uint8_t>& d, size_t off) {
    if (off + 4 > d.size()) throw std::runtime_error("COB read out of range");
    return d[off] | (d[off + 1] << 8) | (d[off + 2] << 16) | (uint32_t(d[off + 3]) << 24);
}

std::string cstr(const std::vector<uint8_t>& d, size_t off) {
    if (off >= d.size()) throw std::runtime_error("COB string out of range");
    const char* s = reinterpret_cast<const char*>(&d[off]);
    return std::string(s, strnlen(s, d.size() - off));
}

bool iequal(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
           });
}

} // namespace

File load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::vector<uint8_t> d(std::filesystem::file_size(path));
    in.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(d.size()));
    return load(d, path.string());
}

File load(const std::vector<uint8_t>& d, const std::string& origin) {
    uint32_t version = u32(d, 0);
    if (version != 4 && version != 6)
        throw std::runtime_error(origin + ": unsupported COB version " +
                                 std::to_string(version));
    uint32_t numScripts = u32(d, 4);
    uint32_t numPieces = u32(d, 8);
    uint32_t codeWords = u32(d, 12);
    uint32_t numStatics = u32(d, 16);
    uint32_t offIndex = u32(d, 24);
    uint32_t offScriptNames = u32(d, 28);
    uint32_t offPieceNames = u32(d, 32);
    uint32_t offCode = u32(d, 36);

    File f;
    f.numStatics = numStatics;
    f.code.resize(codeWords);
    for (uint32_t i = 0; i < codeWords; ++i) f.code[i] = u32(d, offCode + i * 4);

    for (uint32_t i = 0; i < numScripts; ++i) {
        Script s;
        s.name = cstr(d, u32(d, offScriptNames + i * 4));
        s.entry = u32(d, offIndex + i * 4);
        f.scripts.push_back(std::move(s));
    }
    for (uint32_t i = 0; i < numPieces; ++i)
        f.pieces.push_back(cstr(d, u32(d, offPieceNames + i * 4)));

    // COB v6 name/string table (two extra header words past offCode): 0x2c = offset to
    // the name-offset array, 0x30 = its count. Indexed by PLAY_SOUND and the mission
    // MAP_COMMAND opcode. Bounds-guarded (absent/garbage on older/odd cobs -> empty).
    uint32_t offNames = u32(d, 0x2c);
    uint32_t numNames = u32(d, 0x30);
    if (numNames <= 4096 && offNames > 0 &&
        size_t(offNames) + size_t(numNames) * 4 <= d.size()) {
        f.names.reserve(numNames);
        for (uint32_t i = 0; i < numNames; ++i)
            f.names.push_back(cstr(d, u32(d, offNames + i * 4)));
    }
    f.createHold = createHold(f);
    return f;
}

int File::scriptIndex(const std::string& name) const {
    for (size_t i = 0; i < scripts.size(); ++i)
        if (iequal(scripts[i].name, name)) return int(i);
    return -1;
}

namespace {

struct OpInfo {
    uint32_t op;
    const char* name;
    int args;   // trailing argument words
};

// TA/TAK COB opcode set.
const OpInfo kOps[] = {
    {0x10001000, "MOVE", 2},          // piece, axis; target+speed on stack
    {0x10002000, "TURN", 2},
    {0x10003000, "SPIN", 2},
    {0x10004000, "STOP_SPIN", 2},
    {0x10005000, "SHOW", 1},
    {0x10006000, "HIDE", 1},
    {0x10007000, "CACHE", 1},
    {0x10008000, "DONT_CACHE", 1},
    {0x10009000, "RENDER_ON", 1},     // 1-arg cosmetic piece render flags (ignored)
    {0x1000A000, "RENDER_OFF", 1},
    {0x1000B000, "MOVE_NOW", 2},
    {0x1000C000, "TURN_NOW", 2},
    {0x1000D000, "SHADE", 1},
    {0x1000E000, "DONT_SHADE", 1},
    {0x1000F000, "EMIT_SFX", 1},      // pops the packed sfx-type code
    {0x10011000, "WAIT_TURN", 2},
    {0x10012000, "WAIT_MOVE", 2},
    {0x10013000, "SLEEP", 0},
    {0x10021001, "PUSH_CONST", 1},
    {0x10021002, "PUSH_LOCAL", 1},
    {0x10021004, "PUSH_STATIC", 1},
    {0x10022000, "CREATE_LOCAL", 0},
    {0x10023002, "POP_LOCAL", 1},
    {0x10023004, "POP_STATIC", 1},
    {0x10024000, "POP_STACK", 0},
    {0x10031000, "ADD", 0},
    {0x10032000, "SUB", 0},
    {0x10033000, "MUL", 0},
    {0x10034000, "DIV", 0},
    {0x10035000, "AND", 0},           // TAK numbering (retail KINGDOMS.icd @0x56c966):
    {0x10036000, "OR", 0},            // 5=AND 6=OR 7=XOR 8=NOT 9=SHL A=SHR B=MOD
    {0x10037000, "XOR", 0},
    {0x10038000, "NOT", 0},           // unary
    {0x10039000, "SHL", 0},
    {0x1003A000, "SHR", 0},
    {0x1003B000, "MOD", 0},
    {0x10041000, "RAND", 0},
    {0x10042000, "GET_UNIT_VALUE", 0},
    {0x10043000, "GET", 0},
    {0x10044000, "GET_Q1", 0},        // engine queries (pop1/push1, pop0/push1)
    {0x10045000, "GET_Q0", 0},
    {0x10051000, "LESS", 0},
    {0x10052000, "LESS_EQ", 0},
    {0x10053000, "GREATER", 0},
    {0x10054000, "GREATER_EQ", 0},
    {0x10055000, "EQ", 0},
    {0x10056000, "NOT_EQ", 0},
    {0x10057000, "AND", 0},
    {0x10058000, "OR", 0},
    {0x10059000, "XOR", 0},
    {0x1005A000, "NOT", 0},
    {0x10061000, "START_SCRIPT", 2},  // script, paramCount
    {0x10062000, "CALL_SCRIPT", 2},
    {0x10063000, "START_CALL_V", 2},  // start/call variant (absent from TAK data)
    {0x10064000, "JUMP", 1},
    {0x10065000, "RETURN", 0},
    {0x10066000, "JUMP_IF_FALSE", 1},
    {0x10067000, "SIGNAL", 0},
    {0x10068000, "SET_SIGNAL_MASK", 0},
    {0x10071000, "EXPLODE", 1},
    {0x10072000, "PLAY_SOUND", 1},
    {0x10073000, "MAP_COMMAND", 2},
    {0x10074000, "ENGINE_CMD2", 2},   // engine cmds (stack-neutral)
    {0x10075000, "ENGINE_CMD1", 1},
    {0x10082000, "SET_UNIT_VALUE", 0},
    {0x10083000, "ATTACH_UNIT", 0},
    {0x10084000, "DROP_UNIT", 0},
};

const OpInfo* findOp(uint32_t op) {
    for (const auto& o : kOps)
        if (o.op == op) return &o;
    return nullptr;
}

} // namespace

std::vector<uint8_t> explosionReachability(const File& f) {
    const size_t count=f.code.size();
    std::vector<uint8_t> unsafe(count);
    std::vector<std::vector<size_t>> predecessors(count);
    std::vector<size_t> work;
    const auto mark=[&](size_t pc) {
        if(!unsafe[pc]) {unsafe[pc]=1;work.push_back(pc);}
    };
    for(size_t pc=0;pc<count;++pc) {
        const auto op=f.code[pc];const auto* info=findOp(op);
        if(!info || op==0x10071000 || op==0x10063000) {mark(pc);continue;}
        const size_t next=pc+1+info->args;
        if(next>count) {mark(pc);continue;}
        const auto edge=[&](size_t target) {
            if(target>=count)mark(pc);
            else predecessors[target].push_back(pc);
        };
        if(op==0x10065000)continue;
        if(op==0x10064000) {edge(f.code[pc+1]);continue;}
        edge(next);
        if(op==0x10066000)edge(f.code[pc+1]);
        if(op==0x10061000 || op==0x10062000) {
            const auto script=f.code[pc+1];
            if(script>=f.scripts.size())mark(pc);
            else edge(f.scripts[script].entry);
        }
    }
    for(size_t i=0;i<work.size();++i)
        for(const auto pc:predecessors[work[i]])mark(pc);
    return unsafe;
}

CreateHold createHold(const File& f) {
    const int create = f.scriptIndex("Create");
    if (create < 0 || f.scripts[size_t(create)].entry >= f.code.size()) return {};
    const uint32_t begin = f.scripts[size_t(create)].entry;
    uint32_t end = uint32_t(f.code.size());
    for (const auto& script : f.scripts)
        if (script.entry > begin) end = std::min(end, script.entry);
    // Can a RETURN be reached from the entry? Unknown words end a path.
    std::vector<uint8_t> seen(end - begin);
    std::vector<uint32_t> work{begin};
    while (!work.empty()) {
        const uint32_t pc = work.back();
        work.pop_back();
        if (pc < begin || pc >= end || seen[pc - begin]) continue;
        seen[pc - begin] = 1;
        const auto op = f.code[pc];
        const auto* info = findOp(op);
        if (!info || pc + 1 + uint32_t(info->args) > end) continue;
        if (op == 0x10065000) return {begin, end};
        if (op == 0x10064000) { work.push_back(f.code[pc + 1]); continue; }
        if (op == 0x10066000) work.push_back(f.code[pc + 1]);
        work.push_back(pc + 1 + uint32_t(info->args));
    }
    // It cannot return: hold only until its build-wait lets it through.
    bool read = false;
    for (uint32_t pc = begin; pc < end;) {
        const auto op = f.code[pc];
        const auto* info = findOp(op);
        if (!info) break;
        if (op == 0x10042000 && pc >= begin + 2 && f.code[pc - 2] == 0x10021001 && f.code[pc - 1] == 17)
            read = true;
        if (read && op == 0x10066000) {
            const uint32_t exit = f.code[pc + 1];
            return exit > begin && exit <= end ? CreateHold{begin, exit} : CreateHold{};
        }
        pc += 1 + uint32_t(info->args);
    }
    return {};
}

std::vector<uint8_t> gaitCode(const File& f) {
    const auto gait=[](std::string name) {
        std::transform(name.begin(),name.end(),name.begin(),[](char c){return char(std::tolower(uint8_t(c)));});
        if(name.find("control")!=std::string::npos)return false;
        for(const char* prefix:{"walk","def_walk","swim","stumble","wheel_"})
            if(name.starts_with(prefix))return true;
        for(const char* exact:{"row","slowrow","fastrow","drive","superdynamicwheelspinner"})
            if(name==exact)return true;
        return false;
    };
    std::vector<uint32_t> entries;
    for(const auto& script:f.scripts)entries.push_back(script.entry);
    std::sort(entries.begin(),entries.end());
    std::vector<uint8_t> code;
    for(const auto& script:f.scripts) {
        if(!gait(script.name) || script.entry>=f.code.size())continue;
        const auto next=std::upper_bound(entries.begin(),entries.end(),script.entry);
        const size_t end=next==entries.end() ? f.code.size() : std::min<size_t>(*next,f.code.size());
        if(code.empty())code.resize(f.code.size());
        std::fill(code.begin()+script.entry,code.begin()+std::ptrdiff_t(end),uint8_t(1));
    }
    return code;
}

std::string disassemble(const File& f, int script) {
    if (script < 0 || size_t(script) >= f.scripts.size())
        throw std::runtime_error("bad script index");
    uint32_t pc = f.scripts[script].entry;
    uint32_t end = script + 1 < int(f.scripts.size()) ? f.scripts[script + 1].entry
                                                      : uint32_t(f.code.size());
    std::ostringstream out;
    out << f.scripts[script].name << ":\n";
    while (pc < end && pc < f.code.size()) {
        uint32_t op = f.code[pc];
        const OpInfo* info = findOp(op);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%6u: ", pc);
        out << buf;
        if (!info) {
            std::snprintf(buf, sizeof buf, "0x%08x ?", op);
            out << buf << "\n";
            ++pc;
            continue;
        }
        out << info->name;
        for (int a = 0; a < info->args && pc + 1 + a < f.code.size(); ++a)
            out << " " << int32_t(f.code[pc + 1 + a]);
        // Annotate the name-table operand of the string-indexed opcodes so a mission
        // "god" cob reads as its command strings, not bare indices.
        if ((op == 0x10073000 /*MAP_COMMAND*/ || op == 0x10072000 /*PLAY_SOUND*/) &&
            pc + 1 < f.code.size()) {
            const std::string& nm = f.name(f.code[pc + 1]);
            if (!nm.empty()) out << "   ; \"" << nm << "\"";
        }
        out << "\n";
        pc += 1 + uint32_t(info->args);
    }
    return out.str();
}

} // namespace tak::cob
