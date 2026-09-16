#pragma once

// Campaign spine: the ordered list of missions in a `camps/<name>.tdf` and a small
// controller for progression (current index, win -> next / lose -> retry). SDL-free
// and data-only, so it lives in ta-formats and is testable headless. The actual
// mission run happens elsewhere (setupMission + the server); this just picks which
// mission and tracks how far the player has got. See docs/campaign-design.md.

#include <string>
#include <vector>

namespace ta {
namespace hpi { class Vfs; }

struct CampaignMission {
    std::string stem;       // "takmission01_mt" -- the bundle stem (missionname)
    std::string otaFile;    // "takmission01_mt.ota" (missionfile, informational)
    // The chapter's real name, from translate/missions.tdf (base) or
    // translate/ipmissions.tdf (Iron Plague): "All Hell Broken Loose". Empty when
    // the stem has no entry, and the UI then falls back to "MISSION n".
    std::string title;
};

struct Campaign {
    std::string file;       // "camps/book of darien.tdf" (VFS path)
    std::string id;         // "book of darien" (lowercased stem, the persistence key)
    std::string title;      // "Book of Darien" (display)
    std::string side;       // [HEADER] campaignside (vestigial in retail)
    std::vector<CampaignMission> missions;
    // An alternate final mission (branch), e.g. Iron Plague's ipalt ending. It shares
    // the campaign's earlier missions and is offered alongside the normal finale once
    // the campaign is at its last mission. Empty when there is no branch.
    std::string altFinal;
    bool empty() const { return missions.empty(); }
    int count() const { return int(missions.size()); }
};

// Load one campaign from its `camps/*.tdf` VFS path. Returns false if absent/empty.
bool loadCampaign(const hpi::Vfs& vfs, const std::string& file, Campaign& out);

// Every `camps/*.tdf` in the VFS, ordered for display: Book of Darien, then The Iron
// Plague, then the alt-ending branch, then any others alphabetically.
std::vector<Campaign> loadCampaigns(const hpi::Vfs& vfs);

// A mission's objective lines from `missions/<stem>.txt`: one per line, with the retail
// bullet glyph and surrounding whitespace stripped, blank lines dropped. Empty if the
// file is absent. Shared by the briefing screen and the in-game objectives panel.
std::vector<std::string> loadObjectives(const hpi::Vfs& vfs, const std::string& stem);

}  // namespace ta
