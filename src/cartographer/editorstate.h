#pragma once
// Shared state of a running Cartographer (see editor.h for the entry point).
// The editor used to be one function whose locals were captured by lambdas;
// those locals are now members, so each concern lives in its own file:
//   main.cpp      startup (Session ctor), Editor setup, the frame loop
//   session.cpp   save / open / undo restore / recovery / Test Map
//   headless.cpp  command line, --stamp/--save/--bundle, --shot capture
//   palette.cpp   left browser: search, categories, thumbnails grid
//   canvas.cpp    map canvas: stamping, units/features/starts/regions editing
//   chrome.cpp    menus, toolbar, keyboard shortcuts, status bar, minimap, hints
//   dialogs.cpp   modal dialogs, regions/use-only/placed-unit lists, async jobs
//   scripting.cpp the trigger-rule overlay
//   aiprofileswindow.cpp the AI Profiles window (SCENARIO > AI Profiles, A)
//   symmetrytools.cpp Symmetry menu, Symmetrize, live mirroring, terrain clipboard
// Declaration order is the old local order, so destruction order is unchanged.

#include "cartographer/editor.h"
#include "cartographer/aiprofiles.h"
#include "cartographer/blockers.h"
#include "cartographer/editorfont.h"
#include "cartographer/featureselection.h"
#include "cartographer/features.h"
#include "cartographer/history.h"
#include "cartographer/newmap.h"
#include "cartographer/overlay.h"
#include "cartographer/playtest.h"
#include "cartographer/preferences.h"
#include "cartographer/recovery.h"
#include "cartographer/regions.h"
#include "cartographer/scrolling.h"
#include "cartographer/ruleedit.h"
#include "cartographer/sectiongroups.h"
#include "cartographer/sections.h"
#include "cartographer/selection.h"
#include "cartographer/symmetry.h"
#include "cartographer/textedit.h"
#include "cartographer/thumbnails.h"
#include "cartographer/units.h"
#include "cartographer/validation.h"
#include "client/datafolderpick.h"
#include "client/mapview.h"
#include "client/modelview.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "tnt/mapgen.h"
#include "tnt/ota.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cart {

// UI magnification. The whole editor is drawn in a fixed logical coordinate
// space and scaled up by this factor via SDL_RenderSetScale, so every panel,
// glyph, and dialog doubles uniformly on hi-DPI displays. Input coordinates are
// divided back down to logical space at the event source.
extern float kUIScale;
// Renderer pixels per window point: 1 except in a Linux native Wayland window (opened with
// SDL_WINDOW_ALLOW_HIGHDPI, client/waylandchoice.h), where it is the desktop scale (1.5,
// 2, ...). Mouse positions and window sizes are points, so they divide by kUIScale alone;
// the renderer output divides by renderScale(). Updated every frame.
extern float kBacking;
inline float renderScale() { return kUIScale * kBacking; }

inline std::string pathText(const std::filesystem::path& path) {const auto text=path.u8string();return {text.begin(),text.end()};}
inline std::string lowerText(std::string value) {std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return char(std::tolower(c));});return value;}
inline void fillRect(SDL_Renderer* r, int x, int y, int w, int h, Uint8 cr, Uint8 cg, Uint8 cb) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    SDL_Rect rc{x, y, w, h};
    SDL_RenderFillRect(r, &rc);
}

// The browser's unit groups (the Units palette and the AI Profiles window).
inline std::string unitGroupOf(const tak::sim::UnitType* type) {
    if(!type)return "OTHER";
    if(type->onMana)return "MANA";
    if(type->isBuilder)return "BUILDERS";
    if(type->isStructure())return "BUILDINGS";
    if(type->canFly)return "AIR";
    if(type->domain==tak::sim::UnitType::Domain::Water)return "NAVAL";
    return "GROUND";
}

// A fresh map for the New dialog / --new launch: either a flat ground stamp or
// procedural terrain from the engine's map generator (the "~gen1~" generator the
// lobby uses -- coastlines, relief, trees/rocks/mana, and start positions).
struct FreshMap {
    tak::tnt::Map map;
    std::string recipe;
    std::vector<tak::tnt::StartPos> starts;   // only for random terrain
    std::string note;   // random terrain: what the generator could not fit (shown on its preview)
};
FreshMap buildFreshMap(const tak::hpi::Vfs& vfs, cart::SectionLibrary& sections,
                       tak::terrain::Compositor& comp, const std::string& world,
                       int wUnits, int hUnits, bool random, const tak::mapgen::Params* requested=nullptr);

// The open document and the app services it needs, built by the constructor
// (main.cpp). Usage errors, startup failures and headless one-shots end the
// run by throwing Exit before any interactive state exists.
struct Session {
    using FrameHook=std::function<void(SDL_Window*,SDL_Renderer*,int)>;
    struct Exit {int code;};
    Session(int argc,char** argv,const FrameHook& frameHook,const EditorHooks& hooks);

    // Command line (headless.cpp).
    std::string dataRoot, mapName, outDir = ".", exportPath, bundlePath, stampName, newWorld = "aramon", shotPath, shotTool;   // --shot / --shot-tool terrain|features|units|starts|blocked
    int stampBX = 0, stampBY = 0, newW = 0, newH = 0;
    bool explicitOutput=false;
    bool randomTerrain = false;   // --random: generate procedural terrain for --new
    void parseArguments(int argc,char** argv);
    std::optional<int> runHeadless();   // --stamp / --save / --bundle exit code
    // GAME DATA FOLDER (client/datafolder.h): where dataRoot came from (a --data or
    // launch-folder override greys the Preferences row), the launch's arguments and
    // what reopens the open document, for the restart that applies a new folder.
    tak::DataFolderOrigin dataOrigin=tak::DataFolderOrigin::Picker;
    std::vector<std::string> launchArgs;
    std::string documentRequest;   // its saved file, or the installed map's name ("" = none)
    std::string pendingOpen;       // a .kmp/.tnt FILE named at launch: opened once the editor is up

