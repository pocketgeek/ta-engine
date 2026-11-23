#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>
namespace cart {
struct EditorPreferences {
    int width=0,height=0,scalePercent=200;
    bool showFeatures=true,showUnits=true,showStarts=true,showRegions=true,showGrid=false;
    bool guideSeen=false;
    bool tracePlaytest=false;
    bool edgeScroll=false;    // hover at the canvas edge pans (retail only auto-scrolled drags past the view)
    bool fullscreen=false;    // borderless desktop fullscreen; width/height stay the windowed size
    bool showBlocked=false;   // the blocked-cell overlay with every tool (the BLOCKED tool always shows it)
    // Linux bookkeeping: the window kind of the last session, the last native Wayland
    // scale and the X11 / logical desktop widths that tell the X11 kinds apart
    // (tak::wayland::advanceDisplay), so a NATIVE WAYLAND switch keeps the UI size.
    int displayMode=0;float displayScale=0;int displayX11Width=0,displayLogicalWidth=0;
    std::vector<std::string> recent;
    std::map<std::string,std::string> sectionGroups;   // world -> terrain browser group id
    void remember(const std::string& request);
};
EditorPreferences loadEditorPreferences(const std::filesystem::path& folder);
bool saveEditorPreferences(const std::filesystem::path& folder,const EditorPreferences& preferences,std::string& error);
}
