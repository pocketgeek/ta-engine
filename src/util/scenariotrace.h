#pragma once
#include "sim/scenario.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace tak {
inline std::filesystem::path scenarioTracePath(const std::filesystem::path& preferences,
                                              const std::filesystem::path& snapshot) {
    auto name=snapshot.parent_path().filename();if(name.empty())name=snapshot.stem();name+=".log";
    return preferences/"playtest-logs"/name;
}
// Local opt-in diagnostics. A bounded file survives deletion of the temporary
// map; map names and operands cannot insert fake log records with newlines.
class ScenarioTraceLog {
public:
    explicit ScenarioTraceLog(const std::filesystem::path& path,size_t limit=4*1024*1024):limit_(limit) {
        std::filesystem::create_directories(path.parent_path());
        out_.open(path,std::ios::binary|std::ios::trunc);
        if(!out_)throw std::runtime_error("Cannot create trigger execution log");
        out_<<"TAK Test Map trigger execution log\nTicks are game ticks (30 per second); player, group and action numbers are 1-based.\nAction records describe attempted execution, not successful outcomes.\n";
        out_.flush();
    }
    void append(int32_t tick,int player,int group,int action,const crt::Rule* rule) {
        if(!out_.is_open())return;
        std::ostringstream line;line<<"tick="<<tick<<" player="<<player+1<<" group="<<group+1;
        if(action<0)line<<" FIRED";
        else {
            line<<" action="<<action+1;
            if(rule) {line<<" opcode="<<rule->opcode;for(const auto& slot:rule->slot)line<<" \""<<escape(slot)<<'"';}
        }
        line<<'\n';const auto bytes=line.str();
        if(bytes.size()>limit_ || written_>limit_-bytes.size()) {
            out_<<"LOG LIMIT REACHED; further records omitted.\n";out_.close();return;
        }
        out_<<bytes;out_.flush();written_+=bytes.size();
        if(!out_)out_.close();
    }
private:
    static std::string escape(const std::string& text) {
        std::string out;const char* hex="0123456789abcdef";
        for(unsigned char c:text) {
            if(c=='\\' || c=='"') {out+='\\';out+=char(c);}
            else if(c<32 || c==127) {out+="\\x";out+=hex[c>>4];out+=hex[c&15];}
            else out+=char(c);
        }
        return out;
    }
    std::ofstream out_;size_t limit_,written_=0;
};
}
