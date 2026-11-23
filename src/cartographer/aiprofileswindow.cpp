// The AI Profiles window (SCENARIO > AI Profiles, A): the map's DEFAULT AI profile
// (ai/<aiprofile>.txt) and the one file all six of our levels read (ai/takengine.txt,
// previewed for one level at a time), shown as a unit table or as text. Built from the editor's own widgets (the Scripting-sized
// panel, the Placed Units search list, the Use Only checklist). The model and
// every rule about the two grammars live in aiprofiles.h; this file only draws,
// routes input and writes the bytes back (one undo step per committed edit).
// Opened from a game mod, the same window edits that pack's files instead (no undo;
// SAVE TO PACK writes them back), and SAVE AS GAME MOD writes into a pack. File > New
// AI mod / Open AI mod opens it with a pack as the only document: no map (aiModOnly).

#include "cartographer/editorstate.h"
#include "cartographer/dialog.h"
#include "ai/builtinprofile.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <tuple>
#include "util/u8path.h"

namespace {
using tak::ai::Difficulty;
constexpr const char* kSideCodes[]={"","ARA","TAR","VER","ZON","CRE"};
constexpr const char* kSideNames[]={"ALL SIDES","ARAMON","TAROS","VERUNA","ZHON","CREON"};
constexpr const char* kGroups[]={"ALL GROUPS","AIR","BUILDERS","BUILDINGS","GROUND","MANA","NAVAL","OTHER"};
constexpr int kGroupCount=8,kRowH=26,kLeftW=168;
constexpr const char* kPresetNames[]={"NO FLYERS","NO SHIPS","CLEAR ALL"};

std::string upperText(std::string s) {for(char& c:s)c=char(std::toupper(static_cast<unsigned char>(c)));return s;}
std::string trimmed(std::string s) {
    while(!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))s.pop_back();
    size_t i=0;while(i<s.size() && std::isspace(static_cast<unsigned char>(s[i])))++i;
    return s.substr(i);
}
bool whole(std::string_view s,int& v) {
    if(s.empty())return false;
    const auto r=std::from_chars(s.data(),s.data()+s.size(),v);
    return r.ec==std::errc() && r.ptr==s.data()+s.size();
}
std::string bytesText(const std::vector<uint8_t>& b) {return {b.begin(),b.end()};}
std::vector<uint8_t> bytesOf(std::string_view s) {return {s.begin(),s.end()};}
std::string lineText(std::string_view line) {
    std::string out(line);
    while(!out.empty() && (out.back()=='\r' || out.back()==' '))out.pop_back();
    return out;
}
// Wrapped text, at most `maxLines` rows; returns the rows drawn.
int drawWrapped(SDL_Renderer* ren,const std::string& text,int x,int y,int width,int maxLines,Uint8 r,Uint8 g,Uint8 b) {
    const auto rows=cart::TextEdit::lines(text,std::max(1,width/6));
    int n=0;
    for(const auto& row:rows) {
        if(n==maxLines)break;
        cart::drawText(ren,text.substr(row.begin,row.end-row.begin),x,y+n*12,1,r,g,b);++n;
    }
    return n;
}
// A bare text box (drawField without its label).
SDL_Rect inlineField(SDL_Renderer* ren,int x,int y,int w,const std::string& text,bool focused,const cart::TextEdit* edit) {
    return cart::drawField(ren,x,y-10,w,"",text,focused,edit);
}
bool doesNothing(cart::AiLine::What what) {
    using W=cart::AiLine::What;
    return what==W::Unknown || what==W::Other || what==W::Skipped;
}
} // namespace

std::optional<tak::ai::Difficulty> cart::Editor::aiTabLevel() const {
    if(aiTab<=0)return std::nullopt;
    return aiPreviewLevel;
}

void cart::Editor::aiStepPreview(int by) {
    // The lobby's order, less Passive (it plays Easy's table) and Retail (its own tab).
    static constexpr Difficulty order[]={Difficulty::Easy,Difficulty::Normal,Difficulty::Hard,Difficulty::Absurd,
                                         Difficulty::Defensive,Difficulty::AbsurdlyDefensive};
    constexpr int n=int(std::size(order));
    const int at=int(std::find(std::begin(order),std::end(order),aiPreviewLevel)-std::begin(order));
    aiPreviewLevel=order[(at+n+by)%n];
    if(aiTab!=1)aiSelectTab(1);
}

std::string cart::Editor::aiKeyFor(int tab) const {
    if(tab>0)return tak::ai::kEngineProfileFile;
    if(aiMod)return aiMod->defaultKey;   // a pack's DEFAULT file: ai/default.txt or a profile name
    if(!cart::standardAiProfile(scenario.aiProfile))return cart::aiProfileFileKey(scenario.aiProfile);
    if(aiFiles.count("ai/default.txt"))return "ai/default.txt";   // kept from the map; never written new
    return {};
}

const cart::Editor::AiTabView& cart::Editor::aiTabView() {
    const auto key=aiTabKey();const AiKind kind=aiTab?AiKind::Level:AiKind::Default;
    std::string bytes;bool readOnly=false;
    if(key.empty()) {bytes=tak::ai::classicProfileScript(vfs,"").text;readOnly=true;}   // the installed standard profile
    else if(aiView==AiView::Text && aiTextKey==key)bytes=aiText;
    else if(const auto it=aiSet().find(key);it!=aiSet().end())bytes=bytesText(it->second);
    auto& v=aiViewCache;
    if(v.key==key && v.bytes==bytes && v.readOnly==readOnly && v.draft.kind==kind && v.built && (!aiTab || v.preview==aiPreviewLevel))return v;
    v=AiTabView{};v.built=true;v.key=key;v.bytes=std::move(bytes);v.readOnly=readOnly;v.preview=aiPreviewLevel;
    const auto& catalogue=*aiCatalogue;
    v.draft=cart::parseAiDraft(v.bytes,kind,catalogue);
    v.lines=cart::attributeAiLines(v.bytes,kind,catalogue);   // views into v.bytes
    if(!key.empty())v.issues=cart::checkAiFile(key,v.bytes,{&catalogue,&aiRegistry()});
    if(kind==AiKind::Default) {v.classic.reset(catalogue.size());v.classic.execute(v.bytes,catalogue);}
    else {v.builtin=tak::ai::builtinProfile(*aiTabLevel());v.level=v.builtin;tak::ai::applyProfileText(v.bytes,v.level);}
    return v;
}

void cart::Editor::aiSetFile(const std::string& key,std::string_view bytes) {
    auto& files=aiSet();
    const auto it=files.find(key);
    const bool same=bytes.empty()?it==files.end():it!=files.end() && bytesText(it->second)==bytes;
    if(same)return;
    cart::setAiFile(files,key,bytes);
    if(!aiMod)dirty=historyPending=true;   // a pack's files are not the map's: no undo, no unsaved map
}

// One committed edit of the shown file: the draft is changed, written in the
// canonical form, verified by the engine's own parser and stored. False (with
// aiNote saying why) when the file cannot be edited as rows or the write fails.
bool cart::Editor::aiEdit(const std::function<bool(AiDraft&)>& change,const std::string& header) {
    const auto key=aiTabKey();
    if(key.empty()) {aiNote="Choose \"This map's own profile\" first: the standard profile is the installed file, which this map does not carry.";return false;}
    const auto& view=aiTabView();
    if(view.draft.asWritten) {aiNote="This file was written by hand. Press MAKE EDITABLE first, or edit it in the Text view.";return false;}
    AiDraft draft=view.draft;
    if(draft.header.empty())draft.header=!header.empty()?header:aiHeaderFor(aiTab);
    if(!change(draft))return false;
    std::erase_if(draft.units,[](const auto& unit){return unit.second.empty();});
    std::string bytes,error;
    if(!cart::saveAiDraft(draft,*aiCatalogue,bytes,error)) {aiNote=error;std::fprintf(stderr,"cartographer: %s\n",error.c_str());return false;}
    aiSetFile(key,bytes);aiNote.clear();return true;
}

std::string cart::Editor::aiHeaderFor(int tab) const {
    return std::string(tab?"TAK ENGINE AI (all six levels)":"DEFAULT AI")+" profile for "+(aiMod?"game mod \""+aiMod->pack:"map \""+mapName)+
        "\" ("+aiKeyFor(tab)+"). Edit in Cartographer: SCENARIO > AI Profiles.";
}

// The window's catalogue and unit rows, from the base install's registry or, while a
// game mod is edited or previewed, from that pack's (aiRegistry).
void cart::Editor::aiBuildRoster() {
    const auto& registry=aiRegistry();
    aiCatalogue=tak::ai::ClassicCatalogue::from(registry);
    const auto reach=cart::aiReachableUnits(registry);
    const auto* added=!aiRosterPack.empty() && aiPackRoster?&aiPackRoster->added:nullptr;
    auto row=[&](std::string key,const tak::sim::UnitType* type) {
        AiRosterRow r;r.key=std::move(key);r.type=type;
        r.label=type?(type->name.empty()?type->id:type->name):r.key;
        r.group=cart::unitGroupOf(type);r.side=type?upperText(type->side):"";
        r.reachable=type && reach.count(type->id);
        r.fromPack=type && added && added->count(type->id);
        return r;
    };
    aiDefaultRoster.clear();aiLevelRoster.clear();
    for(size_t i=1;i<aiCatalogue->size();++i) {
        const auto* type=aiCatalogue->types[i];
        if(type && type->commander)continue;   // the AI never builds a Monarch
        auto r=row(aiCatalogue->names[i],type);
        r.variant=type && lowerText(type->classicFbi.unitName.empty()?type->id:type->classicFbi.unitName)!=r.key;
        r.lockedForDefault=!cart::classicCanSingleOut(*aiCatalogue,r.key);
        aiDefaultRoster.push_back(std::move(r));
    }
    for(const auto& [id,type]:registry.types())if(!type.commander)aiLevelRoster.push_back(row(id,&type));
    for(auto* roster:{&aiDefaultRoster,&aiLevelRoster})
        std::sort(roster->begin(),roster->end(),[](const AiRosterRow& a,const AiRosterRow& b) {
            return std::tie(a.group,a.label,a.key)<std::tie(b.group,b.label,b.key);
        });
    aiViewCache=AiTabView{};aiWrittenCache.clear();aiIssues.clear();   // catalogue indices changed
    if(aiView==AiView::Issues)aiView=AiView::Table;
}

// Lists `pack`'s units ("" = the base install's): the install mounted with that one
// game mod, read once and cached.
void cart::Editor::aiUseRoster(const std::string& pack) {
    if(aiCatalogue && pack==aiRosterPack && (pack.empty() || (aiPackRoster && aiPackRoster->pack==pack)))return;
    if(!pack.empty() && !(aiPackRoster && aiPackRoster->pack==pack)) {
        try {
            auto roster=std::make_unique<AiPackRoster>();roster->pack=pack;
            const auto packVfs=tak::hpi::mountRetailRoot(tak::u8path(dataRoot),{pack});
            tak::sim::setupRegistry(roster->registry,packVfs,false);
            for(const auto& [id,type]:roster->registry.types())if(!unitRegistry.find(id))roster->added.insert(id);
            aiPackRoster=std::move(roster);
        } catch(const std::exception& error) {
            aiNote="Could not read the units of game mod "+pack+" ("+error.what()+"); the base install's units are listed.";
            aiRosterPack.clear();aiBuildRoster();return;
        }
    }
    aiRosterPack=pack;aiBuildRoster();
}

bool cart::Editor::aiRosterLists(const std::string& pack) const {
    return !aiRosterPack.empty() && aiPackRoster && aiPackRoster->pack==aiRosterPack && lowerText(aiRosterPack)==lowerText(pack);
}

void cart::Editor::openAiProfiles() {
    if(aiOpen)return;
    try {aiPacks=tak::hpi::modPacks(tak::u8path(dataRoot),tak::hpi::ModKind::Game);} catch(const std::exception&) {aiPacks.clear();}
    if(aiPack>int(aiPacks.size())) {aiPack=0;aiPackFiles.reset();}
    aiPackRoster.reset();aiRosterPack.clear();aiCatalogue.reset();   // units are read afresh each time
    aiMod.reset();aiModSaveOpen=aiModOpenOpen=false;aiView=AiView::Table;
    aiUseRoster(aiPack?aiPacks[size_t(aiPack-1)]:"");
    commitHistory();aiUndoFloor=history.revision();aiRedoRevisions.clear();aiRedoAt=0;
    aiOpened=aiFiles;aiOpen=true;aiStartOpen=false;aiFocus=AI_NONE;aiWordPick=-2;aiGroupSelected=-1;aiGroupCell=-1;
    if(aiNote.find("Could not read the units")!=0)aiNote.clear();
    aiName=scenario.aiProfile;aiPendingName.clear();aiScroll=0;aiGroupScroll=aiGroupUnitScroll=0;aiIssues.clear();
    aiViewCache=AiTabView{};menuOpen=-1;
    SDL_StartTextInput();
}

void cart::Editor::closeAiProfiles() {
    if(!aiOpen)return;
    aiCommitText();
    if(aiModDirty()) {aiClosePack([this] {closeAiProfiles();});return;}   // asks first; closes after
    if(aiModOnly) {finishAiModWorkspace();return;}   // closing the pack ends an AI mod without a map
    aiMod.reset();aiModSaveOpen=aiModOpenOpen=false;
    aiOpen=aiStartOpen=false;aiFocus=AI_NONE;aiView=AiView::Table;aiPendingName.clear();aiWordPick=-2;aiGroupCell=-1;
    SDL_StopTextInput();
}

void cart::Editor::aiCommitText() {
    if(aiView!=AiView::Text || aiTextKey.empty())return;
    if(aiText.size()>cart::kAiFileMaxBytes) {aiNote="The text is over 256 KiB, so it was not stored.";return;}
    aiSetFile(aiTextKey,aiText);
}

void cart::Editor::aiSelectTab(int tab) {
    aiCommitText();
    if(aiFocus==AI_CELL || aiFocus==AI_TEXT)aiFocus=AI_NONE;
    aiTab=std::clamp(tab,0,1);aiScroll=0;aiNote.clear();
    if(aiTab && aiView==AiView::Groups)aiView=AiView::Table;   // group rules are the DEFAULT AI's only
    if(aiFocus==AI_GROUP)aiFocus=AI_NONE;
    if(aiView==AiView::Text) {
        aiTextKey=aiTabKey();
        if(aiTextKey.empty())aiText=tak::ai::classicProfileScript(vfs,"").text;
        else if(const auto it=aiSet().find(aiTextKey);it!=aiSet().end())aiText=bytesText(it->second);
        else aiText.clear();
        aiTextScroll=0;aiTextEdit.focus(aiText,false);aiTextEdit.caret=aiTextEdit.anchor=0;
        aiFocus=aiTextKey.empty()?AI_NONE:AI_TEXT;
    }
}

std::vector<const cart::Editor::AiRosterRow*> cart::Editor::aiVisibleRows() {
    const auto& view=aiTabView();
    std::set<std::string> set;
    for(const auto& line:view.lines)
        if(line.what==AiLine::What::Exact || line.what==AiLine::What::Overridden)
            set.insert(aiTab?line.word:aiCatalogue->names[line.unit]);
    const auto query=lowerText(aiSearch);
    std::vector<const AiRosterRow*> out;
    for(const auto& row:aiRoster()) {
        const bool named=set.count(row.key)>0;
        if(aiSetOnly && !named)continue;
        if(!aiShowUnbuildable && !row.reachable && !named)continue;   // a row the file sets is never hidden
        if(aiSide && row.side!=kSideCodes[aiSide])continue;
        if(aiGroup && row.group!=kGroups[aiGroup])continue;
        if(!query.empty() && lowerText(row.label+" "+row.key).find(query)==std::string::npos)continue;
        out.push_back(&row);
    }
    return out;
}

