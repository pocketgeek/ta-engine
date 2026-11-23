#include "cartographer/sectiongroups.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace cart {
namespace {

std::string lower(std::string s) {for(char& c:s)c=char(std::tolower((unsigned char)c));return s;}
bool isDir(char c) {return c=='n' || c=='s' || c=='e' || c=='w';}
bool tagLetter(char c) {return std::string_view("abcdfgxyz").find(c)!=std::string_view::npos;}

// Artist words seen in retail names (docs survey). kind=true words name the piece;
// the rest describe it. Every word starting with n/s/e/w must be listed, or its
// first letter would read as a direction.
struct Word {const char* text;const char* noun;bool kind;};
constexpr Word kWords[]={
    {"clif","Cliff",true},{"cliff","Cliff",true},{"trans","Transition",true},{"ramp","Ramp",true},
    {"mid","Mid slope",true},{"deadend","Dead end",true},{"path","Path",true},{"hwy","Road",true},
    {"road","Road",true},{"gate","Gate",true},{"sett","Settlement",true},{"settle","Settlement",true},
    {"end","End",true},{"head","Head",true},{"corn","Corner",true},{"clearing","Clearing",true},
    {"patch","Patch",true},{"stair","Stairs",true},{"rvr","River",true},{"river","River",true},
    {"delta","Delta",true},{"xing","Crossing",true},{"cross","Crossing",true},{"bridge","Bridge",true},
    {"rivbr","Bridge",true},{"mouth","Mouth",true},{"water","Water",true},{"coast","Coast",true},
    {"coastal","Coast town",true},{"isle","Island",true},{"mudisle","Mud island",true},{"sink","Sinkhole",true},
    {"hills","Hills",true},{"hill","Hill",true},{"hillet","Hillock",true},{"mttp","Mountain top",true},
    {"mt","Mountain",true},{"plazard","Plaza road",true},{"plaza","Plaza",true},{"breaker","Breaker",true},
    {"break","Breaker",true},{"crack","Crack",true},{"unique","Unique",true},{"outpost","Outpost",true},
    {"farm","Farm",true},{"ent","Entrance",true},{"int","Interior",true},{"interior","Interior",true},
    {"center","Center",true},{"quarry","Quarry",true},{"mounds","Mounds",true},{"rings","Rings",true},
    {"fossil","Fossil",true},{"arena","Arena",true},{"home","Home",true},{"crab","Crab",true},
    {"sandcrab","Sand crab",true},{"fish","Fish",true},{"seahazard","Sea hazard",true},{"fmn","Forget-me-not",true},
    {"tile","Tile",true},{"blacktile","Black tile",true},{"flat","Flat",true},{"lowland","Lowland",true},
    {"highland","Highland",true},{"plateau","Plateau",true},{"mountainplateau","Plateau",true},{"plazatop","Plaza top",true},
    {"cobb","Cobbles",true},{"cobbflat","Cobbles",true},{"cobbles","Cobbles",true},{"ground","Ground",true},
    {"sea","Sea",true},{"lava","Lava",true},{"base","Base",true},{"voided","Void",true},{"dunes","Dunes",true},
    {"low","low",false},{"lo","low",false},{"high","high",false},{"hi","high",false},{"jungle","jungle",false},
    {"savannah","savannah",false},{"snow","snow",false},{"sand","sand",false},{"flatsand","flat sand",false},
    {"wavysand","wavy sand",false},{"dirt","dirt",false},{"darkdirt","dark dirt",false},{"lightdirt","light dirt",false},
    {"darkgr","dark grass",false},{"lightgr","light grass",false},{"small","small",false},{"lft","left",false},
    {"rgt","right",false},{"top","top",false},{"btm","bottom",false},{"lit","lit",false},{"unlit","unlit",false},
    {"pat","pattern",false},{"redo","redo",false},{"stinks","stinks",false},
};
const Word* wordAt(std::string_view s,size_t pos) {
    const Word* best=nullptr;size_t length=0;
    for(const auto& w:kWords) {const std::string_view t=w.text;if(t.size()>length && s.substr(pos,t.size())==t){best=&w;length=t.size();}}
    return best;
}

struct Token {enum Type {WORD,DIR,NUM,FRAG} type;std::string text;bool sep=false;const Word* word=nullptr;};

std::vector<Token> tokenize(std::string s) {
    for(char& c:s)if(c==' ' || c=='-' || c=='\'')c='_';
    for(size_t p;(p=s.find("h2o"))!=std::string::npos;)s.replace(p,3,"water_");
    // Low/high elevation suffix (aramon "_80" / "_200"): constant per folder.
    for(const char* suffix:{"_80","_200","_230","_00"}) {
        const std::string_view x=suffix;
        if(s.size()>x.size() && s.ends_with(x)) {s.resize(s.size()-x.size());break;}
    }
    std::vector<Token> out;bool sep=false;
    for(size_t i=0;i<s.size();) {
        if(s[i]=='_') {sep=!out.empty();++i;continue;}
        size_t j=i;
        if(std::isdigit((unsigned char)s[i])) {
            while(j<s.size() && std::isdigit((unsigned char)s[j]))++j;
            out.push_back({Token::NUM,s.substr(i,j-i),sep});
        } else {
            while(j<s.size() && s[j]!='_' && !std::isdigit((unsigned char)s[j]))++j;
            const std::string_view a=std::string_view(s).substr(i,j-i);
            for(size_t p=0;p<a.size();) {
                const bool first=p==0;
                if(const Word* w=wordAt(a,p)) {out.push_back({Token::WORD,std::string(w->text),sep && first,w});p+=std::string_view(w->text).size();continue;}
                if(isDir(a[p])) {
                    size_t q=p;while(q<a.size() && isDir(a[q]) && (q==p || !wordAt(a,q)))++q;
                    if(q==a.size() || wordAt(a,q) || (a.size()-q==1 && tagLetter(a[q]))) {
                        out.push_back({Token::DIR,std::string(a.substr(p,q-p)),sep && first});p=q;continue;
                    }
                }
                size_t q=p+1;while(q<a.size() && !wordAt(a,q))++q;
                out.push_back({Token::FRAG,std::string(a.substr(p,q-p)),sep && first});p=q;
            }
        }
        sep=false;i=j;
    }
    return out;
}

std::string trimZeros(const std::string& n) {const auto p=n.find_first_not_of('0');return p==std::string::npos?"0":n.substr(p);}
bool sizeNumber(const std::string& n) {return n=="256" || n=="512" || n=="1024";}

std::string directionText(const std::vector<std::string>& runs) {
    static const std::set<std::string> compass={"n","s","e","w","ne","nw","se","sw","nne","ene","ese","sse","ssw","wsw","wnw","nnw"};
    auto upper=[](std::string s){for(char& c:s)c=char(std::toupper((unsigned char)c));return s;};
    std::string out;
    for(const auto& run:runs) {
        if(!out.empty())out+='+';
        if(runs.size()>1 || compass.count(run))out+=upper(run);
        else if(std::set<char>(run.begin(),run.end()).size()==run.size()) {for(size_t i=0;i<run.size();++i)out+=std::string(i?"+":"")+char(std::toupper((unsigned char)run[i]));}
        else out+=upper(run);
    }
    return out;
}

const char* const kGroupOrder[]={"Cliffs","Transitions","Water & coast","Paths & roads","Towns & forts","Hills & mountains","Flats","Specials & details","Other"};

} // namespace

