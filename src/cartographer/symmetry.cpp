#include "cartographer/symmetry.h"
#include "cartographer/blockers.h"
#include "cartographer/regions.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace cart {
namespace {
std::string lower(std::string s) {for(char& c:s)c=char(std::tolower(static_cast<unsigned char>(c)));return s;}
bool named(const tak::tnt::Map& map,size_t i) {return i<map.features.size() && map.features[i]<0xFFFA && map.features[i]<map.featureNames.size();}
uint16_t intern(tak::tnt::Map& map,const std::string& name) {
    for(size_t i=0;i<map.featureNames.size();++i)if(map.featureNames[i]==name)return uint16_t(i);
    if(map.featureNames.size()>=0xFFFA)return 0xFFFF;
    map.featureNames.push_back(name);return uint16_t(map.featureNames.size()-1);
}
int round(double v) {return int(std::floor(v+.5));}
// Unit footprint in cells (continuous), centred on the unit.
bool overlaps(const PlacedUnit& a,const PlacedUnit& b) {
    return std::abs(a.x-b.x)*2<(a.footX+b.footX)*16-0.01f && std::abs(a.z-b.z)*2<(a.footZ+b.footZ)*16-0.01f;
}
bool samePlace(const PlacedUnit& a,float x,float z) {return std::abs(a.x-x)<.01f && std::abs(a.z-z)<.01f;}
bool sameUnit(const PlacedUnit& a,const PlacedUnit& b) {
    return a.type==b.type && a.player==b.player && a.x==b.x && a.z==b.z && a.angle==b.angle && a.name==b.name &&
           a.health==b.health && a.armor==b.armor && a.weapon==b.weapon && a.veteran==b.veteran;
}
// Direction letters and the lft/rgt/top/btm words as compass vectors.
std::pair<int,int> letterDirection(char c) {return c=='n'?std::pair{0,-1}:c=='s'?std::pair{0,1}:c=='e'?std::pair{1,0}:std::pair{-1,0};}
char directionLetter(std::pair<int,int> d) {return d.second<0?'n':d.second>0?'s':d.first>0?'e':'w';}
bool directionChar(char c) {return c=='n' || c=='s' || c=='e' || c=='w';}
bool letter(char c) {return std::isalpha(static_cast<unsigned char>(c))!=0;}
struct Word {const char* text;char direction;int family;};
const Word kWords[]={{"bottom",'s',1},{"right",'e',1},{"left",'w',1},{"top",'n',2},{"btm",'s',0},{"rgt",'e',0},{"lft",'w',0}};
const char* wordFor(char direction,int family) {
    if(direction=='n')return "top";
    for(const auto& word:kWords)if(word.direction==direction && (word.family==family || (family==2 && word.family==0)))return word.text;
    return "";
}
struct Token {size_t pos=0,len=0;bool word=false,clean=false;int family=0;bool tail=false;};
// Mode 0 joins underscore runs (e_n_s) only at word starts, so the "ns" of
// trans_e01 stays apart; mode 1 also joins mid-word runs (hwye_n01); mode 2
// leaves the first letter of a mid-word run to its word (cornne_se).
std::vector<Token> directionTokens(const std::string& s,int mode) {
    std::vector<Token> tokens;
    for(size_t i=0;i<s.size();) {
        bool matched=false;
        for(const auto& word:kWords)if(s.compare(i,std::char_traits<char>::length(word.text),word.text)==0) {
            Token token{i,std::char_traits<char>::length(word.text),true,false,word.family};tokens.push_back(token);i+=token.len;matched=true;break;
        }
        if(matched)continue;
        if(!directionChar(s[i])) {++i;continue;}
        size_t j=i;while(j<s.size() && directionChar(s[j]))++j;
        const bool midWord=i>0 && letter(s[i-1]);
        if(mode==2 && midWord && j-i>=2)++i;
        auto wordFinal=[&](size_t end){return end>=s.size() || !letter(s[end]);};
        if(!midWord || mode>0)
            while(wordFinal(j) && j+1<s.size() && s[j]=='_' && directionChar(s[j+1])) {
                size_t k=j+1;while(k<s.size() && directionChar(s[k]))++k;
                if(!wordFinal(k))break;
                j=k;
            }
        tokens.push_back({i,j-i,false,false,0});i=j;
    }
    for(auto& token:tokens)token.clean=(token.pos==0 || !letter(s[token.pos-1])) && (token.pos+token.len>=s.size() || !letter(s[token.pos+token.len]));
    return tokens;
}
std::string digitless(std::string s) {
    std::string out;
    for(size_t i=0;i<s.size();++i)if(!std::isdigit(static_cast<unsigned char>(s[i])))out+=s[i];else if(out.empty() || out.back()!='#')out+='#';
    return out;
}
std::vector<std::string> tokenVariants(const std::string& text,const Token& token,Xf xf) {
    std::vector<std::string> out;
    auto add=[&](const std::string& value){if(std::find(out.begin(),out.end(),value)==out.end())out.push_back(value);};
    auto mapLetter=[&](char c){const auto [dx,dz]=letterDirection(c);return directionLetter(transformDirection(xf,dx,dz));};
    if(token.word) {
        for(const auto& word:kWords)if(text==word.text) {add(wordFor(mapLetter(word.direction),token.family));break;}
        return out;
    }
    std::string mapped=text;for(char& c:mapped)if(c!='_')c=mapLetter(c);
    add(mapped);
    const bool separated=text.find('_')!=std::string::npos;
    // The last letter may not be a direction: the exterior "e" of nee01/nwe01
    // or the "s" of stair in 1enstair. Keeping it literally is the last resort,
    // except in a run of three or more whose last letter no other name of the
    // category turns (`Token::tail`): there it comes before reordering, since
    // a permutation turns nwe01 back into itself and 1nestair (stair on the
    // north side) into 1enstair (east side).
    const bool tail=!separated && text.size()>=2;
    if(tail && token.tail && mapped.substr(0,mapped.size()-1)+text.back()!=text)add(mapped.substr(0,mapped.size()-1)+text.back());
    if(separated) {
        std::vector<std::string> groups;size_t start=0;
        for(size_t i=0;i<=mapped.size();++i)if(i==mapped.size() || mapped[i]=='_') {groups.push_back(mapped.substr(start,i-start));start=i+1;}
        if(groups.size()<=6) {
            std::sort(groups.begin(),groups.end());
            do {std::string joined;for(const auto& group:groups)joined+=(joined.empty()?"":"_")+group;add(joined);}
            while(std::next_permutation(groups.begin(),groups.end()));
        }
    } else if(mapped.size()<=6) {
        auto letters=mapped;std::sort(letters.begin(),letters.end());
        do add(letters);while(std::next_permutation(letters.begin(),letters.end()));
        if(tail) {
            add(mapped.substr(0,mapped.size()-1)+text.back());
            letters=mapped.substr(0,mapped.size()-1);std::sort(letters.begin(),letters.end());
            do add(letters+text.back());while(std::next_permutation(letters.begin(),letters.end()));
        }
    }
    return out;
}
constexpr Xf kAll[]={Xf::FlipX,Xf::FlipZ,Xf::Rot180,Xf::Transpose,Xf::AntiTranspose,Xf::Rot90,Xf::Rot270};
} // namespace