void cart::Editor::aiCommitCell() {
    const auto key=aiCellKey;const int column=aiCellColumn;
    std::string t=lowerText(trimmed(aiCellText));
    aiFocus=AI_NONE;
    if(t.empty()) {aiClearCell(key,column);return;}
    int v=0;
    if(column==0) {
        if(!aiTab) {
            if(t.ends_with('%'))t=trimmed(t.substr(0,t.size()-1));
            if(!whole(t,v) || v<0 || v>100) {aiNote="Type a whole percentage from 0 to 100 (100 % is normal).";aiFocus=AI_CELL;return;}
        } else {
            if(t.find_first_of(".,")!=std::string::npos) {aiNote="Levels use whole numbers (0.5 would read as 0).";aiFocus=AI_CELL;return;}
            if(!whole(t,v) || v<0 || v>999) {aiNote="Type a whole weight from 0 to 999 (0 never builds it).";aiFocus=AI_CELL;return;}
        }
    } else {
        if(t=="no limit" || t=="none" || t=="unlimited" || t=="-1") {
            if(aiTab) {aiNote="Leave the limit blank for the built-in value; 0 means never built.";aiFocus=AI_CELL;return;}
            v=-1;
        } else if(t.find_first_of(".,")!=std::string::npos || !whole(t,v) || v<0 || v>9999) {
            aiNote=aiTab?"Type a whole limit from 0 to 9999 (0 = never built), or leave it blank.":"Type a whole limit from 0 to 9999, \"no limit\", or leave it blank.";aiFocus=AI_CELL;return;
        }
    }
    aiEdit([&](AiDraft& d) {
        auto& rule=d.units[key];
        if(column==1)rule.limit=v;else if(aiTab)rule.weight=v;else rule.chance=v;
        return true;
    });
}

void cart::Editor::aiClearCell(const std::string& key,int column) {
    const auto& view=aiTabView();
    const auto it=view.draft.units.find(key);
    if(!view.draft.asWritten && !view.key.empty() && it==view.draft.units.end())return;   // nothing set
    aiEdit([&](AiDraft& d) {
        auto& rule=d.units[key];
        if(column!=1) {rule.chance.reset();rule.weight.reset();}
        if(column!=0)rule.limit.reset();
        return true;
    });
}

// BUILD off = limit 0 (both grammars). BUILD on undoes this file's zeros, then
// asks the engine whether the unit is still off and, for the DEFAULT AI, names it
// at full chance with no limit (an exact row overrides any group rule).
void cart::Editor::aiToggleBuild(const std::string& key) {
    const auto& catalogue=*aiCatalogue;
    auto builds=[&](const std::string& bytes) {
        if(!aiTab) {
            tak::ai::ClassicProfile p(catalogue.size());p.execute(bytes,catalogue);
            const auto i=catalogue.byName.at(key);return p.pref[i]>0 && p.limit[i]!=0;
        }
        auto p=tak::ai::builtinProfile(*aiTabLevel());tak::ai::applyProfileText(bytes,p);
        const auto w=p.weight.find(key);const auto l=p.limit.find(key);
        return w!=p.weight.end() && w->second>0 && !(l!=p.limit.end() && l->second==0);
    };
    if(!aiTab && !catalogue.byName.count(key))return;
    const bool on=builds(aiTabView().bytes);
    bool stillOff=false;
    aiEdit([&](AiDraft& d) {
        auto& rule=d.units[key];
        if(on) {rule.limit=0;return true;}
        if(rule.limit==0)rule.limit.reset();
        if(rule.chance==0)rule.chance.reset();
        if(rule.weight && *rule.weight<=0)rule.weight.reset();
        if(!builds(cart::writeAiDraft(d))) {
            if(aiTab)stillOff=true;
            else {
                tak::ai::ClassicProfile p(catalogue.size());p.execute(cart::writeAiDraft(d),catalogue);
                const auto i=catalogue.byName.at(key);
                if(p.pref[i]==0)rule.chance=100;
                if(p.limit[i]==0)rule.limit=-1;
            }
        }
        return true;
    });
    if(stillOff)aiNote=std::string(cart::aiLevelName(*aiTabLevel()))+" still never builds it: type a weight above 0 (units its built-in table does not name need one).";
}

void cart::Editor::aiPreset(int which) {
    const auto key=aiTabKey();
    if(key.empty()) {aiNote="Choose \"This map's own profile\" first.";return;}
    const auto& view=aiTabView();
    if(view.draft.asWritten) {aiNote="This file was written by hand. Press MAKE EDITABLE first.";return;}
    if(which==2) {
        size_t rows=0;for(const auto& [unit,rule]:view.draft.units)rows+=!rule.empty();
        if(!rows) {aiNote="Clear all: this file sets no units.";return;}
        openConfirm("CLEAR ALL","Clear all removes the "+std::to_string(rows)+" unit settings from "+key+". Group rules and kept lines stay. "+(aiMod?"A pack edit has no undo: CLOSE PACK discards what is not saved.":"Undo restores them."),[this] {
            aiEdit([](AiDraft& d) {d.units.clear();return true;});
        });
        return;
    }
    // Keep my own settings: units this file already sets are left as they are.
    std::vector<std::string> targets;
    for(const auto& row:aiRoster()) {
        if(!row.type || (!aiTab && row.lockedForDefault))continue;
        const bool match=which==0?row.type->canFly:row.type->domain==tak::sim::UnitType::Domain::Water && !row.type->isStructure();
        if(!match)continue;
        if(const auto it=view.draft.units.find(row.key);it!=view.draft.units.end() && !it->second.empty())continue;
        targets.push_back(row.key);
    }
    const std::string name=which==0?"No flyers":"No ships";
    if(targets.empty()) {aiNote=name+": every matching unit is already set in this file.";return;}
    openConfirm(upperText(name),name+" sets "+std::to_string(targets.size())+" units to never built (limit 0) in "+key+
        ". Units you already set keep their settings. "+(aiMod?"A pack edit has no undo: CLOSE PACK discards what is not saved.":"Undo restores the file."),[this,targets] {
        aiEdit([&](AiDraft& d) {for(const auto& unit:targets)d.units[unit].limit=0;return true;});
    });
}

void cart::Editor::aiStartFrom(bool standard) {
    const std::string name=aiPendingName.empty()?scenario.aiProfile:aiPendingName;
    const auto key=aiMod?aiMod->defaultKey:cart::aiProfileFileKey(name);
    aiStartOpen=false;
    if(!aiMod && (key.empty() || cart::standardAiProfile(name))) {aiPendingName.clear();return;}
    const auto& catalogue=*aiCatalogue;
    const std::string header="DEFAULT AI profile for "+(aiMod?"game mod \""+aiMod->pack:"map \""+mapName)+"\" ("+key+"). Edit in Cartographer: SCENARIO > AI Profiles.";
    std::string bytes,error;
    if(standard) {
        // The user's own install, read now; nothing of it is part of the editor.
        const auto script=tak::ai::classicProfileScript(vfs,"");
        if(script.text.empty()) {aiNote="This install has no ai/default.txt, so the profile starts empty.";standard=false;}
        else {
            auto draft=cart::parseAiDraft(script.text,AiKind::Default,catalogue);
            std::string why;
            if(cart::makeEditable(draft,catalogue,&why)) {
                draft.header="Started from the installed "+tak::hpi::MountSet::key(script.path)+". "+header;
                if(!cart::saveAiDraft(draft,catalogue,bytes,error))bytes=script.text;
            } else bytes=script.text;   // kept as written: it still plays exactly as installed
        }
    }
    if(!standard) {
        AiDraft draft;draft.kind=AiKind::Default;draft.header=header;
        draft.kept={"// Started empty."};   // a file, so this map's units are all at full chance
        if(!cart::saveAiDraft(draft,catalogue,bytes,error)) {aiNote=error;aiPendingName.clear();return;}
    }
    if(!aiMod && !aiPendingName.empty() && scenario.aiProfile!=aiPendingName) {scenario.aiProfile=aiPendingName;dirty=historyPending=true;}
    aiName=scenario.aiProfile;aiPendingName.clear();
    aiSetFile(key,bytes);aiTab=0;aiScroll=0;
}

void cart::Editor::aiChooseStandard() {
    if(aiMod || cart::standardAiProfile(scenario.aiProfile))return;
    const auto key=cart::aiProfileFileKey(scenario.aiProfile);
    auto apply=[this,key] {
        scenario.aiProfile="DEFAULT";aiName=scenario.aiProfile;
        if(aiFiles.erase(key))aiNote.clear();
        dirty=historyPending=true;
    };
    if(!aiFiles.count(key)) {apply();return;}
    openConfirm("STANDARD PROFILE","The DEFAULT AI will use the standard profile (aiprofile=DEFAULT), and this map's "+key+
        " is deleted. Undo restores both.",apply);
}

void cart::Editor::aiChooseOwn() {
    if(aiMod || !cart::standardAiProfile(scenario.aiProfile))return;
    const auto name=cart::proposeAiProfileName(mapName);
    const auto key=cart::aiProfileFileKey(name);
    if(aiFiles.count(key)) {scenario.aiProfile=name;aiName=name;dirty=historyPending=true;return;}
    if(aiFiles.count("ai/default.txt")) {
        openConfirm("OWN PROFILE","Move this map's ai/default.txt to its own profile "+key+" (aiprofile="+name+
            ")? The editor never writes ai/default.txt into a map. Undo restores it.",[this,name,key] {
            auto bytes=std::move(aiFiles["ai/default.txt"]);aiFiles.erase("ai/default.txt");aiFiles[key]=std::move(bytes);
            scenario.aiProfile=name;aiName=name;dirty=historyPending=true;
        });
        return;
    }
    aiPendingName=name;aiStartOpen=true;   // the name and its first file are one undo step
}

void cart::Editor::aiCommitName() {
    aiFocus=AI_NONE;
    if(aiMod) {aiCommitModName();return;}
    const auto typed=trimmed(aiName);
    if(typed==scenario.aiProfile)return;
    if(const auto error=cart::aiProfileNameError(typed);!error.empty()) {aiNote=error;aiName=scenario.aiProfile;return;}
    if(cart::aiProfileFileKey(typed)!=cart::aiProfileFileKey(scenario.aiProfile) && aiFiles.count(cart::aiProfileFileKey(typed))) {aiNote=cart::aiProfileFileKey(typed)+" already exists in this map; choose another name.";aiName=scenario.aiProfile;return;}
    // A case-only rename names the same file, so it passes the clash check above.
    cart::renameAiProfile(aiFiles,scenario.aiProfile,typed);   // ai/<old>.txt -> ai/<new>.txt, same undo step
    scenario.aiProfile=aiName=typed;dirty=historyPending=true;aiNote.clear();
}

void cart::Editor::aiMakeEditable() {
    const auto& view=aiTabView();
    if(view.key.empty() || !view.draft.asWritten)return;
    AiDraft draft=view.draft;std::string why;
    if(!cart::makeEditable(draft,*aiCatalogue,&why)) {aiNote="Not converted: "+why+". The file stays as written.";return;}
    if(draft.header.empty())draft.header="Converted by Cartographer for "+(aiMod?"game mod \""+aiMod->pack:"map \""+mapName)+"\" ("+view.key+"). Edit in Cartographer: SCENARIO > AI Profiles.";
    std::string bytes,error;
    if(!cart::saveAiDraft(draft,*aiCatalogue,bytes,error)) {aiNote=error;return;}
    aiSetFile(view.key,bytes);aiNote="Converted: every unit plays exactly as before.";
}

void cart::Editor::aiCheck() {
    aiCommitText();
    if(aiMod) {   // a pack has no aiprofile name: each file's own checks
        aiIssues.clear();
        for(const auto& [key,bytes]:aiMod->files)
            for(auto& issue:cart::checkAiFile(key,bytesText(bytes),{&*aiCatalogue,&aiRegistry()}))aiIssues.push_back(std::move(issue));
    } else aiIssues=cart::checkAiMap(aiFiles,scenario.aiProfile,vfs,{&*aiCatalogue,&aiRegistry()});
    aiView=AiView::Issues;aiIssueScroll=0;if(aiFocus==AI_TEXT || aiFocus==AI_CELL)aiFocus=AI_NONE;
}

// ---- undo inside the window -------------------------------------------------------------
// Each committed edit is already its own map history step, so the window steps the map's
// history directly -- but only back to where it opened: map edits from before stay for
// Ctrl+Z after DONE, where the user can see them. A pack's set has no history.
void cart::Editor::aiUndo(bool redo) {
    aiCommitText();
    if(aiFocus==AI_CELL || aiFocus==AI_NAME || aiFocus==AI_GROUP || aiFocus==AI_TEXT)aiFocus=AI_NONE;
    if(aiMod) {aiNote="Edits to a game mod's files have no undo: CLOSE PACK discards what is not saved.";return;}
    commitHistory();
    if(history.revision()!=aiRedoAt)aiRedoRevisions.clear();   // an edit since the last undo
    if(!redo) {
        if(history.revision()==aiUndoFloor || !history.canUndo()) {aiNote="Nothing to undo in this window. Map edits made before it opened undo after DONE.";return;}
        const auto before=history.revision();
        restoreHistory(history.undo());aiRedoRevisions.push_back(before);
    } else {
        if(aiRedoRevisions.empty() || !history.canRedo()) {aiNote="Nothing to redo in this window.";return;}
        restoreHistory(history.redo());aiRedoRevisions.pop_back();
    }
    aiRedoAt=history.revision();
    // The window follows the restored files and name.
    aiName=scenario.aiProfile;aiPendingName.clear();aiStartOpen=false;aiWordPick=-2;
    aiViewCache=AiTabView{};aiIssues.clear();if(aiView==AiView::Issues)aiView=AiView::Table;
    if(aiView==AiView::Text) {aiTextKey.clear();aiSelectTab(aiTab);}
    aiNote=redo?"Redone.":"Undone.";
}

// ---- group rules (design 4: DEFAULT AI, Advanced) -------------------------------------------

bool cart::Editor::aiGroupsEditable() {
    const auto& view=aiTabView();
    return !aiTab && !view.key.empty() && !view.draft.asWritten;
}

void cart::Editor::aiPickWord(const std::string& word) {
    const int rule=aiWordPick;aiWordPick=-2;if(aiFocus==AI_WORD)aiFocus=AI_NONE;
    if(!aiCatalogue->categories.count(word) || cart::classicCanSingleOut(*aiCatalogue,word))return;   // never a unit name
    const int count=int(aiTabView().draft.groups.size());
    if(aiEdit([&](AiDraft& d) {
        if(rule<0) {d.groups.push_back({true,word,cart::classicChanceText(50)});return true;}
        if(rule>=int(d.groups.size()))return false;
        d.groups[size_t(rule)].word=word;return true;
    })) {
        aiGroupSelected=rule<0?count:rule;aiGroupUnitScroll=0;
        if(rule<0)aiNote="Added \"weight "+word+"\" at 50 %. Click WEIGHT to make it a limit rule, or the value to change it.";
    }
}

void cart::Editor::aiCommitGroupCell() {
    const int rule=aiGroupCell;aiFocus=AI_NONE;aiGroupCell=-1;
    const auto& groups=aiTabView().draft.groups;
    if(rule<0 || rule>=int(groups.size()))return;
    const bool weight=groups[size_t(rule)].weight;
    std::string t=lowerText(trimmed(aiCellText)),value;int v=0;
    if(weight) {
        if(t.ends_with('%'))t=trimmed(t.substr(0,t.size()-1));
        if(!whole(t,v) || v<0 || v>100) {aiNote="Type a whole percentage from 0 to 100: what the rule makes of a unit at 100 %.";aiFocus=AI_GROUP;aiGroupCell=rule;return;}
        value=cart::classicChanceText(v);
    } else if(t=="no limit" || t=="none" || t=="unlimited" || t=="-1")value="-1";
    else if(!whole(t,v) || v<0 || v>9999) {aiNote="Type a whole limit from 0 to 9999, or \"no limit\".";aiFocus=AI_GROUP;aiGroupCell=rule;return;}
    else value=std::to_string(v);
    aiEdit([&](AiDraft& d) {d.groups[size_t(rule)].value=value;return true;});
}

