#include "cartographer/history.h"
#include "cartographer/scenarioinfo.h"
#include <algorithm>
#include <limits>

namespace cart {
HistoryState historyState(const tak::tnt::Map& map,const tak::tnt::Scenario& metadata,
                          const tak::crt::Scenario& scenario,const std::vector<PlacedUnit>& units,
                          const std::set<std::string>& useOnly,const AiFiles& aiFiles,const std::string& name) {
    return {map.save(),metadata.write(),saveScenario(scenario,units),useOnly,name,map.seaLevel,map.stockTerrain,scenarioInfo(scenario),aiFiles};
}

History::Patch History::Patch::make(const std::vector<uint8_t>& before,const std::vector<uint8_t>& after) {
    Patch p;p.beforeSize=before.size();p.afterSize=after.size();
    size_t i=0,end=std::max(before.size(),after.size());
    while(i<end) {
        if(i<before.size() && i<after.size() && before[i]==after[i]) {++i;continue;}
        const size_t start=i++;
        // Coalesce tiny gaps; per-byte edits otherwise cost more than their data.
        size_t last=i;
        while(i<end && i-last<16) {
            if(i>=before.size() || i>=after.size() || before[i]!=after[i]) last=i+1;
            ++i;
        }
        i=last;
        Run run{start,{},{}};
        if(start<before.size()) run.before.assign(before.begin()+start,before.begin()+std::min(i,before.size()));
        if(start<after.size()) run.after.assign(after.begin()+start,after.begin()+std::min(i,after.size()));
        p.cost+=sizeof(Run)+run.before.size()+run.after.size();p.runs.push_back(std::move(run));
    }
    return p;
}
void History::Patch::apply(std::vector<uint8_t>& bytes,bool forward) const {
    bytes.resize(std::max(beforeSize,afterSize));
    for(const auto& run:runs) {
        const auto& value=forward?run.after:run.before;
        std::copy(value.begin(),value.end(),bytes.begin()+run.offset);
    }
    bytes.resize(forward?afterSize:beforeSize);
}
void History::reset(HistoryState state,bool saved) {
    current_=std::move(state);entries_.clear();position_=bytes_=0;
    revision_=++serial_;saved_=saved?revision_:std::numeric_limits<uint64_t>::max();
}
bool History::commit(HistoryState state) {
    if(state==current_)return false;
    while(entries_.size()>position_) {bytes_-=entries_.back().cost;entries_.pop_back();}
    Entry e;e.terrain=Patch::make(current_.terrain,state.terrain);e.scenario=Patch::make(current_.scenario,state.scenario);
    // Move the previous arrays away before retaining metadata in the entry.
    auto oldTerrain=std::move(current_.terrain),oldScenario=std::move(current_.scenario);
    e.before=std::move(current_);
    e.after={ {},state.metadata,{},state.useOnly,state.name,state.seaLevel,state.stockTerrain,state.editorMetadata,state.aiFiles};
    e.beforeRevision=revision_;e.afterRevision=++serial_;revision_=e.afterRevision;
    e.cost=sizeof(Entry)+e.terrain.cost+e.scenario.cost+e.before.metadata.size()+e.after.metadata.size()+e.before.editorMetadata.size()+e.after.editorMetadata.size();
    for(const auto& s:e.before.useOnly)e.cost+=sizeof(std::string)+s.size();
    for(const auto& s:e.after.useOnly)e.cost+=sizeof(std::string)+s.size();
    for(const auto* files:{&e.before.aiFiles,&e.after.aiFiles})
        for(const auto& [path,data]:*files)e.cost+=sizeof(std::string)+path.size()+data.size();
    bytes_+=e.cost;entries_.push_back(std::move(e));position_=entries_.size();current_=std::move(state);
    // Keep at least the most recent edit, even when a whole-map replacement is large.
    while(bytes_>budget_ && entries_.size()>1) {bytes_-=entries_.front().cost;entries_.pop_front();--position_;}
    return true;
}
const HistoryState* History::undo() {
    if(!canUndo())return nullptr;
    const auto& e=entries_[--position_];
    e.terrain.apply(current_.terrain,false);e.scenario.apply(current_.scenario,false);
    auto terrain=std::move(current_.terrain),scenario=std::move(current_.scenario);
    current_=e.before;current_.terrain=std::move(terrain);current_.scenario=std::move(scenario);
    revision_=e.beforeRevision;return &current_;
}
const HistoryState* History::redo() {
    if(!canRedo())return nullptr;
    const auto& e=entries_[position_++];
    e.terrain.apply(current_.terrain,true);e.scenario.apply(current_.scenario,true);
    auto terrain=std::move(current_.terrain),scenario=std::move(current_.scenario);
    current_=e.after;current_.terrain=std::move(terrain);current_.scenario=std::move(scenario);
    revision_=e.afterRevision;return &current_;
}
} // namespace cart