    const FrameHook& frameHook;
    const EditorHooks& hooks;
    Uint64 recoveryClock() const;
    struct SdlLifetime { bool active=false; ~SdlLifetime() { if(active) SDL_Quit(); } } sdlLifetime;
    std::filesystem::path preferencesFolder;
    EditorPreferences preferences;
    SDL_Window* win=nullptr;
    std::unique_ptr<SDL_Window,decltype(&SDL_DestroyWindow)> windowOwner{nullptr,SDL_DestroyWindow};
    SDL_Renderer* ren=nullptr;
    // Declared before MapView: its worker and textures must die before SDL's
    // renderer, including on export/error returns.
    std::unique_ptr<SDL_Renderer,decltype(&SDL_DestroyRenderer)> rendererOwner{nullptr,SDL_DestroyRenderer};
    std::optional<EditorFont> editorFont;
    void persistPreferences();

    // Mount the retail install exactly like the engine (loose + *.hpi, retail
    // precedence). The Vfs must outlive the MapView (it borrows it by ref).
    tak::hpi::Vfs vfs;
    bool interactive=false;
    std::filesystem::path recoveryFolder, recoveredFrom;
    std::string recoveredMapPath;
    std::unique_ptr<RecoveryFile> recoverySession,recoveredSession;
    std::string recoveryStatus;

    bool showWelcome=false;
    // No map is open: a bare launch (File makes or opens one), or an AI mod discarded the
    // map (or a new one nothing was done to). mapViewPtr then holds a hidden, clean
    // placeholder that is never drawn or edited; File > New map / Open map replace it.
    bool noDocument=false;
    std::string mapPath;
    tak::tnt::Scenario scenario;
    std::unique_ptr<MapView> mapViewPtr;
    // The map's scenario .crt, kept in full so a save preserves the trigger
    // rules, regions, and custom types the unit tool doesn't edit. `units` is
    // the editor's working view (map pixels); `scen` is everything else.
    tak::crt::Scenario scen;
    std::vector<PlacedUnit> units;
    bool unitsEdited = false;
    // Use-only restriction: the allowed unit types (UPPERCASE). Loaded from the
    // sibling <map>.tdf when the OTA references one; empty = unrestricted.
    std::set<std::string> useOnly;
    // The map's ai/<x>.txt profiles (document.h AiFiles), carried byte for byte
    // through open, save, Test Map, recovery and undo.
    AiFiles aiFiles;
    // Set when the terrain is painted, so Save regenerates the minimaps (an
    // unedited save stays byte-identical to the source; an edited one gets a
    // fresh overview reflecting the paint).
    bool edited = false;
    bool minimapRefreshNeeded=true;
    std::shared_ptr<std::atomic_bool> minimapCancel=std::make_shared<std::atomic_bool>(false);
    uint64_t minimapRevision=0;
    bool minimapPreviewPending=false;
    Uint64 minimapDue=0;
    void invalidateMinimap();
    // Unsaved-changes flag for the exit prompt. Set by every edit (terrain,
    // features, units, starts, scenario props, use-only, triggers), cleared on a
    // successful save. Broader than `edited` (which only gates minimap regen).
    bool dirty = false;
    bool historyPending=false, resetHistory=false;
    bool overlayInvalidated=false;
    History history;
    HistoryState historySnapshot();
    void commitHistory();
    bool wantNew = false;   // deferred "open New Map" after a discard confirmation
    std::string saveError;
    bool saveDocument(const std::string& destination,bool bundle);
    bool saveMap(const std::string& path);
    bool saveBundle(const std::string& path);
    // An installed map (a listMaps path) as the files Test Map sends, read the way the
    // editor opens a map by name: what opening it and pressing Test Map would send.
    DocumentFiles installedMapFiles(const std::string& mapPath,const std::string& name) const;

    // Section-prefab palette for this map's world (falls back to aramon).
    SectionLibrary sections;
    std::string world;
    FeatureLibrary features;
    std::vector<std::string> unitTypes;
    tak::sim::TypeRegistry unitRegistry;
    // The same install's CRUSADES roster (unitscb/ alone): Check Map names what the
    // map's scenario loses with CRUSADES on (validateCrusadesRoster).
    tak::sim::TypeRegistry crusadesRegistry;
    // Stamp a named section into the map at block (bx,by) (canvas.cpp).
    bool stampByName(const std::string& name, int bx, int by);
};

// The interactive editor: everything the frame loop reads and writes.
struct Editor : Session {
    Editor(int argc,char** argv,const FrameHook& frameHook,const EditorHooks& hooks);
    int run();   // main.cpp: the frame loop, returns the exit code

    static constexpr int kMenuH = 44;    // top menu-bar strip
    static constexpr int kStatusH = 22;  // bottom status strip
    static constexpr int kPaletteW = 200;   // left section-palette panel
    static constexpr int kPaletteTop=kMenuH+76;
    static constexpr int kCellH=106;
    static constexpr int kThumb = 88;       // section thumbnail cell (px)
    static constexpr int cols = std::max(1, (kPaletteW - 8) / (kThumb + 4));
    // Toolbar row (y 25..41, under the menu strip): tool buttons, mode buttons,
    // REGIONS and TEST MAP. It fits the narrowest logical window (900 px).
    static constexpr int kToolX=96,kToolW=64;      // five tool buttons, 60 px wide
    static constexpr int kModeX=424,kModeW=64;     // PLACE / SELECT / ERASE / PAN
    static constexpr int kRegionsX=692,kRegionsW=80,kTestX=780,kTestW=116;
    MapView& mapView=*mapViewPtr;