const char* mirrorLabel(Mirror kind) {
    static const char* labels[]={"VERTICAL AXIS (LEFT | RIGHT)","HORIZONTAL AXIS (TOP / BOTTOM)","DIAGONAL \\ (SQUARE MAPS)",
        "DIAGONAL / (SQUARE MAPS)","ROTATE 2-WAY (180 DEGREES)","ROTATE 4-WAY (90 DEGREES, SQUARE)","MIRROR 4-WAY (BOTH AXES)"};
    return labels[int(kind)];
}
bool mirrorNeedsSquare(Mirror kind) {return kind==Mirror::Diagonal || kind==Mirror::AntiDiagonal || kind==Mirror::Rotate4;}
std::vector<Xf> mirrorImages(Mirror kind) {
    switch(kind) {
    case Mirror::Vertical:return {Xf::FlipX};
    case Mirror::Horizontal:return {Xf::FlipZ};
    case Mirror::Diagonal:return {Xf::Transpose};
    case Mirror::AntiDiagonal:return {Xf::AntiTranspose};
    case Mirror::Rotate2:return {Xf::Rot180};
    case Mirror::Rotate4:return {Xf::Rot90,Xf::Rot180,Xf::Rot270};
    case Mirror::Quad:return {Xf::FlipX,Xf::FlipZ,Xf::Rot180};
    }
    return {};
}
std::vector<std::string> mirrorSources(Mirror kind) {
    switch(kind) {
    case Mirror::Vertical:return {"LEFT HALF","RIGHT HALF"};
    case Mirror::Horizontal:return {"TOP HALF","BOTTOM HALF"};
    case Mirror::Diagonal:return {"UPPER-RIGHT TRIANGLE","LOWER-LEFT TRIANGLE"};
    case Mirror::AntiDiagonal:return {"UPPER-LEFT TRIANGLE","LOWER-RIGHT TRIANGLE"};
    case Mirror::Rotate2:return {"TOP HALF","BOTTOM HALF","LEFT HALF","RIGHT HALF"};
    case Mirror::Rotate4:case Mirror::Quad:return {"TOP-LEFT QUADRANT","TOP-RIGHT QUADRANT","BOTTOM-RIGHT QUADRANT","BOTTOM-LEFT QUADRANT"};
    }
    return {};
}
bool swapsAxes(Xf xf) {return xf==Xf::Transpose || xf==Xf::AntiTranspose || xf==Xf::Rot90 || xf==Xf::Rot270;}
Xf inverse(Xf xf) {return xf==Xf::Rot90?Xf::Rot270:xf==Xf::Rot270?Xf::Rot90:xf;}

PointF transform(Xf xf,PointF p,double width,double height) {
    switch(xf) {
    case Xf::Identity:return p;
    case Xf::FlipX:return {width-p.x,p.z};
    case Xf::FlipZ:return {p.x,height-p.z};
    case Xf::Rot180:return {width-p.x,height-p.z};
    case Xf::Transpose:return {p.z,p.x};
    case Xf::AntiTranspose:return {height-p.z,width-p.x};
    case Xf::Rot90:return {height-p.z,p.x};
    case Xf::Rot270:return {p.z,width-p.x};
    }
    return p;
}
Box transform(Xf xf,Box b,int width,int height) {
    const auto a=transform(xf,PointF{double(b.x0),double(b.z0)},width,height),c=transform(xf,PointF{double(b.x1),double(b.z1)},width,height);
    return {int(std::min(a.x,c.x)),int(std::min(a.z,c.z)),int(std::max(a.x,c.x)),int(std::max(a.z,c.z))};
}
std::pair<int,int> transformDirection(Xf xf,int dx,int dz) {
    switch(xf) {
    case Xf::Identity:return {dx,dz};
    case Xf::FlipX:return {-dx,dz};
    case Xf::FlipZ:return {dx,-dz};
    case Xf::Rot180:return {-dx,-dz};
    case Xf::Transpose:return {dz,dx};
    case Xf::AntiTranspose:return {-dz,-dx};
    case Xf::Rot90:return {-dz,dx};
    case Xf::Rot270:return {dz,-dx};
    }
    return {dx,dz};
}
bool inSource(Mirror kind,int source,PointF p,double width,double height) {
    const double cx=width/2,cz=height/2,x=p.x,z=p.z;
    const bool centre=x==cx && z==cz;
    switch(kind) {
    case Mirror::Vertical:return source==0?x<=cx:x>=cx;
    case Mirror::Horizontal:return source==0?z<=cz:z>=cz;
    case Mirror::Diagonal:return source==0?x>=z:x<=z;
    case Mirror::AntiDiagonal:return source==0?x+z<=width:x+z>=width;
    case Mirror::Rotate2:
        if(source==0)return z<cz || (z==cz && x<=cx);
        if(source==1)return z>cz || (z==cz && x>=cx);
        if(source==2)return x<cx || (x==cx && z>=cz);
        return x>cx || (x==cx && z<=cz);
    case Mirror::Rotate4:
        // TL -> TR -> BR -> BL under the clockwise quarter turn.
        if(source==0)return (x<cx && z<=cz) || centre;
        if(source==1)return (x>=cx && z<cz) || centre;
        if(source==2)return (x>cx && z>=cz) || centre;
        return (x<=cx && z>cz) || centre;
    case Mirror::Quad:
        if(source==0)return x<=cx && z<=cz;
        if(source==1)return x>=cx && z<=cz;
        if(source==2)return x>=cx && z>=cz;
        return x<=cx && z>=cz;
    }
    return false;
}
int mirrorAngle(Xf xf,int degrees) {
    // Facing (sin a, cos a): flips negate a component, transposes swap them.
    int a=degrees;
    switch(xf) {
    case Xf::Identity:break;
    case Xf::FlipX:a=-degrees;break;
    case Xf::FlipZ:a=180-degrees;break;
    case Xf::Rot180:a=degrees+180;break;
    case Xf::Transpose:a=90-degrees;break;
    case Xf::AntiTranspose:a=270-degrees;break;
    case Xf::Rot90:a=degrees-90;break;
    case Xf::Rot270:a=degrees+90;break;
    }
    a%=360;return a<0?a+360:a;
}

