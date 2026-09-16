#include "campaign/campaign.h"

#include <algorithm>
#include <unordered_map>
#include <cctype>

#include "hpi/hpi.h"
#include "tdf/tdf.h"

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
    tdf::Node root = tdf::parseText(std::string(bytes.begin(), bytes.end()), file);

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
        cm.stem = m->valueOr("missionname", "");
        if (cm.stem.empty() && !cm.otaFile.empty()) cm.stem = stemOf(cm.otaFile);
        if (!cm.stem.empty()) out.missions.push_back(std::move(cm));
    }
    return !out.missions.empty();
}

std::vector<std::string> loadObjectives(const hpi::Vfs& vfs, const std::string& stem) {
    std::vector<std::string> out;
    std::string path = "missions/" + stem + ".txt";
    if (!vfs.has(path)) return out;
    std::vector<uint8_t> bytes = vfs.read(path);
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
