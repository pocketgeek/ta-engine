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
    // The file stem, taken from `missionfile` -- "AC01" in TA, "takmission01_mt"
    // in Kingdoms. NOT `missionname`, which is a different thing in the two games.
    std::string stem;
    std::string otaFile;    // "AC01.ota" / "takmission01_mt.ota" (missionfile)
    // The chapter's name. TA states it inline in the campaign file
    // ("1: A Hero Returns"); Kingdoms leaves it to translate/missions.tdf, which
    // the caller fills in afterwards. Empty when neither supplies one, and the UI
    // then falls back to "MISSION n".
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

// The same, straight from the .tdf text, so the mission-stem rule can be pinned
// in CI without a retail install. `file` names the source (it also supplies the
// campaign's id and display title, which come from the filename).
bool parseCampaignText(const std::string& text, const std::string& file, Campaign& out);

// Every `camps/*.tdf` in the VFS, ordered for display: Book of Darien, then The Iron
// Plague, then the alt-ending branch, then any others alphabetically.
std::vector<Campaign> loadCampaigns(const hpi::Vfs& vfs);

// A mission's objective lines from `missions/<stem>.txt`: one per line, with the retail
// bullet glyph and surrounding whitespace stripped, blank lines dropped. Empty if the
// file is absent. Shared by the briefing screen and the in-game objectives panel.
std::vector<std::string> loadObjectives(const hpi::Vfs& vfs, const std::string& stem);

// Parse a TA briefing file's text (camps/briefs/*.txt) into display lines:
// colour markup removed, the id and END lines dropped, blank lines kept as
// paragraph breaks. Exposed so the format can be pinned without an install.
std::vector<std::string> parseTaBriefing(const std::string& text);

}  // namespace ta