    // Frame layout, recomputed at the top of every frame (logical pixels).
    int w=0,h=0,canvasW=0,canvasH=0;
    SDL_Rect miniRect{};
    bool running = true;

    // --- Palette (palette.cpp) ------------------------------------------------
    int selected = sections.list().empty() ? -1 : 0;   // section index (TERRAIN)
    int selectedFeat = features.list().empty() ? -1 : 0; // feature index (FEATURES)
    int paletteScroll = 0;
    bool& showGrid=preferences.showGrid;
    bool& showFeatures=preferences.showFeatures;
    bool& showUnits=preferences.showUnits;
    bool& showStarts=preferences.showStarts;
    StampLayers stampLayers;
    // Feature-sprite textures (GAF frame 0), cached by feature name, used both in
    // the palette and to draw placed features on the canvas.
    std::map<std::string, SDL_Texture*> featTex;
    const FeatSprite* featureSpriteFor(const FeatureRef& ref);
    SDL_Texture* featTextureFor(const FeatureRef& r);
    Thumbnails thumbs{ren,vfs};
    Thumbnails unitPortraits{ren,vfs,64};
    SDL_Texture* thumbFor(const std::string& path);

    // --- Canvas tools (canvas.cpp) --------------------------------------------
    int lastStampX=-1,lastStampY=-1;
    void stampAtMouse(int mx, int my, int w, int h);
    enum Tool { TERRAIN, FEATURES, UNITS, STARTS, BLOCKED };
    static constexpr int kToolCount=5;
    static constexpr const char* kToolNames[kToolCount]={"TERRAIN","FEATURES","UNITS","STARTS","BLOCKED"};
    int selectedType = unitTypes.empty() ? -1 : 0;   // unit-type index (UNITS palette)
    int currentPlayer = 0;                            // player id new units get
    int draggingUnit = -1;                            // index into units while dragging
    int unitAt(int mx, int my);   // placed unit whose marker is near (mx,my), or -1
    Tool tool = TERRAIN;
    enum EditMode { MODE_PLACE,MODE_SELECT,MODE_ERASE,MODE_PAN };
    EditMode editMode=MODE_PLACE;
    bool toolVisible();
    void revealTool();
    UnitSelection selectedUnits;
    FeatureSelection selectedFeatures;
    bool selectionBox=false,selectionAppend=false;
    int selectionX0=0,selectionZ0=0,selectionX1=0,selectionZ1=0;

    // --- Palette browser state (palette.cpp) ----------------------------------
    std::string paletteSearch;
    TextEdit paletteEditor;
    bool paletteFocus=false;
    int categoryIndex=0,factionIndex=0;
    std::vector<int> paletteItems;
    std::vector<std::string> paletteCategories{"ALL"};
    std::string paletteKey;
    const tak::sim::UnitType* unitInfo(int index);
    std::string unitCategory(int index);
    std::string paletteLabel(int index);
    void refreshPalette();
    int paletteRowAt(int x,int y) const;   // browser row under (x,y), unclamped
    std::string paletteHint(int y) const;
    bool handlePaletteFocus(SDL_Event& e);
    bool handlePaletteClick(SDL_Event& e);
    bool handlePaletteWheel(SDL_Event& e);
    // Browser scrollbar: drag the thumb, click the track to page; the wheel scrolls too.
    std::string paletteListKey;   // the list's identity: a new list starts at the top
    bool paletteThumbDrag=false;int paletteThumbGrab=0;
    int paletteContentHeight() const;
    ScrollBar paletteScrollBar() const;
    bool handlePaletteScrollbar(SDL_Event& e);
    void drawPalette();
    // Terrain groups: kinds and retail folders; numbered variants share one row.
    SectionCatalog sectionCatalog;
    std::string catalogWorld,sectionGroup;   // group id, remembered per world
    size_t catalogSize=0;unsigned catalogGeneration=0;
    std::vector<std::vector<int>> paletteVariants;   // TERRAIN: each row's variants
    bool sectionMenuOpen=false,sectionMenuPress=false;
    int sectionMenuScroll=0;
    static constexpr int kSectionMenuW=300,kSectionRowH=16;
    void selectSectionGroup(const std::string& id);
    int sectionMenuRowAt(int x,int y) const;
    bool handleSectionMenu(SDL_Event& e);
    void drawSectionMenu();

    // --- Canvas editing (canvas.cpp) ------------------------------------------
    void placeFeature(int mx, int my, bool erase);   // feature plane at the mouse cell
    int draggingStart = -1;           // index into scenario.starts while dragging
    bool clearArm = false, clearDrag = false;   // Clear Area drag-box (screen px)
    int clx0 = 0, cly0 = 0, clx1 = 0, cly1 = 0;
    bool mouseCell(int mx, int my, int& cx, int& cz);   // canvas mouse -> 16px cell
    int startAt(int mx, int my);   // start marker near (mx,my), or -1
    bool handleRegionDrag(SDL_Event& e);
    bool handleCanvasKeys(SDL_Event& e);
    void handleCanvasInput(SDL_Event& e,float pointerDX,float pointerDZ);
    void drawCanvas();
    void drawClearArea();

    // --- Blocked cells: the BLOCKED tool and its overlay (canvas.cpp) ----------
    int blockedBrush=0;                            // index into cart::kBlockedBrushSizes (not saved)
    bool blockedStroke=false,blockedRect=false;    // a Place/Erase drag, or a Shift+drag rectangle
    int blockedLastX=-1,blockedLastZ=-1;           // the stroke's previous cell (-1: none yet)
    int blockedRectX0=0,blockedRectZ0=0,blockedRectX1=0,blockedRectZ1=0;
    bool handleBlockedInput(SDL_Event& e);
    bool blockedShown() const {return preferences.showBlocked || tool==BLOCKED;}
    // One texel per cell (blocked red, roads blue), updated where the plane changed.
    std::unique_ptr<SDL_Texture,decltype(&SDL_DestroyTexture)> blockedTexture{nullptr,SDL_DestroyTexture};
    std::vector<uint16_t> blockedPlane;            // the plane the texture shows
    int blockedW=0,blockedH=0,blockedCount=0;
    uint64_t blockedRevision=~uint64_t(0);
    void refreshBlockedOverlay();
    void drawBlocked();

