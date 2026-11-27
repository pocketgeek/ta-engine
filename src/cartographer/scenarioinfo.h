#pragma once
#include "crt/crt.h"
#include <iomanip>
#include <sstream>
#include <set>
namespace cart {
// Version 2 numbers the rule banks as a 3.0 .crt does (crt.h Owners: banks 1..10 =
// players 1..10, 11 = neutral); a version 1 sidecar (eight-player banks) moves its banks
// from 9 on up two, as crt::parse moves an old file's.
inline std::string scenarioInfo(const ta::crt::Scenario& scenario) {
    std::ostringstream out;out<<"TA_EDITOR_RULE_NAMES 2\n";
    for(size_t p=0;p<scenario.players.size();++p)for(size_t g=0;g<scenario.players[p].size();++g)
        if(!scenario.players[p][g].editorName.empty())out<<p<<' '<<g<<' '<<std::quoted(scenario.players[p][g].editorName)<<'\n';
    return out.str();
}
// Reject malformed metadata atomically; ordinary retail scenarios have no sidecar.
inline bool applyScenarioInfo(ta::crt::Scenario& scenario,const std::string& text) {
    if(text.size()>4*1024*1024)return false;
    std::istringstream in(text);std::string magic;int version=0;
    if(!(in>>magic>>version) || magic!="TA_EDITOR_RULE_NAMES" || (version!=1 && version!=2))return false;
    struct Name {size_t player,group;std::string text;};std::vector<Name> names;
    std::set<std::pair<size_t,size_t>> seen;
    while(in>>std::ws && !in.eof()) {
        Name name;
        if(!(in>>name.player>>name.group>>std::quoted(name.text)))return false;
        if(version==1 && name.player>=ta::crt::kOldBanks)name.player+=ta::crt::kBanks-ta::crt::kOldBanks;
        if(name.player>=scenario.players.size() ||
           name.group>=scenario.players[name.player].size() || name.text.size()>255 || name.text.find('\0')!=std::string::npos ||
           !seen.insert({name.player,name.group}).second)return false;
        names.push_back(std::move(name));
    }
    for(auto& player:scenario.players)for(auto& group:player)group.editorName.clear();
    for(auto& name:names)scenario.players[name.player][name.group].editorName=std::move(name.text);
    return true;
}
}