SectionName parseSectionName(const std::string& raw,bool stripTag) {
    SectionName r;
    auto tokens=tokenize(lower(raw));
    // Footprint tokens ("_512x", "1024") come out of the token stream: they would
    // otherwise hide a trailing variant letter (ariver01b_512x).
    std::string size;
    for(size_t i=0;i<tokens.size();)
        if(tokens[i].type==Token::NUM && sizeNumber(tokens[i].text)) {
            size=tokens[i].text;tokens.erase(tokens.begin()+long(i));
            if(i<tokens.size() && tokens[i].type==Token::FRAG && tokens[i].text=="x" && !tokens[i].sep)tokens.erase(tokens.begin()+long(i));
        } else ++i;
    // A lone n/s/e/w glued to a two-digit number is a variant letter (ariver01e);
    // after a one-digit number it is the corner piece's direction (1ne).
    for(size_t i=1;i<tokens.size();++i)
        if(tokens[i].type==Token::DIR && tokens[i].text.size()==1 && !tokens[i].sep && tokens[i-1].type==Token::NUM && tokens[i-1].text.size()>=2)tokens[i].type=Token::FRAG;
    // A short leading artist/world code ("a"river, "ac"coast, "y"_e_btm) is noise.
    bool prefix=tokens.size()>1 && tokens[0].type==Token::FRAG && tokens[0].text.size()<=3 && (tokens[1].type==Token::WORD || tokens[1].type==Token::DIR);
    std::string tag;
    if(!tokens.empty() && tokens.back().type==Token::WORD && tokens.back().text=="redo") {tag="redo";tokens.pop_back();}
    if(stripTag && tokens.size()>1) {
        auto& last=tokens.back();
        if(last.type==Token::FRAG && std::string_view("abcdefgxyz").find(last.text.back())!=std::string_view::npos) {
            const std::string t(1,last.text.back());last.text.pop_back();if(last.text.empty())tokens.pop_back();
            tag=tag.empty()?t:t+" "+tag;
        } else if(last.type==Token::WORD && last.text=="lit") {tokens.pop_back();tag=tag.empty()?"lit":"lit "+tag;}
    }
    r.tag=tag;
    std::vector<bool> corner(tokens.size(),false);
    bool directed=false;
    for(size_t i=0;i<tokens.size();++i) {
        if(tokens[i].type==Token::DIR)directed=true;
        if(tokens[i].type==Token::NUM && tokens[i].text.size()==1 && tokens[i].text[0]>='1' && tokens[i].text[0]<='3' &&
           i+1<tokens.size() && tokens[i+1].type==Token::DIR && !tokens[i+1].sep)corner[i]=true;
    }
    int variantAt=-1;
    for(size_t i=0;i<tokens.size();++i)if(tokens[i].type==Token::NUM && !corner[i] && !sizeNumber(tokens[i].text))variantAt=int(i);
    if(!directed)variantAt=-1;
    std::vector<std::string> dirs,details,numbers;
    for(size_t i=0;i<tokens.size();++i) {
        const auto& t=tokens[i];
        if(i)r.key+=t.sep && int(i)!=variantAt?"_":"";
        if(int(i)==variantAt) {r.variant=trimZeros(t.text);r.key+="#";continue;}
        r.key+=t.text;
        if(corner[i]) {r.corner=t.text=="1"?"outer":t.text=="2"?"inner":"type 3";continue;}
        switch(t.type) {
        case Token::DIR:dirs.push_back(t.text);break;
        case Token::NUM:
            if(directed)details.push_back("#"+trimZeros(t.text));
            else numbers.push_back(trimZeros(t.text));
            break;
        case Token::WORD:
            if(t.word->kind && r.kind.empty())r.kind=t.word->noun;
            else details.push_back(lower(t.word->noun));
            break;
        case Token::FRAG:{
            if(i==0 && prefix)break;
            if(i+1==tokens.size() && t.text.size()==1 && i) {r.suffix=char(std::toupper((unsigned char)t.text[0]));break;}
            std::string up=t.text;for(char& c:up)c=char(std::toupper((unsigned char)c));details.push_back(up);break;}
        }
    }
    if(!size.empty()) {r.key+="@"+size;if(size!="512")details.push_back(size+"px");}
    r.direction=directionText(dirs);
    for(size_t i=0;i<numbers.size();++i)r.number+=(i?"-":"")+numbers[i];
    for(size_t i=0;i<details.size();++i)r.details+=(i?" ":"")+details[i];
    return r;
}