    // --- Minimap (chrome.cpp) -------------------------------------------------
    std::unique_ptr<SDL_Texture,decltype(&SDL_DestroyTexture)> minimapTexture{nullptr,SDL_DestroyTexture};
    const uint8_t* minimapSource=nullptr;
    bool minimapDrag=false;
    void refreshMinimap();
    std::future<MinimapPreview> minimapJob;
    uint64_t minimapJobRevision=0;
    std::string minimapError;
    void updateLiveMinimap();
    bool handleMinimapInput(SDL_Event& e);
    void drawMinimap();

    // --- GAME DATA FOLDER (dialogs.cpp, drawn in View > Preferences) ------------
    // One setting shared with the game (Settings::dataDir). A valid pick is saved at
    // once; it is LOADED by restarting, after the usual save prompt, and the restarted
    // editor reopens this document (cart::restartArguments).
    tak::DataFolderPicker folderPick;
    std::string folderSaved;            // the shared setting, as last read or written
    std::string folderNote;             // the last pick's outcome, if it needs saying
    bool folderNoteError=false;
    SDL_Rect mFolderChange{}, mFolderRestart{};
    bool restartAfterQuit=false;        // M_QUITSAVE asks before a RESTART, not an exit
    std::optional<std::vector<std::string>> restartArgs;   // set once run() ends to restart
    void clickDataFolder();
    void pollDataFolder();
    void requestRestart();
    void drawDataFolder(const SDL_Rect& content);

    // --- Modal dialogs (dialogs.cpp) ------------------------------------------
    enum Modal { M_NONE, M_SCENARIO, M_RESIZE, M_UNIT, M_MESSAGE, M_RULE, M_RULENAME, M_CONFIRM, M_NEW, M_QUITSAVE, M_SAVEAS, M_OPENPATH, M_REGION, M_GENERATOR, M_GENERATING, M_GENERATED, M_ANALYZING, M_CHECKING, M_ISSUES, M_MODEL_LOADING, M_MODEL, M_OBJECTS, M_SYMMETRIZE, M_PREFERENCES, M_NEWAIMOD };
    static constexpr int kMaxFields = 15;
    std::function<void()> confirmAction;   // M_CONFIRM: run on OK
    Modal modal = M_NONE;
    std::string mfOriginal[kMaxFields];
    std::string mf[kMaxFields];              // field buffers
    bool mfNumeric[kMaxFields] = {};
    // Choice lists are read-only unless explicitly editable (flag suggestions).
    const std::vector<std::string>* mfChoices[kMaxFields] = {};
    bool mfEditableChoice[kMaxFields] = {};
    int scrPlayer = 0;                     // 0=All Players, 1..8=Player 1..8
    int mDropScroll=0;
    std::vector<std::string> ruleChoices[kMaxFields];
    int mDropOpen = -1;                       // which field's dropdown list is open (-1 none)
    std::vector<SDL_Rect> mDropRects;         // hit rects of the open list's rows
    SDL_Rect mRandom{};                       // New Map: the RANDOM button rect
    SDL_Rect mRegenerate{};
    SDL_Rect mAiProfiles{};                   // Scenario Properties: EDIT AI PROFILES...
    // Dropdown option lists (New Map): map sizes in units, and the four worlds.
    const std::vector<std::string> kSizeOpts = {"8", "16", "24", "32", "48", "64"};
    const std::vector<std::string> kWorldOpts = {"aramon", "veruna", "taros", "zhon", "creon", "volcano"};
    const std::vector<std::string> kDisplayOpts={"WINDOWED","BORDERLESS FULLSCREEN"},kOnOffOpts={"OFF","ON"},
        kScaleOpts={"50%","75%","100%","125%","150%","175%","200%"};
    tak::mapgen::Params generatorParams;
    bool regeneratingCurrent=false;
    std::array<std::string,4> generatorDraft;
    std::vector<std::string> generatorLayouts,generatorScatter;
    const char* mLabel[kMaxFields] = {};
    std::string mTitle,modalError;
    int mN = 0;                              // active field count
    int mfocus = 0;
    TextEdit fieldEditor;
    int descriptionScroll=0;bool descriptionFollowCaret=true;
    std::vector<std::pair<std::string,std::string>> openCatalog,openMatches;
    SDL_Rect openList{};int openScroll=0;std::string openFilterPrevious;bool openFilterDirty=true;
    bool openForTest=false;                   // M_OPENPATH picks the map for an AI mod's TEST IN GAME
    void filterOpenMaps();
    bool regionsOpen=false;
    bool& showRegions=preferences.showRegions;
    bool regionCanvas=false;
    RegionDrag regionDrag;
    int regionSelected=-1,regionScroll=0,editRegion=-1;
    SDL_Rect regionList{},regionNew{},regionEdit{},regionDelete{},regionDone{},regionOnMap{};
    int editUnit = -1;                       // UNITS: index being edited (M_UNIT)
    bool editRuleAction=false;
    tak::crt::RuleGroup* editRuleGroup=nullptr;
    tak::crt::Rule* editRule = nullptr;      // M_RULE: rule whose params are edited
    std::vector<std::string> mMsg;           // M_MESSAGE: wrapped text lines
    SDL_Rect mBox[kMaxFields]{}, mOK{}, mCancel{}, mQuit{};   // render-computed hit rects
    void openMessage(const std::string& title, const std::string& text);
    void openConfirm(const std::string& title, const std::string& text,std::function<void()> action);
    void openModal(Modal m, int unitIdx = -1);
    std::future<FreshMap> generatorJob;
    std::optional<FreshMap> generatedPreview;
    std::unique_ptr<SDL_Texture,decltype(&SDL_DestroyTexture)> generatedTexture{nullptr,SDL_DestroyTexture};
    std::string generatingName,generatingWorld;
    bool cancelGeneration=false,quitAfterGeneration=false;
    void adoptFreshMap(FreshMap fm,const std::string& nm,const std::string& wld);
    void applyNewMap(bool random);
    void openGeneratorRecipe();
    void applyModal();
    void openRuleEditor(tak::crt::Rule* r, bool isAction);
    void openRuleName(tak::crt::RuleGroup* group);