void cart::Editor::aiGroupAction(int rule,int what) {
    if(rule<0 || rule>=int(aiTabView().draft.groups.size()))return;
    if(what==1 && rule==0)return;
    if(what==3 && rule+1==int(aiTabView().draft.groups.size()))return;
    aiEdit([&](AiDraft& d) {
        auto& g=d.groups[size_t(rule)];
        if(what==0) {g.weight=!g.weight;g.value=g.weight?cart::classicChanceText(50):"2";}
        else if(what==1)std::swap(d.groups[size_t(rule)],d.groups[size_t(rule-1)]);
        else if(what==3)std::swap(d.groups[size_t(rule)],d.groups[size_t(rule+1)]);
        else d.groups.erase(d.groups.begin()+rule);
        return true;
    });
    if(what==0)aiNote=aiTabView().draft.groups[size_t(rule)].weight?"Now a weight rule, at 50 %.":"Now a limit rule, at 2.";
    aiGroupSelected=what==1?rule-1:what==3?rule+1:what==2?std::min(rule,int(aiTabView().draft.groups.size())-1):rule;aiGroupUnitScroll=0;
}

std::string cart::Editor::aiRowExplanation(const AiRosterRow& row) {
    const auto& view=aiTabView();
    std::string out=row.label+" ("+row.key+")";
    if(!aiTab) {
        if(row.lockedForDefault)return out+": the DEFAULT AI cannot single out this unit (an original-game rule). Only group rules change it.";
        const auto& catalogue=*aiCatalogue;const auto i=catalogue.byName.at(row.key);
        const AiLine *weight=nullptr,*limit=nullptr,*groupWeight=nullptr,*groupLimit=nullptr;
        for(const auto& line:view.lines) {
            if(line.what==AiLine::What::Exact && line.unit==i)(line.weight?weight:limit)=&line;
            if(line.what==AiLine::What::Group) {
                const auto it=catalogue.categories.find(line.word);
                if(it!=catalogue.categories.end() && std::find(it->second.begin(),it->second.end(),i)!=it->second.end())
                    (line.weight?groupWeight:groupLimit)=&line;
            }
        }
        const int pref=view.classic.pref[i],lim=view.classic.limit[i];
        const std::string chance=std::to_string(pref)+" %";
        if(weight) {
            out+=": chance "+chance+" (line "+std::to_string(weight->number)+")";
            if(weight->value!=cart::classicChanceText(pref) || !weight->combines.empty()) {
                out+=", file says "+(weight->value.empty()?std::string("nothing"):weight->value)+" = "+chance;
                if(!weight->combines.empty())out+=" (after '"+lineText(view.lines[size_t(weight->combines.back()-1)].text)+"')";
            }
        } else if(groupWeight)
            out+=": chance "+chance+" from line "+std::to_string(groupWeight->number)+" ('"+lineText(groupWeight->text)+"')";
        else out+=": chance "+chance+" (not named)";
        const std::string limitText=lim<0?"no limit":"limit "+std::to_string(lim);
        if(limit)out+="; "+limitText+" (line "+std::to_string(limit->number)+").";
        else if(groupLimit)out+="; "+limitText+" from line "+std::to_string(groupLimit->number)+" ('"+lineText(groupLimit->text)+"').";
        else out+="; "+limitText+(weight?".":" (not named).");
        if(row.variant)out+=" A variant: only the DEFAULT AI lists it.";
    } else {
        const AiLine *weight=nullptr,*limit=nullptr;
        for(const auto& line:view.lines)if(line.what==AiLine::What::Exact && line.word==row.key)(line.weight?weight:limit)=&line;
        const auto w=view.level.weight.find(row.key);const auto l=view.level.limit.find(row.key);
        const bool builtinWeight=view.builtin.weight.count(row.key)>0;
        if(weight)out+=": weight "+std::to_string(weight->levelValue)+" (line "+std::to_string(weight->number)+")";
        else if(w!=view.level.weight.end())out+=": weight "+std::to_string(w->second)+" (built-in)";
        else out+=": never built by this level (not named)";
        if(limit)out+="; limit "+std::to_string(limit->levelValue)+" (line "+std::to_string(limit->number)+").";
        else if(l!=view.level.limit.end())out+="; limit "+std::to_string(l->second)+" (built-in).";
        else out+="; no cap"+std::string(builtinWeight || weight?".":" ");
        out+=" (Previewing "+std::string(cart::aiLevelName(*aiTabLevel()))+"'s built-in table.)";
        const auto effective=cart::aiEffectiveLimit(*aiTabLevel(),l==view.level.limit.end()?std::optional<int>():l->second,row.type,&aiRegistry());
        if(effective.changed || effective.mayRise)out+=" In play on "+std::string(cart::aiLevelName(*aiTabLevel()))+": "+effective.text+".";
    }
    for(const auto& issue:view.issues)if(issue.unit==row.key && issue.severity!=AiIssue::Severity::Info) {out+=" "+issue.message;break;}
    if(!row.reachable)out+=" No builder of its side can make it.";
    if(row.fromPack)out+=" Added by game mod "+aiRosterPack+".";
    return out;
}

// The window swallows input while up (like Scripting); Ctrl+Z / Ctrl+Y step through
// the edits made since it opened (aiUndo).
void cart::Editor::handleAiProfiles(SDL_Event& e) {
    if(handleAiModDialogs(e))return;
    if(aiWordPick!=-2) {   // the group word picker
        if(e.type==SDL_TEXTINPUT) {if(aiWordSearchEdit.input(e,aiWordSearch))aiWordScroll=0;}
        else if(e.type==SDL_KEYDOWN) {
            const auto key=e.key.keysym.sym;
            if(key==SDLK_ESCAPE) {aiWordPick=-2;aiFocus=AI_NONE;}
            else if(key==SDLK_RETURN || key==SDLK_KP_ENTER) {if(aiWordRows.size()==1)aiPickWord(aiWordRows[0].second);}
            else if(aiWordSearchEdit.input(e,aiWordSearch))aiWordScroll=0;
        } else if(e.type==SDL_MOUSEWHEEL)aiWordScroll=std::max(0,aiWordScroll-e.wheel.y*40);
        else if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
            const int mx=e.button.x,my=e.button.y;
            if(cart::pointIn(mx,my,aiWordCancel)) {aiWordPick=-2;aiFocus=AI_NONE;return;}
            if(cart::pointIn(mx,my,aiWordList))for(const auto& [rect,word]:aiWordRows)if(cart::pointIn(mx,my,rect)) {aiPickWord(word);return;}
        }
        return;
    }
    if(aiStartOpen) {
        if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE) {aiStartOpen=false;aiPendingName.clear();}
        else if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
            const int mx=e.button.x,my=e.button.y;
            if(cart::pointIn(mx,my,aiStartStandard))aiStartFrom(true);
            else if(cart::pointIn(mx,my,aiStartEmpty))aiStartFrom(false);
            else if(cart::pointIn(mx,my,aiStartCancel)) {aiStartOpen=false;aiPendingName.clear();}
        }
        return;
    }
    const int textColumns=std::max(1,(aiTextBox.w-8)/6);
    auto typeInto=[&](const SDL_Event& ev) {
        if(aiFocus==AI_SEARCH) {if(aiSearchEdit.input(ev,aiSearch))aiScroll=0;}
        else if(aiFocus==AI_NAME)aiNameEdit.input(ev,aiName);
        else if(aiFocus==AI_CELL || aiFocus==AI_GROUP)aiCellEdit.input(ev,aiCellText);
        else if(aiFocus==AI_TEXT) {aiTextEdit.input(ev,aiText,false,true,textColumns);aiTextFollow=true;}
    };
    if(e.type==SDL_TEXTINPUT) {typeInto(e);return;}
    if(e.type==SDL_KEYDOWN) {
        const auto key=e.key.keysym.sym;const bool ctrl=(e.key.keysym.mod&(KMOD_CTRL|KMOD_GUI))!=0;
        const bool enter=key==SDLK_RETURN || key==SDLK_KP_ENTER;
        if(ctrl && (key==SDLK_z || key==SDLK_y)) {aiUndo(key==SDLK_y || (e.key.keysym.mod&KMOD_SHIFT));return;}
        if(aiModOnly && ctrl && (e.key.keysym.mod&KMOD_SHIFT) && (key==SDLK_n || key==SDLK_o)) {requestAiMod(key==SDLK_n);return;}
        if(aiFocus==AI_CELL) {if(key==SDLK_ESCAPE)aiFocus=AI_NONE;else if(enter)aiCommitCell();else typeInto(e);return;}
        if(aiFocus==AI_GROUP) {if(key==SDLK_ESCAPE) {aiFocus=AI_NONE;aiGroupCell=-1;} else if(enter)aiCommitGroupCell();else typeInto(e);return;}
        if(aiFocus==AI_NAME) {if(key==SDLK_ESCAPE) {aiFocus=AI_NONE;aiName=scenario.aiProfile;} else if(enter)aiCommitName();else typeInto(e);return;}
        if(aiFocus==AI_SEARCH) {if(key==SDLK_ESCAPE || enter)aiFocus=AI_NONE;else typeInto(e);return;}
        if(aiFocus==AI_TEXT) {
            if(key==SDLK_ESCAPE) {aiCommitText();aiFocus=AI_NONE;}
            else if(enter) {SDL_Event line{};line.type=SDL_TEXTINPUT;SDL_strlcpy(line.text.text,"\n",sizeof line.text.text);typeInto(line);}
            else typeInto(e);
            return;
        }
        if(key==SDLK_ESCAPE)closeAiProfiles();
        else if((key==SDLK_DELETE || key==SDLK_BACKSPACE) && !aiSelected.empty() && aiView==AiView::Table)aiClearCell(aiSelected,-1);
        return;
    }
    if(e.type==SDL_MOUSEWHEEL) {
        int mx,my;SDL_GetMouseState(&mx,&my);mx=int(mx/kUIScale);my=int(my/kUIScale);
        if(aiView==AiView::Text && cart::pointIn(mx,my,aiTextBox)) {aiTextScroll=std::max(0,aiTextScroll-e.wheel.y*3);aiTextFollow=false;}
        else if(aiView==AiView::Issues)aiIssueScroll=std::max(0,aiIssueScroll-e.wheel.y*36);
        else if(aiView==AiView::Groups) {
            if(cart::pointIn(mx,my,aiGroupUnits))aiGroupUnitScroll=std::max(0,aiGroupUnitScroll-e.wheel.y*36);
            else aiGroupScroll=std::max(0,aiGroupScroll-e.wheel.y*40);
        }
        else aiScroll=std::max(0,aiScroll-e.wheel.y*kRowH*2);
        return;
    }
    if(e.type!=SDL_MOUSEBUTTONDOWN)return;
    const int mx=e.button.x,my=e.button.y;
    if(e.button.button==SDL_BUTTON_RIGHT) {   // a cell back to "not set"; the preview level steps back
        if(cart::pointIn(mx,my,aiPreviewBox)) {aiStepPreview(-1);return;}
        if(aiView==AiView::Table)for(const auto& cell:aiCells)if(cell.column<2 && cart::pointIn(mx,my,cell.rect)) {aiFocus=AI_NONE;aiClearCell(cell.key,cell.column);return;}
        return;
    }
    if(e.button.button!=SDL_BUTTON_LEFT)return;
    // A click elsewhere leaves a field: cell and name edits are dropped (Enter commits them).
    if(aiFocus==AI_NAME && !cart::pointIn(mx,my,aiNameBox)) {aiFocus=AI_NONE;aiName=scenario.aiProfile;}
    if(aiFocus==AI_CELL)aiFocus=AI_NONE;
    if(aiFocus==AI_GROUP) {aiFocus=AI_NONE;aiGroupCell=-1;}
    if(aiFocus==AI_SEARCH && !cart::pointIn(mx,my,aiSearchBox))aiFocus=AI_NONE;
    for(int t=0;t<2;++t)if(cart::pointIn(mx,my,aiTabRects[t])) {if(t!=aiTab)aiSelectTab(t);return;}
    if(cart::pointIn(mx,my,aiPreviewBox)) {aiStepPreview(1);return;}
    for(int p=0;p<3;++p)if(cart::pointIn(mx,my,aiPresetRects[p])) {aiPreset(p);return;}
    if(cart::pointIn(mx,my,aiDoneButton)) {closeAiProfiles();return;}
    if(cart::pointIn(mx,my,aiModSaveButton)) {aiCommitText();aiOpenModSave();return;}
    if(aiModOnly && cart::pointIn(mx,my,aiTestButton)) {aiTestInGame();return;}
    if(aiMod && cart::pointIn(mx,my,aiModNewNameButton)) {
        aiCommitText();
        if(aiTab)aiSelectTab(0);
        aiFocus=AI_NAME;aiName.clear();aiNameEdit.focus(aiName);aiNote.clear();return;
    }
    if(cart::pointIn(mx,my,aiModOpenButton)) {
        aiCommitText();
        if(aiModOnly)closeAiProfiles();   // CLOSE PACK ends an AI mod without a map (asking about its edits)
        else if(aiMod)aiClosePack();
        else if(aiPacks.empty())aiNote="This install has no game mods yet (mods/game/<pack>/). SAVE AS GAME MOD makes one.";
        else {aiOpenPackList();aiNote.clear();}
        return;
    }
    if(aiMod && cart::pointIn(mx,my,aiModDefaultButton)) {aiCommitText();aiStepModDefault();return;}
    if(!aiMod && cart::pointIn(mx,my,aiPackButton)) {
        aiPack=(aiPack+1)%(int(aiPacks.size())+1);aiPackFiles.reset();
        if(aiPack) try {
            auto files=tak::hpi::modFiles(tak::u8path(dataRoot),tak::hpi::ModKind::Game,{aiPacks[size_t(aiPack-1)]});
            std::erase_if(files,[](const auto& file){return !file.first.starts_with("ai/");});   // only ai/ is asked
            aiPackFiles=std::move(files);
        } catch(const std::exception& error) {aiNote=std::string("Game mod: ")+error.what();aiPack=0;}
        aiUseRoster(aiPack?aiPacks[size_t(aiPack-1)]:"");   // that pack's units are listed too
        return;
    }
    if(cart::pointIn(mx,my,aiViewButton)) {
        if(aiView==AiView::Text) {aiCommitText();aiView=AiView::Table;aiFocus=AI_NONE;}
        else {aiView=AiView::Text;aiTextKey.clear();aiSelectTab(aiTab);}
        return;
    }
    if(cart::pointIn(mx,my,aiCheckButton)) {if(aiView==AiView::Issues)aiView=AiView::Table;else aiCheck();return;}
    if(cart::pointIn(mx,my,aiGroupsButton)) {
        aiCommitText();
        if(aiView==AiView::Groups)aiView=AiView::Table;
        else {aiView=AiView::Groups;aiGroupScroll=aiGroupUnitScroll=0;if(aiFocus==AI_TEXT)aiFocus=AI_NONE;
              if(aiGroupSelected<0 && !aiTabView().draft.groups.empty())aiGroupSelected=0;}
        return;
    }
    if(cart::pointIn(mx,my,aiEditableButton)) {aiCommitText();aiMakeEditable();return;}
    if(cart::pointIn(mx,my,aiStartButton)) {aiCommitText();if(!aiTab && (aiMod || !cart::standardAiProfile(scenario.aiProfile)))aiStartOpen=true;return;}
    if(cart::pointIn(mx,my,aiInsertButton)) {
        if(aiSelected.empty()) {aiNote="Select a unit in the table first; INSERT UNIT adds its "+std::string(aiTab?"UnitType id":"unit name")+".";return;}
        if(aiFocus!=AI_TEXT)return;
        SDL_Event word{};word.type=SDL_TEXTINPUT;SDL_strlcpy(word.text.text,aiSelected.c_str(),sizeof word.text.text);
        aiTextEdit.input(word,aiText,false,true,textColumns);aiTextFollow=true;return;
    }
    if(!aiTab && !aiMod && cart::pointIn(mx,my,aiStandardRadio)) {aiChooseStandard();return;}
    if(!aiTab && !aiMod && cart::pointIn(mx,my,aiOwnRadio)) {aiChooseOwn();return;}
    if(!aiTab && !aiMod && cart::pointIn(mx,my,aiNameBox) && !cart::standardAiProfile(scenario.aiProfile)) {
        if(aiFocus!=AI_NAME) {aiFocus=AI_NAME;aiName=scenario.aiProfile;aiNameEdit.focus(aiName);}
        return;
    }
    if(aiView==AiView::Text) {
        if(cart::pointIn(mx,my,aiTextBox) && !aiTextKey.empty()) {
            aiFocus=AI_TEXT;aiTextEdit.click(aiText,textColumns,(my-aiTextBox.y-4)/12+aiTextScroll,(mx-aiTextBox.x-4)/6,(SDL_GetModState()&KMOD_SHIFT)!=0);aiTextFollow=true;
        }
        return;
    }
    if(aiView==AiView::Groups) {
        const bool editable=aiGroupsEditable();
        if(cart::pointIn(mx,my,aiAddGroupButton)) {
            if(!editable) {aiNote=aiTabKey().empty()?"Choose \"This map's own profile\" first: the standard profile is the installed file.":"This file was written by hand. Press MAKE EDITABLE first, or edit it in the Text view.";return;}
            aiWordPick=-1;aiWordSearch.clear();aiWordScroll=0;aiFocus=AI_WORD;aiWordSearchEdit.focus(aiWordSearch);return;
        }
        if(!cart::pointIn(mx,my,aiGroupList))return;
        for(int i=0;i<int(aiGroupRows.size());++i) {
            const auto& row=aiGroupRows[size_t(i)];
            if(!cart::pointIn(mx,my,row.row))continue;
            aiGroupSelected=i;aiGroupUnitScroll=0;
            if(!editable)return;
            if(cart::pointIn(mx,my,row.keyword))aiGroupAction(i,0);
            else if(cart::pointIn(mx,my,row.up))aiGroupAction(i,1);
            else if(cart::pointIn(mx,my,row.down))aiGroupAction(i,3);
            else if(cart::pointIn(mx,my,row.remove))aiGroupAction(i,2);
            else if(cart::pointIn(mx,my,row.word)) {aiWordPick=i;aiWordSearch.clear();aiWordScroll=0;aiFocus=AI_WORD;aiWordSearchEdit.focus(aiWordSearch);}
            else if(cart::pointIn(mx,my,row.value)) {
                const auto& g=aiTabView().draft.groups[size_t(i)];
                const int v=g.weight?cart::aiGroupRuleChance(g):cart::aiGroupRuleLimit(g);
                aiCellText=!g.weight && v<0?"no limit":std::to_string(v);
                aiGroupCell=i;aiFocus=AI_GROUP;aiCellEdit.focus(aiCellText);aiNote.clear();
            }
            return;
        }
        return;
    }
    if(aiView==AiView::Issues) {
        for(const auto& [rect,index]:aiIssueRows)if(cart::pointIn(mx,my,rect) && index<aiIssues.size()) {
            const auto issue=aiIssues[index];
            int tab=-1;
            if(cart::aiEngineFile(issue.file))tab=1;
            else if(issue.file.empty() || issue.file==aiKeyFor(0))tab=0;
            if(tab<0) {aiNote=issue.file+" is not one of the profiles shown here.";return;}
            aiView=AiView::Text;aiTextKey.clear();aiSelectTab(tab);
            size_t at=0;for(int n=1;n<issue.line && at!=std::string::npos;++n) {at=aiText.find('\n',at);if(at!=std::string::npos)++at;}
            aiTextEdit.caret=aiTextEdit.anchor=at==std::string::npos?aiText.size():at;aiTextFollow=true;
            return;
        }
        return;
    }
    if(cart::pointIn(mx,my,aiSearchBox)) {if(aiFocus!=AI_SEARCH) {aiFocus=AI_SEARCH;aiSearchEdit.focus(aiSearch);}return;}
    if(cart::pointIn(mx,my,aiSideBox)) {aiSide=(aiSide+1)%6;aiScroll=0;return;}
    if(cart::pointIn(mx,my,aiGroupBox)) {aiGroup=(aiGroup+1)%kGroupCount;aiScroll=0;return;}
    if(cart::pointIn(mx,my,aiSetOnlyBox)) {aiSetOnly=!aiSetOnly;aiScroll=0;return;}
    if(cart::pointIn(mx,my,aiUnbuildableBox)) {aiShowUnbuildable=!aiShowUnbuildable;aiScroll=0;return;}
    for(const auto& cell:aiCells)if(cart::pointIn(mx,my,cell.rect)) {
        aiSelected=cell.key;
        const AiRosterRow* row=nullptr;for(const auto& r:aiRoster())if(r.key==cell.key) {row=&r;break;}
        if(!aiTab && row && row->lockedForDefault)return;   // disabled on the DEFAULT tab only
        if(cell.column==2) {aiToggleBuild(cell.key);return;}
        if(aiTabKey().empty()) {aiNote="Choose \"This map's own profile\" first: the standard profile is the installed file, which this map does not carry.";return;}
        if(aiTabView().draft.asWritten) {aiNote="This file was written by hand. Press MAKE EDITABLE first, or edit it in the Text view.";return;}
        const auto& rules=aiTabView().draft.units;const auto it=rules.find(cell.key);
        std::optional<int> value;
        if(it!=rules.end())value=cell.column==1?it->second.limit:aiTab?it->second.weight:it->second.chance;
        aiCellText=!value?"":cell.column==1 && *value<0?"no limit":std::to_string(*value);
        aiCellKey=cell.key;aiCellColumn=cell.column;aiFocus=AI_CELL;aiCellEdit.focus(aiCellText);aiNote.clear();
        return;
    }
    for(const auto& [rect,unit]:aiRows)if(cart::pointIn(mx,my,rect)) {aiSelected=unit;return;}
}