std::string sectionKindGroup(const std::string& folder) {
    const std::string f=lower(folder);
    auto has=[&](std::initializer_list<const char*> words) {for(auto* w:words)if(f.find(w)!=std::string::npos)return true;return false;};
    if(has({"cliff","wall"}) || f=="base")return "Cliffs";
    if(has({" to ","transition","hillside","snow line"}))return "Transitions";
    if(has({"town","fort","farm"}))return "Towns & forts";
    if(has({"path","road","walkway","plaza"}))return "Paths & roads";
    if(has({"coast","shore","river","canal","water","sea","island","bridge"}))return "Water & coast";
    if(has({"breaker","special","forgetmenot","clearing"}))return "Specials & details";
    if(has({"mountain","hill","berm","sinkhole"}))return "Hills & mountains";
    if(has({"flat","tile"}))return "Flats";
    return "Other";
}

std::string sectionFolderNoun(const std::string& folder) {
    const std::string f=lower(folder);
    static const std::pair<const char*,const char*> nouns[]={
        {"cliff","Cliff"},{"wall","Wall"},{"hillside","Hillside"},{"snow line","Snow line"},{" to ","Transition"},
        {"transition","Transition"},{"town","Town"},{"fort","Fort"},{"farm","Farm"},{"walkway","Walkway"},
        {"plaza","Plaza"},{"path","Path"},{"road","Road"},{"river","River"},{"canal","Canal"},{"bridge","Bridge"},
        {"shore","Shore"},{"coast","Coast"},{"berm","Berm"},{"water","Water"},{"island","Island"},{"mountain","Mountain"},
        {"sinkhole","Sinkhole"},{"hill","Hill"},{"breaker","Breaker"},{"special","Special"},{"clearing","Clearing"},
        {"forgetmenot","Forget-me-not"},{"flat","Flat"},{"tile","Tile"},{"base","Edge"}};
    for(auto [key,noun]:nouns)if(f.find(key)!=std::string::npos)return noun;
    std::string out=folder;if(!out.empty())out[0]=char(std::toupper((unsigned char)out[0]));return out;
}