    // --- Use Only restriction list, placed units, model, overlay, Check Map ---
    bool useOnlyOpen = false;
    int useOnlyScroll = 0;
    SDL_Rect uoList{}, uoDone{}, uoClear{};   // render-computed hit rects
    std::string objectSearch;
    TextEdit objectEditor;
    std::vector<int> objectMatches;
    int objectScroll=0,objectRow=0;
    SDL_Rect objectList{},objectSearchBox{},objectClose{};
    void filterObjects();
    void openObjects();
    void chooseObject(int row,bool edit);
    std::future<ModelView::Asset> modelJob;
    std::shared_ptr<std::atomic_bool> modelCancel;
    std::unique_ptr<ModelView> modelPreview;
    std::string modelTitle;
    std::string modelWarning;
    SDL_Rect modelArea{},modelClose{};
    Uint64 modelTime=0;
    bool quitAfterModel=false;
    void previewModel();
    std::future<TerrainOverlay> overlayJob;
    std::unique_ptr<SDL_Texture,decltype(&SDL_DestroyTexture)> overlayTexture{nullptr,SDL_DestroyTexture};
    std::string overlayLegend;
    int overlayWidth=0,overlayHeight=0;
    bool discardOverlay=false,quitAfterOverlay=false;
    std::shared_ptr<std::atomic_bool> overlayCancel;
    void buildOverlay(OverlayKind kind);
    std::vector<MapIssue> mapIssues;
    std::future<std::vector<MapIssue>> checkJob;
    bool discardCheck=false,quitAfterCheck=false;
    uint64_t mapIssueRevision=0,checkRevision=0;
    int issueScroll=0,issueContentHeight=0;
    SDL_Rect issueList{},issueRecheck{},issueClose{};
    std::vector<SDL_Rect> issueRows;
    size_t issueIndex=0;
    void showMapIssue();
    void checkMap();
    void pollJobs();                          // collect finished background jobs
    bool swallowWhileBusy(const SDL_Event& e); // input while a job's busy modal is up
    bool handleGeneratedPreview(SDL_Event& e);
    void handleRegionsList(SDL_Event& e);
    void handleUseOnly(SDL_Event& e);
    void handleModal(SDL_Event& e);
    void drawRegionsList();
    void drawModal();
    void drawUseOnly();

    // --- Scenario Scripting (per-player trigger rules) overlay (scripting.cpp) -
    bool scriptOpen = false;
    int scrGroup = -1;                     // selected rule-group in this player
    int scrCondSel = -1, scrActSel = -1;   // selected condition / action row
    int scrRuleScroll = 0, scrCondScroll = 0, scrActScroll = 0;
    int scrDetailScroll=0,scrDetailMax=0;
    std::string scrDetailText;
    SDL_Rect rRuleDetail{},rCopyRuleText{};
    bool pickOpen = false, pickAction = false, pickTemplate=false;
    std::vector<RuleTemplate> templateCatalog;
    std::string templateQuery;
    TextEdit templateEditor;
    int templateSelection=0;
    SDL_Rect rTemplates{},rTemplateSearch{};
    int pickScroll = 0;
    SDL_Rect rRuleList{}, rCondList{}, rActList{}, rPickList{};   // render-computed
    SDL_Rect rPrevP{}, rNextP{}, rAddRule{}, rDelRule{}, rAddCond{}, rDelCond{},
             rAddAct{}, rDelAct{}, rNameRule{}, rScrDone{}, rPickCancel{};
    RuleColumn ruleColumn=RuleColumn::Group;
    RuleClipboard ruleClipboard;
    SDL_Rect ruleCopy{},rulePaste{},ruleDuplicate{},ruleUp{},ruleDown{},playerCopy{},playerPaste{};
    std::vector<tak::crt::RuleGroup>& scrGroups();
    tak::crt::RuleGroup* curGroup();
    void ruleOperation(int operation);
    void openScripting();
    void openTemplates();
    void insertTemplate();
    void handleScripting(SDL_Event& e);
    void drawScripting();