void cart::Editor::drawAiProfiles() {
    if(!aiOpen)return;
    int pointerX,pointerY;SDL_GetMouseState(&pointerX,&pointerY);pointerX=int(pointerX/kUIScale);pointerY=int(pointerY/kUIScale);
    aiHint.clear();
    const auto ct=cart::drawPanel(ren,w,h,780,520,aiModOnly && aiMod?"AI MOD (NO MAP) - GAME MOD PACK: "+upperText(aiMod->pack):
        aiMod?"AI PROFILES - GAME MOD PACK: "+upperText(aiMod->pack):"AI PROFILES - "+upperText(mapName));
    const auto& view=aiTabView();
    const auto& catalogue=*aiCatalogue;
    const bool standard=cart::standardAiProfile(scenario.aiProfile);
    const AiFiles& files=aiMod?aiMod->files:aiFiles;
    const AiFiles& openedFiles=aiMod?aiMod->opened:aiOpened;

    // ---- left column: the profiles, the preview level, presets, game-mod preview ----
    for(int t=0;t<2;++t) {
        const auto key=aiKeyFor(t);
        const SDL_Rect r{ct.x,ct.y+t*28,kLeftW,26};
        aiTabRects[t]=r;
        fillRect(ren,r.x,r.y,r.w,r.h,t==aiTab?58:30,t==aiTab?68:33,t==aiTab?95:42);
        const auto file=key.empty()?files.end():files.find(key);
        const bool inMap=file!=files.end();
        const auto opened=openedFiles.find(key);
        const bool edited=!key.empty() && (inMap!=(opened!=openedFiles.end()) || (inMap && opened->second!=file->second));
        bool written=false;
        if(inMap) {
            auto& cached=aiWrittenCache[key];const auto bytes=bytesText(file->second);
            if(cached.first!=bytes)cached={bytes,cart::parseAiDraft(bytes,t?AiKind::Level:AiKind::Default,catalogue).asWritten};
            written=cached.second;
        }
        const std::string label=t?"TAK ENGINE AI":"DEFAULT AI";
        cart::drawText(ren,(t==aiTab?">":" ")+label,r.x+3,r.y+3,1,t==aiTab?255:215,t==aiTab?235:220,t==aiTab?200:230);
        const std::string tag=std::string(written?"as written ":"")+(inMap?aiMod?"pack":"map":"")+(edited?" *":"");
        cart::drawText(ren,tag,r.x+r.w-4-cart::textWidth(tag,1),r.y+3,1,written?235:160,written?190:200,written?120:170);
        cart::drawText(ren,key.empty()?"   standard profile":"   "+key,r.x+3,r.y+15,1,150,165,185);
    }
    int ly=ct.y+60;
    cart::drawText(ren,"PREVIEW LEVEL",ct.x,ly,1,150,200,150);
    aiPreviewBox=cart::drawButton(ren,ct.x,ly+12,kLeftW,18,std::string("< ")+upperText(cart::aiLevelName(aiPreviewLevel))+" >",aiTab==1);
    ly+=36;
    ly+=4;
    cart::drawText(ren,"PRESETS",ct.x,ly,1,150,200,150);
    for(int p=0;p<3;++p)aiPresetRects[p]=cart::drawButton(ren,ct.x,ly+12+p*22,kLeftW,18,kPresetNames[p],false);
    ly+=12+3*22+10;
    aiPackButton={};
    if(!aiMod) {
        cart::drawText(ren,"PREVIEW WITH GAME MOD",ct.x,ly,1,150,200,150);
        std::string pack=aiPack?aiPacks[size_t(aiPack-1)]:"NONE";
        if(pack.size()>24)pack=pack.substr(0,22)+"..";
        aiPackButton=cart::drawButton(ren,ct.x,ly+12,kLeftW,18,"PACK: "+pack,false);
        drawWrapped(ren,aiPacks.empty()?"No game mods in this install.":aiPack && aiRosterPack==aiPacks[size_t(aiPack-1)]?
            "Its units are listed: "+std::to_string(aiPackRoster->added.size())+" added by the pack.":"Click to step through mods/game packs.",ct.x,ly+34,kLeftW,2,140,150,165);
    } else {
        // The pack banner: these files are the pack's, edited apart from the map.
        fillRect(ren,ct.x,ly-2,kLeftW,60,70,52,28);
        cart::drawText(ren,"PACK: "+(aiMod->pack.size()>20?aiMod->pack.substr(0,18)+"..":aiMod->pack),ct.x+3,ly,1,255,215,150);
        drawWrapped(ren,std::string(aiModOnly?"An AI mod: no map is open. No undo; SAVE TO PACK writes the files.":"Game-mod files, not the map's. No undo; SAVE TO PACK writes them.")+std::string(aiRosterPack==aiMod->pack && aiPackRoster?
            " Units listed with the pack ("+std::to_string(aiPackRoster->added.size())+" added).":""),ct.x+3,ly+14,kLeftW-6,4,235,205,160);
    }
    ly+=64;
    cart::drawText(ren,"GAME MOD",ct.x,ly,1,150,200,150);
    aiModSaveButton=cart::drawButton(ren,ct.x,ly+12,kLeftW,18,aiMod?(aiModDirty()?"SAVE TO PACK... *":"SAVE TO PACK..."):"SAVE AS GAME MOD...",aiModDirty());
    aiModOpenButton=cart::drawButton(ren,ct.x,ly+34,kLeftW,18,aiMod?"CLOSE PACK":"OPEN FROM GAME MOD...",false);
    aiTestButton=aiModOnly?cart::drawButton(ren,ct.x,ly+56,kLeftW,18,playtest.running()?"TEST RUNNING":"TEST IN GAME...",true):SDL_Rect{};

    // ---- right side ----
    const int x0=ct.x+kLeftW+10,W=ct.x+ct.w-x0,y0=ct.y;
    cart::drawText(ren,aiTab?"TAK ENGINE RULES: WHOLE NUMBERS; UNITS BY NAME ONLY; LATER LINES WIN.":"ORIGINAL-GAME RULES: THE FIRST LINE NAMING A UNIT WINS; GROUP RULES COMBINE.",x0,y0,1,235,220,160);
    aiStandardRadio=aiOwnRadio=aiNameBox=aiModDefaultButton=aiModNewNameButton={};
    if(!aiTab && aiMod) {
        cart::drawText(ren,"This pack's DEFAULT AI file:",x0,y0+17,1,220,225,235);
        aiModDefaultButton=cart::drawButton(ren,x0+174,y0+13,180,16,aiMod->defaultKey,false);
        // NEW NAME...: a named profile ai/<name>.txt, for maps that ask for it by name.
        if(aiFocus==AI_NAME)aiNameBox=inlineField(ren,x0+360,y0+13,W-360,aiName,true,&aiNameEdit);
        else aiModNewNameButton=cart::drawButton(ren,x0+360,y0+13,96,16,"NEW NAME...",false);
        cart::drawText(ren,aiFocus==AI_NAME?"Type a profile name, then Enter (Esc cancels): maps with aiprofile=<name> read ai/<name>.txt.":
            (aiMod->defaultKey=="ai/default.txt"?std::string("Read by every map using the standard profile."):
             "Read only by maps whose profile name is \""+aiMod->defaultKey.substr(3,aiMod->defaultKey.size()-7)+"\".")+" Click the file name to switch.",
            x0,y0+31,1,150,160,175);
    } else if(!aiTab) {
        aiStandardRadio={x0,y0+13,300,12};
        cart::drawText(ren,std::string(standard?"(o)":"( )")+" The standard profile (aiprofile=DEFAULT)",x0,y0+15,1,220,225,235);
        aiOwnRadio={x0,y0+28,206,14};
        cart::drawText(ren,std::string(standard?"( )":"(o)")+" This map's own profile, named",x0,y0+31,1,220,225,235);
        if(!standard) {
            aiNameBox=inlineField(ren,x0+210,y0+27,150,aiName,aiFocus==AI_NAME,aiFocus==AI_NAME?&aiNameEdit:nullptr);
            cart::drawText(ren,"-> "+cart::aiProfileFileKey(scenario.aiProfile)+" in the map",x0+366,y0+31,1,160,175,195);
        } else cart::drawText(ren,"(proposed: "+cart::proposeAiProfileName(mapName)+")",x0+210,y0+31,1,140,150,165);
    } else {
        cart::drawText(ren,"File: "+aiTabKey()+", read by all six levels over each one's own BUILT-IN table.",x0,y0+15,1,200,205,215);
        cart::drawText(ren,"Showing "+upperText(cart::aiLevelName(aiPreviewLevel))+": rows not set keep its built-in value (grey). Unit ids only.",x0,y0+31,1,150,160,175);
    }
    const int listTop=y0+90,listBottom=ct.y+ct.h-112;
    aiCells.clear();aiRows.clear();aiIssueRows.clear();
    aiSearchBox=aiSideBox=aiGroupBox=aiSetOnlyBox=aiUnbuildableBox=aiTextBox={};aiList={};
    aiGroupList=aiGroupUnits=aiAddGroupButton={};aiGroupRows.clear();
    if(aiView==AiView::Table) {
        aiSearchBox=cart::drawField(ren,x0,y0+46,150,"FIND (NAME OR KEY)",aiSearch,aiFocus==AI_SEARCH,aiFocus==AI_SEARCH?&aiSearchEdit:nullptr);
        aiSideBox=cart::drawChoice(ren,x0+158,y0+46,100,"SIDE",kSideNames[aiSide],false);
        aiGroupBox=cart::drawChoice(ren,x0+266,y0+46,100,"GROUP",kGroups[aiGroup],false);
        aiSetOnlyBox=cart::drawButton(ren,x0+374,y0+56,74,16,std::string(aiSetOnly?"[X]":"[ ]")+" SET ONLY",aiSetOnly);
        aiUnbuildableBox=cart::drawButton(ren,x0+454,y0+56,W-454,16,std::string(aiShowUnbuildable?"[X]":"[ ]")+" UNBUILDABLE",aiShowUnbuildable);
        cart::drawText(ren,"UNIT",x0+46,y0+80,1,150,200,150);
        cart::drawText(ren,"BUILD",x0+234,y0+80,1,150,200,150);
        cart::drawText(ren,aiTab?"WEIGHT":"CHANCE",x0+280,y0+80,1,150,200,150);
        cart::drawText(ren,aiTab?"BUILT-IN":"SET BY",x0+378,y0+80,1,150,200,150);
        cart::drawText(ren,aiTab?"LIMIT (IN PLAY)":"LIMIT",x0+462,y0+80,1,150,200,150);
        aiList={x0,listTop,W,listBottom-listTop};
        fillRect(ren,aiList.x,aiList.y,aiList.w,aiList.h,22,24,32);
        const auto rows=aiVisibleRows();
        aiScroll=std::clamp(aiScroll,0,std::max(0,int(rows.size())*kRowH-aiList.h));
        std::map<std::string,const AiLine*> setWeight,setLimit;
        for(const auto& line:view.lines)if(line.what==AiLine::What::Exact) {
            const auto unit=aiTab?line.word:catalogue.names[line.unit];
            (line.weight?setWeight:setLimit)[unit]=&line;
        }
        std::set<std::string> flagged;
        for(const auto& issue:view.issues)if(issue.severity!=AiIssue::Severity::Info && !issue.unit.empty())flagged.insert(issue.unit);
        SDL_RenderSetClipRect(ren,&aiList);
        const bool editable=!view.key.empty() && !view.draft.asWritten;
        for(int i=aiScroll/kRowH;i<int(rows.size()) && i*kRowH<aiScroll+aiList.h;++i) {
            const auto& row=*rows[size_t(i)];const int y=aiList.y+i*kRowH-aiScroll;
            const bool selected=row.key==aiSelected,locked=!aiTab && row.lockedForDefault;
            if(selected)fillRect(ren,aiList.x,y,aiList.w,kRowH-1,50,60,82);
            aiRows.push_back({{aiList.x,y,aiList.w,kRowH-1},row.key});
            if(flagged.count(row.key))cart::drawText(ren,"!",x0+2,y+9,1,255,170,90);
            if(row.type) {
                if(auto* portrait=unitPortraits.get("anims/buildpic/"+row.type->id+".jpg")) {
                    int pw=0,ph=0;SDL_QueryTexture(portrait,nullptr,nullptr,&pw,&ph);
                    if(pw>0 && ph>0) {
                        const float scale=std::min(32.f/pw,24.f/ph);
                        SDL_Rect dst{x0+10+(32-int(pw*scale))/2,y+1+(24-int(ph*scale))/2,int(pw*scale),int(ph*scale)};
                        SDL_RenderCopy(ren,portrait,nullptr,&dst);
                    }
                }
            }
            const SDL_Rect nameArea{x0+46,y,182,kRowH};SDL_Rect nameClip{};SDL_IntersectRect(&nameArea,&aiList,&nameClip);
            SDL_RenderSetClipRect(ren,&nameClip);
            cart::drawText(ren,(locked?"(lock) ":"")+row.label,x0+46,y+3,1,locked?150:235,locked?150:230,locked?150:200);
            cart::drawText(ren,row.key+" - "+row.group+(row.side.empty()?"":" - "+row.side)+(row.variant?" - variant":"")+(row.fromPack?" - pack":""),x0+46,y+14,1,row.fromPack?235:150,row.fromPack?205:165,row.fromPack?140:185);
            SDL_RenderSetClipRect(ren,&aiList);
            if(locked) {
                cart::drawText(ren,"can't be set alone (?)",x0+234,y+9,1,150,150,150);
                if(cart::pointIn(pointerX,pointerY,{aiList.x,y,aiList.w,kRowH}) && cart::pointIn(pointerX,pointerY,aiList))
                    aiHint="The DEFAULT AI cannot single out this unit (an original-game rule). Only group rules change it.";
                continue;
            }
            // Effective values, from the engine's reading of the file.
            bool builds=true;std::string value,limit,middle,inPlay;
            bool valueSet=setWeight.count(row.key)>0,limitSet=setLimit.count(row.key)>0;
            Uint8 vr=235,vg=235,vb=240;
            if(!aiTab) {
                const auto index=catalogue.byName.at(row.key);
                const int pref=view.classic.pref[index],lim=view.classic.limit[index];
                builds=pref>0 && lim!=0;
                value=std::to_string(pref)+" %";limit=lim<0?"no limit":std::to_string(lim);
                if(valueSet || limitSet)middle="line "+std::to_string((valueSet?setWeight:setLimit)[row.key]->number);
                else if(pref!=100 || lim!=-1)middle="group rule";
            } else {
                const auto wv=view.level.weight.find(row.key);const auto lv=view.level.limit.find(row.key);
                const auto bw=view.builtin.weight.find(row.key);const auto bl=view.builtin.limit.find(row.key);
                builds=wv!=view.level.weight.end() && wv->second>0 && !(lv!=view.level.limit.end() && lv->second==0);
                if(wv!=view.level.weight.end())value=std::to_string(wv->second);
                else {value="never";vr=255;vg=120;vb=110;}
                limit=lv!=view.level.limit.end()?std::to_string(lv->second):"no cap";
                middle=bw==view.builtin.weight.end()?"-":std::to_string(bw->second);
                middle+=bl==view.builtin.limit.end()?"":" / "+std::to_string(bl->second);
                // What the level makes of the limit while it plays (Easy halves, Hard x1.5, ...).
                const auto effective=cart::aiEffectiveLimit(*aiTabLevel(),lv==view.level.limit.end()?std::optional<int>():lv->second,row.type,&aiRegistry());
                if(effective.changed)inPlay=effective.limit<0?"> no cap":"> "+std::to_string(effective.limit);
                if(effective.mayRise)inPlay+=inPlay.empty()?"+":" +";
            }
            const SDL_Rect buildCell{x0+232,y+5,40,16},valueCell{x0+278,y+5,92,16},limitCell{x0+460,y+5,W-462,16};
            aiCells.push_back({buildCell,row.key,2});aiCells.push_back({valueCell,row.key,0});aiCells.push_back({limitCell,row.key,1});
            fillRect(ren,buildCell.x,buildCell.y,buildCell.w,buildCell.h,builds?40:70,builds?70:36,builds?46:36);
            cart::drawText(ren,builds?"ON":"OFF",buildCell.x+(builds?14:11),buildCell.y+5,1,builds?190:255,builds?240:170,builds?190:160);
            auto cell=[&](const SDL_Rect& r,int column,const std::string& text,bool set,Uint8 cr,Uint8 cg,Uint8 cb) {
                if(aiFocus==AI_CELL && aiCellKey==row.key && aiCellColumn==column) {inlineField(ren,r.x,r.y,r.w,aiCellText,true,&aiCellEdit);return;}
                SDL_SetRenderDrawColor(ren,editable?62:44,editable?66:46,editable?82:54,255);SDL_RenderDrawRect(ren,&r);
                if(!set) {cr=std::min<Uint8>(cr,150);cg=std::min<Uint8>(cg,150);cb=std::min<Uint8>(cb,160);}
                cart::drawText(ren,text+(set?" *":""),r.x+4,r.y+5,1,cr,cg,cb);
            };
            cell(valueCell,0,value,valueSet,vr,vg,vb);
            cart::drawText(ren,middle,x0+378,y+10,1,140,150,165);
            cell(limitCell,1,limit,limitSet,235,235,240);
            if(!inPlay.empty() && !(aiFocus==AI_CELL && aiCellKey==row.key && aiCellColumn==1))
                cart::drawText(ren,inPlay,limitCell.x+limitCell.w-4-cart::textWidth(inPlay,1),limitCell.y+5,1,150,190,230);
        }
        if(rows.empty())cart::drawText(ren,aiSetOnly?"This file sets no units that match.":"No units match.",x0+6,aiList.y+6,1,205,210,220);
        SDL_RenderSetClipRect(ren,nullptr);
        SDL_SetRenderDrawColor(ren,70,74,90,255);SDL_RenderDrawRect(ren,&aiList);
    } else if(aiView==AiView::Text) {
        const int boxTop=y0+46;
        const std::string label=view.readOnly?"TEXT (THE INSTALLED STANDARD PROFILE: READ ONLY)":"TEXT OF "+aiTextKey+" (! DOES NOTHING, ~ SHADOWED; HOVER A MARK)";
        const std::string& shown=aiTextKey.empty()?view.bytes:aiText;
        aiTextBox=cart::drawTextArea(ren,x0+14,boxTop,W-14,listBottom-boxTop-10,label,shown,aiFocus==AI_TEXT,aiTextEdit,aiTextScroll,aiTextFollow);
        // Gutter marks on each line's first row (the rows drawTextArea wrapped).
        const int columns=std::max(1,(aiTextBox.w-8)/6),visible=std::max(1,(aiTextBox.h-8)/12);
        const auto rows=cart::TextEdit::lines(shown,columns);
        int number=1;
        for(int i=0;i<int(rows.size());++i) {
            const bool first=rows[size_t(i)].begin==0 || shown[rows[size_t(i)].begin-1]=='\n';
            if(i>0 && first)++number;
            if(!first || i<aiTextScroll || i>=aiTextScroll+visible || number>int(view.lines.size()))continue;
            const auto& line=view.lines[size_t(number-1)];
            const char* mark=doesNothing(line.what)?"!":line.what==AiLine::What::Shadowed || line.what==AiLine::What::Overridden?"~":nullptr;
            if(!mark)continue;
            const int my=aiTextBox.y+4+(i-aiTextScroll)*12;
            cart::drawText(ren,mark,x0+4,my,1,255,170,90);
            if(cart::pointIn(pointerX,pointerY,{x0,my-2,14,12})) {
                aiHint="line "+std::to_string(number)+": this line does nothing.";
                for(const auto& issue:view.issues)if(issue.line==number) {aiHint=issue.message;break;}
            }
        }
    } else if(aiView==AiView::Groups) {
        drawAiGroups(x0,y0,W,listBottom);
    } else {
        cart::drawText(ren,std::to_string(aiIssues.size())+" AI PROFILE CHECKS. CLICK ONE TO SHOW ITS LINE. NONE BLOCK SAVING.",x0,y0+46,1,235,220,160);
        const SDL_Rect list{x0,y0+60,W,listBottom-y0-60};
        fillRect(ren,list.x,list.y,list.w,list.h,22,24,32);
        SDL_RenderSetClipRect(ren,&list);
        int y=list.y+4-aiIssueScroll,total=0;
        for(size_t i=0;i<aiIssues.size();++i) {
            const auto& issue=aiIssues[i];
            const char* kind=issue.severity==AiIssue::Severity::Error?"ERROR: ":issue.severity==AiIssue::Severity::Warning?"WARNING: ":"NOTE: ";
            const std::string where=issue.file.empty()?"aiprofile: ":issue.file+(issue.line?"":": ");
            const std::string text=std::string(kind)+where+(issue.line?" "+issue.message:issue.message);
            const auto lines=cart::TextEdit::lines(text,std::max(1,(list.w-8)/6));
            const int height=int(lines.size())*12+4;
            aiIssueRows.push_back({{list.x,y,list.w,height},i});
            const bool error=issue.severity==AiIssue::Severity::Error,info=issue.severity==AiIssue::Severity::Info;
            for(size_t l=0;l<lines.size();++l)
                cart::drawText(ren,text.substr(lines[l].begin,lines[l].end-lines[l].begin),list.x+4,y+int(l)*12,1,error?255:info?170:235,error?145:info?190:215,error?140:info?205:180);
            y+=height;total+=height;
        }
        if(aiIssues.empty())cart::drawText(ren,aiMod?"No problems found in this pack's AI files.":"No problems found in this map's AI files.",list.x+4,list.y+4,1,200,230,205);
        aiIssueScroll=std::clamp(aiIssueScroll,0,std::max(0,total-list.h+8));
        SDL_RenderSetClipRect(ren,nullptr);
        SDL_SetRenderDrawColor(ren,70,74,90,255);SDL_RenderDrawRect(ren,&list);
    }

    // ---- help strip: the selected row, the hand-written banner, or the last note ----
    const int helpY=listBottom+4;
    std::string help;Uint8 hr=200,hg=210,hb=225;
    if(!aiNote.empty()) {help=aiNote;hr=255;hg=200;hb=130;}
    else if(view.draft.asWritten && aiView==AiView::Table) {
        int nothing=0,skipped=0;std::string first,shadow;
        for(const auto& line:view.lines) {
            if(doesNothing(line.what)) {if(!nothing++)first=lineText(line.text);}
            if(line.what==AiLine::What::Shadowed || line.what==AiLine::What::Overridden) {if(!skipped++)shadow=lineText(line.text)+"\" (line "+std::to_string(line.by)+(aiTab?" wins)":" already set this unit)");}
        }
        help="This profile was written by hand. Showing what it does; it is saved unchanged.";
        if(nothing)help+=" "+std::to_string(nothing)+(nothing==1?" line does":" lines do")+" nothing: \""+first+"\".";
        if(skipped)help+=" "+std::to_string(skipped)+(skipped==1?" line is":" lines are")+" skipped: \""+shadow+".";
        help+=" MAKE EDITABLE converts it if every unit plays the same.";hr=235;hg=215;hb=150;
    } else if(!aiSelected.empty() && aiView==AiView::Table) {
        for(const auto& row:aiRoster())if(row.key==aiSelected) {help=aiRowExplanation(row);break;}
    }
    if(help.empty() && aiView==AiView::Groups)
        help="A group rule changes every unit of a category word (FBI category=) or ALL. Weight rules multiply a unit's chance (50 % halves it) and combine; "
             "the last limit rule wins. Neither touches a unit set individually: its own row comes first and locks it.";
    if(help.empty() && aiView==AiView::Table) {
        if(!aiTab && view.key.empty())help="The standard profile is shown read-only: the installed ai/default.txt (or every unit at 100 % when there is none). Choose this map's own profile to edit.";
        else if(!aiTab && !aiMod && view.key=="ai/default.txt")help="This map carries ai/default.txt: every map without its own profile name also reads ai/default.txt, and an installed one may be used instead. Choose this map's own profile to move it.";
        else help=aiTab?"One file for all six levels: a row replaces that unit's value in every level; blank keeps each level's built-in. WEIGHT: 0 never builds it. LIMIT: 0 means never. Easy halves limits, Hard and Absurd raise them by half (minimum 1); Defensive levels ignore limits except 0.":
            "CHANCE: 100 % is normal; lower numbers make the AI pick this unit less often against others it wants equally; 0 % never builds it. The real odds also depend on what the AI needs during play. LIMIT: a number, or no limit.";
    }
    drawWrapped(ren,help,x0,helpY,W,3,hr,hg,hb);

    // ---- footer: which file plays, the Test Map hint, buttons ----
    const int footY=ct.y+ct.h-64;
    fillRect(ren,ct.x,footY-3,ct.w,1,70,74,90);
    std::string plays;
    if(aiModOnly && aiMod) {   // no map: what the pack's file does wherever the pack is ticked
        const auto key=aiTabKey();const bool has=aiMod->files.count(key)>0;
        const std::string pack=aiMod->pack;
        if(aiTab)plays=has?"Plays: with "+pack+" ticked, all six levels read this pack's "+key+" over their own built-in tables, on every map; "
            "a game mod wins over a map's own copy (the install never supplies this file). Without the pack nothing changes.":
            "Plays: this pack has no "+key+" yet, so with "+pack+" ticked our levels read the map's copy, else their built-in tables.";
        else if(key=="ai/default.txt")plays=has?"Plays: with "+pack+" ticked, RETAIL reads this pack's ai/default.txt on every map using the standard profile, "
            "over the map's and the install's. A map with its own profile name reads ai/<name>.txt instead.":
            "Plays: this pack has no ai/default.txt yet, so with "+pack+" ticked RETAIL reads the map's or the install's.";
        else plays="Plays: with "+pack+" ticked, RETAIL reads this pack's "+key+" on maps whose profile name asks for it, over the map's own copy"+
            (has?".":" (the pack has no such file yet).");
    } else {
        const auto* modFiles=aiMod?&aiMod->files:aiPackFiles?&*aiPackFiles:nullptr;
        const auto choice=cart::whichFile(aiTab?*aiTabLevel():Difficulty::Classic,vfs,aiFiles,scenario.aiProfile,modFiles);
        plays="Plays: "+choice.message;
        if(aiMod)plays+=" (on this map, with pack "+aiMod->pack+" ticked)";
        else if(aiPack)plays+=" (with pack "+aiPacks[size_t(aiPack-1)]+" ticked)";
    }
    drawWrapped(ren,plays,ct.x,footY+2,ct.w,2,180,220,190);
    cart::drawText(ren,aiModOnly?"TEST IN GAME: pick a map; its room ticks this pack. RETAIL plays the DEFAULT AI, other levels ai/takengine.txt.":
        aiTab?"Test Map: every level you pick in the room reads ai/takengine.txt. Game-mod files are tested by ticking that pack.":
        "Test Map: pick RETAIL for the DEFAULT AI in the room. Game-mod files are tested by ticking that pack.",ct.x,footY+28,1,150,160,175);
    const int by=ct.y+ct.h-18;
    aiStartButton=aiEditableButton=aiInsertButton={};
    int bx=ct.x;
    if(!aiTab && (aiMod || !standard)) {aiStartButton=cart::drawButton(ren,bx,by,96,16,"START FROM...",false);bx+=102;}
    aiViewButton=cart::drawButton(ren,bx,by,96,16,aiView==AiView::Text?"TABLE VIEW":"TEXT VIEW",aiView==AiView::Text);bx+=102;
    aiCheckButton=cart::drawButton(ren,bx,by,64,16,aiView==AiView::Issues?"TABLE":"CHECK",aiView==AiView::Issues);bx+=70;
    aiGroupsButton={};
    if(!aiTab) {aiGroupsButton=cart::drawButton(ren,bx,by,96,16,aiView==AiView::Groups?"TABLE":"GROUP RULES",aiView==AiView::Groups);bx+=102;}
    if(view.draft.asWritten && !view.key.empty()) {aiEditableButton=cart::drawButton(ren,bx,by,104,16,"MAKE EDITABLE",true);bx+=110;}
    if(aiView==AiView::Text && !aiTextKey.empty()) {aiInsertButton=cart::drawButton(ren,bx,by,96,16,"INSERT UNIT",false);bx+=102;}
    cart::drawText(ren,aiMod?"Pack files: no undo.":"Ctrl+Z undoes.",bx+6,by+5,1,140,150,165);
    aiDoneButton=cart::drawButton(ren,ct.x+ct.w-60,by,60,16,aiModOnly?"CLOSE":"DONE",true);

    if(aiStartOpen) {
        const auto pc=cart::drawPanel(ren,w,h,460,136,"START FROM");
        const auto key=cart::aiProfileFileKey(aiPendingName.empty()?scenario.aiProfile:aiPendingName);
        cart::drawText(ren,"The game's standard DEFAULT AI: copies your install's ai/default.txt.",pc.x,pc.y,1,220,225,235);
        cart::drawText(ren,"Empty: every unit at full chance with no limit (plays worse).",pc.x,pc.y+14,1,220,225,235);
        const auto target=aiMod?aiMod->defaultKey:key;
        cart::drawText(ren,aiMod?(aiMod->files.count(target)?"This replaces the pack's "+target+" (no undo; CLOSE PACK discards it).":"The profile becomes the pack's "+target+"."):
            aiFiles.count(key)?"This replaces the map's "+key+" (Undo restores it).":"The profile is written to "+key+" in the map.",pc.x,pc.y+34,1,170,185,200);
        aiStartStandard=cart::drawButton(ren,pc.x,pc.y+pc.h-20,190,18,"STANDARD DEFAULT AI",true);
        aiStartEmpty=cart::drawButton(ren,pc.x+196,pc.y+pc.h-20,80,18,"EMPTY",false);
        aiStartCancel=cart::drawButton(ren,pc.x+pc.w-74,pc.y+pc.h-20,70,18,"CANCEL",false);
    }
    drawAiWordPicker();
    drawAiModDialogs();
    if(!aiHint.empty() && !aiStartOpen && !aiModSaveOpen && !aiModOpenOpen && aiWordPick==-2 && modal==M_NONE)cart::drawTooltip(ren,w,h,pointerX,pointerY,aiHint);
}