void SectionCatalog::build(const std::vector<SectionRef>& sections) {
    refs_=sections;names_.assign(sections.size(),{});pieceOf_.assign(sections.size(),-1);pieces_.clear();groups_.clear();
    // A trailing letter/"lit" is a variant only where siblings prove it: the same
    // stem with two different endings (accoast01a/b, 1ne1/1ne1lit). A constant
    // suffix (1netrans1a, 1netrans2a ...) stays part of the piece's name.
    std::vector<SectionName> plain(sections.size()),tagged(sections.size());
    std::map<std::string,std::set<std::string>> endings;
    for(size_t i=0;i<sections.size();++i) {
        const std::string folder=lower(sections[i].category)+"/";
        plain[i]=parseSectionName(sections[i].name,false);tagged[i]=parseSectionName(sections[i].name,true);
        endings[folder+plain[i].key].insert(plain[i].tag);
        if(tagged[i].tag!=plain[i].tag)endings[folder+tagged[i].key].insert(tagged[i].tag);
    }
    // A trailing letter most of a folder shares is the tileset's code (the "x" of
    // Low Jungle to High Jungle): hidden. A rarer one stays in the label.
    std::map<std::string,int> folderSize,suffixCount;
    for(size_t i=0;i<sections.size();++i) {
        const std::string folder=lower(sections[i].category)+"/";
        const bool useTag=tagged[i].tag!=plain[i].tag && endings[folder+tagged[i].key].size()>=2;
        names_[i]=useTag?tagged[i]:plain[i];
        ++folderSize[folder];if(!names_[i].suffix.empty())++suffixCount[folder+names_[i].suffix];
    }
    // An unnumbered name joins its numbered siblings (BRIDGENS + bridgens2).
    std::set<std::string> numbered;
    for(size_t i=0;i<sections.size();++i)if(!names_[i].variant.empty())numbered.insert(lower(sections[i].category)+"/"+names_[i].key);
    std::map<std::string,int> pieceIndex;
    for(size_t i=0;i<sections.size();++i) {
        const std::string folder=lower(sections[i].category)+"/";
        if(names_[i].variant.empty() && !names_[i].direction.empty() && numbered.count(folder+names_[i].key+"#"))names_[i].key+="#";
        if(!names_[i].suffix.empty() && suffixCount[folder+names_[i].suffix]*2>=folderSize[folder])names_[i].suffix.clear();
        auto [it,added]=pieceIndex.try_emplace(folder+names_[i].key,int(pieces_.size()));
        if(added) {
            Piece p;p.folder=sections[i].category;p.kind=names_[i].kind.empty()?sectionFolderNoun(p.folder):names_[i].kind;
            p.direction=names_[i].direction;p.corner=names_[i].corner;pieces_.push_back(std::move(p));
        }
        pieces_[size_t(it->second)].variants.push_back(int(i));
    }
    auto number=[](const std::string& s){return s.empty()?-1:std::stoi(s.substr(0,9));};
    for(auto& p:pieces_)std::stable_sort(p.variants.begin(),p.variants.end(),[&](int a,int b) {
        const auto& x=names_[size_t(a)];const auto& y=names_[size_t(b)];
        return number(x.variant)!=number(y.variant)?number(x.variant)<number(y.variant):x.tag<y.tag;});
    for(auto& p:pieces_) {
        p.sameNumber=true;
        for(int v:p.variants)p.sameNumber=p.sameNumber && names_[size_t(v)].variant==names_[size_t(p.variants[0])].variant;
    }
    // Display order: folder, kind, then compass order of the direction.
    static const char* compass[]={"N","NE","E","SE","S","SW","W","NW"};
    auto cornerRank=[](const std::string& c){return c.empty()?0:c=="outer"?1:c=="inner"?2:3;};
    auto dirRank=[](const std::string& d){for(int i=0;i<8;++i)if(d==compass[i])return i;return d.empty()?-1:8;};
    std::vector<int> order(pieces_.size());for(size_t i=0;i<order.size();++i)order[i]=int(i);
    std::stable_sort(order.begin(),order.end(),[&](int a,int b) {
        const auto& x=pieces_[size_t(a)];const auto& y=pieces_[size_t(b)];
        const auto fx=lower(x.folder),fy=lower(y.folder);if(fx!=fy)return fx<fy;
        if(x.kind!=y.kind)return x.kind<y.kind;
        if(x.corner!=y.corner)return cornerRank(x.corner)<cornerRank(y.corner);
        if(dirRank(x.direction)!=dirRank(y.direction))return dirRank(x.direction)<dirRank(y.direction);
        if(x.direction!=y.direction)return x.direction<y.direction;
        return lower(refs_[size_t(x.variants[0])].name)<lower(refs_[size_t(y.variants[0])].name);
    });
    {std::vector<Piece> sorted;for(int i:order)sorted.push_back(std::move(pieces_[size_t(i)]));pieces_=std::move(sorted);}
    for(size_t p=0;p<pieces_.size();++p)for(int s:pieces_[p].variants)pieceOf_[size_t(s)]=int(p);
    // Groups: All, then each kind group followed by its folders.
    Group all;all.label="All sections";all.sections=int(sections.size());
    for(size_t p=0;p<pieces_.size();++p)all.pieces.push_back(int(p));
    groups_.push_back(std::move(all));
    for(const char* kind:kGroupOrder) {
        Group g;g.id=std::string("kind:")+kind;g.label=kind;
        std::vector<Group> folders;
        for(size_t p=0;p<pieces_.size();++p) {
            if(sectionKindGroup(pieces_[p].folder)!=kind)continue;
            const std::string id="folder:"+lower(pieces_[p].folder);
            if(folders.empty() || folders.back().id!=id) {Group f;f.id=id;f.label=pieces_[p].folder;f.depth=1;folders.push_back(std::move(f));}
            const int count=int(pieces_[p].variants.size());
            folders.back().pieces.push_back(int(p));folders.back().sections+=count;g.pieces.push_back(int(p));g.sections+=count;
        }
        if(folders.empty())continue;
        groups_.push_back(std::move(g));
        for(auto& f:folders)groups_.push_back(std::move(f));
    }
}