    // --- AI Profiles window (aiprofileswindow.cpp) ------------------------------
    // The map's ai/ files (Session::aiFiles) are the truth; the window parses the
    // shown file into a draft (aiprofiles.h) and every committed edit writes the
    // bytes back, one undo step each. Ctrl+Z / Ctrl+Y inside the window step through
    // the steps made since it opened (aiUndo); older map steps undo after DONE.
    struct AiRosterRow {
        std::string key;                         // DEFAULT: FBI unitname; levels: UnitType::id
        const tak::sim::UnitType* type=nullptr;
        std::string label,group,side;
        bool variant=false,reachable=true,lockedForDefault=false,fromPack=false;
    };
    struct AiTabView {                           // the shown file and what it does
        bool built=false;std::string key,bytes;bool readOnly=false;
        AiDraft draft;std::vector<AiLine> lines;std::vector<AiIssue> issues;
        tak::ai::ClassicProfile classic;tak::ai::Profile level,builtin;
        tak::ai::Difficulty preview=tak::ai::Difficulty::Normal;   // the level `level` and `builtin` are for
    };
    enum class AiView {Table,Text,Issues,Groups};   // Groups: the DEFAULT tab's group rules (Advanced)
    enum AiFocus {AI_NONE,AI_SEARCH,AI_NAME,AI_CELL,AI_TEXT,AI_PACK,AI_GROUP,AI_WORD};
    bool aiOpen=false,aiStartOpen=false,aiSetOnly=false,aiShowUnbuildable=false,aiTextFollow=true;
    std::string aiPendingName;                   // own profile chosen, waiting for Start from (one undo step with its file)
    std::map<std::string,std::pair<std::string,bool>> aiWrittenCache;   // key -> (bytes, opens as written)
    int aiTab=0;                                 // 0 = DEFAULT AI, 1 = TAK ENGINE AI (ai/takengine.txt, all six levels)
    tak::ai::Difficulty aiPreviewLevel=tak::ai::Difficulty::Normal;   // the level whose built-in table and limits the TAK ENGINE tab shows
    AiView aiView=AiView::Table;
    AiFocus aiFocus=AI_NONE;
    int aiSide=0,aiGroup=0,aiScroll=0,aiTextScroll=0,aiIssueScroll=0,aiPack=0;
    std::string aiSearch,aiName,aiCellText,aiText,aiTextKey,aiSelected,aiCellKey,aiNote,aiHint;
    int aiCellColumn=-1;                         // 0 = chance/weight, 1 = limit
    TextEdit aiSearchEdit,aiNameEdit,aiCellEdit,aiTextEdit;
    std::optional<tak::ai::ClassicCatalogue> aiCatalogue;
    std::vector<AiRosterRow> aiDefaultRoster,aiLevelRoster;
    std::vector<std::string> aiPacks;
    std::optional<tak::hpi::Vfs::Files> aiPackFiles;
    AiFiles aiOpened;                            // the files as the window opened ('*' = edited since)
    AiTabView aiViewCache;
    std::vector<AiIssue> aiIssues;
    std::vector<std::pair<SDL_Rect,size_t>> aiIssueRows;
    struct AiCell {SDL_Rect rect;std::string key;int column;};   // column 0/1 = value/limit, 2 = BUILD
    std::vector<AiCell> aiCells;
    std::vector<std::pair<SDL_Rect,std::string>> aiRows;
    SDL_Rect aiTabRects[2]{},aiPreviewBox{},aiPresetRects[3]{},aiSearchBox{},aiSideBox{},aiGroupBox{},aiSetOnlyBox{},aiUnbuildableBox{},
             aiList{},aiStandardRadio{},aiOwnRadio{},aiNameBox{},aiStartButton{},aiViewButton{},aiCheckButton{},
             aiEditableButton{},aiInsertButton{},aiPackButton{},aiDoneButton{},aiTextBox{},aiStartStandard{},aiStartEmpty{},aiStartCancel{};
    std::optional<tak::ai::Difficulty> aiTabLevel() const;   // the preview level on the TAK ENGINE tab
    void aiStepPreview(int by);
    std::string aiKeyFor(int tab) const;
    std::string aiTabKey() const {return aiKeyFor(aiTab);}   // the file this tab edits ("" = DEFAULT on the standard profile, no map file)
    const AiTabView& aiTabView();
    const std::vector<AiRosterRow>& aiRoster() const {return aiTab?aiLevelRoster:aiDefaultRoster;}
    std::vector<const AiRosterRow*> aiVisibleRows();
    bool aiEdit(const std::function<bool(AiDraft&)>& change,const std::string& header={});
    void aiSetFile(const std::string& key,std::string_view bytes);
    void openAiProfiles();
    void closeAiProfiles();
    void aiCommitText();
    void aiCommitCell();
    void aiCommitName();
    void aiClearCell(const std::string& key,int column);
    void aiToggleBuild(const std::string& key);
    void aiPreset(int which);
    void aiStartFrom(bool standard);
    void aiChooseStandard();
    void aiChooseOwn();
    void aiMakeEditable();
    void aiCheck();
    void aiSelectTab(int tab);
    std::string aiRowExplanation(const AiRosterRow&);
    std::string aiHeaderFor(int tab) const;      // the header line a new file of this tab gets
    void handleAiProfiles(SDL_Event& e);
    void drawAiProfiles();
    // Group rules (design 4, DEFAULT only): rows of the draft's category-word lines,
    // each listing the units it covers, words only from this install's catalogue.
    struct AiGroupRow {SDL_Rect row{},keyword{},word{},value{},up{},down{},remove{};};
    std::vector<AiGroupRow> aiGroupRows;
    std::vector<std::pair<SDL_Rect,std::string>> aiWordRows;
    int aiGroupSelected=-1,aiGroupCell=-1,aiGroupScroll=0,aiGroupUnitScroll=0,aiWordScroll=0;
    int aiWordPick=-2;                           // the word picker: -2 closed, -1 adds a rule, else that rule's word
    std::string aiWordSearch;TextEdit aiWordSearchEdit;
    SDL_Rect aiGroupsButton{},aiAddGroupButton{},aiGroupList{},aiGroupUnits{},aiWordSearchBox{},aiWordList{},aiWordCancel{};
    bool aiGroupsEditable();
    void aiPickWord(const std::string& word);
    void aiCommitGroupCell();
    void aiGroupAction(int rule,int what);       // 0 keyword, 1 move up, 2 remove, 3 move down
    void drawAiGroups(int x0,int y0,int W,int listBottom);
    void drawAiWordPicker();
    // The unit roster of a game mod (design 7.2, phase 5): when a pack is edited or
    // previewed, the window lists the base install's units with that pack mounted
    // (an AI-window-only registry; the map's own registry is untouched).
    struct AiPackRoster {std::string pack;tak::sim::TypeRegistry registry;std::set<std::string> added;};
    std::unique_ptr<AiPackRoster> aiPackRoster;  // the last pack's, cached
    std::string aiRosterPack;                    // the pack whose units are listed ("" = the base install)
    const tak::sim::TypeRegistry& aiRegistry() const {return !aiRosterPack.empty() && aiPackRoster && aiPackRoster->pack==aiRosterPack?aiPackRoster->registry:unitRegistry;}
    void aiUseRoster(const std::string& pack);
    bool aiRosterLists(const std::string& pack) const;   // the units listed are this pack's
    void aiBuildRoster();
    // Undo inside the window: only the steps made since it opened.
    uint64_t aiUndoFloor=0,aiRedoAt=0;std::vector<uint64_t> aiRedoRevisions;
    void aiUndo(bool redo);
    // Game mods (design 3.5, 7.2). OPEN FROM GAME MOD edits one pack's ai/ files as
    // a separate set with a pack banner: not the map's, not in map undo (a file
    // operation, like Save); SAVE TO PACK writes them back. SAVE AS GAME MOD writes
    // the map's files into <data>/mods/game/<pack>/ai/ (document.h writeModProfiles).
    struct AiModSet {std::string pack;AiFiles files,opened;std::string defaultKey="ai/default.txt";};
    std::optional<AiModSet> aiMod;
    AiFiles& aiSet() {return aiMod?aiMod->files:aiFiles;}   // the files the window edits
    bool aiModDirty() const {return aiMod && aiMod->files!=aiMod->opened;}
    struct AiModRow {std::string label,source,target;bool tick=false,isDefault=false;SDL_Rect box{};};
    bool aiModSaveOpen=false,aiModOpenOpen=false;
    std::string aiModPack;TextEdit aiModPackEdit;
    int aiModDefaultAs=0;                        // DEFAULT written as 0 = ai/default.txt, 1 = ai/<aiprofile>.txt
    std::vector<AiModRow> aiModRows;
    std::vector<std::string> aiModDeletes;       // emptied drafts the pack still has (offered, unticked)
    std::set<std::string> aiModDeleteTicked;
    std::vector<std::pair<SDL_Rect,std::string>> aiModDeleteBoxes,aiModPackRows;
    // OPEN FROM GAME MOD's pack list: 14 rows shown, the rest by wheel, scrollbar
    // (drag the thumb, click the track to page) or Up/Down/PgUp/PgDn/Home/End + Enter.
    static constexpr int kAiPackRowH=20,kAiPackRowsShown=14;
    int aiModPackScroll=0,aiModPackSel=0;bool aiModPackThumbDrag=false;int aiModPackThumbGrab=0;
    SDL_Rect aiModPackList{};
    ScrollBar aiModPackBar() const;
    void aiOpenPackList();
    void aiRevealPack();   // scrolls the selected pack into view
    ModProfilePlan aiModPlan;std::string aiModPlanError,aiModNote;
    SDL_Rect aiModSaveButton{},aiModOpenButton{},aiModDefaultButton{},aiModPackBox{},aiModPackPrev{},aiModPackNext{},
             aiModDefaultAsBox{},aiModWriteButton{},aiModCancelButton{};
    void aiOpenModSave();
    void aiModReplan();
    void aiModSave();
    void aiOpenPack(const std::string& pack);
    void aiClosePack(std::function<void()> then={});
    void aiStepModDefault();
    void aiCommitModName();                      // a pack's named DEFAULT profile (NEW NAME...)
    SDL_Rect aiModNewNameButton{};
    bool handleAiModDialogs(SDL_Event& e);
    void drawAiModDialogs();
    // ---- an AI mod without a map (File > New AI mod / Open AI mod) ----
    // The AI Profiles window with one game mod pack as the only document: the map is
    // neither shown nor edited (it is put aside clean, or replaced by no document when it
    // had to be discarded), map menus are disabled, and closing the pack ends the mode.
    bool aiModOnly=false;
    // No map to show or edit (an AI mod's workspace, or no document at all): only the
    // File commands that need no map run, and the canvas, browser and minimap are inert.
    bool noMap() const {return aiModOnly || noDocument;}
    bool aiModPickWorkspace=false;               // the pack list was opened by File > Open AI mod
    std::function<void()> aiAfterSave;           // TEST IN GAME: continues after SAVE TO PACK succeeded
    SDL_Rect aiTestButton{};
    bool mapHasUnsavedWork() const;
    void requestAiMod(bool create);              // the menu command: asks about the map, then names or lists packs
    void startAiModWorkspace(const std::string& pack,bool fresh);
    void finishAiModWorkspace();
    void leaveAiModThen(std::function<void()> then);   // asks about unsaved pack edits first
    bool menuRowEnabled(int menu,int row) const;
    void aiTestInGame();
    void openTestMapPicker();
    void aiLaunchTest(const std::string& mapPath);