// ---- game mods (design 3.5, 7.2) -------------------------------------------------

void cart::Editor::aiOpenModSave() {
    aiModRows.clear();aiModDeletes.clear();aiModDeleteTicked.clear();aiModNote.clear();aiModDeleteBoxes.clear();
    const auto& files=aiSet();
    auto has=[&](const std::string& key) {const auto it=files.find(key);return it!=files.end() && !it->second.empty();};
    auto label=[](const std::string& key) {return std::string(cart::aiEngineFile(key)?"TAK ENGINE AI":"DEFAULT AI");};
    if(aiMod) {   // back to the pack it came from: what changed since it was opened
        aiModPack=aiMod->pack;
        for(const auto& [key,bytes]:files)if(!bytes.empty()) {
            const auto opened=aiMod->opened.find(key);
            aiModRows.push_back({label(key),key,key,opened==aiMod->opened.end() || opened->second!=bytes,false,{}});
        }
        for(const auto& [key,bytes]:aiMod->opened)if(!has(key))aiModDeletes.push_back(key);
    } else {
        if(aiModPack.empty())aiModPack=aiPacks.empty()?cart::proposeAiProfileName(mapName)+"-ai":aiPacks.front();
        // DEFAULT unticked: a game mod's file applies on every map (design 3.5).
        const auto def=aiKeyFor(0);
        if(!def.empty() && has(def))aiModRows.push_back({"DEFAULT AI",def,def,false,true,{}});
        const auto key=aiKeyFor(1);   // ai/takengine.txt
        if(has(key))aiModRows.push_back({label(key),key,key,true,false,{}});
        else if(aiOpened.count(key))aiModDeletes.push_back(key);   // emptied in this window
    }
    if(aiModRows.empty() && aiModDeletes.empty()) {
        aiNote=aiMod?"This pack has no AI files to save.":"This map has no AI profiles to save yet: edit TAK ENGINE AI, or choose the DEFAULT AI's own profile first.";
        return;
    }
    aiModSaveOpen=true;if(aiFocus!=AI_NONE)aiFocus=AI_NONE;
    aiModReplan();
}