const SectionCatalog::Group* SectionCatalog::group(const std::string& id) const {
    for(const auto& g:groups_)if(g.id==id)return &g;
    return nullptr;
}

int SectionCatalog::pieceOf(int section) const {return section>=0 && section<int(pieceOf_.size())?pieceOf_[size_t(section)]:-1;}

std::string SectionCatalog::variantText(int section) const {
    const auto& n=names_[size_t(section)];const int p=pieceOf(section);
    // 2ne1pata/b/c: the shared "1" says nothing, the letter is the variant.
    if(p>=0 && pieces_[size_t(p)].sameNumber && pieces_[size_t(p)].variants.size()>1 && !n.tag.empty())return n.tag;
    return n.variant.empty()?n.tag:n.tag.empty()?n.variant:n.variant+" "+n.tag;
}

std::string SectionCatalog::label(int section) const {
    const int p=pieceOf(section);if(p<0)return {};
    const auto& n=names_[size_t(section)];const auto& piece=pieces_[size_t(p)];
    std::string out=piece.kind;
    auto add=[&](const std::string& part){if(!part.empty())out+=" \xc2\xb7 "+part;};
    add(n.direction.empty()?n.corner:n.corner.empty()?n.direction:n.direction+" "+n.corner);
    add(n.number.empty()?"":"#"+n.number);
    add(n.details.empty()?n.suffix:n.suffix.empty()?n.details:n.details+" "+n.suffix);
    const auto v=variantText(section);
    if(piece.variants.size()>1)add("variant "+(v.empty()?std::string("base"):v));
    else if(!v.empty() && n.variant.empty())add(v);
    return out;
}

std::string SectionCatalog::shortLabel(int section) const {
    const int p=pieceOf(section);if(p<0)return {};
    const auto& n=names_[size_t(section)];const auto& piece=pieces_[size_t(p)];
    std::string kind=piece.kind.substr(0,piece.kind.find(' '));
    std::string where=n.direction;
    if(!n.corner.empty())where+=n.corner=="outer"?" out":n.corner=="inner"?" in":" t3";
    if(where.empty())where=n.number.empty()?n.details:"#"+n.number;
    if(where.empty())return kind;
    if(kind.size()+1+where.size()<=14)return kind+" "+where;
    return where.size()>9?where.substr(0,14):kind.substr(0,13-where.size())+" "+where;
}

std::string SectionCatalog::searchText(int section) const {
    const auto& r=refs_[size_t(section)];
    return lower(r.name+" "+label(section)+" "+r.category+" "+sectionKindGroup(r.category));
}

} // namespace cart