std::optional<PlacedUnit> mirrorUnit(const PlacedUnit& unit,Xf xf,int width,int height) {
    const auto centre=transform(xf,PointF{unit.x/16.0,unit.z/16.0},width,height);
    const int outW=swapsAxes(xf)?height:width,outH=swapsAxes(xf)?width:height;
    // The unit keeps its footprint orientation; snap its origin to the cells
    // centred on the mirrored centre (exact for flips and half turns).
    const int ox=round(centre.x-unit.footX/2.0),oz=round(centre.z-unit.footZ/2.0);
    PlacedUnit out=unit;out.x=float(ox*16+unit.footX*8);out.z=float(oz*16+unit.footZ*8);
    if(out.x<0 || out.z<0 || out.x>=outW*16.f || out.z>=outH*16.f)return std::nullopt;
    out.angle=float(mirrorAngle(xf,int(std::lround(unit.angle))));
    return out;
}
std::optional<std::pair<int,int>> mirrorFeature(int x,int z,int fx,int fz,Xf xf,int width,int height) {
    fx=std::max(1,fx);fz=std::max(1,fz);
    const auto box=transform(xf,Box{x,z,x+fx,z+fz},width,height);
    int ox=box.x0,oz=box.z0;
    if(box.x1-box.x0!=fx || box.z1-box.z0!=fz) {   // non-square under a transposing turn
        ox=round((box.x0+box.x1)/2.0-fx/2.0);oz=round((box.z0+box.z1)/2.0-fz/2.0);
    }
    const int outW=swapsAxes(xf)?height:width,outH=swapsAxes(xf)?width:height;
    if(ox<0 || oz<0 || ox>=outW || oz>=outH)return std::nullopt;
    return std::pair{ox,oz};
}
std::optional<tak::tnt::StartPos> mirrorStart(const tak::tnt::StartPos& start,Xf xf,int width,int height) {
    const auto p=transform(xf,PointF{double(start.xpos),double(start.zpos)},width,height);
    const int outW=swapsAxes(xf)?height:width,outH=swapsAxes(xf)?width:height;
    tak::tnt::StartPos out=start;out.xpos=int(p.x);out.zpos=int(p.z);
    if(out.xpos<0 || out.zpos<0 || out.xpos>=outW || out.zpos>=outH)return std::nullopt;
    return out;
}
std::optional<tak::crt::Region> mirrorRegion(const tak::crt::Region& region,Xf xf,int width,int height) {
    const auto box=transform(xf,Box{std::min(region.x1,region.x2),std::min(region.z1,region.z2),std::max(region.x1,region.x2)+1,std::max(region.z1,region.z2)+1},width,height);
    const int outW=swapsAxes(xf)?height:width,outH=swapsAxes(xf)?width:height;
    if(box.x0<0 || box.z0<0 || box.x1>outW || box.z1>outH)return std::nullopt;
    auto out=region;out.x1=box.x0;out.z1=box.z0;out.x2=box.x1-1;out.z2=box.z1-1;
    return out;
}
int nextStartNumber(const std::vector<tak::tnt::StartPos>& starts) {
    for(int n=1;n<=8;++n)if(std::none_of(starts.begin(),starts.end(),[&](const auto& s){return s.number==n;}))return n;
    return 0;
}
std::array<int,9> mirrorPlayers(const std::vector<tak::tnt::StartPos>& starts,Xf xf,int width,int height) {
    std::array<int,9> players{};for(int i=0;i<9;++i)players[size_t(i)]=i;
    for(const auto& start:starts)if(start.number>=1 && start.number<=8)
        if(const auto image=mirrorStart(start,xf,width,height))
            for(const auto& other:starts)if(other.number>=1 && other.number<=8 && other.xpos==image->xpos && other.zpos==image->zpos)
                players[size_t(start.number-1)]=other.number-1;
    return players;
}
std::string uniqueRegionName(const std::vector<tak::crt::Region>& regions,const std::string& base) {
    auto taken=[&](const std::string& name){return std::any_of(regions.begin(),regions.end(),[&](const auto& r){return lower(r.name)==lower(name);});};
    const auto stem=base.substr(0,56);
    if(!taken(stem))return stem;
    for(int n=2;;++n)if(const auto name=stem+" "+std::to_string(n);!taken(name))return name;
}

std::optional<std::string> sectionTwinName(const std::string& name,const std::string& category,
                                           const std::vector<std::string>& categoryNames,Xf xf) {
    const auto source=lower(name);
    auto exists=[&](const std::string& candidate){return std::find(categoryNames.begin(),categoryNames.end(),candidate)!=categoryNames.end();};
    std::set<std::string> families;for(const auto& other:categoryNames)families.insert(digitless(other));
    // A mid-word run is anchored when another name of the category puts a
    // different direction letter at its start: hwyn_w01 anchors the "e" of
    // hwye_n01, while no "tra?" name anchors the "s" of trans_e01.
    auto anchored=[&](size_t pos) {
        return std::any_of(categoryNames.begin(),categoryNames.end(),[&](const std::string& other) {
            return other.size()>pos && other.compare(0,pos,source,0,pos)==0 && directionChar(other[pos]) && other[pos]!=source[pos];
        });
    };
    std::optional<std::string> self,first,best;std::vector<Token> previous;
    for(int mode=0;mode<3;++mode) {
        auto tokens=directionTokens(source,mode);
        // Only runs of three or more: a pair is ambiguous (n|stair, we|clearing).
        for(auto& token:tokens)token.tail=!token.word && token.len>=3 && !anchored(token.pos+token.len-1);
        if(mode && tokens.size()==previous.size() && std::equal(tokens.begin(),tokens.end(),previous.begin(),[](const Token& a,const Token& b){return a.pos==b.pos && a.len==b.len;}))continue;
        previous=tokens;
        auto text=[&](const Token& t){return source.substr(t.pos,t.len);};
        auto replace=[&](const std::vector<std::string>& values) {
            std::string out;size_t at=0;
            for(size_t i=0;i<tokens.size();++i) {out+=source.substr(at,tokens[i].pos-at);out+=values[i];at=tokens[i].pos+tokens[i].len;}
            return out+source.substr(at);
        };
        // A token is a direction when it stands alone (n01, hwy e_n01) or when
        // a transform turns it into another name family of the category: the
        // "s" of sclif4 is real because nclif1 exists.
        std::vector<bool> real(tokens.size(),false);
        for(size_t i=0;i<tokens.size();++i) {
            real[i]=tokens[i].clean;
            for(Xf other:kAll) {
                if(real[i])break;
                std::vector<std::string> keep,mapped;
                for(const auto& t:tokens) {keep.push_back(text(t));mapped.push_back(tokenVariants(text(t),t,other).front());}
                for(auto* values:{&keep,&mapped})for(const auto& variant:tokenVariants(text(tokens[i]),tokens[i],other))if(variant!=text(tokens[i])) {
                    auto candidate=*values;candidate[i]=variant;const auto joined=replace(candidate);
                    if(joined!=source && families.count(digitless(joined))) {real[i]=true;break;}
                }
            }
        }
        std::optional<std::string> found;
        if(std::none_of(real.begin(),real.end(),[](bool r){return r;})) {
            if(lower(category).find("flat")!=std::string::npos)found=source;   // direction-free ground
        } else {
            std::vector<std::vector<std::string>> options;
            for(size_t i=0;i<tokens.size();++i)options.push_back(real[i]?tokenVariants(text(tokens[i]),tokens[i],xf):std::vector<std::string>{text(tokens[i])});
            std::vector<size_t> pick(tokens.size(),0);
            for(;;) {
                std::vector<std::string> values;for(size_t i=0;i<tokens.size();++i)values.push_back(options[i][pick[i]]);
                if(const auto candidate=replace(values);exists(candidate)) {found=candidate;break;}
                size_t i=0;
                while(i<pick.size() && ++pick[i]==options[i].size()) {pick[i]=0;++i;}
                if(i==pick.size())break;
            }
        }
        // An unchanged name can mean the tokens were misread (hwye_n01 under
        // a left-right mirror), so a different twin from a later reading wins.
        // A later reading also overrides a different twin when every mid-word
        // run it turns is anchored and the letter it leaves to the word is not:
        // mode 0 leaves the "e" of hwye_n01 and the "nw" of cornnw_ne unturned
        // under a half turn, while patchne's "n" belongs to the run (patchse).
        if(found && *found!=source) {
            bool sure=false;
            for(size_t i=0;i<tokens.size() && mode>0;++i)if(const size_t pos=tokens[i].pos;real[i] && pos>0 && letter(source[pos-1])) {
                sure=anchored(pos) && !(directionChar(source[pos-1]) && anchored(pos-1));   // corn|ne, not patc|hne
                if(!sure)break;
            }
            if(!first)first=found;
            else if(sure && !best && *found!=*first)best=found;
        }
        if(found && *found==source && !self)self=found;
    }
    return best?best:first?first:self;
}