void cart::Editor::aiModReplan() {
    const auto& files=aiSet();
    const bool own=!cart::standardAiProfile(scenario.aiProfile);
    for(auto& row:aiModRows)
        if(row.isDefault)row.target=aiModDefaultAs==1 && own && row.source!="ai/default.txt"?cart::aiProfileFileKey(scenario.aiProfile):"ai/default.txt";
    const std::set<std::string> remove(aiModDeletes.begin(),aiModDeletes.end());
    auto plan=[&](const std::string pack) {   // a copy: planning resets aiModPlan.pack
        AiFiles write;
        for(const auto& row:aiModRows)if(row.tick)if(const auto it=files.find(row.source);it!=files.end()) {
            // A map's DEFAULT profile written as the pack's ai/default.txt names that file in
            // its header, not the map's; a hand-written file keeps its bytes.
            if(row.isDefault && !aiMod && row.target=="ai/default.txt")
                write[row.target]=bytesOf(cart::aiFileWithHeader(bytesText(it->second),AiKind::Default,
                    cart::aiModDefaultHeader(pack,mapName,row.source),*aiCatalogue));
            else write[row.target]=it->second;
        }
        aiModPlanError.clear();
        if(!cart::planModProfiles(tak::u8path(dataRoot),pack,write,remove,aiModPlan,aiModPlanError))aiModPlan=ModProfilePlan{};
    };
    plan(trimmed(aiModPack));
    // An existing pack keeps its own spelling on disk; the header names it that way.
    if(aiModPlanError.empty() && !aiModPlan.pack.empty() && aiModPlan.pack!=trimmed(aiModPack))plan(aiModPlan.pack);
}

void cart::Editor::aiModSave() {
    using A=ModProfileChange::Action;
    aiModReplan();
    if(!aiModPlanError.empty())return;   // shown in the dialog
    auto plan=aiModPlan;
    std::erase_if(plan.changes,[&](const ModProfileChange& c) {return c.action==A::Delete && !aiModDeleteTicked.count(c.key);});
    if(!plan.writes()) {aiModNote="Nothing to write: every ticked file is already the same in the pack.";return;}
    std::string compare;bool replaces=false;
    for(const auto& change:plan.changes) {
        if(change.action==A::Replace)compare+=cart::compareAiFiles(change.key,bytesText(change.existing),bytesText(change.data),*aiCatalogue)+" ";
        else if(change.action==A::Delete)compare+="Delete "+change.key+" from the pack. ";
        replaces|=change.action==A::Replace || change.action==A::Delete;
    }
    auto write=[this,plan] {
        std::string error;std::filesystem::path backup;
        if(!cart::writeModProfiles(tak::u8path(dataRoot),plan,preferencesFolder,error,&backup)) {aiAfterSave=nullptr;openMessage("SAVE FAILED",error);return;}
        int written=0,same=0,deleted=0;
        for(const auto& change:plan.changes) {
            if(change.action==A::Same)++same;else if(change.action==A::Delete)++deleted;else ++written;
            if(aiMod) {   // the pack now holds these bytes
                if(change.action==A::Delete)aiMod->opened.erase(change.key);
                else if(aiMod->files.count(change.key))aiMod->opened[change.key]=change.data;
            }
        }
        if(aiMod)aiMod->pack=plan.pack;
        aiModSaveOpen=false;aiModPack=plan.pack;
        try {aiPacks=tak::hpi::modPacks(tak::u8path(dataRoot),tak::hpi::ModKind::Game);} catch(const std::exception&) {}
        aiPack=0;aiPackFiles.reset();   // the preview reads packs again when picked
        aiUseRoster(aiMod?aiMod->pack:"");
        std::string text="Saved to "+pathText(plan.folder)+": "+std::to_string(written)+" written";
        if(same)text+=", "+std::to_string(same)+" identical (skipped)";
        if(deleted)text+=", "+std::to_string(deleted)+" deleted";
        text+=".";
        if(!backup.empty())text+=" The previous files are in "+pathText(backup)+".";
        text+=" Tick "+plan.pack+" under Mods. A game already running shows the new pack the next time a room opens.";
        if(aiAfterSave) {   // TEST IN GAME asked for this save: carry on to its map picker
            auto next=std::move(aiAfterSave);aiAfterSave=nullptr;
            aiNote="Saved to "+pathText(plan.folder)+".";next();return;
        }
        openMessage("SAVED",text);
    };
    if(replaces)openConfirm("REPLACE PACK FILES",compare+"The old files are kept in "+pathText(preferencesFolder/"mod-backups"/tak::u8path(plan.pack))+
        ", not in the pack. Save?",write);
    else write();
}

void cart::Editor::aiOpenPack(const std::string& pack) {
    aiModOpenOpen=false;
    if(aiModPickWorkspace && !aiOpen) {aiModPickWorkspace=false;startAiModWorkspace(pack,false);return;}   // File > Open AI mod
    try {
        auto files=cart::readModProfiles(tak::u8path(dataRoot),pack);
        AiModSet set;set.pack=pack;set.files=files;set.opened=std::move(files);
        // The DEFAULT tab shows ai/default.txt, or the pack's file for this map's profile name.
        if(!set.files.count("ai/default.txt")) {
            if(const auto own=cart::aiProfileFileKey(scenario.aiProfile);!aiModOnly && !cart::standardAiProfile(scenario.aiProfile) && set.files.count(own))set.defaultKey=own;
            else for(const auto& [key,bytes]:set.files)if(cart::aiKindOf(key)==AiKind::Default) {set.defaultKey=key;break;}
        }
        const auto count=set.files.size();
        aiMod=std::move(set);
        aiUseRoster(pack);   // this pack's units are listed (its additions too)
        aiViewCache=AiTabView{};aiIssues.clear();aiView=AiView::Table;aiFocus=AI_NONE;aiScroll=0;
        aiNote="Opened game mod "+pack+" ("+std::to_string(count)+" AI file"+(count==1?"":"s")+")."+
            (aiModOnly?" No map is open; edits have no undo.":" Edits here are the pack's, not the map's, and have no undo.");
    } catch(const std::exception& error) {aiNote="Could not open game mod "+pack+": "+error.what();}
}

void cart::Editor::aiClosePack(std::function<void()> then) {
    if(!aiMod) {if(then)then();return;}
    auto close=[this,then] {
        if(aiModOnly) {finishAiModWorkspace();if(then)then();return;}   // no map to return to inside the window
        aiMod.reset();aiModSaveOpen=false;aiViewCache=AiTabView{};aiIssues.clear();aiView=AiView::Table;aiFocus=AI_NONE;aiNote.clear();
        if(aiOpen)aiUseRoster(aiPack?aiPacks[size_t(aiPack-1)]:"");
        if(then)then();
    };
    if(aiModDirty())openConfirm("CLOSE PACK","Discard the unsaved changes to game mod "+aiMod->pack+
        "? They are not part of the map, so Undo cannot bring them back.",close);
    else close();
}

void cart::Editor::aiStepModDefault() {
    if(!aiMod)return;
    std::vector<std::string> keys{"ai/default.txt"};
    if(!aiModOnly && !cart::standardAiProfile(scenario.aiProfile))keys.push_back(cart::aiProfileFileKey(scenario.aiProfile));
    for(const auto& [key,bytes]:aiMod->files)if(cart::aiKindOf(key)==AiKind::Default)keys.push_back(key);
    keys.push_back(aiMod->defaultKey);   // a name given with NEW NAME... stays until the file switches
    std::vector<std::string> unique;for(const auto& key:keys)if(std::find(unique.begin(),unique.end(),key)==unique.end())unique.push_back(key);
    const auto at=std::find(unique.begin(),unique.end(),aiMod->defaultKey);
    aiMod->defaultKey=at==unique.end() || at+1==unique.end()?unique.front():*(at+1);
    aiViewCache=AiTabView{};aiScroll=0;aiNote.clear();
    if(aiView==AiView::Text) {aiTextKey.clear();aiSelectTab(aiTab);}
}

void cart::Editor::aiOpenPackList() {
    aiModOpenOpen=true;aiModPackScroll=aiModPackSel=0;aiModPackThumbDrag=false;
    aiModPickWorkspace=!aiOpen;   // from File > Open AI mod (the window is not up)
}

// NEW NAME...: the DEFAULT tab edits a named profile ai/<name>.txt of the pack, which a
// map reads when its aiprofile= names it (the map's own-profile name rules).
void cart::Editor::aiCommitModName() {
    if(!aiMod)return;
    const auto name=trimmed(aiName);aiName.clear();
    if(name.empty())return;
    if(const auto why=cart::aiProfileNameError(name);!why.empty()) {aiNote=why;return;}
    const auto key=cart::aiProfileFileKey(name);
    aiMod->defaultKey=key;aiViewCache=AiTabView{};aiScroll=0;
    if(aiView==AiView::Text) {aiTextKey.clear();aiSelectTab(0);}
    aiNote=aiMod->files.count(key)?"The DEFAULT tab shows the pack's "+key+".":
        "The DEFAULT tab now edits "+key+": maps whose profile name is \""+name+"\" read it when the pack is ticked. "
        "START FROM... fills it, or set units in the table.";
}

// ---- an AI mod without a map (File > New AI mod / Open AI mod) ------------------------------
// The window opens with a game mod pack as the only document. The map is never shown: a
// map without unsaved work is put aside as it is (and comes back when the mod closes);
// one with unsaved work is discarded after the menu command asked, leaving a blank
// placeholder; a new map nothing was done to counts as no work. Either of those two
// leaves no document behind (noDocument): closing the mod returns to the empty editor.
bool cart::Editor::mapHasUnsavedWork() const {
    return dirty && (historyPending || history.canUndo() || history.canRedo() || !recoveredFrom.empty() || !mapPath.empty() ||
                     !scenario.generatorRecipe.empty());
}

void cart::Editor::requestAiMod(bool create) {
    auto next=[this,create] {
        if(create) {openModal(M_NEWAIMOD);return;}
        try {aiPacks=tak::hpi::modPacks(tak::u8path(dataRoot),tak::hpi::ModKind::Game);} catch(const std::exception&) {aiPacks.clear();}
        if(aiPacks.empty()) {openMessage("OPEN AI MOD","This install has no game mods yet (mods/game/<pack>/). File > New AI mod makes one.");return;}
        aiOpenPackList();
    };
    if(aiModOnly) {leaveAiModThen(next);return;}
    if(aiOpen)return;   // the map's own AI window: DONE first
    commitHistory();
    if(mapHasUnsavedWork())openConfirm(create?"NEW AI MOD":"OPEN AI MOD","An AI mod is edited without a map. Close map \""+mapName+
        "\" and discard its unsaved changes?",next);
    else next();
}