    // --- Document commands (session.cpp) --------------------------------------
    bool openDocument(const std::string& request);
    Playtest playtest;
    bool playtestAiMod=false;                    // the running test is an AI mod's TEST IN GAME, not TEST MAP
    void testMap();
    std::filesystem::path gameClient() const;    // takclient beside the editor (a test hook may stand in)

    // --- Menus, toolbar, shortcuts, status, hints (chrome.cpp) ----------------
    int menuOpen=-1;
    std::string hoveredHint;Uint64 hintSince=0;
    struct ViewBookmark {float x,z,zoom;};
    std::optional<ViewBookmark> viewBookmark;
    const std::vector<std::string> menuNames={"FILE","EDIT","VIEW","SCENARIO","SYMMETRY","HELP"};
    const std::vector<std::vector<std::string>> menuRows={
        {"New map (Ctrl+N)","Open map (Ctrl+O)","Save (Ctrl+S)","Save As (Ctrl+Shift+S)","Export loose files","Test Map (F5)",
         "New AI mod... (Ctrl+Shift+N)","Open AI mod... (Ctrl+Shift+O)","Exit"},
        {"Undo (Ctrl+Z)","Redo (Ctrl+Y)","Clear area (K)","Terrain brush: protect objects","Copy terrain selection (Ctrl+C)","Paste terrain clipboard (Ctrl+V)"},
        {"Fit map","100% terrain zoom","Toggle grid (G)","Toggle regions","Frame selection","Store view bookmark","Restore view bookmark","Overlay: movement","Overlay: buildability","Overlay: water depth","Overlay: slopes","Hide terrain overlay","Smaller UI","Larger UI","Show features","Show units","Show starts","Unit model preview (F6)","Preferences (F11 fullscreen)",
         "Show blocked cells (B)"},
        {"Properties (P)","Resize (R)","Use Only units (U)","Check map (C)","Scripting (T)","Next issue","Regions","Validation results","Regenerate from recipe","Log Test Map triggers","Placed units","AI Profiles... (A)"},
        {"Live symmetry (M)","Symmetrize map (Shift+M)","Axis: vertical (left | right)","Axis: horizontal (top / bottom)","Axis: diagonal \\ (square maps)",
         "Axis: diagonal / (square maps)","Rotate 2-way (180)","Rotate 4-way (90, square maps)","Mirror 4-way (both axes)","Locate unmirrored terrain"},
        {"Editor controls","About"}};
    void menuAction(int menu,int row);
    bool handleMenuInput(SDL_Event& e);
    bool handleToolbarModes(SDL_Event& e);
    bool handleShortcuts(SDL_Event& e);
    bool handleToolbarTools(SDL_Event& e);
    void drawChrome();
    void drawMenuRows();   // the open menu's rows
    void drawHints();
    // Display mode and edge scrolling, both editor preferences (View > Preferences).
    std::optional<SDL_Rect> windowedRect;   // size/position to restore on leaving fullscreen
    void applyDisplayMode();                // preferences.fullscreen -> the window, live
    void toggleFullscreen();                // F11
    Uint64 edgeSince=0,edgeLast=0;          // dwell start / previous frame (SDL ticks)
    void edgeScroll();                      // once per frame, after input