void SectionMatcher::build(const std::vector<SectionRef>& refs,const Loader& load) {
    refs_=refs;sections_.assign(refs.size(),nullptr);tiles_.clear();categories_.clear();twins_.clear();
    for(size_t s=0;s<refs.size();++s) {
        categories_[lower(refs[s].category)].push_back(lower(refs[s].name));
        const auto* map=load(s);sections_[s]=map;
        if(!map || map->blocksX<=0 || map->blocksY<=0 || map->tileKeys.size()<size_t(map->blocksX)*map->blocksY)continue;
        for(int z=0;z<map->blocksY;++z)for(int x=0;x<map->blocksX;++x) {
            const size_t i=size_t(z)*map->blocksX+x;
            tiles_[{map->tileKeys[i],map->tileCols[i],map->tileRows[i]}].push_back({int(s),x,z});
        }
    }
    built_=true;
}
std::vector<SectionMatcher::Source> SectionMatcher::attribute(const tak::tnt::Map& map,Box b) const {
    b={std::max(0,b.x0),std::max(0,b.z0),std::min(map.blocksX,b.x1),std::min(map.blocksY,b.z1)};
    std::vector<Source> out(size_t(std::max(0,b.x1-b.x0))*std::max(0,b.z1-b.z0));
    std::map<std::tuple<int,int,int>,int> scores;
    auto score=[&](int s,int ox,int oz) {
        const auto key=std::tuple{s,ox,oz};
        if(const auto found=scores.find(key);found!=scores.end())return found->second;
        const auto* sec=sections_[size_t(s)];int matches=0;
        for(int z=0;z<sec->blocksY;++z)for(int x=0;x<sec->blocksX;++x) {
            const int mx=ox+x,mz=oz+z;if(mx<0 || mz<0 || mx>=map.blocksX || mz>=map.blocksY)continue;
            const size_t i=size_t(z)*sec->blocksX+x,j=size_t(mz)*map.blocksX+mx;
            matches+=sec->tileKeys[i]==map.tileKeys[j] && sec->tileCols[i]==map.tileCols[j] && sec->tileRows[i]==map.tileRows[j];
        }
        return scores[key]=matches;
    };
    for(int z=b.z0;z<b.z1;++z)for(int x=b.x0;x<b.x1;++x) {
        const size_t j=size_t(z)*map.blocksX+x;
        const auto found=tiles_.find({map.tileKeys[j],map.tileCols[j],map.tileRows[j]});if(found==tiles_.end())continue;
        // The placement explaining the most neighbouring blocks wins.
        int best=-1;auto& source=out[size_t(z-b.z0)*(b.x1-b.x0)+(x-b.x0)];
        for(const auto& tile:found->second)if(const int value=score(tile.section,x-tile.x,z-tile.z);value>best) {best=value;source={tile.section,x-tile.x,z-tile.z};}
    }
    return out;
}
int SectionMatcher::twin(int section,Xf xf) const {
    if(section<0 || size_t(section)>=refs_.size())return -1;
    if(xf==Xf::Identity)return section;
    const auto key=std::pair{section,xf};
    if(const auto found=twins_.find(key);found!=twins_.end())return found->second;
    int result=-1;
    const auto& ref=refs_[size_t(section)];const auto category=lower(ref.category);
    if(const auto name=sectionTwinName(ref.name,ref.category,categories_.at(category),xf)) {
        const auto* own=sections_[size_t(section)];
        for(size_t i=0;i<refs_.size() && own;++i)if(lower(refs_[i].category)==category && lower(refs_[i].name)==*name && sections_[i]) {
            const int w=swapsAxes(xf)?own->blocksY:own->blocksX,h=swapsAxes(xf)?own->blocksX:own->blocksY;
            if(sections_[i]->blocksX!=w || sections_[i]->blocksY!=h)continue;
            // Case-only duplicates (E01 / e01): prefer the source's spelling style.
            const bool sameCase=std::isupper(static_cast<unsigned char>(refs_[i].name[0]))==std::isupper(static_cast<unsigned char>(ref.name[0]));
            if(result<0 || sameCase)result=int(i);
            if(sameCase)break;
        }
    }
    return twins_[key]=result;
}

TerrainMirror mirrorTerrain(const SectionMatcher& matcher,const tak::tnt::Map& source,Box b,
                            const std::vector<SectionMatcher::Source>& attribution,Xf xf,tak::tnt::Map& out,
                            const std::function<bool(int,int)>& include) {
    TerrainMirror result;
    const int width=source.blocksX,height=source.blocksY,boxW=b.x1-b.x0;
    for(int z=b.z0;z<b.z1;++z)for(int x=b.x0;x<b.x1;++x) {
        if(include && !include(x,z))continue;
        const auto image=transform(xf,Box{x,z,x+1,z+1},width,height);
        if(image.x0<0 || image.z0<0 || image.x0>=out.blocksX || image.z0>=out.blocksY)continue;
        const auto& from=attribution[size_t(z-b.z0)*boxW+(x-b.x0)];
        const int twin=matcher.twin(from.section,xf);
        const auto* own=matcher.section(from.section);const auto* sec=matcher.section(twin);
        if(twin<0 || !own || !sec) {result.failed.push_back({image.x0,image.z0});result.failedSection.push_back(from.section);continue;}
        // The twin occupies the mirror of the whole source placement.
        const auto placed=transform(xf,Box{from.x,from.z,from.x+own->blocksX,from.z+own->blocksY},width,height);
        const int lx=image.x0-placed.x0,lz=image.z0-placed.z0;
        if(lx<0 || lz<0 || lx>=sec->blocksX || lz>=sec->blocksY) {result.failed.push_back({image.x0,image.z0});result.failedSection.push_back(from.section);continue;}
        const size_t si=size_t(lz)*sec->blocksX+lx,mi=size_t(image.z0)*out.blocksX+image.x0;
        out.tileKeys[mi]=sec->tileKeys[si];out.tileCols[mi]=sec->tileCols[si];out.tileRows[mi]=sec->tileRows[si];
        for(int dz=0;dz<2;++dz)for(int dx=0;dx<2;++dx) {
            const size_t sc=size_t(lz*2+dz)*sec->width+lx*2+dx,mc=size_t(image.z0*2+dz)*out.width+image.x0*2+dx;
            if(mc>=out.heights.size())continue;
            if(sc<sec->heights.size())out.heights[mc]=sec->heights[sc];
            if(sc<sec->features.size() && !named(out,mc)) {const uint16_t v=sec->features[sc];out.features[mc]=v>=0xFFFA?v:0xFFFF;}
        }
        ++result.mirrored;
    }
    return result;
}