void cart::Editor::startAiModWorkspace(const std::string& pack,bool fresh) {
    commitHistory();
    if(dirty) {   // nothing to return to: closing the mod leaves no document open
        if(mapHasUnsavedWork()) {   // asked already (requestAiMod): a blank placeholder replaces it
            mapView.quiesce();
            try {
                cart::SectionLibrary freshSections;freshSections.scan(vfs,"aramon");
                auto map=buildFreshMap(vfs,freshSections,mapView.compositor(),"aramon",8,8,false);
                if(map.map.width)adoptFreshMap(std::move(map),"Untitled","aramon");
            } catch(const std::exception& error) {openMessage("AI MOD",error.what());return;}
            resetHistory=true;commitHistory();
        }
        history.markSaved();dirty=historyPending=false;noDocument=true;
    }
    aiPack=0;aiPackFiles.reset();
    openAiProfiles();
    aiModOnly=true;aiModPickWorkspace=false;aiTab=1;aiPreviewLevel=Difficulty::Normal;
    if(fresh) {
        AiModSet set;set.pack=pack;aiMod=std::move(set);
        aiNote="New AI mod "+pack+": edit TAK ENGINE AI (all six levels) and DEFAULT AI (RETAIL), then SAVE TO PACK makes mods/game/"+pack+"/ai/.";
    } else {
        aiOpenPack(pack);
        if(!aiMod) {const auto why=aiNote;finishAiModWorkspace();openMessage("OPEN AI MOD FAILED",why);return;}
    }
    std::fprintf(stderr,"cartographer: AI mod %s (%s), no map open\n",pack.c_str(),fresh?"new":"opened");
}

void cart::Editor::finishAiModWorkspace() {
    aiMod.reset();aiModSaveOpen=aiModOpenOpen=aiModPickWorkspace=false;aiAfterSave=nullptr;
    aiOpen=aiStartOpen=false;aiFocus=AI_NONE;aiView=AiView::Table;aiPendingName.clear();aiWordPick=-2;aiGroupCell=-1;
    aiViewCache=AiTabView{};aiIssues.clear();aiNote.clear();
    aiModOnly=false;   // back to the map put aside, or to no document (noDocument)
    SDL_StopTextInput();
}

void cart::Editor::leaveAiModThen(std::function<void()> then) {
    aiCommitText();
    auto leave=[this,then] {finishAiModWorkspace();if(then)then();};
    if(aiModDirty())openConfirm("CLOSE AI MOD","Discard the unsaved changes to game mod "+aiMod->pack+"? They are not saved anywhere else.",leave);
    else leave();
}

// TEST IN GAME: the pack as saved, on one of the install's maps, in the normal private
// single-player room with this pack ticked (takclient --play-map <snapshot> --game-mod <pack>).
void cart::Editor::aiTestInGame() {
    aiCommitText();
    if(!aiMod)return;
    if(playtest.running()) {aiNote="A test is already running. Close its game window before starting another.";return;}
    auto pick=[this] {
        if(!aiMod)return;
        std::vector<std::string> packs;
        try {packs=tak::hpi::modPacks(tak::u8path(dataRoot),tak::hpi::ModKind::Game);} catch(const std::exception&) {}
        if(std::none_of(packs.begin(),packs.end(),[&](const std::string& p){return p==aiMod->pack;})) {
            aiNote="Save the pack first: mods/game/"+aiMod->pack+" is not in the data folder yet, so the game has nothing to tick.";return;
        }
        openTestMapPicker();
    };
    if(aiModDirty())openConfirm("TEST IN GAME","The game plays the pack as it is saved in the data folder. Save the changes to "+aiMod->pack+" first?",
        [this,pick] {aiOpenModSave();if(aiModSaveOpen)aiAfterSave=pick;});
    else pick();
}

void cart::Editor::openTestMapPicker() {
    openModal(M_OPENPATH);
    openForTest=true;mTitle="TEST IN GAME: CHOOSE A MAP";mLabel[0]="SEARCH THE INSTALL'S MAPS";
    openCatalog.clear();
    for(const auto& [name,path]:tak::hpi::listMaps(vfs))openCatalog.emplace_back(name,path);
    openFilterDirty=true;filterOpenMaps();
}

void cart::Editor::aiLaunchTest(const std::string& mapPath) {
    if(!aiMod)return;
    std::string label=mapPath;
    for(const auto& [name,path]:openCatalog)if(path==mapPath)label=name;
    try {
        const auto client=gameClient();
        // A distinct name keeps earlier cached test copies apart, as Test Map does.
        const auto name="Test "+std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        const auto files=installedMapFiles(mapPath,name);
        std::string error;
        if(!playtest.start(client,tak::u8path(dataRoot),files,error,aiMod->pack))throw std::runtime_error(error);
        playtestAiMod=true;
        openMessage("TEST IN GAME","Opened a private game lobby on a test copy of "+label+" with game mod "+aiMod->pack+
            " ticked for that room only (your own ticks are kept). Seat AIs at the levels to test (RETAIL for the DEFAULT AI), then start. Close the game to return here."+
            cart::alwaysEnabledGameNote(tak::u8path(dataRoot)));
    } catch(const std::exception& e) {openMessage("TEST IN GAME FAILED",e.what());}
}

cart::ScrollBar cart::Editor::aiModPackBar() const {
    ScrollBar bar;bar.w=8;bar.x=aiModPackList.x+aiModPackList.w-bar.w;bar.y=aiModPackList.y;bar.h=aiModPackList.h;
    bar.content=int(aiPacks.size())*kAiPackRowH;bar.view=std::min<int>(int(aiPacks.size()),kAiPackRowsShown)*kAiPackRowH;
    return bar;
}

void cart::Editor::aiRevealPack() {
    const int view=std::min<int>(int(aiPacks.size()),kAiPackRowsShown)*kAiPackRowH,top=aiModPackSel*kAiPackRowH;
    if(top<aiModPackScroll)aiModPackScroll=top;
    else if(top+kAiPackRowH>aiModPackScroll+view)aiModPackScroll=top+kAiPackRowH-view;
}

// The Save-as-game-mod and Open-from-game-mod panels take all input while up.
bool cart::Editor::handleAiModDialogs(SDL_Event& e) {
    if(aiModOpenOpen) {
        const int n=int(aiPacks.size());
        const auto bar=aiModPackBar();
        if(aiModPackThumbDrag) {   // the thumb owns the pointer until release
            if(e.type==SDL_MOUSEMOTION) {
                if(!(e.motion.state&SDL_BUTTON_LMASK))aiModPackThumbDrag=false;
                else aiModPackScroll=bar.scrollForThumb(e.motion.y-aiModPackThumbGrab);
            } else if(e.type==SDL_MOUSEBUTTONUP && e.button.button==SDL_BUTTON_LEFT)aiModPackThumbDrag=false;
            return true;
        }
        if(e.type==SDL_MOUSEWHEEL)aiModPackScroll=bar.clamp(aiModPackScroll-e.wheel.y*2*kAiPackRowH);
        else if(e.type==SDL_KEYDOWN) {
            const auto key=e.key.keysym.sym;const int page=kAiPackRowsShown-1;
            if(key==SDLK_ESCAPE)aiModOpenOpen=false;
            else if((key==SDLK_RETURN || key==SDLK_KP_ENTER) && n>0)aiOpenPack(aiPacks[size_t(std::clamp(aiModPackSel,0,n-1))]);
            else if(n>0) {
                int sel=aiModPackSel;
                if(key==SDLK_UP)--sel;else if(key==SDLK_DOWN)++sel;
                else if(key==SDLK_PAGEUP)sel-=page;else if(key==SDLK_PAGEDOWN)sel+=page;
                else if(key==SDLK_HOME)sel=0;else if(key==SDLK_END)sel=n-1;
                else return true;
                aiModPackSel=std::clamp(sel,0,n-1);aiRevealPack();
            }
        } else if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
            const int mx=e.button.x,my=e.button.y;
            if(bar.inTrack(mx,my)) {
                if(bar.inThumb(mx,my,aiModPackScroll)) {aiModPackThumbDrag=true;aiModPackThumbGrab=my-bar.thumbTop(aiModPackScroll);}
                else aiModPackScroll=bar.page(aiModPackScroll,my);
                return true;
            }
            if(cart::pointIn(mx,my,aiModPackList))for(const auto& [rect,pack]:aiModPackRows)if(cart::pointIn(mx,my,rect)) {aiOpenPack(pack);return true;}
            if(cart::pointIn(mx,my,aiModCancelButton))aiModOpenOpen=false;
        }
        return true;
    }
    if(!aiModSaveOpen)return false;
    if(e.type==SDL_TEXTINPUT) {if(aiFocus==AI_PACK && aiModPackEdit.input(e,aiModPack))aiModReplan();return true;}
    if(e.type==SDL_KEYDOWN) {
        const auto key=e.key.keysym.sym;const bool enter=key==SDLK_RETURN || key==SDLK_KP_ENTER;
        if(aiFocus==AI_PACK) {
            if(key==SDLK_ESCAPE || enter)aiFocus=AI_NONE;
            else if(aiModPackEdit.input(e,aiModPack))aiModReplan();
            return true;
        }
        if(key==SDLK_ESCAPE) {aiModSaveOpen=false;aiAfterSave=nullptr;} else if(enter)aiModSave();
        return true;
    }
    if(e.type!=SDL_MOUSEBUTTONDOWN || e.button.button!=SDL_BUTTON_LEFT)return true;
    const int mx=e.button.x,my=e.button.y;
    if(aiFocus==AI_PACK && !cart::pointIn(mx,my,aiModPackBox))aiFocus=AI_NONE;
    if(!aiMod && cart::pointIn(mx,my,aiModPackBox)) {if(aiFocus!=AI_PACK) {aiFocus=AI_PACK;aiModPackEdit.focus(aiModPack);}return true;}
    if(!aiMod && !aiPacks.empty() && (cart::pointIn(mx,my,aiModPackPrev) || cart::pointIn(mx,my,aiModPackNext))) {
        const int n=int(aiPacks.size());int at=-1;
        for(int i=0;i<n;++i)if(lowerText(aiPacks[size_t(i)])==lowerText(trimmed(aiModPack)))at=i;
        at=cart::pointIn(mx,my,aiModPackNext)?(at+1)%n:at<=0?n-1:at-1;
        aiModPack=aiPacks[size_t(at)];aiModReplan();return true;
    }
    if(cart::pointIn(mx,my,aiModDefaultAsBox)) {aiModDefaultAs^=1;aiModReplan();return true;}
    if(cart::pointIn(mx,my,aiModWriteButton)) {aiModSave();return true;}
    if(cart::pointIn(mx,my,aiModCancelButton)) {aiModSaveOpen=false;aiAfterSave=nullptr;return true;}
    for(auto& row:aiModRows)if(cart::pointIn(mx,my,row.box)) {row.tick=!row.tick;aiModNote.clear();aiModReplan();return true;}
    for(const auto& [rect,key]:aiModDeleteBoxes)if(cart::pointIn(mx,my,rect)) {
        if(!aiModDeleteTicked.erase(key))aiModDeleteTicked.insert(key);
        aiModNote.clear();return true;
    }
    return true;
}

