#pragma once

// Cartographer symmetry tools and the terrain clipboard (no SDL; unit-tested).
//
// A mirror kind is a set of rigid map transforms (the dihedral group of the
// map rectangle). Objects keep their own footprint orientation: a mirrored unit
// or feature occupies the mirrored cells (flips, 180 degrees) or, for the
// transposing transforms on square maps, the cells centred on the mirrored
// centre. Facing angles use the engine convention: CRT angle a faces
// (sin a, cos a) in map x/z, so 0 faces south (+z) and 90 faces east (+x).
//
// Terrain is never pixel-flipped: its art has baked lighting and relief.
// A block is attributed to the section placement that best explains its tile
// (tile triples are nearly unique per section), and the mirrored block takes
// the section's MIRROR TWIN, found by name (ne<->nw, lft<->rgt, ...), at the
// mirrored placement. Blocks without a known section or twin are left alone
// and reported.

#include "cartographer/sections.h"
#include "cartographer/units.h"
#include "cartographer/validation.h"
#include "tnt/ota.h"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace cart {

// Rigid transforms of a W x H domain. Rot90 turns clockwise on screen (east
// becomes south). Transpose, AntiTranspose, Rot90 and Rot270 swap W and H.
enum class Xf : uint8_t { Identity,FlipX,FlipZ,Rot180,Transpose,AntiTranspose,Rot90,Rot270 };
enum class Mirror : uint8_t { Vertical,Horizontal,Diagonal,AntiDiagonal,Rotate2,Rotate4,Quad };
constexpr int kMirrorKinds=7;

const char* mirrorLabel(Mirror);                    // "VERTICAL AXIS (LEFT | RIGHT)"
bool mirrorNeedsSquare(Mirror);                     // diagonals and 90-degree turns
std::vector<Xf> mirrorImages(Mirror);               // the non-identity transforms
std::vector<std::string> mirrorSources(Mirror);     // source half / quadrant names
bool swapsAxes(Xf);
Xf inverse(Xf);

struct PointF {double x=0,z=0;};
struct Box {int x0=0,z0=0,x1=0,z1=0;};              // half-open cell/block rectangle
PointF transform(Xf,PointF,double width,double height);
Box transform(Xf,Box,int width,int height);
std::pair<int,int> transformDirection(Xf,int dx,int dz);
// Whether a point (object centre) lies in the chosen source region. Regions are
// half-open so a source and its images tile the map; axis points that the
// transforms fix belong to every source and are never copied onto themselves.
bool inSource(Mirror,int source,PointF,double width,double height);
int mirrorAngle(Xf,int degrees);                    // 0..359

// Mirrored placements. Each returns nothing when the image leaves the map.
std::optional<PlacedUnit> mirrorUnit(const PlacedUnit&,Xf,int mapWidthCells,int mapHeightCells);
// Feature footprint origin (cells) for a feature of fx x fz cells at (x,z).
std::optional<std::pair<int,int>> mirrorFeature(int x,int z,int fx,int fz,Xf,int width,int height);
std::optional<tak::tnt::StartPos> mirrorStart(const tak::tnt::StartPos&,Xf,int width,int height);
std::optional<tak::crt::Region> mirrorRegion(const tak::crt::Region&,Xf,int width,int height);
// Next free start number 1..8, or 0 when all eight are used.
int nextStartNumber(const std::vector<tak::tnt::StartPos>&);
// Owner remap from symmetric starts: player p (start p+1) maps to the owner of
// the start at the mirror of start p+1's position; others are unchanged.
std::array<int,9> mirrorPlayers(const std::vector<tak::tnt::StartPos>&,Xf,int width,int height);
// "Base mirror", "Base mirror 2"...: unique (case-insensitive), at most 63 bytes.
std::string uniqueRegionName(const std::vector<tak::crt::Region>&,const std::string& base);

// The mirror twin of a section name among the lowercase names of its category,
// or nothing. Direction tokens (n/s/e/w runs such as "ne", "e_n_s", "nwe",
// and lft/rgt/top/btm) are transformed; a name whose tokens are invariant is
// its own twin. A direction-free name is its own twin only in a flat category.
std::optional<std::string> sectionTwinName(const std::string& name,const std::string& category,
                                           const std::vector<std::string>& categoryNames,Xf);

// Section placements recognized from tile content, plus memoized twins.
class SectionMatcher {
public:
    using Loader=std::function<const tak::tnt::Map*(size_t)>;
    struct Source {int section=-1,x=0,z=0;};   // placement origin in map blocks
    void build(const std::vector<SectionRef>&,const Loader&);
    bool built() const {return built_;}
    const std::string& key() const {return key_;}
    void setKey(std::string key) {key_=std::move(key);}
    // Attribution of every block in `blocks` (row-major), -1 when unknown.
    std::vector<Source> attribute(const tak::tnt::Map&,Box blocks) const;
    int twin(int section,Xf) const;              // -1 when there is none
    const tak::tnt::Map* section(int index) const {return index>=0 && size_t(index)<sections_.size()?sections_[size_t(index)]:nullptr;}
    const std::string& name(int index) const {return refs_[size_t(index)].name;}
private:
    struct Tile {int section,x,z;};
    std::vector<SectionRef> refs_;
    std::vector<const tak::tnt::Map*> sections_;
    std::map<std::tuple<uint32_t,uint8_t,uint8_t>,std::vector<Tile>> tiles_;
    std::map<std::string,std::vector<std::string>> categories_;   // lowercase names
    mutable std::map<std::pair<int,Xf>,int> twins_;
    std::string key_;
    bool built_=false;
};

