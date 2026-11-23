#pragma once
#include "crt/crt.h"
#include <optional>
namespace cart {
enum class RuleColumn { Group, Condition, Action };
struct RuleClipboard {
    RuleColumn column=RuleColumn::Group;
    std::vector<ta::crt::RuleGroup> groups;
    std::optional<ta::crt::Rule> rule;
    bool copy(const std::vector<ta::crt::RuleGroup>& source,int group,int row,RuleColumn selected,bool wholePlayer=false);
    // Inserts after the selection (or at the end). Incompatible operand kinds
    // are rejected, rather than silently turning an action into a condition.
    bool paste(std::vector<ta::crt::RuleGroup>& target,int& group,int& row,RuleColumn selected) const;
};
struct RuleTemplate {
    std::string name,description;
    ta::crt::RuleGroup group;
};
// CRT rule bank: 0 = All Players, 1..8 = Player 1..8 (not placed-unit owner ids).
std::vector<RuleTemplate> ruleTemplates(int player,const std::string& unitType,const std::string& location);
std::vector<int> matchingRuleTemplates(const std::vector<RuleTemplate>& templates,const std::string& query);
std::vector<std::string> ruleFlags(const std::vector<ta::crt::RuleGroup>& groups);
std::string ruleDetails(const ta::crt::RuleGroup& group,RuleColumn selected,int row);
bool moveRule(std::vector<ta::crt::RuleGroup>& groups,int& group,int& row,RuleColumn selected,int direction);
}
