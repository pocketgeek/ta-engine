#include "cartographer/ruleedit.h"
#include "cartographer/triggers.h"
#include <algorithm>
#include <cctype>
namespace cart {
bool RuleClipboard::copy(const std::vector<tak::crt::RuleGroup>& source,int group,int row,RuleColumn selected,bool wholePlayer) {
    if(wholePlayer) {column=RuleColumn::Group;groups=source;rule.reset();return !groups.empty();}
    if(group<0 || group>=int(source.size()))return false;
    if(selected==RuleColumn::Group) {column=selected;groups={source[group]};rule.reset();return true;}
    const auto& list=selected==RuleColumn::Action?source[group].actions:source[group].conditions;
    if(row<0 || row>=int(list.size()))return false;
    column=selected;rule=list[row];groups.clear();return true;
}
bool RuleClipboard::paste(std::vector<tak::crt::RuleGroup>& target,int& group,int& row,RuleColumn selected) const {
    if(column==RuleColumn::Group) {
        if(groups.empty())return false;
        const int at=group>=0 && group<int(target.size())?group+1:int(target.size());
        target.insert(target.begin()+at,groups.begin(),groups.end());group=at;row=-1;return true;
    }
    if(column!=selected || !rule || group<0 || group>=int(target.size()))return false;
    auto& list=selected==RuleColumn::Action?target[group].actions:target[group].conditions;
    const int at=row>=0 && row<int(list.size())?row+1:int(list.size());
    list.insert(list.begin()+at,*rule);row=at;return true;
}
std::vector<RuleTemplate> ruleTemplates(int player,const std::string& unitType,const std::string& location) {
    std::vector<RuleTemplate> result;
    auto add=[&](std::string name,std::string description,tak::crt::Rule condition,tak::crt::Rule action) {
        tak::crt::RuleGroup group;
        group.editorName=name;group.conditions.push_back(std::move(condition));
        group.actions.push_back(std::move(action));group.actions.push_back({14,{}}); // Run once.
        result.push_back({std::move(name),std::move(description),std::move(group)});
    };
    const auto where=location.empty()?"Anywhere":location;
    if(player>=0 && player<=8)
        add("Opening message",player==0?"Show a briefing to all players at the start.":"Show a briefing to this player at the start.",
            {0,{}},{13,{player==0?"All Players":"Player "+std::to_string(player),"Complete your objective."}});
    add("Timed victory","Win after five minutes. Edit the condition to change the time.",
        {1,{"300"}},{5,{}});
    add("Reach a region","Win when one of your units enters the chosen area.",
        {15,{"0","Any Unit",where}},{5,{}});
    add("Eliminate all opponents","Win when no opponents remain in the game.",
        {21,{"1"}},{5,{}});
    add("Lose all forces","Lose when you have no living units anywhere on the map.",
        {16,{"1","Any Unit","Anywhere"}},{6,{}});
    if(!unitType.empty())
        add("Timed reinforcements","Create one unit after a minute. Edit the unit, area and time.",
            {1,{"60"}},{7,{unitType,where}});
    return result;
}
std::vector<int> matchingRuleTemplates(const std::vector<RuleTemplate>& templates,const std::string& query) {
    auto lower=[](std::string value) {
        for(char& c:value)c=char(std::tolower(static_cast<unsigned char>(c)));
        return value;
    };
    const auto term=lower(query);std::vector<int> result;
    for(int i=0;i<int(templates.size());++i)
        if(lower(templates[i].name+" "+templates[i].description).find(term)!=std::string::npos)result.push_back(i);
    return result;
}
std::vector<std::string> ruleFlags(const std::vector<tak::crt::RuleGroup>& groups) {
    std::vector<std::string> result;
    auto collect=[&](const auto& rules,const auto& defs) {
        for(const auto& rule:rules) {
            if(rule.opcode<0 || rule.opcode>=int(defs.size()))continue;
            const auto& params=defs[rule.opcode].params;
            for(size_t i=0;i<params.size() && i<5;++i)
                if(params[i]==PKind::Flag && !rule.slot[i].empty())result.push_back(rule.slot[i]);
        }
    };
    for(const auto& group:groups) {collect(group.conditions,conditionDefs());collect(group.actions,actionDefs());}
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());
    return result;
}
std::string ruleDetails(const tak::crt::RuleGroup& group,RuleColumn selected,int row) {
    std::string text=group.editorName;
    auto append=[&](bool action,int i) {
        const auto& rules=action?group.actions:group.conditions;
        if(i<0 || i>=int(rules.size()))return;
        if(!text.empty())text+="\n";
        text+=(action?"Action ":"Condition ")+std::to_string(i+1)+": "+formatRule(action,rules[i]);
    };
    if(selected==RuleColumn::Group) {
        for(int i=0;i<int(group.conditions.size());++i)append(false,i);
        for(int i=0;i<int(group.actions.size());++i)append(true,i);
        if(text.empty())text="This rule has no conditions or actions.";
    } else append(selected==RuleColumn::Action,row);
    return text;
}
bool moveRule(std::vector<tak::crt::RuleGroup>& groups,int& group,int& row,RuleColumn selected,int direction) {
    if(direction!=-1 && direction!=1)return false;
    if(group<0 || group>=int(groups.size()))return false;
    if(selected==RuleColumn::Group) {
        if(group+direction<0 || group+direction>=int(groups.size()))return false;
        std::swap(groups[group],groups[group+direction]);group+=direction;return true;
    }
    auto& list=selected==RuleColumn::Action?groups[group].actions:groups[group].conditions;
    if(row<0 || row>=int(list.size()) || row+direction<0 || row+direction>=int(list.size()))return false;
    std::swap(list[row],list[row+direction]);row+=direction;return true;
}
}
