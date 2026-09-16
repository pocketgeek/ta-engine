#include "campaign/campaign.h"

#include <algorithm>
#include <unordered_map>
#include <cctype>

#include "hpi/hpi.h"
#include "tdf/tdf.h"
#include "tnt/ota.h"
#include "util/strcase.h"

namespace ta {
namespace {

// "camps/Book Of Darien.tdf" -> "book of darien"
std::string stemOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t start = slash == std::string::npos ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    size_t end = (dot == std::string::npos || dot < start) ? path.size() : dot;
    std::string s = path.substr(start, end - start);
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::string titleCase(const std::string& id) {
    if (id == "ipalt") return "The Iron Plague (Alt. Ending)";
    // Title case, but keep short function words lowercase unless they lead.
    static const char* kSmall[] = {"of", "the", "and", "a", "an", "in", "to"};
    std::string out;
    size_t i = 0;
    bool first = true;
    while (i < id.size()) {
        size_t j = id.find(' ', i);
        std::string w = id.substr(i, j == std::string::npos ? std::string::npos : j - i);
        bool small = false;
        for (const char* s : kSmall) if (w == s) small = true;
        if (!w.empty() && (first || !small)) w[0] = char(std::toupper((unsigned char)w[0]));
        if (!out.empty()) out += ' ';
        out += w;
        first = false;
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return out;
}

// Display rank: the two shipped campaigns first (base game, then expansion), the
// alt-ending branch after, everything else alphabetical.
int rankOf(const std::string& id) {
    if (id == "book of darien") return 0;
    if (id == "the iron plague") return 1;
    if (id == "ipalt") return 2;
    return 3;
}

}  // namespace

bool loadCampaign(const hpi::Vfs& vfs, const std::string& file, Campaign& out) {
    if (!vfs.has(file)) return false;
    std::vector<uint8_t> bytes = vfs.read(file);
    return parseCampaignText(std::string(bytes.begin(), bytes.end()), file, out);
}

bool parseCampaignText(const std::string& text, const std::string& file, Campaign& out) {
    tdf::Node root = tdf::parseText(text, file);

    out = Campaign{};
    out.file = file;
    out.id = stemOf(file);
    out.title = titleCase(out.id);
    if (const tdf::Node* h = root.child("header")) out.side = h->valueOr("campaignside", "");

    // Missions are [MISSION0], [MISSION1], ... contiguous in retail; stop at the
    // first gap so a stray later section can't reorder the campaign.
    for (int n = 0;; ++n) {
        const tdf::Node* m = root.child("mission" + std::to_string(n));
        if (!m) break;
        CampaignMission cm;
        cm.otaFile = m->valueOr("missionfile", "");
        // The STEM comes from `missionfile`, which is the actual file in both
        // games ("AC01.ota", "takmission01_mt.ota"). `missionname` is NOT
        // interchangeable with it: in TA it is the chapter's DISPLAY title --
        // "1: A Hero Returns" -- so using it as a stem asked the VFS for a file
        // by that name and every TA campaign came up empty. Kingdoms happened to
        // repeat the stem there, which is why it worked.
        if (!cm.otaFile.empty()) cm.stem = stemOf(cm.otaFile);
        const std::string declaredName = m->valueOr("missionname", "");
        if (cm.stem.empty()) cm.stem = declaredName;        // no missionfile: fall back
        // TA states the title inline; Kingdoms leaves it to translate/missions.tdf,
        // which the caller fills in afterwards. Only take it when it is not just
        // the stem repeated.
        if (declaredName != cm.stem) cm.title = declaredName;
        if (!cm.stem.empty()) out.missions.push_back(std::move(cm));
    }
    return !out.missions.empty();
}

// TA's briefing text: `camps/briefs/<brief>.txt`, where <brief> is named by the
// mission's own .ota (`brief=ArmCampaign1`). The file is prose rather than a
// bullet list, and carries three things a reader has to handle:
//
//   * a `MISSION 1.0001ARME` id line at the top, and an `END` line at the bottom
//   * inline colour runs written `&Y...&` / `&R...&` (yellow for the priority
//     banner, red for a warning), which are markup and not content
//
// Kingdoms instead shipped `missions/<stem>.txt` as a plain bulleted list, which
// is what the parser below handles. Both are returned as a list of lines.
std::string taBriefPath(const hpi::Vfs& vfs, const std::string& stem) {
    const std::string ota = "maps/" + stem + ".ota";
    if (!vfs.has(ota)) return {};
    try {
        auto b = vfs.read(ota);
        tnt::Scenario sc = tnt::Scenario::parse(std::string(b.begin(), b.end()));
        if (sc.brief.empty()) return {};
        // The key sometimes carries the extension and sometimes does not: the
        // campaign missions write `brief=ArmCampaign1` while the Battle Tactics
        // scenarios write `brief=I09Brief.txt`. Appending unconditionally asks
        // for "I09Brief.txt.txt" and finds nothing, which is why only the
        // campaign proper had briefing text.
        std::string name = sc.brief;
        if (!ta::iendsWith(name, ".txt")) name += ".txt";
        const std::string p = "camps/briefs/" + name;
        if (vfs.has(p)) return p;
    } catch (const std::exception&) {}
    return {};
}

std::vector<std::string> parseTaBriefing(const std::string& raw) {
    std::vector<std::string> out;
    const std::string& bytes = raw;

    // Strip the colour markup, drop the id and END lines, and keep the rest as
    // paragraphs -- blank lines separate them and are preserved as empty
    // entries so a caller can lay the briefing out.
    //
    // A colour run is written `&X ... &`: the OPENING delimiter carries a
    // one-letter colour and the closing one does not. Dropping only the '&'
    // characters leaves that letter glued to the text ("RExpect Core
    // patrols"). Across the 50 shipped brief files the opens are exactly R,
    // Y and G -- 38, 31 and 13 of them -- and the closes are the other 82
    // occurrences, so the two sides balance and the rule is simply: '&'
    // followed by one of those letters consumes both, any other '&' consumes
    // itself.
    std::string text;
    text.reserve(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) {
        const char c = char(bytes[i]);
        if (c == '&') {
        const char n = i + 1 < bytes.size() ? char(bytes[i + 1]) : '\0';
        if (n == 'R' || n == 'Y' || n == 'G') ++i;   // opening: eat the colour too
        continue;
        }
        if (c != '\r') text += c;
    }
    std::string line;
    bool first = true;
    auto push = [&] {
        size_t b = line.find_last_not_of(" \t");
        std::string t = b == std::string::npos ? std::string() : line.substr(0, b + 1);
        const bool isId = first && t.rfind("MISSION ", 0) == 0;
        first = false;
        if (isId) { line.clear(); return; }
        if (t == "END") { line.clear(); return; }
        out.push_back(t);
        line.clear();
    };
    for (char c : text) {
        if (c == '\n') push();
        else line += c;
    }
    push();
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

std::vector<std::string> loadObjectives(const hpi::Vfs& vfs, const std::string& stem) {
    std::vector<std::string> out;
    std::string path = taBriefPath(vfs, stem);
    const bool taBrief = !path.empty();
    if (!taBrief) path = "missions/" + stem + ".txt";
    if (!vfs.has(path)) return out;
    std::vector<uint8_t> bytes = vfs.read(path);
    if (taBrief) return parseTaBriefing(std::string(bytes.begin(), bytes.end()));
    std::string cur;
    auto flush = [&] {
        size_t a = cur.find_first_not_of(" \t");
        // drop the leading bullet + spaces (retail prefixes each line with CP1252 0x95)
        while (a != std::string::npos && a < cur.size() &&
               !std::isalnum((unsigned char)cur[a]) && cur[a] != '"')
            ++a;
        size_t b = cur.find_last_not_of(" \t\r");
        if (a != std::string::npos && b != std::string::npos && b >= a)
            out.push_back(cur.substr(a, b - a + 1));
        cur.clear();
    };
    for (uint8_t c : bytes) {
        if (c == '\n') flush();
        else if (c != '\r') cur += char(c);
    }
    flush();
    return out;
}

// Chapter titles. Retail ships them in translate/missions.tdf (base) and
// translate/ipmissions.tdf (Iron Plague) as one section per mission stem, with a
// value per language. The campaign picker showed the raw stem instead, so every
// row read "takmission01_mt" rather than "All Hell Broken Loose". English only for
// now -- the other languages are right there in the file when we localise the UI.
static std::unordered_map<std::string, std::string> chapterTitles(const hpi::Vfs& vfs) {
    std::unordered_map<std::string, std::string> out;
    for (const char* path : {"translate/missions.tdf", "translate/ipmissions.tdf"}) {
        if (!vfs.has(path)) continue;
        try {
            auto b = vfs.read(path);
            tdf::Node root = tdf::parseText(std::string(b.begin(), b.end()), path);
            for (const auto& [key, node] : root.children) {
                const std::string* en = node.value("english");
                if (!en || en->empty()) continue;
                std::string k = key;
                std::transform(k.begin(), k.end(), k.begin(), ::tolower);
                if (k == "chapter") continue;   // the word "Chapter" itself
                out.emplace(k, *en);
            }
        } catch (const std::exception&) {}
    }
    return out;
}

std::vector<Campaign> loadCampaigns(const hpi::Vfs& vfs) {
    const auto titles = chapterTitles(vfs);
    std::vector<Campaign> camps;
    for (const std::string& p : vfs.list("camps/")) {
        if (p.size() < 4) continue;
        std::string ext = p.substr(p.size() - 4);
        for (char& c : ext) c = char(std::tolower((unsigned char)c));
        if (ext != ".tdf") continue;
        Campaign c;
        if (loadCampaign(vfs, p, c)) {
            for (auto& m : c.missions) {
                std::string k = m.stem;
                std::transform(k.begin(), k.end(), k.begin(), ::tolower);
                if (auto it = titles.find(k); it != titles.end()) m.title = it->second;
            }
            camps.push_back(std::move(c));
        }
    }
    std::stable_sort(camps.begin(), camps.end(), [](const Campaign& a, const Campaign& b) {
        int ra = rankOf(a.id), rb = rankOf(b.id);
        return ra != rb ? ra < rb : a.id < b.id;
    });
    // Fold the alt-ending branch (ipalt) into its parent campaign: it shares every
    // mission but the last, so instead of listing it as a redundant standalone
    // campaign, record its finale as the parent's altFinal and drop it.
    auto idIs = [](const Campaign& c, const char* s) { return c.id == s; };
    auto ipalt = std::find_if(camps.begin(), camps.end(), [&](const Campaign& c) { return idIs(c, "ipalt"); });
    auto ip = std::find_if(camps.begin(), camps.end(), [&](const Campaign& c) { return idIs(c, "the iron plague"); });
    if (ipalt != camps.end() && ip != camps.end() && !ipalt->missions.empty()) {
        ip->altFinal = ipalt->missions.back().stem;
        camps.erase(ipalt);
    }
    return camps;
}

}  // namespace ta