// Writes, for each attributed source block, the twin's block at the mirrored
// placement into `out` (same domain unless the transform swaps axes). Tiles,
// heights and terrain markers move together; named feature anchors in `out`
// are preserved, as by a protected stamp. Failed image blocks are returned.
struct TerrainMirror {int mirrored=0;std::vector<std::pair<int,int>> failed;std::vector<int> failedSection;};
// `include` filters the source blocks (all when empty).
TerrainMirror mirrorTerrain(const SectionMatcher&,const tak::tnt::Map& source,Box blocks,
                            const std::vector<SectionMatcher::Source>& attribution,Xf,tak::tnt::Map& out,
                            const std::function<bool(int,int)>& include={});

// Symmetrize: the document parts are edited in place.
struct SymmetryRequest {
    Mirror kind=Mirror::Vertical;
    int source=0;                     // index into mirrorSources(kind); -1 = selection
    std::set<int> units,featureCells; // selection source
    std::optional<Box> blocks;        // selection source: terrain rectangle
    bool terrain=true;
};
struct SymmetryReport {
    int units=0,features=0,starts=0,regions=0,blocks=0;
    int blocked=0;                    // blocked cells copied onto empty image cells
    int removedUnits=0,removedFeatures=0,removedStarts=0,removedRegions=0,keptRegions=0;
    int skippedUnits=0,skippedFeatures=0,skippedStarts=0,skippedRegions=0;
    int unmirroredBlocks=0;
    std::vector<MapIssue> terrainIssues;
    std::string summary() const;      // counts for the confirmation / status bar
};
using FootprintLookup=std::function<std::pair<int,int>(const std::string&)>;
SymmetryReport symmetrize(tak::tnt::Map&,std::vector<tak::tnt::StartPos>&,tak::crt::Scenario&,
                          std::vector<PlacedUnit>&,const SymmetryRequest&,const FootprintLookup&,
                          const SectionMatcher*);

// Live symmetry: mirror the edits between `before` and the current document.
// Additions get mirrored copies, removals remove the partner at each image,
// and moves move it (indices of existing units/starts never shift on a move).
struct LiveMirrorResult {int copies=0,skipped=0;};
LiveMirrorResult mirrorUnitEdits(const std::vector<PlacedUnit>& before,std::vector<PlacedUnit>& units,
                                 Mirror,const std::vector<tak::tnt::StartPos>&,int width,int height);
LiveMirrorResult mirrorFeatureEdits(const std::vector<uint16_t>& beforeFeatures,const std::vector<std::string>& beforeNames,
                                    tak::tnt::Map& map,Mirror,const FootprintLookup&);
LiveMirrorResult mirrorStartEdits(const std::vector<tak::tnt::StartPos>& before,std::vector<tak::tnt::StartPos>& starts,
                                  Mirror,int width,int height);
// Blocked cells painted (empty -> 0xFFFC) or erased (0xFFFC -> empty) since
// `before` are painted or erased at each image under the BLOCKED tool's rules:
// paint only on empty cells, erase only blockers. An image holding a feature or
// a road is skipped.
LiveMirrorResult mirrorBlockerEdits(const std::vector<uint16_t>& before,tak::tnt::Map& map,Mirror);

// Terrain clipboard: a block rectangle with its tiles, heights, feature plane
// (named features remapped by name) and the units whose centres lie inside.
struct TerrainClip {
    tak::tnt::Map map;                         // blocksX x blocksY sized
    std::vector<PlacedUnit> units;             // pixels relative to the clip origin
    std::vector<SectionMatcher::Source> attribution;   // relative placement origins
    std::string world,matcherKey;
    bool empty() const {return map.blocksX<=0 || map.blocksY<=0;}
};
TerrainClip copyTerrain(const tak::tnt::Map&,Box blocks,const std::vector<PlacedUnit>&,const SectionMatcher*);
// Paste at block (bx,bz). Objects follow StampLayers: protected pastes keep
// existing objects and add none. Units landing off-map or overlapping an
// existing footprint are skipped. Returns false when fully off-map.
struct PasteResult {bool pasted=false;int units=0,skippedUnits=0;};
PasteResult pasteTerrain(tak::tnt::Map&,std::vector<PlacedUnit>&,const TerrainClip&,int bx,int bz,StampLayers);
// The clip seen through a transform, or nothing unless every block has a twin.
std::optional<TerrainClip> mirrorClip(const TerrainClip&,Xf,const SectionMatcher&,const FootprintLookup&,std::string& why);

} // namespace cart