    // --- Recovery and undo (session.cpp) --------------------------------------
    bool recoveryFilesPresent=!recoveredFrom.empty();
    std::future<std::string> recoveryJob;
    Uint64 nextRecovery=0;
    void collectRecovery();
    void clearRecovery();
    void autosaveRecovery();
    void restoreHistory(const HistoryState* state);

    // --- Symmetry and the terrain clipboard (symmetrytools.cpp) ---------------
    cart::Mirror symmetryKind=cart::Mirror::Vertical;
    bool liveSymmetry=false;
    std::string symmetryStatus;Uint64 symmetryStatusAt=0;
    std::vector<MapIssue> symmetryIssues;      // unmirrored terrain from the last Symmetrize
    std::vector<std::string> symmetryKindOpts,symmetrySourceOpts;
    const std::vector<std::string> symmetryTerrainOpts={"MIRROR SECTION TWINS","LEAVE TERRAIN"};
    cart::SectionMatcher sectionMatcher;unsigned sectionMatcherGeneration=0;
    const cart::SectionMatcher& symmetrySections();   // built lazily per world
    cart::FootprintLookup featureFootprint();
    bool symmetryFits(cart::Mirror) const;            // square-only kinds on square maps
    bool symmetrySelection() const;                   // a selection can be the source
    void setSymmetryStatus(std::string text);
    std::string symmetryStatusText() const;
    std::string symmetryMenuLabel(size_t row) const;
    void symmetryMenu(int row);
    void toggleLiveSymmetry();
    void setupSymmetrizeFields();                     // openModal(M_SYMMETRIZE)
    void refreshSymmetryDialog();
    void applySymmetrize();
    // Snapshot of the edited layer before a canvas handler; the destructor
    // mirrors whatever the handler changed (live symmetry).
    struct LiveEdit {
        Editor& editor;bool active=false;
        std::vector<PlacedUnit> units;std::vector<tak::tnt::StartPos> starts;
        std::vector<uint16_t> features;std::vector<std::string> featureNames;
        LiveEdit(Editor& owner,const SDL_Event& e,bool keyboard);
        ~LiveEdit();
    };
    void stampMirrors(int section,int x,int z);
    std::optional<cart::Box> terrainSelection;
    bool terrainSelecting=false;int terrainSelX=0,terrainSelZ=0;
    cart::TerrainClip terrainClip;
    bool clipBrush=false;int clipBrushSection=-1;
    std::string clipWorldAccepted;
    std::unique_ptr<SDL_Texture,decltype(&SDL_DestroyTexture)> clipTexture{nullptr,SDL_DestroyTexture};
    bool clipboardBrush() const;
    cart::Box clipBrushBox(float wx,float wz) const;
    bool handleTerrainSelection(SDL_Event& e);
    bool handleTerrainKeys(SDL_Event& e);
    void copyTerrainSelection();
    void useTerrainClipboard();
    void pasteTerrainAtMouse(int mx,int my);
    void pasteTerrainAt(int bx,int bz);
    void drawSymmetryOverlay();

    // --- --shot (headless.cpp) ------------------------------------------------
    int shotPending=0;       // visible palette cells whose async art has not resolved this frame
    Uint64 shotWaitStart=0;  // SDL ticks of the first frame that had to wait (0 = not waiting)
    void prepareShot();
    void captureShot();
};

} // namespace cart