void cart::Editor::drawAiModDialogs() {
    using A=ModProfileChange::Action;
    if(aiModOpenOpen) {
        const int n=int(aiPacks.size()),shown=std::min(n,kAiPackRowsShown);
        const auto pc=cart::drawPanel(ren,w,h,420,96+shown*kAiPackRowH,aiModPickWorkspace?"OPEN AI MOD":"OPEN FROM GAME MOD");
        cart::drawText(ren,aiModPickWorkspace?"Edit one game mod pack's ai/ files, without a map. No undo.":
            "Edit one pack's ai/ files. They stay the pack's: not in the map, no undo.",pc.x,pc.y,1,220,225,235);
        aiModPackList={pc.x,pc.y+18,pc.w,shown*kAiPackRowH};
        const auto bar=aiModPackBar();
        aiModPackScroll=bar.clamp(aiModPackScroll);aiModPackSel=n?std::clamp(aiModPackSel,0,n-1):0;
        const int rowW=bar.active()?bar.x-2-pc.x:pc.w;
        aiModPackRows.clear();
        SDL_RenderSetClipRect(ren,&aiModPackList);
        for(int i=aiModPackScroll/kAiPackRowH;i<n && i*kAiPackRowH<aiModPackScroll+aiModPackList.h;++i) {
            const SDL_Rect r{pc.x,aiModPackList.y+i*kAiPackRowH-aiModPackScroll,rowW,kAiPackRowH-2};
            if(i==aiModPackSel)fillRect(ren,r.x,r.y,r.w,r.h,50,60,82);else fillRect(ren,r.x,r.y,r.w,r.h,30,34,44);
            cart::drawText(ren,aiPacks[size_t(i)],r.x+4,r.y+6,1,215,225,240);
            aiModPackRows.push_back({r,aiPacks[size_t(i)]});
        }
        SDL_RenderSetClipRect(ren,nullptr);
        if(bar.active()) {   // track, then the thumb (brighter while hovered or dragged), as in the browser
            int mx,my;SDL_GetMouseState(&mx,&my);mx=int(mx/kUIScale);my=int(my/kUIScale);
            const bool hot=aiModPackThumbDrag || bar.inThumb(mx,my,aiModPackScroll);
            fillRect(ren,bar.x,bar.y,bar.w,bar.h,22,24,30);
            fillRect(ren,bar.x+1,bar.thumbTop(aiModPackScroll)+1,bar.w-2,bar.thumbHeight()-2,hot?140:96,hot?150:104,hot?185:132);
            cart::drawText(ren,std::to_string(n)+" packs: the wheel or the bar scrolls; Up/Down and Enter pick.",pc.x,aiModPackList.y+aiModPackList.h+4,1,150,160,175);
        }
        aiModCancelButton=cart::drawButton(ren,pc.x+pc.w-74,pc.y+pc.h-20,70,18,"CANCEL",false);
        return;
    }
    if(!aiModSaveOpen)return;
    const auto pc=cart::drawPanel(ren,w,h,660,470,aiMod?"SAVE TO GAME MOD PACK":"SAVE AS GAME MOD");
    int y=pc.y;
    aiModPackBox=aiModPackPrev=aiModPackNext=aiModDefaultAsBox={};
    if(!aiMod) {
        aiModPackBox=cart::drawField(ren,pc.x,y,260,"PACK (NEW, OR AN EXISTING ONE)",aiModPack,aiFocus==AI_PACK,aiFocus==AI_PACK?&aiModPackEdit:nullptr);
        if(!aiPacks.empty()) {
            aiModPackPrev=cart::drawButton(ren,pc.x+266,y+10,20,16,"<",false);
            aiModPackNext=cart::drawButton(ren,pc.x+290,y+10,20,16,">",false);
        }
        cart::drawText(ren,aiPacks.empty()?"No game mods yet: this makes the first.":std::to_string(aiPacks.size())+" existing pack"+(aiPacks.size()==1?"":"s")+" (< > steps through them).",
                       pc.x+318,y+15,1,150,160,175);
    } else cart::drawText(ren,"PACK: "+aiMod->pack+(aiModOnly?" (mods/game/"+aiMod->pack+" in the data folder)":" (the pack these files were opened from)"),pc.x,y+12,1,255,215,150);
    y+=34;
    const std::string folder=aiModPlan.folder.empty()?"":pathText(aiModPlan.folder)+"/";
    cart::drawText(ren,"FOLDER",pc.x,y,1,150,200,150);
    y+=12+12*drawWrapped(ren,folder.empty()?"-":folder,pc.x,y+12,pc.w,2,220,225,235);
    if(!aiModPlan.create.empty())cart::drawText(ren,aiModPlan.create.size()>=2?"A new pack: the missing folders are created.":"The pack's ai/ folder is created.",pc.x,y+2,1,170,200,175);
    else if(!aiModPlan.folder.empty() && trimmed(aiModPack)!=aiModPlan.pack)cart::drawText(ren,"Existing pack \""+aiModPlan.pack+"\" (its own spelling is kept).",pc.x,y+2,1,170,200,175);
    y+=20;
    cart::drawText(ren,"WRITE",pc.x,y,1,150,200,150);y+=14;
    auto changeFor=[&](const std::string& key)->const ModProfileChange* {
        for(const auto& change:aiModPlan.changes)if(change.key==key)return &change;
        return nullptr;
    };
    for(auto& row:aiModRows) {
        row.box={pc.x,y,16,14};
        SDL_SetRenderDrawColor(ren,96,98,112,255);SDL_RenderDrawRect(ren,&row.box);
        if(row.tick)cart::drawText(ren,"x",pc.x+5,y+4,1,255,235,200);
        std::string text=row.label+" -> "+row.target;
        if(row.isDefault && row.source!=row.target)text=row.label+" ("+row.source+") as";
        cart::drawText(ren,text,pc.x+22,y+4,1,220,225,235);
        int sx=pc.x+22+cart::textWidth(text,1)+8;
        if(row.isDefault) {
            const bool own=!cart::standardAiProfile(scenario.aiProfile) && row.source!="ai/default.txt";
            aiModDefaultAsBox=own?cart::drawButton(ren,sx,y,150,14,row.target,false):SDL_Rect{};
            if(!own)cart::drawText(ren,"(this map's file is ai/default.txt)",sx,y+4,1,150,160,175);
            sx+=158;
        }
        std::string status;Uint8 r=160,g=200,b=170;
        if(!row.tick)status="not written";
        else if(const auto* change=changeFor(row.target)) {
            if(change->action==A::Create)status="new";
            else if(change->action==A::Same)status="identical: skipped";
            else {status="replaces the pack's file (old copy backed up)";r=235;g=200;b=130;}
            if(!change->archive.empty())status+="; the loose file will replace the one inside "+change->archive;
        }
        cart::drawText(ren,status,std::max(sx,pc.x+330),y+4,1,r,g,b);
        y+=18;
        if(row.isDefault) {
            const auto name=cart::aiProfileFileKey(scenario.aiProfile);
            cart::drawText(ren,"default.txt: every map using the standard profile.",pc.x+22,y,1,150,160,175);y+=12;
            if(!cart::standardAiProfile(scenario.aiProfile))
                {cart::drawText(ren,name.substr(3)+": only maps whose profile name is \""+scenario.aiProfile+"\".",pc.x+22,y,1,150,160,175);y+=12;}
            y+=4;
        }
    }
    aiModDeleteBoxes.clear();
    for(const auto& key:aiModDeletes) {
        const auto* change=changeFor(key);
        if(!change || change->action!=A::Delete)continue;   // the pack does not have it
        const SDL_Rect box{pc.x,y,16,14};aiModDeleteBoxes.push_back({box,key});
        SDL_SetRenderDrawColor(ren,96,98,112,255);SDL_RenderDrawRect(ren,&box);
        const bool ticked=aiModDeleteTicked.count(key)>0;
        if(ticked)cart::drawText(ren,"x",pc.x+5,y+4,1,255,235,200);
        cart::drawText(ren,"Delete "+key+" from the pack (its draft is empty)",pc.x+22,y+4,1,220,225,235);
        if(ticked)cart::drawText(ren,"old copy backed up",pc.x+330,y+4,1,235,200,130);
        y+=18;
    }
    y+=6;
    y+=12*drawWrapped(ren,"! A game mod's file replaces the same file on every map, the whole file, not line by line: its ai/takengine.txt replaces the map's for all six levels. "
        "It applies only when the pack is ticked (multiplayer: the host ticks it in the room) or is in mods/game/always_enabled/ of the host's install. "
        "A game mod's default.txt does not replace a map's own profile.",pc.x,y,pc.w,4,255,205,140)+4;
    // The roster note: the units listed are this pack's when the window reads them (editing or previewing it).
    const std::string target=aiModPlan.pack.empty()?trimmed(aiModPack):aiModPlan.pack;
    if(aiRosterLists(target))
        y+=12*drawWrapped(ren,"The units listed are this pack's, its "+std::to_string(aiPackRoster->added.size())+" additions included.",pc.x,y,pc.w,2,150,160,175)+4;
    else
        y+=12*drawWrapped(ren,"Units this pack adds are not listed (PREVIEW WITH GAME MOD lists a pack's units). Our levels never build a unit that neither their built-in table nor ai/takengine.txt names; the DEFAULT AI builds them at full chance.",
            pc.x,y,pc.w,2,150,160,175)+4;
    if(!aiModPlanError.empty())drawWrapped(ren,aiModPlanError,pc.x,y,pc.w,3,255,150,135);
    else if(!aiModNote.empty())drawWrapped(ren,aiModNote,pc.x,y,pc.w,2,255,200,130);
    cart::drawText(ren,"Backups go to the Cartographer profile folder, not the pack.",pc.x,pc.y+pc.h-14,1,140,150,165);
    aiModWriteButton=cart::drawButton(ren,pc.x+pc.w-150,pc.y+pc.h-20,70,18,"SAVE",aiModPlanError.empty());
    aiModCancelButton=cart::drawButton(ren,pc.x+pc.w-74,pc.y+pc.h-20,70,18,"CANCEL",false);
}

// ---- group rules: the DEFAULT tab's Advanced view (design 4) ------------------------------

void cart::Editor::drawAiGroups(int x0,int y0,int W,int listBottom) {
    using cart::AiGroupUnit;
    const auto& view=aiTabView();const auto& catalogue=*aiCatalogue;
    const bool editable=aiGroupsEditable();
    struct Shown {bool weight;std::string word,value;std::vector<AiGroupUnit> units;};
    std::vector<Shown> rules;
    if(editable) {
        for(size_t i=0;i<view.draft.groups.size();++i) {
            const auto& g=view.draft.groups[i];
            rules.push_back({g.weight,g.word,g.value,cart::aiGroupRuleUnits(view.draft,i,catalogue)});
        }
    } else {   // the file's own group lines, read only
        for(size_t i=0;i<view.lines.size();++i)if(view.lines[i].what==AiLine::What::Group) {
            const auto& line=view.lines[i];
            rules.push_back({line.weight,line.word,line.value,cart::aiGroupLineUnits(view.lines,i,catalogue)});
        }
    }
    aiGroupSelected=rules.empty()?-1:std::clamp(aiGroupSelected,0,int(rules.size())-1);
    auto valueText=[](const Shown& r) {
        const AiGroupRule g{r.weight,r.word,r.value};
        if(r.weight) {
            const int p=cart::aiGroupRuleChance(g);
            return std::to_string(p)+" %"+(r.value==cart::classicChanceText(p)?"":" ("+(r.value.empty()?std::string("none"):r.value)+")");
        }
        const int n=cart::aiGroupRuleLimit(g);
        return n<0?std::string("no limit"):std::to_string(n);
    };
    cart::drawText(ren,"GROUP RULES: THEY APPLY AFTER THE UNIT ROWS AND NEVER CHANGE A UNIT SET INDIVIDUALLY.",x0,y0+46,1,235,220,160);
    if(!editable)cart::drawText(ren,view.key.empty()?"The installed standard profile: read only. Choose this map's own profile to add rules.":
        "Written by hand: read only until MAKE EDITABLE (the Text view edits it).",x0,y0+58,1,150,160,175);
    cart::drawText(ren,"KEYWORD",x0+4,y0+72,1,150,200,150);
    cart::drawText(ren,"WORD",x0+70,y0+72,1,150,200,150);
    cart::drawText(ren,"VALUE",x0+196,y0+72,1,150,200,150);
    cart::drawText(ren,"COVERS",x0+290,y0+72,1,150,200,150);
    aiGroupList={x0,y0+82,W,6*20+4};
    fillRect(ren,aiGroupList.x,aiGroupList.y,aiGroupList.w,aiGroupList.h,22,24,32);
    aiGroupScroll=std::clamp(aiGroupScroll,0,std::max(0,int(rules.size())*20+4-aiGroupList.h));
    SDL_RenderSetClipRect(ren,&aiGroupList);
    for(int i=0;i<int(rules.size());++i) {
        const auto& r=rules[size_t(i)];const int y=aiGroupList.y+2+i*20-aiGroupScroll;
        AiGroupRow row;row.row={x0,y,W,19};
        if(i==aiGroupSelected)fillRect(ren,x0,y,W,19,50,60,82);
        if(editable) {
            row.keyword=cart::drawButton(ren,x0+4,y+1,60,17,r.weight?"WEIGHT":"LIMIT",false);
            row.word=cart::drawButton(ren,x0+70,y+1,120,17,upperText(r.word),false);
        } else {
            cart::drawText(ren,r.weight?"WEIGHT":"LIMIT",x0+8,y+6,1,200,205,215);
            cart::drawText(ren,upperText(r.word),x0+74,y+6,1,200,205,215);
        }
        row.value={x0+196,y+1,88,17};
        if(aiFocus==AI_GROUP && aiGroupCell==i)inlineField(ren,row.value.x,row.value.y,row.value.w,aiCellText,true,&aiCellEdit);
        else {
            SDL_SetRenderDrawColor(ren,editable?62:44,editable?66:46,editable?82:54,255);SDL_RenderDrawRect(ren,&row.value);
            cart::drawText(ren,valueText(r),row.value.x+4,row.value.y+6,1,235,235,240);
        }
        int skipped=0;bool twice=false;
        for(const auto& u:r.units) {skipped+=u.skipped;twice|=u.times>1;}
        std::string covers=std::to_string(r.units.size())+(r.units.size()==1?" unit":" units");
        if(skipped)covers+=", "+std::to_string(skipped)+" set individually (skipped)";
        if(twice && r.weight)covers+=", some twice";
        cart::drawText(ren,covers,x0+290,y+6,1,170,185,200);
        if(editable) {
            row.up=i>0?cart::drawButton(ren,x0+W-70,y+1,20,17,"^",false):SDL_Rect{};
            row.down=i+1<int(rules.size())?cart::drawButton(ren,x0+W-46,y+1,20,17,"v",false):SDL_Rect{};
            row.remove=cart::drawButton(ren,x0+W-22,y+1,20,17,"X",false);
        }
        aiGroupRows.push_back(row);
    }
    if(rules.empty())cart::drawText(ren,editable?"No group rules. ADD GROUP RULE picks one of this install's category words, or ALL.":"This file has no group rules.",x0+6,aiGroupList.y+6,1,205,210,220);
    SDL_RenderSetClipRect(ren,nullptr);
    SDL_SetRenderDrawColor(ren,70,74,90,255);SDL_RenderDrawRect(ren,&aiGroupList);
    const int ay=aiGroupList.y+aiGroupList.h+4;
    aiAddGroupButton=cart::drawButton(ren,x0,ay,150,16,"+ ADD GROUP RULE",editable);
    cart::drawText(ren,"Words from this install's unit categories, or ALL.",x0+158,ay+5,1,140,150,165);

    // The units the selected rule covers, with the ones its keyword's lock skips.
    const int uy=ay+24;
    if(aiGroupSelected<0)return;
    const auto& r=rules[size_t(aiGroupSelected)];
    cart::drawText(ren,"UNITS COVERED BY \""+std::string(r.weight?"weight ":"limit ")+r.word+"\" ("+valueText(r)+"): NOW",x0,uy,1,150,200,150);
    aiGroupUnits={x0,uy+12,W,std::max(24,listBottom-uy-12)};
    fillRect(ren,aiGroupUnits.x,aiGroupUnits.y,aiGroupUnits.w,aiGroupUnits.h,22,24,32);
    const int columns=2,colW=W/columns,rowsShown=int(r.units.size()+columns-1)/columns;
    aiGroupUnitScroll=std::clamp(aiGroupUnitScroll,0,std::max(0,rowsShown*12+6-aiGroupUnits.h));
    SDL_RenderSetClipRect(ren,&aiGroupUnits);
    for(size_t k=0;k<r.units.size();++k) {
        const auto& u=r.units[k];
        const int cx=x0+4+int(k%columns)*colW,cy=aiGroupUnits.y+3+int(k/columns)*12-aiGroupUnitScroll;
        if(cy<aiGroupUnits.y-12 || cy>aiGroupUnits.y+aiGroupUnits.h)continue;
        const auto* type=catalogue.types[u.index];
        std::string label=type && !type->name.empty()?type->name:catalogue.names[u.index];
        if(label.size()>18)label=label.substr(0,17)+".";
        label+=" ("+catalogue.names[u.index]+")";
        std::string state;
        if(u.skipped)state="set individually, skipped";
        else if(r.weight)state=std::to_string(view.classic.pref[u.index])+" %"+(u.times>1?" (x"+std::to_string(u.times)+")":"");
        else state=view.classic.limit[u.index]<0?"no limit":"limit "+std::to_string(view.classic.limit[u.index]);
        cart::drawText(ren,label,cx,cy,1,u.skipped?150:215,u.skipped?150:225,u.skipped?160:240);
        cart::drawText(ren,state,cx+colW-12-cart::textWidth(state,1),cy,1,u.skipped?150:170,u.skipped?150:200,u.skipped?160:175);
    }
    if(r.units.empty())cart::drawText(ren,"No unit of this install has this word.",x0+4,aiGroupUnits.y+3,1,205,210,220);
    SDL_RenderSetClipRect(ren,nullptr);
    SDL_SetRenderDrawColor(ren,70,74,90,255);SDL_RenderDrawRect(ren,&aiGroupUnits);
}

void cart::Editor::drawAiWordPicker() {
    if(aiWordPick==-2)return;
    const auto pc=cart::drawPanel(ren,w,h,520,380,aiWordPick<0?"ADD GROUP RULE":"GROUP RULE WORD");
    cart::drawText(ren,"Category words of this install's units (FBI category=), and ALL. The number counts the units.",pc.x,pc.y,1,220,225,235);
    aiWordSearchBox=cart::drawField(ren,pc.x,pc.y+14,200,"FIND",aiWordSearch,aiFocus==AI_WORD,aiFocus==AI_WORD?&aiWordSearchEdit:nullptr);
    const auto query=lowerText(trimmed(aiWordSearch));
    std::vector<std::string> words;
    for(auto& word:cart::aiGroupWords(*aiCatalogue))if(query.empty() || word.find(query)!=std::string::npos)words.push_back(std::move(word));
    aiWordList={pc.x,pc.y+50,pc.w,pc.h-50-26};
    fillRect(ren,aiWordList.x,aiWordList.y,aiWordList.w,aiWordList.h,22,24,32);
    constexpr int columns=3;const int colW=aiWordList.w/columns;
    aiWordScroll=std::clamp(aiWordScroll,0,std::max(0,int(words.size()+columns-1)/columns*20+4-aiWordList.h));
    aiWordRows.clear();
    SDL_RenderSetClipRect(ren,&aiWordList);
    for(size_t i=0;i<words.size();++i) {
        const SDL_Rect r{aiWordList.x+4+int(i%columns)*colW,aiWordList.y+3+int(i/columns)*20-aiWordScroll,colW-8,17};
        const auto& ids=aiCatalogue->categories.at(words[i]);
        const size_t count=std::set<uint16_t>(ids.begin(),ids.end()).size();
        fillRect(ren,r.x,r.y,r.w,r.h,36,40,52);
        std::string label=upperText(words[i]);if(label.size()>16)label=label.substr(0,15)+".";
        cart::drawText(ren,label+" ("+std::to_string(count)+")",r.x+4,r.y+6,1,215,225,240);
        aiWordRows.push_back({r,words[i]});
    }
    if(words.empty())cart::drawText(ren,"No category word matches.",aiWordList.x+6,aiWordList.y+6,1,205,210,220);
    SDL_RenderSetClipRect(ren,nullptr);
    SDL_SetRenderDrawColor(ren,70,74,90,255);SDL_RenderDrawRect(ren,&aiWordList);
    cart::drawText(ren,"Enter picks the only match.",pc.x,pc.y+pc.h-14,1,140,150,165);
    aiWordCancel=cart::drawButton(ren,pc.x+pc.w-74,pc.y+pc.h-20,70,18,"CANCEL",false);
}