std::string SymmetryReport::summary() const {
    auto count=[](int n,const char* what){return std::to_string(n)+" "+what+(n==1?"":"s");};
    std::string text="Copies: "+count(units,"unit")+", "+count(features,"feature")+", "+count(starts,"start")+", "+count(regions,"region")+", "+count(blocks,"terrain block")+", "+count(blocked,"blocked cell")+".";
    text+=" Replaced: "+count(removedUnits,"unit")+", "+count(removedFeatures,"feature")+", "+count(removedStarts,"start")+", "+count(removedRegions,"region")+".";
    if(keptRegions)text+=" Kept "+count(keptRegions,"region")+" used by rules.";
    if(const int skipped=skippedUnits+skippedFeatures+skippedStarts+skippedRegions)
        text+=" Skipped "+std::to_string(skipped)+" (off the map, overlapping, or no free start number).";
    if(unmirroredBlocks)text+=" "+count(unmirroredBlocks,"terrain block")+" left unchanged: no known section twin.";
    return text;
}

SymmetryReport symmetrize(tak::tnt::Map& map,std::vector<tak::tnt::StartPos>& starts,tak::crt::Scenario& scenario,
                          std::vector<PlacedUnit>& units,const SymmetryRequest& request,const FootprintLookup& footprint,
                          const SectionMatcher* matcher) {
    SymmetryReport report;
    const int width=map.width,height=map.height;
    const bool selection=request.source<0;
    const auto images=mirrorImages(request.kind);
    auto source=[&](PointF p){return inSource(request.kind,request.source,p,width,height);};

    // Starts first: unit owners follow the start each owner's start mirrors to.
    if(!selection) {
        std::vector<tak::tnt::StartPos> kept;
        for(const auto& start:starts)if(source({double(start.xpos),double(start.zpos)}))kept.push_back(start);else ++report.removedStarts;
        auto ordered=kept;std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.number<b.number;});
        for(const auto& start:ordered) {
            std::vector<std::pair<int,int>> seen{{start.xpos,start.zpos}};
            for(Xf xf:images) {
                const auto image=mirrorStart(start,xf,width,height);
                if(!image) {++report.skippedStarts;continue;}
                if(std::find(seen.begin(),seen.end(),std::pair{image->xpos,image->zpos})!=seen.end())continue;
                seen.push_back({image->xpos,image->zpos});
                const bool taken=std::any_of(kept.begin(),kept.end(),[&](const auto& s){return s.xpos==image->xpos && s.zpos==image->zpos;});
                const int number=nextStartNumber(kept);
                if(taken || !number) {++report.skippedStarts;continue;}
                kept.push_back({number,image->xpos,image->zpos});++report.starts;
            }
        }
        starts=std::move(kept);
    }

    if(!selection) {
        std::vector<tak::crt::Region> sources;
        for(int i=int(scenario.regions.size())-1;i>=0;--i) {
            const auto& r=scenario.regions[size_t(i)];
            const PointF centre{(std::min(r.x1,r.x2)+std::max(r.x1,r.x2)+1)/2.0,(std::min(r.z1,r.z2)+std::max(r.z1,r.z2)+1)/2.0};
            if(source(centre)) {sources.insert(sources.begin(),r);continue;}
            std::string error;
            if(removeRegion(scenario,i,error))++report.removedRegions;else ++report.keptRegions;
        }
        for(const auto& region:sources) {
            std::vector<tak::crt::Region> seen{region};
            for(Xf xf:images) {
                auto image=mirrorRegion(region,xf,width,height);
                if(!image) {++report.skippedRegions;continue;}
                if(std::any_of(seen.begin(),seen.end(),[&](const auto& r){return r.x1==image->x1 && r.z1==image->z1 && r.x2==image->x2 && r.z2==image->z2;}))continue;
                seen.push_back(*image);
                image->name=uniqueRegionName(scenario.regions,region.name+" mirror");std::string error;
                if(setRegion(scenario,-1,*image,width,height,error))++report.regions;else ++report.skippedRegions;
            }
        }
    }

    // Units: copies follow their owner's mirrored start and drop unique names.
    {
        std::vector<PlacedUnit> sources,kept;std::vector<bool> selected(units.size(),false);
        for(int i:request.units)if(i>=0 && size_t(i)<units.size())selected[size_t(i)]=true;
        for(size_t i=0;i<units.size();++i) {
            const bool from=selection?bool(selected[i]):source({units[i].x/16.0,units[i].z/16.0});
            if(from)sources.push_back(units[i]);
            if(from || selection)kept.push_back(units[i]);else ++report.removedUnits;
        }
        std::vector<std::array<int,9>> owners;for(Xf xf:images)owners.push_back(mirrorPlayers(starts,xf,width,height));
        std::vector<PlacedUnit> copies;
        for(const auto& unit:sources) {
            std::vector<std::pair<float,float>> seen{{unit.x,unit.z}};
            for(size_t k=0;k<images.size();++k) {
                auto image=mirrorUnit(unit,images[k],width,height);
                if(!image) {++report.skippedUnits;continue;}
                if(std::find(seen.begin(),seen.end(),std::pair{image->x,image->z})!=seen.end())continue;
                seen.push_back({image->x,image->z});
                if(unit.player>=0 && unit.player<=8)image->player=owners[k][size_t(unit.player)];
                image->name.clear();copies.push_back(*image);
            }
        }
        if(selection) {   // a selection's images replace the unselected units under them
            std::vector<PlacedUnit> remaining;
            for(size_t i=0;i<units.size();++i) {
                const bool replaced=!selected[i] && std::any_of(copies.begin(),copies.end(),[&](const auto& c){return overlaps(c,units[i]);});
                if(replaced)++report.removedUnits;else remaining.push_back(units[i]);
            }
            kept=std::move(remaining);
        }
        for(const auto& copy:copies) {
            if(std::any_of(kept.begin(),kept.end(),[&](const auto& u){return overlaps(u,copy);})) {++report.skippedUnits;continue;}
            kept.push_back(copy);++report.units;
        }
        units=std::move(kept);
    }

    // Features: clear the target side, mirror terrain (markers included), then
    // place the copies so anchors are not overwritten by the twin's markers.
    struct Anchor {int x,z,fx,fz;std::string name;};
    std::vector<Anchor> sources;
    for(size_t i=0;i<map.features.size();++i)if(named(map,i)) {
        const int x=int(i%size_t(width)),z=int(i/size_t(width));const auto& name=map.featureNames[map.features[i]];
        const auto [fx,fz]=footprint?footprint(name):std::pair{1,1};
        const bool from=selection?request.featureCells.count(int(i))!=0:source({x+fx/2.0,z+fz/2.0});
        if(from)sources.push_back({x,z,std::max(1,fx),std::max(1,fz),name});
        else if(!selection) {map.features[i]=0xFFFF;++report.removedFeatures;}
    }
    if(request.terrain && matcher && matcher->built() && (!selection || request.blocks)) {
        const auto snapshot=map;
        Box box=selection?*request.blocks:Box{0,0,map.blocksX,map.blocksY};
        box={std::max(0,box.x0),std::max(0,box.z0),std::min(map.blocksX,box.x1),std::min(map.blocksY,box.z1)};
        const auto attribution=matcher->attribute(snapshot,box);
        std::map<std::pair<int,int>,std::pair<int,MapIssue>> areas;
        std::set<std::pair<int,int>> failedBlocks;
        for(Xf xf:images) {
            // Blocks the axis maps onto themselves stay as they are.
            // A selected rectangle straddling the axis keeps its own blocks.
            auto include=[&](int x,int z) {
                const auto image=transform(xf,Box{x,z,x+1,z+1},map.blocksX,map.blocksY);
                if(image.x0==x && image.z0==z)return false;
                if(selection)return image.x0<box.x0 || image.x0>=box.x1 || image.z0<box.z0 || image.z0>=box.z1;
                return inSource(request.kind,request.source,{x+.5,z+.5},map.blocksX,map.blocksY);
            };
            const auto result=mirrorTerrain(*matcher,snapshot,box,attribution,xf,map,include);
            report.blocks+=result.mirrored;
            for(size_t i=0;i<result.failed.size();++i) {
                const auto [bx,bz]=result.failed[i];if(!failedBlocks.insert({bx,bz}).second)continue;
                auto& area=areas[{bx/16,bz/16}];
                if(!area.first) {
                    const int s=result.failedSection[i];
                    area.second.message=s<0?"Terrain not mirrored: tiles here match no section of this world":"Terrain not mirrored: section '"+matcher->name(s)+"' has no mirror twin";
                    area.second.x=bx*32.f+16;area.second.z=bz*32.f+16;
                }
                ++area.first;
            }
        }
        report.unmirroredBlocks=int(failedBlocks.size());
        for(auto& [cell,area]:areas) {
            area.second.message+=" ("+std::to_string(area.first)+" block"+(area.first==1?"":"s")+" near this point)";
            report.terrainIssues.push_back(std::move(area.second));
        }
    }
    for(const auto& anchor:sources) {
        std::vector<std::pair<int,int>> seen{{anchor.x,anchor.z}};
        for(Xf xf:images) {
            const auto image=mirrorFeature(anchor.x,anchor.z,anchor.fx,anchor.fz,xf,width,height);
            if(!image) {++report.skippedFeatures;continue;}
            if(std::find(seen.begin(),seen.end(),*image)!=seen.end())continue;
            seen.push_back(*image);
            const size_t cell=size_t(image->second)*width+image->first;
            if(named(map,cell)) {
                if(!selection || request.featureCells.count(int(cell))) {++report.skippedFeatures;continue;}
                ++report.removedFeatures;   // a selection's image replaces an unselected feature
            }
            const auto value=intern(map,anchor.name);
            if(value==0xFFFF) {++report.skippedFeatures;continue;}
            map.features[cell]=value;++report.features;
        }
    }
    // Blocked cells on the source side block their images too, on empty cells and
    // whichever TERRAIN choice: twin sections bring only their own markers, so
    // cells painted with the BLOCKED tool would stay on one side. A selection's
    // source is its terrain rectangle. No blocker on the other side is removed.
    if(!selection || request.blocks) {
        const Box cells=selection?Box{request.blocks->x0*2,request.blocks->z0*2,request.blocks->x1*2,request.blocks->z1*2}:Box{0,0,width,height};
        auto inCells=[&](int x,int z) {return x>=cells.x0 && x<cells.x1 && z>=cells.z0 && z<cells.z1;};
        std::vector<std::pair<int,int>> blocked;
        for(int z=0;z<height;++z)for(int x=0;x<width;++x)
            if(map.features[size_t(z)*width+x]==kCellBlocked && (selection?inCells(x,z):source({x+.5,z+.5})))blocked.push_back({x,z});
        for(const auto& [x,z]:blocked)for(Xf xf:images) {
            const auto image=mirrorFeature(x,z,1,1,xf,width,height);
            if(!image || *image==std::pair{x,z} || (selection && inCells(image->first,image->second)))continue;
            uint16_t& v=map.features[size_t(image->second)*width+image->first];
            if(v==kCellEmpty) {v=kCellBlocked;++report.blocked;}
        }
    }
    return report;
}

LiveMirrorResult mirrorUnitEdits(const std::vector<PlacedUnit>& before,std::vector<PlacedUnit>& units,
                                 Mirror kind,const std::vector<tak::tnt::StartPos>& starts,int width,int height) {
    LiveMirrorResult result;const auto images=mirrorImages(kind);
    auto addCopy=[&](const PlacedUnit& unit,Xf xf,size_t self) {
        auto image=mirrorUnit(unit,xf,width,height);
        if(!image) {++result.skipped;return;}
        if(samePlace(unit,image->x,image->z))return;
        if(unit.player>=0 && unit.player<=8)image->player=mirrorPlayers(starts,xf,width,height)[size_t(unit.player)];
        image->name.clear();
        for(size_t j=0;j<units.size();++j)if(j!=self && overlaps(units[j],*image)) {++result.skipped;return;}
        units.push_back(*image);++result.copies;
    };
    auto partner=[&](const PlacedUnit& unit,const PlacedUnit& image,size_t self)->int {
        for(size_t j=0;j<units.size();++j)if(j!=self && units[j].type==unit.type && samePlace(units[j],image.x,image.z))return int(j);
        return -1;
    };
    if(units.size()==before.size()) {
        std::set<size_t> changed;
        for(size_t i=0;i<units.size();++i)if(!sameUnit(units[i],before[i]))changed.insert(i);
        // Partners are found where they were: a selection holding both a unit
        // and its mirror moves both before this runs.
        auto partnerBefore=[&](const PlacedUnit& unit,const PlacedUnit& image,size_t self)->int {
            for(size_t j=0;j<before.size();++j)if(j!=self && before[j].type==unit.type && samePlace(before[j],image.x,image.z))return int(j);
            return -1;
        };
        for(size_t i:changed)for(Xf xf:images) {
            const auto oldImage=mirrorUnit(before[i],xf,width,height);
            // A unit leaving the axis (its own image) gains a copy like any other.
            const bool axis=oldImage && samePlace(before[i],oldImage->x,oldImage->z);
            const int j=oldImage && !axis?partnerBefore(before[i],*oldImage,i):-1;
            if(j>=0 && changed.count(size_t(j)))continue;   // the partner is moved explicitly too
            if(j<0) {addCopy(units[i],xf,i);continue;}      // a moved unit gains missing partners
            if(const auto image=mirrorUnit(units[i],xf,width,height)) {
                units[size_t(j)].x=image->x;units[size_t(j)].z=image->z;units[size_t(j)].angle=image->angle;
            } else ++result.skipped;
        }
    } else if(units.size()>before.size()) {
        const size_t count=units.size();
        for(size_t i=before.size();i<count;++i) {
            std::vector<std::pair<float,float>> seen{{units[i].x,units[i].z}};
            for(Xf xf:images)if(const auto image=mirrorUnit(units[i],xf,width,height)) {
                if(std::find(seen.begin(),seen.end(),std::pair{image->x,image->z})!=seen.end())continue;
                seen.push_back({image->x,image->z});addCopy(units[i],xf,i);
            } else ++result.skipped;
        }
    } else {
        // Removed units: the multiset difference, then each image's partner.
        std::vector<bool> matched(units.size(),false);std::vector<PlacedUnit> removed;
        for(const auto& unit:before) {
            bool found=false;
            for(size_t j=0;j<units.size();++j)if(!matched[j] && sameUnit(units[j],unit)) {matched[j]=found=true;break;}
            if(!found)removed.push_back(unit);
        }
        for(const auto& unit:removed)for(Xf xf:images)if(const auto image=mirrorUnit(unit,xf,width,height))
            if(const int j=partner(unit,*image,units.size());j>=0) {units.erase(units.begin()+j);++result.copies;}
    }
    return result;
}

LiveMirrorResult mirrorFeatureEdits(const std::vector<uint16_t>& beforeFeatures,const std::vector<std::string>& beforeNames,
                                    tak::tnt::Map& map,Mirror kind,const FootprintLookup& footprint) {
    LiveMirrorResult result;const auto images=mirrorImages(kind);
    if(beforeFeatures.size()!=map.features.size())return result;
    struct Change {int x,z;std::string name;};
    std::vector<Change> removed,added;std::set<size_t> edited;
    for(size_t i=0;i<map.features.size();++i)if(map.features[i]!=beforeFeatures[i]) {
        const uint16_t old=beforeFeatures[i];
        const std::string was=old<0xFFFA && old<beforeNames.size()?beforeNames[old]:"",now=named(map,i)?map.featureNames[map.features[i]]:"";
        if(was==now)continue;
        edited.insert(i);
        const int x=int(i%size_t(map.width)),z=int(i/size_t(map.width));
        if(!was.empty())removed.push_back({x,z,was});
        if(!now.empty())added.push_back({x,z,now});
    }
    auto each=[&](const Change& change,auto&& fn) {
        const auto [fx,fz]=footprint?footprint(change.name):std::pair{1,1};
        std::vector<std::pair<int,int>> seen{{change.x,change.z}};
        for(Xf xf:images) {
            const auto image=mirrorFeature(change.x,change.z,fx,fz,xf,map.width,map.height);
            if(!image) {++result.skipped;continue;}
            if(std::find(seen.begin(),seen.end(),*image)!=seen.end())continue;
            seen.push_back(*image);
            const size_t cell=size_t(image->second)*map.width+image->first;
            if(!edited.count(cell))fn(cell);
        }
    };
    for(const auto& change:removed)each(change,[&](size_t cell) {
        if(named(map,cell) && map.featureNames[map.features[cell]]==change.name) {map.features[cell]=0xFFFF;++result.copies;}
    });
    for(const auto& change:added)each(change,[&](size_t cell) {
        if(named(map,cell)) {if(map.featureNames[map.features[cell]]!=change.name)++result.skipped;return;}
        const auto value=intern(map,change.name);
        if(value==0xFFFF) {++result.skipped;return;}
        map.features[cell]=value;++result.copies;
    });
    return result;
}

LiveMirrorResult mirrorBlockerEdits(const std::vector<uint16_t>& before,tak::tnt::Map& map,Mirror kind) {
    LiveMirrorResult result;const auto images=mirrorImages(kind);
    if(before.size()!=map.features.size())return result;
    std::vector<std::pair<size_t,bool>> changes;   // cell, painted (else erased); ascending
    for(size_t i=0;i<map.features.size();++i) {
        const uint16_t was=before[i],now=map.features[i];
        if((was==kCellEmpty && now==kCellBlocked) || (was==kCellBlocked && now==kCellEmpty))changes.push_back({i,now==kCellBlocked});
    }
    auto edited=[&](size_t cell) {
        const auto at=std::lower_bound(changes.begin(),changes.end(),cell,[](const auto& change,size_t c){return change.first<c;});
        return at!=changes.end() && at->first==cell;
    };
    for(const auto& [i,paint]:changes) {
        const int x=int(i%size_t(map.width)),z=int(i/size_t(map.width));
        std::vector<std::pair<int,int>> seen{{x,z}};
        for(Xf xf:images) {
            const auto image=mirrorFeature(x,z,1,1,xf,map.width,map.height);
            if(!image) {++result.skipped;continue;}
            if(std::find(seen.begin(),seen.end(),*image)!=seen.end())continue;
            seen.push_back(*image);
            const size_t cell=size_t(image->second)*map.width+image->first;
            if(edited(cell))continue;
            uint16_t& v=map.features[cell];
            if(paint && v==kCellEmpty) {v=kCellBlocked;++result.copies;}
            else if(paint && v!=kCellBlocked)++result.skipped;   // a feature or road holds the image
            else if(!paint && v==kCellBlocked) {v=kCellEmpty;++result.copies;}
        }
    }
    return result;
}

LiveMirrorResult mirrorStartEdits(const std::vector<tak::tnt::StartPos>& before,std::vector<tak::tnt::StartPos>& starts,
                                  Mirror kind,int width,int height) {
    LiveMirrorResult result;const auto images=mirrorImages(kind);
    auto at=[&](int x,int z,size_t self)->int {
        for(size_t j=0;j<starts.size();++j)if(j!=self && starts[j].xpos==x && starts[j].zpos==z)return int(j);
        return -1;
    };
    auto addCopy=[&](const tak::tnt::StartPos& start,Xf xf) {
        const auto image=mirrorStart(start,xf,width,height);
        if(!image) {++result.skipped;return;}
        if(image->xpos==start.xpos && image->zpos==start.zpos)return;
        const int number=nextStartNumber(starts);
        if(at(image->xpos,image->zpos,starts.size())>=0 || !number) {++result.skipped;return;}
        starts.push_back({number,image->xpos,image->zpos});++result.copies;
    };
    if(starts.size()==before.size()) {
        std::set<size_t> changed;
        for(size_t i=0;i<starts.size();++i)if(starts[i].xpos!=before[i].xpos || starts[i].zpos!=before[i].zpos)changed.insert(i);
        for(size_t i:changed)for(Xf xf:images) {
            const auto oldImage=mirrorStart(before[i],xf,width,height);
            const bool axis=oldImage && oldImage->xpos==before[i].xpos && oldImage->zpos==before[i].zpos;
            const int j=oldImage && !axis?at(oldImage->xpos,oldImage->zpos,i):-1;
            if(j>=0 && changed.count(size_t(j)))continue;
            if(j<0) {addCopy(starts[i],xf);continue;}
            if(const auto image=mirrorStart(starts[i],xf,width,height)) {starts[size_t(j)].xpos=image->xpos;starts[size_t(j)].zpos=image->zpos;}
            else ++result.skipped;
        }
    } else if(starts.size()>before.size()) {
        const size_t count=starts.size();
        for(size_t i=before.size();i<count;++i) {
            std::vector<std::pair<int,int>> seen{{starts[i].xpos,starts[i].zpos}};
            for(Xf xf:images)if(const auto image=mirrorStart(starts[i],xf,width,height)) {
                if(std::find(seen.begin(),seen.end(),std::pair{image->xpos,image->zpos})!=seen.end())continue;
                seen.push_back({image->xpos,image->zpos});addCopy(starts[i],xf);
            } else ++result.skipped;
        }
    } else {
        for(const auto& start:before) {
            if(std::any_of(starts.begin(),starts.end(),[&](const auto& s){return s.number==start.number && s.xpos==start.xpos && s.zpos==start.zpos;}))continue;
            for(Xf xf:images)if(const auto image=mirrorStart(start,xf,width,height))
                if(const int j=at(image->xpos,image->zpos,starts.size());j>=0) {starts.erase(starts.begin()+j);++result.copies;}
        }
    }
    return result;
}

TerrainClip copyTerrain(const tak::tnt::Map& map,Box b,const std::vector<PlacedUnit>& units,const SectionMatcher* matcher) {
    TerrainClip clip;
    b={std::max(0,b.x0),std::max(0,b.z0),std::min(map.blocksX,b.x1),std::min(map.blocksY,b.z1)};
    if(b.x1<=b.x0 || b.z1<=b.z0)return clip;
    auto& c=clip.map;
    c.blocksX=b.x1-b.x0;c.blocksY=b.z1-b.z0;c.width=c.blocksX*2;c.height=c.blocksY*2;
    c.seaLevel=map.seaLevel;c.stockTerrain=map.stockTerrain;
    c.heights.resize(size_t(c.width)*c.height);c.features.resize(c.heights.size(),0xFFFF);
    c.tileKeys.resize(size_t(c.blocksX)*c.blocksY);c.tileCols.resize(c.tileKeys.size());c.tileRows.resize(c.tileKeys.size());
    for(int z=0;z<c.blocksY;++z)for(int x=0;x<c.blocksX;++x) {
        const size_t i=size_t(z)*c.blocksX+x,j=size_t(z+b.z0)*map.blocksX+x+b.x0;
        c.tileKeys[i]=map.tileKeys[j];c.tileCols[i]=map.tileCols[j];c.tileRows[i]=map.tileRows[j];
    }
    for(int z=0;z<c.height;++z)for(int x=0;x<c.width;++x) {
        const size_t i=size_t(z)*c.width+x,j=size_t(z+b.z0*2)*map.width+x+b.x0*2;
        c.heights[i]=map.heights[j];
        const uint16_t v=map.features[j];
        c.features[i]=v>=0xFFFA?v:named(map,j)?intern(c,map.featureNames[v]):0xFFFF;
    }
    for(const auto& unit:units)if(unit.x>=b.x0*32 && unit.x<b.x1*32 && unit.z>=b.z0*32 && unit.z<b.z1*32) {
        auto copy=unit;copy.x-=b.x0*32.f;copy.z-=b.z0*32.f;copy.name.clear();clip.units.push_back(std::move(copy));
    }
    if(matcher && matcher->built()) {
        clip.attribution=matcher->attribute(map,b);clip.matcherKey=matcher->key();
        for(auto& source:clip.attribution)if(source.section>=0) {source.x-=b.x0;source.z-=b.z0;}
    }
    return clip;
}
PasteResult pasteTerrain(tak::tnt::Map& map,std::vector<PlacedUnit>& units,const TerrainClip& clip,int bx,int bz,StampLayers layers) {
    PasteResult result;
    if(clip.empty() || !stampSection(map,clip.map,bx,bz,layers))return result;
    result.pasted=true;
    if(layers.objects)for(auto unit:clip.units) {
        unit.x+=bx*32.f;unit.z+=bz*32.f;unit.name.clear();
        if(unit.x<0 || unit.z<0 || unit.x>=map.width*16.f || unit.z>=map.height*16.f ||
           std::any_of(units.begin(),units.end(),[&](const auto& u){return overlaps(u,unit);})) {++result.skippedUnits;continue;}
        units.push_back(std::move(unit));++result.units;
    }
    return result;
}
std::optional<TerrainClip> mirrorClip(const TerrainClip& clip,Xf xf,const SectionMatcher& matcher,const FootprintLookup& footprint,std::string& why) {
    if(clip.empty()) {why="The terrain clipboard is empty.";return std::nullopt;}
    if(!matcher.built() || clip.matcherKey!=matcher.key() || clip.attribution.size()!=size_t(clip.map.blocksX)*clip.map.blocksY) {
        why="the clipboard came from another world, so its sections are unknown here";return std::nullopt;
    }
    for(size_t i=0;i<clip.attribution.size();++i) {
        const int s=clip.attribution[i].section;
        if(s<0) {why="block ("+std::to_string(i%size_t(clip.map.blocksX))+", "+std::to_string(i/size_t(clip.map.blocksX))+") of the clipboard is not a known section";return std::nullopt;}
        if(matcher.twin(s,xf)<0) {why="section '"+matcher.name(s)+"' has no mirror twin";return std::nullopt;}
    }
    TerrainClip out;out.world=clip.world;out.matcherKey=clip.matcherKey;
    auto& m=out.map;const auto& c=clip.map;
    m.blocksX=swapsAxes(xf)?c.blocksY:c.blocksX;m.blocksY=swapsAxes(xf)?c.blocksX:c.blocksY;m.width=m.blocksX*2;m.height=m.blocksY*2;
    m.seaLevel=c.seaLevel;m.stockTerrain=c.stockTerrain;
    m.heights.resize(size_t(m.width)*m.height);m.features.resize(m.heights.size(),0xFFFF);
    m.tileKeys.resize(size_t(m.blocksX)*m.blocksY);m.tileCols.resize(m.tileKeys.size());m.tileRows.resize(m.tileKeys.size());
    const auto result=mirrorTerrain(matcher,c,{0,0,c.blocksX,c.blocksY},clip.attribution,xf,m);
    if(!result.failed.empty()) {why="a clipboard section does not fit its mirror twin";return std::nullopt;}
    for(size_t i=0;i<c.features.size();++i)if(named(c,i)) {
        const auto& name=c.featureNames[c.features[i]];const auto [fx,fz]=footprint?footprint(name):std::pair{1,1};
        if(const auto image=mirrorFeature(int(i%size_t(c.width)),int(i/size_t(c.width)),fx,fz,xf,c.width,c.height)) {
            const auto value=intern(m,name);if(value!=0xFFFF)m.features[size_t(image->second)*m.width+image->first]=value;
        }
    }
    for(const auto& unit:clip.units)if(const auto image=mirrorUnit(unit,xf,c.width,c.height))out.units.push_back(*image);
    return out;
}

} // namespace cart
