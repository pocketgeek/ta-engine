#include "cartographer/aiprofiles.h"

#include "ai/builtinprofile.h"
#include "sim/sim.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <set>
#include <sstream>

namespace cart {

namespace {

using tak::ai::ClassicCatalogue;
using tak::ai::Difficulty;

// The DEFAULT grammar's tokenizer, as ai::ClassicProfile::execute reads a line
// (retail 0x538a20): C isspace separates tokens and '#' ends the line.
bool cSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
std::string lowered(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lowerAscii(c);
    return out;
}
std::vector<std::string_view> classicTokens(std::string_view line) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && cSpace(line[i])) ++i;
        if (i == line.size() || line[i] == '#') break;
        const size_t start = i;
        while (i < line.size() && !cSpace(line[i]) && line[i] != '#') ++i;
        out.push_back(line.substr(start, i - start));
        if (i < line.size() && line[i] == '#') break;
    }
    return out;
}
bool blankLine(std::string_view line) {
    return std::all_of(line.begin(), line.end(), [](char c) { return cSpace(c); });
}
// Lines as both parsers see them: split at '\n'; a final newline adds no line.
std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> out;
    for (size_t start = 0; start < text.size();) {
        const size_t end = text.find('\n', start);
        if (end == std::string_view::npos) { out.push_back(text.substr(start)); break; }
        out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}
// A kept line in LF form: a CR is whitespace to both parsers.
std::string withoutCr(std::string_view line) {
    while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    std::string out(line);
    std::replace(out.begin(), out.end(), '\r', ' ');
    return out;
}
std::vector<std::string_view> spaceParts(std::string_view line) {
    std::vector<std::string_view> out;
    for (size_t start = 0;;) {
        const size_t end = line.find(' ', start);
        out.push_back(line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) return out;
        start = end + 1;
    }
}
bool wholeNumber(std::string_view s, int& v) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

constexpr std::string_view kDefaultNote = "// Units not listed are built at full chance with no limit.";
constexpr std::string_view kLevelNote = "// Units not listed keep the built-in values.";
constexpr std::string_view kGroupMarker = "// group rules";
constexpr std::string_view kKeptMarker = "// kept from the original file (no effect)";

std::string groupLine(const AiGroupRule& g) {
    std::string line = std::string(g.weight ? "weight " : "limit ") + g.word;
    if (!g.value.empty()) line += " " + g.value;
    return line;
}
std::string sanitizedHeader(std::string_view h) {
    std::string out;
    for (unsigned char c : h) out.push_back(c < 32 ? ' ' : c >= 127 ? '?' : char(c));   // ASCII only
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

constexpr Difficulty kLevels[] = {Difficulty::Defensive, Difficulty::Easy, Difficulty::Normal, Difficulty::Hard,
                                  Difficulty::Absurd, Difficulty::AbsurdlyDefensive};

// Edit distance, for "Did you mean".
size_t distance(std::string_view a, std::string_view b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diag = up;
        }
    }
    return row[b.size()];
}
template <class Names>
std::string nearest(const std::string& word, const Names& names) {
    std::string best;
    size_t bestDistance = std::max<size_t>(2, word.size() / 4) + 1;
    for (const auto& candidate : names) {
        const std::string& name = candidate;
        if (name.empty() || name == word) continue;
        const size_t d = distance(word, name);
        if (d < bestDistance || (d == bestDistance && !best.empty() && name < best)) {
            if (d < bestDistance) bestDistance = d;
            best = name;
        }
    }
    return best;
}

std::string inQuotes(std::string_view s) { return "'" + std::string(s) + "'"; }

} // namespace

// ---- kinds -------------------------------------------------------------------------

bool aiEngineFile(const std::string& key) {
    return tak::hpi::MountSet::key(key) == tak::ai::kEngineProfileFile;
}
AiKind aiKindOf(const std::string& key) {
    return aiEngineFile(key) ? AiKind::Level : AiKind::Default;
}
const char* aiLevelName(Difficulty d) {
    switch (d) {
        case Difficulty::Defensive: return "Defensive";
        case Difficulty::Easy: return "Easy";
        case Difficulty::Passive: return "Passive";
        case Difficulty::Normal: return "Normal";
        case Difficulty::Hard: return "Hard";
        case Difficulty::Absurd: return "Absurd";
        case Difficulty::AbsurdlyDefensive: return "Absurdly Defensive";
        default: return "DEFAULT AI";
    }
}

bool AiDraft::empty() const {
    if (asWritten) return original.empty();
    return groups.empty() && kept.empty() &&
           std::all_of(units.begin(), units.end(), [&](const auto& u) {
               return kind == AiKind::Default ? !u.second.chance && !u.second.limit : !u.second.weight && !u.second.limit;
           });
}

std::string classicChanceText(int p) {
    if (p >= 100) return "1";
    if (p <= 0) return "0";
    const int v = p * 10 + 5;   // 15 .. 995
    std::string out = "0.";
    out.push_back(char('0' + v / 100));
    out.push_back(char('0' + v / 10 % 10));
    out.push_back(char('0' + v % 10));
    return out;
}

bool classicCanSingleOut(const ClassicCatalogue& catalogue, const std::string& name) {
    const auto it = catalogue.byName.find(lowered(name));
    return it != catalogue.byName.end() && it->second != 0;
}

// ---- attribution ---------------------------------------------------------------------

std::vector<AiLine> attributeAiLines(std::string_view text, AiKind kind, const ClassicCatalogue& catalogue) {
    std::vector<AiLine> out;
    const auto lines = splitLines(text);
    out.reserve(lines.size());
    if (kind == AiKind::Default) {
        std::vector<int> prefLock(catalogue.size()), limitLock(catalogue.size());
        std::vector<std::vector<int>> scaled(catalogue.size());
        for (size_t n = 0; n < lines.size(); ++n) {
            AiLine line;
            line.number = int(n + 1);
            line.text = lines[n];
            const auto t = classicTokens(lines[n]);
            if (t.empty()) {
                line.what = blankLine(lines[n]) ? AiLine::What::Blank : AiLine::What::Comment;
                out.push_back(std::move(line));
                continue;
            }
            line.command = std::string(t[0]);
            if (t[0].substr(0, 2) == "//") { line.what = AiLine::What::Comment; out.push_back(std::move(line)); continue; }
            const std::string command = lowered(t[0]);
            if (t.size() > 1) line.word = lowered(t[1]);
            if (t.size() > 2) { line.value = std::string(t[2]); line.hasValue = true; }
            if (command != "weight" && command != "limit") { line.what = AiLine::What::Other; out.push_back(std::move(line)); continue; }
            line.weight = command == "weight";
            const auto it = catalogue.byName.find(line.word);
            if (it != catalogue.byName.end() && it->second != 0) {
                line.unit = it->second;
                auto& lock = line.weight ? prefLock : limitLock;
                if (lock[line.unit]) {
                    line.what = AiLine::What::Shadowed;
                    line.by = lock[line.unit];
                } else {
                    line.what = AiLine::What::Exact;
                    lock[line.unit] = line.number;
                    if (line.weight) line.combines = scaled[line.unit];
                }
            } else if (auto ct = catalogue.categories.find(line.word); ct != catalogue.categories.end()) {
                line.what = AiLine::What::Group;
                if (line.weight)
                    for (uint16_t i : ct->second)
                        if (scaled[i].empty() || scaled[i].back() != line.number)   // a locked unit never reads it
                            scaled[i].push_back(line.number);
            } else {
                line.what = AiLine::What::Unknown;
            }
            out.push_back(std::move(line));
        }
        return out;
    }
    // Levels: ai::applyProfileText's reads, line by line.
    std::map<std::pair<bool, std::string>, size_t> last;
    for (size_t n = 0; n < lines.size(); ++n) {
        AiLine line;
        line.number = int(n + 1);
        line.text = lines[n];
        const std::string s(lines[n]);
        if (blankLine(s)) { out.push_back(std::move(line)); continue; }
        if (s.size() < 2 || s[0] == '/') { line.what = AiLine::What::Comment; out.push_back(std::move(line)); continue; }
        std::istringstream ss(s);
        std::string kw, unit;
        int v = 0;
        ss >> kw;
        line.command = kw;
        {
            std::istringstream tokens(s);
            std::string a, b, c;
            tokens >> a >> b;
            line.word = b;
            std::transform(line.word.begin(), line.word.end(), line.word.begin(), ::tolower);
            if (tokens >> c) { line.value = c; line.hasValue = true; }
        }
        const bool keyword = kw == "weight" || kw == "limit";
        line.weight = kw == "weight";
        if (!(ss >> unit) || !(ss >> v)) {
            line.what = keyword ? AiLine::What::Skipped : AiLine::What::Other;
            out.push_back(std::move(line));
            continue;
        }
        if (!keyword) { line.what = AiLine::What::Other; out.push_back(std::move(line)); continue; }
        line.what = AiLine::What::Exact;
        line.levelValue = v;
        last[{line.weight, line.word}] = out.size();
        out.push_back(std::move(line));
    }
    for (size_t i = 0; i < out.size(); ++i) {
        auto& line = out[i];
        if (line.what != AiLine::What::Exact) continue;
        const size_t winner = last.at({line.weight, line.word});
        if (winner != i) { line.what = AiLine::What::Overridden; line.by = out[winner].number; }
    }
    return out;
}

// ---- canonical form -------------------------------------------------------------------

std::string writeAiDraft(const AiDraft& draft) {
    if (draft.asWritten) return draft.original;
    if (draft.empty()) return {};
    std::string out(kAiProfileMagic);
    out += '\n';
    const auto header = sanitizedHeader(draft.header);
    const auto note = draft.kind == AiKind::Default ? kDefaultNote : kLevelNote;
    if (!header.empty() && "// " + header != note && "// " + header != kGroupMarker && "// " + header != kKeptMarker)
        out += "// " + header + "\n";
    out += note;
    out += '\n';
    for (const auto& [key, rule] : draft.units) {
        if (draft.kind == AiKind::Default) {
            if (rule.chance) out += "weight " + key + " " + classicChanceText(*rule.chance) + "\n";
        } else if (rule.weight) {
            out += "weight " + key + " " + std::to_string(*rule.weight) + "\n";
        }
        if (rule.limit) out += "limit " + key + " " + std::to_string(*rule.limit) + "\n";
    }
    if (draft.kind == AiKind::Default && !draft.groups.empty()) {
        out += kGroupMarker;
        out += '\n';
        for (const auto& g : draft.groups) out += groupLine(g) + "\n";
    }
    if (!draft.kept.empty()) {
        out += kKeptMarker;
        out += '\n';
        for (const auto& line : draft.kept) out += line + "\n";
    }
    return out;
}

bool verifyAiDraft(const AiDraft& draft, std::string_view bytes, const ClassicCatalogue& catalogue,
                   std::string* mismatch) {
    auto fail = [&](std::string what) { if (mismatch) *mismatch = std::move(what); return false; };
    if (draft.asWritten) return bytes == draft.original ? true : fail("the file's bytes");
    if (draft.kind == AiKind::Default) {
        tak::ai::ClassicProfile actual(catalogue.size()), groups(catalogue.size());
        actual.execute(bytes, catalogue);
        std::string groupText;
        for (const auto& g : draft.groups) groupText += groupLine(g) + "\n";
        groups.execute(groupText, catalogue);
        std::vector<const AiUnitRule*> rows(catalogue.size(), nullptr);
        for (const auto& [key, rule] : draft.units) {
            if (!rule.chance && !rule.limit) continue;
            const auto it = catalogue.byName.find(key);
            if (it == catalogue.byName.end() || it->second == 0 || key != lowered(key))
                return fail(key + " (no such unit for the DEFAULT AI)");
            rows[it->second] = &rule;
        }
        for (size_t i = 0; i < catalogue.size(); ++i) {
            const auto* row = rows[i];
            const int pref = row && row->chance ? std::clamp(*row->chance, 0, 100) : groups.pref[i];
            const int limit = row && row->limit ? *row->limit : groups.limit[i];
            if (actual.pref[i] != pref || actual.limit[i] != limit)
                return fail(catalogue.names[i] + " (chance " + std::to_string(actual.pref[i]) + " %, limit " +
                            std::to_string(actual.limit[i]) + "; meant " + std::to_string(pref) + " %, limit " +
                            std::to_string(limit) + ")");
        }
        return true;
    }
    tak::ai::Profile actual, expected;
    tak::ai::applyProfileText(bytes, actual);
    for (const auto& [key, rule] : draft.units) {
        if (rule.weight) expected.weight[key] = *rule.weight;
        if (rule.limit) expected.limit[key] = *rule.limit;
    }
    for (const auto* table : {&expected.weight, &expected.limit}) {
        const auto& other = table == &expected.weight ? actual.weight : actual.limit;
        const char* what = table == &expected.weight ? "weight" : "limit";
        for (const auto& [key, v] : *table) {
            const auto it = other.find(key);
            if (it == other.end() || it->second != v)
                return fail(key + " (" + what + " " + (it == other.end() ? std::string("not set") : std::to_string(it->second)) +
                            "; meant " + std::to_string(v) + ")");
        }
        for (const auto& [key, v] : other)
            if (!table->count(key)) return fail(key + " (" + what + " " + std::to_string(v) + "; meant not set)");
    }
    return true;
}

bool saveAiDraft(const AiDraft& draft, const ClassicCatalogue& catalogue, std::string& bytes, std::string& error) {
    bytes = writeAiDraft(draft);
    if (bytes.size() > kAiFileMaxBytes) { error = "AI profile is over 256 KiB"; return false; }
    std::string unit;
    if (!verifyAiDraft(draft, bytes, catalogue, &unit)) {
        error = "AI profile could not be written exactly: " + unit;
        return false;
    }
    return true;
}

AiDraft parseAiDraft(std::string_view text, AiKind kind, const ClassicCatalogue& catalogue) {
    AiDraft asWritten;
    asWritten.kind = kind;
    asWritten.asWritten = true;
    asWritten.original = std::string(text);
    if (text.empty()) { asWritten.asWritten = false; asWritten.original.clear(); return asWritten; }
    const auto lines = splitLines(text);
    if (lines.size() < 2 || lines[0] != kAiProfileMagic || text.back() != '\n') return asWritten;
    AiDraft draft;
    draft.kind = kind;
    const auto note = kind == AiKind::Default ? kDefaultNote : kLevelNote;
    size_t n = 1;
    if (lines[n] != note) {
        if (!lines[n].starts_with("// ")) return asWritten;
        draft.header = std::string(lines[n].substr(3));
        ++n;
    }
    if (n >= lines.size() || lines[n] != note) return asWritten;
    ++n;
    enum { Rows, Groups, Kept } section = Rows;
    for (; n < lines.size(); ++n) {
        const auto line = lines[n];
        if (section == Kept) { draft.kept.emplace_back(line); continue; }
        if (line == kKeptMarker) { section = Kept; continue; }
        if (kind == AiKind::Default && section == Rows && line == kGroupMarker) { section = Groups; continue; }
        const auto parts = spaceParts(line);
        if (parts.size() < 2 || parts.size() > 3 || (parts[0] != "weight" && parts[0] != "limit")) return asWritten;
        const bool weight = parts[0] == "weight";
        const std::string word(parts[1]);
        if (word.empty() || word != lowered(word)) return asWritten;
        if (section == Groups) {
            if (classicCanSingleOut(catalogue, word) || !catalogue.categories.count(word)) return asWritten;
            draft.groups.push_back({weight, word, parts.size() > 2 ? std::string(parts[2]) : std::string()});
            continue;
        }
        if (parts.size() != 3) return asWritten;
        auto& rule = draft.units[word];
        if (kind == AiKind::Default) {
            if (!classicCanSingleOut(catalogue, word)) return asWritten;
            if (weight) {
                if (rule.chance) return asWritten;
                rule.chance = tak::ai::classicScaledPref(100, tak::ai::classicAtof(parts[2]));
            } else {
                if (rule.limit) return asWritten;
                rule.limit = tak::ai::classicAtoi(parts[2]);
            }
        } else {
            int v = 0;
            if (!wholeNumber(parts[2], v) || (weight ? rule.weight : rule.limit)) return asWritten;
            (weight ? rule.weight : rule.limit) = v;
        }
    }
    if (writeAiDraft(draft) != text || !verifyAiDraft(draft, text, catalogue)) return asWritten;
    return draft;
}

bool makeEditable(AiDraft& draft, const ClassicCatalogue& catalogue, std::string* why) {
    auto refuse = [&](std::string reason) { if (why) *why = std::move(reason); return false; };
    if (!draft.asWritten) return true;
    if (draft.original.size() > kAiFileMaxBytes) return refuse("the file is over 256 KiB");
    AiDraft out;
    out.kind = draft.kind;
    out.header = draft.header;
    const auto lines = attributeAiLines(draft.original, draft.kind, catalogue);
    // A file that was once ours keeps its header, not our own comment lines.
    size_t first = 0;
    if (!lines.empty() && withoutCr(lines[0].text) == kAiProfileMagic) {
        first = 1;
        if (lines.size() > 1) {
            const auto second = withoutCr(lines[1].text);
            if (second.starts_with("// ") && second != kDefaultNote && second != kLevelNote &&
                second != kGroupMarker && second != kKeptMarker) { out.header = second.substr(3); first = 2; }
        }
    }
    for (size_t n = first; n < lines.size(); ++n) {
        const auto& line = lines[n];
        if (const auto text = withoutCr(line.text);
            text == kDefaultNote || text == kLevelNote || text == kGroupMarker || text == kKeptMarker) continue;
        switch (line.what) {
            case AiLine::What::Blank: case AiLine::What::Shadowed: case AiLine::What::Overridden: break;
            case AiLine::What::Comment: case AiLine::What::Other: case AiLine::What::Unknown: case AiLine::What::Skipped:
                out.kept.push_back(withoutCr(line.text));
                break;
            case AiLine::What::Group:
                out.groups.push_back({line.weight, line.word, line.value});
                break;
            case AiLine::What::Exact: {
                auto& rule = out.units[draft.kind == AiKind::Default ? catalogue.names[line.unit] : line.word];
                if (draft.kind == AiKind::Level) (line.weight ? rule.weight : rule.limit) = line.levelValue;
                else (line.weight ? rule.chance : rule.limit) = 0;   // filled from the engine below
                break;
            }
        }
    }
    std::string bytes = writeAiDraft(out);
    if (draft.kind == AiKind::Default) {
        tak::ai::ClassicProfile original(catalogue.size());
        original.execute(draft.original, catalogue);
        for (auto& [key, rule] : out.units) {
            const auto i = catalogue.byName.at(key);
            if (rule.chance) rule.chance = original.pref[i];
            if (rule.limit) rule.limit = original.limit[i];
        }
        bytes = writeAiDraft(out);
        tak::ai::ClassicProfile converted(catalogue.size());
        converted.execute(bytes, catalogue);
        for (size_t i = 0; i < catalogue.size(); ++i)
            if (converted.pref[i] != original.pref[i] || converted.limit[i] != original.limit[i])
                return refuse("converting would change " + catalogue.names[i]);
    } else {
        tak::ai::Profile original, converted;
        tak::ai::applyProfileText(draft.original, original);
        tak::ai::applyProfileText(bytes, converted);
        if (original.weight != converted.weight || original.limit != converted.limit)
            return refuse("converting would change a unit's weight or limit");
    }
    if (bytes.size() > kAiFileMaxBytes) return refuse("the converted file is over 256 KiB");
    std::string unit;
    if (!verifyAiDraft(out, bytes, catalogue, &unit)) return refuse("could not be written exactly: " + unit);
    draft = std::move(out);
    return true;
}

std::string aiFileWithHeader(std::string_view bytes, AiKind kind, const std::string& header,
                             const ClassicCatalogue& catalogue) {
    auto draft = parseAiDraft(bytes, kind, catalogue);
    if (draft.asWritten || draft.empty()) return std::string(bytes);
    draft.header = header;
    std::string out, error;
    if (!saveAiDraft(draft, catalogue, out, error)) return std::string(bytes);
    return out;
}

std::string aiModDefaultHeader(const std::string& pack, const std::string& mapName, const std::string& from) {
    return "DEFAULT AI profile for game mod \"" + pack + "\" (ai/default.txt), saved from map \"" + mapName + "\" (" + from +
           "). Edit in Cartographer: SCENARIO > AI Profiles.";
}

// ---- checks ---------------------------------------------------------------------------

namespace {

// What one side's builders can reach from its Monarch (TypeRegistry::buildable,
// which also covers the Zhon's mobile builders).
struct SideRoster { std::string side; std::vector<const tak::sim::UnitType*> units; };
std::vector<SideRoster> sideRosters(const tak::sim::TypeRegistry& registry) {
    std::map<std::string, std::set<std::string>> reach;
    for (const auto& [id, type] : registry.types()) {
        if (!type.commander || type.side.empty()) continue;
        auto& seen = reach[lowered(type.side)];
        std::vector<std::string> open{id};
        seen.insert(id);
        while (!open.empty()) {
            const auto at = open.back();
            open.pop_back();
            for (const auto& next : registry.buildable(at))
                if (seen.insert(next).second) open.push_back(next);
        }
    }
    std::vector<SideRoster> out;
    for (const auto& [side, ids] : reach) {
        SideRoster r{side, {}};
        for (const auto& id : ids)
            if (const auto* t = registry.find(id)) r.units.push_back(t);
        out.push_back(std::move(r));
    }
    return out;
}
std::string classicName(const tak::sim::UnitType& t) {
    return lowered(t.classicFbi.unitName.empty() ? t.id : t.classicFbi.unitName);
}
std::string unitLabel(const tak::sim::UnitType& t) { return t.name.empty() ? t.id : t.name; }

} // namespace

std::set<std::string> aiReachableUnits(const tak::sim::TypeRegistry& registry) {
    std::set<std::string> out;
    for (const auto& side : sideRosters(registry))
        for (const auto* t : side.units)
            if (!t->commander) out.insert(t->id);
    return out;
}

std::vector<AiIssue> checkAiFile(const std::string& key, std::string_view bytes, const AiRoster& roster) {
    using S = AiIssue::Severity;
    std::vector<AiIssue> out;
    const std::string file = tak::hpi::MountSet::key(key);
    auto issue = [&](S s, int line, std::string unit, std::string message) {
        out.push_back({s, file, line, std::move(unit), std::move(message)});
    };
    if (bytes.size() > kAiFileMaxBytes) {
        issue(S::Error, 0, {}, file + " is over 256 KiB");
        return out;
    }
    static const ClassicCatalogue kNone;
    const ClassicCatalogue& catalogue = roster.catalogue ? *roster.catalogue : kNone;
    const bool engine = aiEngineFile(file);
    const AiKind kind = engine ? AiKind::Level : AiKind::Default;
    const auto lines = attributeAiLines(bytes, kind, catalogue);
    auto at = [&](const AiLine& l) { return "line " + std::to_string(l.number); };
    std::set<std::string> levelUnits;
    if (roster.registry)
        for (const auto& [id, type] : roster.registry->types()) levelUnits.insert(id);
    std::vector<std::string> classicWords;
    for (size_t i = 1; i < catalogue.size(); ++i) classicWords.push_back(catalogue.names[i]);
    for (const auto& [word, members] : catalogue.categories) classicWords.push_back(word);

    std::optional<tak::ai::ClassicProfile> classic;
    if (kind == AiKind::Default) { classic.emplace(catalogue.size()); classic->execute(bytes, catalogue); }

    for (const auto& l : lines) {
        if (kind == AiKind::Default) {
            switch (l.what) {
                case AiLine::What::Other:
                    issue(S::Info, l.number, {}, at(l) + ": " + inQuotes(l.command) + " does nothing");
                    break;
                case AiLine::What::Unknown: {
                    if (l.word.empty()) { issue(S::Warning, l.number, {}, at(l) + ": names nothing, so the line does nothing"); break; }
                    std::string message = at(l) + ": " + inQuotes(l.word) + " matches nothing, so the line does nothing.";
                    if (auto it = catalogue.byName.find(l.word); it != catalogue.byName.end() && it->second == 0)
                        message = at(l) + ": the DEFAULT AI cannot single out " + l.word + "; this line does nothing";
                    else if (const auto guess = nearest(l.word, classicWords); !guess.empty())
                        message += " Did you mean " + inQuotes(guess) + "?";
                    issue(S::Warning, l.number, l.word, message);
                    break;
                }
                case AiLine::What::Shadowed:
                    issue(S::Warning, l.number, l.word,
                          at(l) + " is ignored: line " + std::to_string(l.by) + " already set this unit");
                    break;
                case AiLine::What::Group:
                    if (auto it = catalogue.byName.find(l.word); it != catalogue.byName.end() && it->second == 0)
                        issue(S::Warning, l.number, l.word, at(l) + ": the DEFAULT AI cannot single out " + l.word +
                              "; the line applies to the group " + inQuotes(l.word) + " instead");
                    if (!l.hasValue)
                        issue(S::Warning, l.number, {}, at(l) + ": no value: reads as 0 for every unit " +
                              inQuotes(l.word) + " covers (never built)");
                    break;
                case AiLine::What::Exact:
                    if (!l.hasValue)
                        issue(S::Warning, l.number, l.word, at(l) + ": no value: reads as 0 and locks the unit (never built)");
                    else if (l.weight && !l.combines.empty()) {
                        const auto& first = lines[size_t(l.combines.back() - 1)];
                        issue(S::Info, l.number, l.word, at(l) + " combines with line " + std::to_string(first.number) +
                              " (" + inQuotes(withoutCr(first.text)) + "): result " +
                              std::to_string(classic->pref[l.unit]) + " %");
                    }
                    break;
                default: break;
            }
            continue;
        }
        // Levels.
        switch (l.what) {
            case AiLine::What::Other:
                if (const auto c = lowered(l.command); c == "weight" || c == "limit")
                    issue(S::Warning, l.number, l.word, at(l) + ": levels need lowercase " + inQuotes(c) + "; this line does nothing");
                else
                    issue(S::Info, l.number, {}, at(l) + ": " + inQuotes(l.command) + " does nothing");
                break;
            case AiLine::What::Skipped:
                if (l.word.empty() || !l.hasValue)
                    issue(S::Warning, l.number, l.word, at(l) + ": no value; the line is skipped");
                else if (l.value[0] == '#')
                    issue(S::Warning, l.number, l.word, at(l) + ": '#' is not a comment for levels; this line is skipped");
                else
                    issue(S::Warning, l.number, l.word, at(l) + ": " + inQuotes(l.value) + " is not a whole number; the line is skipped");
                break;
            case AiLine::What::Overridden:
                issue(S::Warning, l.number, l.word, at(l) + " is replaced by line " + std::to_string(l.by) + " (later line wins)");
                [[fallthrough]];
            case AiLine::What::Exact: {
                int whole = 0;
                if (!wholeNumber(l.value, whole) && !(l.value.starts_with('+') && wholeNumber(l.value.substr(1), whole)))
                    issue(S::Warning, l.number, l.word, at(l) + ": " + inQuotes(l.value) + " reads as " + std::to_string(l.levelValue) +
                          (l.levelValue <= 0 && (l.weight || l.levelValue == 0) ? ": never built" : ""));
                if (roster.registry && !levelUnits.count(l.word)) {
                    if (l.word == "all" || catalogue.categories.count(l.word))
                        issue(S::Warning, l.number, l.word, at(l) + ": levels read unit names only; this line does nothing");
                    else {
                        std::string message = at(l) + ": " + inQuotes(l.word) + " matches nothing, so the line does nothing.";
                        if (const auto guess = nearest(l.word, levelUnits); !guess.empty())
                            message += " Did you mean " + inQuotes(guess) + "?";
                        issue(S::Warning, l.number, l.word, message);
                    }
                }
                break;
            }
            default: break;
        }
    }
    if (!roster.registry) return out;

    // Sides: what this file switches off, by the engine's own reading -- for
    // ai/takengine.txt in each level (over its own table), the levels a finding holds
    // for named once.
    std::vector<std::pair<Difficulty, tak::ai::Profile>> profiles;
    if (engine)
        for (Difficulty d : kLevels) {
            profiles.emplace_back(d, tak::ai::builtinProfile(d));
            tak::ai::applyProfileText(bytes, profiles.back().second);
        }
    auto off = [&](const tak::sim::UnitType& t, const tak::ai::Profile* profile) {
        if (profile) {
            const auto w = profile->weight.find(t.id);
            const auto l = profile->limit.find(t.id);
            return w == profile->weight.end() || w->second <= 0 || (l != profile->limit.end() && l->second == 0);
        }
        const auto it = catalogue.byName.find(classicName(t));
        if (it == catalogue.byName.end()) return false;
        return classic->pref[it->second] == 0 || classic->limit[it->second] == 0;
    };
    const auto sides = sideRosters(*roster.registry);
    std::set<const tak::sim::UnitType*> reachable;
    for (const auto& side : sides)
        for (const auto* t : side.units) reachable.insert(t);
    std::vector<std::pair<std::string, std::vector<Difficulty>>> sideFindings;   // message, levels (in order)
    const size_t passes = engine ? profiles.size() : 1;
    for (size_t p = 0; p < passes; ++p) {
        const tak::ai::Profile* profile = engine ? &profiles[p].second : nullptr;
        for (const auto& side : sides) {
            bool anything = false, anyBuilder = false, builderOn = false, anyLode = false, lodeOn = false;
            for (const auto* t : side.units) {
                if (t->commander) continue;
                anything = anything || !off(*t, profile);
                if (t->isBuilder) { anyBuilder = true; builderOn = builderOn || !off(*t, profile); }
                if (t->onMana) { anyLode = true; lodeOn = lodeOn || !off(*t, profile); }
            }
            std::string sideName = side.side;
            for (char& c : sideName) c = char(std::toupper(static_cast<unsigned char>(c)));
            std::string message;
            if (!anything && side.units.size() > 1) message = "this AI cannot build anything for " + sideName;
            else if ((anyBuilder && !builderOn) || (anyLode && !lodeOn))
                message = "this AI cannot grow its economy for " + sideName + ": every " +
                          (anyBuilder && !builderOn ? "builder" : "lodestone") + " is off";
            if (message.empty()) continue;
            auto it = std::find_if(sideFindings.begin(), sideFindings.end(), [&](const auto& f) { return f.first == message; });
            if (it == sideFindings.end()) { sideFindings.push_back({message, {}}); it = sideFindings.end() - 1; }
            if (engine) it->second.push_back(profiles[p].first);
        }
    }
    for (const auto& [message, levels] : sideFindings) {
        std::string where;
        if (engine && levels.size() == profiles.size()) where = " (every level)";
        else if (engine) {
            for (size_t i = 0; i < levels.size(); ++i)
                where += std::string(i ? i + 1 == levels.size() ? " and " : ", " : " (") + aiLevelName(levels[i]);
            where += ")";
        }
        issue(S::Error, 0, {}, message + where);
    }
    std::set<std::string> named, unreachable;
    for (const auto& l : lines) {
        if (l.what != AiLine::What::Exact && l.what != AiLine::What::Overridden) continue;
        const tak::sim::UnitType* t = nullptr;
        if (kind == AiKind::Default) t = l.unit < catalogue.types.size() ? catalogue.types[l.unit] : nullptr;
        else t = roster.registry->find(l.word);
        if (kind == AiKind::Level && l.weight) named.insert(l.word);
        if (t && !t->commander && !reachable.count(t) && unreachable.insert(l.word).second)
            issue(S::Info, l.number, l.word, at(l) + ": no builder of its side can make " + unitLabel(*t));
    }
    if (engine) {   // a unit no level's built-in table names, and the file does not either
        std::set<std::string> reported;
        auto anyBuiltin = [&](const std::string& id) {
            return std::any_of(std::begin(kLevels), std::end(kLevels),
                               [&](Difficulty d) { return tak::ai::builtinProfile(d).weight.count(id) > 0; });
        };
        for (const auto& side : sides)
            for (const auto* t : side.units)
                if (!t->commander && !named.count(t->id) && !anyBuiltin(t->id) && reported.insert(t->id).second)
                    issue(S::Warning, 0, t->id, unitLabel(*t) + " (" + t->id + ") is never built by our levels (not named)");
    }
    return out;
}

std::vector<AiIssue> checkAiMap(const AiFiles& files, const std::string& aiProfile, const tak::hpi::Vfs& base,
                                const AiRoster& roster) {
    using S = AiIssue::Severity;
    std::vector<AiIssue> out;
    const std::string own = aiProfileFileKey(aiProfile);
    if (!aiProfile.empty() && !standardAiProfile(aiProfile)) {
        if (aiProfile.find_first_of("/\\") != std::string::npos)
            out.push_back({S::Error, {}, 0, {}, "aiprofile=" + aiProfile + " names a file in another folder"});
        else if (aiEngineFile(own))
            out.push_back({S::Error, {}, 0, {}, "aiprofile=" + aiProfile + " makes the DEFAULT AI read " + own +
                           ", the file our levels read"});
        else {
            if (aiProfile.find('.') != std::string::npos)
                out.push_back({S::Warning, {}, 0, {}, "aiprofile=" + aiProfile + " reads " + own +
                               ": the part after the last '.' is dropped"});
            const auto travel = travellingAiFiles(files, aiProfile);
            const auto script = tak::ai::classicProfileScript(base, aiProfile, nullptr, &travel);
            if (tak::hpi::MountSet::key(script.path) != own)
                out.push_back({S::Warning, {}, 0, {}, "no " + own + " in this map or the install; the AI falls back to ai/default.txt"});
        }
    }
    for (const auto& [key, bytes] : files) {
        if (!aiFileTravels(key, aiProfile))
            out.push_back({S::Warning, key, 0, {}, key + " is kept in the .kmp but not sent to other players or into Test Map"});
        else if (key == "ai/default.txt")
            out.push_back({S::Info, key, 0, {}, "every map without its own profile name also reads ai/default.txt, "
                           "and an installed ai/default.txt may be used instead"});
        for (auto& issue : checkAiFile(key, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), roster))
            out.push_back(std::move(issue));
    }
    return out;
}

// ---- names and files ------------------------------------------------------------------

std::string aiProfileFileKey(const std::string& name) {
    std::string stem = name;
    // ai::classicProfileScript's rule: drop the extension after the last '.'.
    if (const auto dot = stem.find_last_of('.');
        dot != std::string::npos && stem.find_first_of("/\\", dot) == std::string::npos)
        stem.erase(dot);
    if (stem.empty()) return {};
    return tak::hpi::MountSet::key("ai/" + stem + ".txt");
}
bool standardAiProfile(const std::string& name) {
    const auto key = aiProfileFileKey(name);
    return key.empty() || key == "ai/default.txt";
}

std::string proposeAiProfileName(std::string_view mapName) {
    std::string out;
    for (char c : mapName) {
        c = lowerAscii(c);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-') out.push_back(c);
    }
    auto trim = [&] {
        while (!out.empty() && out.front() == ' ') out.erase(out.begin());
        while (!out.empty() && out.back() == ' ') out.pop_back();
    };
    trim();
    if (out.size() > 32) { out.resize(32); trim(); }
    if (out.empty()) out = "map";
    if (!aiProfileNameError(out).empty()) out = out.substr(0, 28) + "-map";
    return out;
}

std::string aiProfileNameError(const std::string& name) {
    if (name.empty()) return "Type a profile name.";
    if (name.find_first_of("./\\") != std::string::npos) return "Profile names cannot contain '.', '/' or '\\'.";
    if (name.size() > 32) return "Profile names are at most 32 characters.";
    const auto key = aiProfileFileKey(name);
    if (key == "ai/default.txt") return "'default' is the standard profile; choose another name.";
    if (aiEngineFile(key)) return inQuotes(name) + " is the file our AI levels read; choose another name.";
    if (!validDocumentName(name) || !aiFileKey(key) || name.front() == ' ')
        return "Choose a name without reserved file name characters.";
    return {};
}

bool aiFileTravels(const std::string& key, const std::string& aiProfile) {
    const auto k = tak::hpi::MountSet::key(key);
    if (k == "ai/default.txt" || aiEngineFile(k)) return true;
    // net::maps' whitelist takes ai/<aiprofile>.txt when it is a safe single name.
    const auto own = aiProfileFileKey(aiProfile);
    return !own.empty() && own == k && own.find('/', 3) == std::string::npos && own.find(':') == std::string::npos &&
           std::none_of(own.begin(), own.end(), [](char c) { return uint8_t(c) < 32 || c == 127; });
}
AiFiles travellingAiFiles(const AiFiles& files, const std::string& aiProfile) {
    AiFiles out;
    for (const auto& [key, bytes] : files)
        if (!bytes.empty() && aiFileTravels(key, aiProfile)) out.emplace(key, bytes);
    return out;
}

void setAiFile(AiFiles& files, const std::string& key, std::string_view bytes) {
    const auto k = tak::hpi::MountSet::key(key);
    if (bytes.empty()) files.erase(k);
    else files[k].assign(bytes.begin(), bytes.end());
}

bool renameAiProfile(AiFiles& files, const std::string& oldName, const std::string& newName) {
    const auto from = aiProfileFileKey(oldName), to = aiProfileFileKey(newName);
    if (standardAiProfile(oldName) || standardAiProfile(newName) || aiKindOf(from) == AiKind::Level ||
        aiKindOf(to) == AiKind::Level)
        return false;
    if (from == to) return true;
    const auto it = files.find(from);
    if (it == files.end()) return true;   // nothing to move
    auto bytes = std::move(it->second);
    files.erase(it);
    files[to] = std::move(bytes);
    return true;
}

// ---- which file plays -----------------------------------------------------------------

AiFileChoice whichFile(Difficulty d, const tak::hpi::Vfs& base, const AiFiles& mapFiles, const std::string& aiProfile,
                       const tak::hpi::Vfs::Files* gameModFiles) {
    AiFileChoice out;
    const auto travel = travellingAiFiles(mapFiles, aiProfile);
    if (d == Difficulty::Classic) {
        tak::ai::ClassicProfileLayer layer = tak::ai::ClassicProfileLayer::None;
        const auto script = tak::ai::classicProfileScript(base, aiProfile, gameModFiles, &travel, &layer);
        out.path = tak::hpi::MountSet::key(script.path);
        const std::string own = aiProfileFileKey(aiProfile);
        const bool standard = standardAiProfile(aiProfile);
        switch (layer) {
            case tak::ai::ClassicProfileLayer::Map:
                out.from = AiFileChoice::From::Map;
                out.message = "DEFAULT AI on this map reads " + out.path + " from this map.";
                if (!gameModFiles)
                    out.message += " A ticked game mod's " + out.path + " would replace it" +
                                   (out.path != "ai/default.txt" ? "; a game mod's ai/default.txt would not." : ".");
                break;
            case tak::ai::ClassicProfileLayer::GameMods:
                out.from = AiFileChoice::From::GameMod;
                out.message = "DEFAULT AI reads " + out.path + " from the game mod" +
                              (travel.count(out.path) ? ", which replaces this map's." : ".");
                break;
            case tak::ai::ClassicProfileLayer::Base:
                out.from = AiFileChoice::From::Base;
                if (standard) out.message = "DEFAULT AI reads the installed " + out.path + " (this map has no profile name).";
                else if (out.path != own)
                    out.message = "DEFAULT AI reads the installed " + out.path + " (no " + own + " in this map or the install).";
                else out.message = "DEFAULT AI reads the installed " + out.path + ".";
                break;
            default:
                out.from = AiFileChoice::From::None;
                out.message = "DEFAULT AI: no profile file, so every unit is built at full chance with no limit.";
                break;
        }
        return out;
    }
    std::string source;
    bool fromMap = false;
    (void)tak::ai::levelProfile(d, gameModFiles, &travel, &source, &fromMap);
    const std::string name = aiLevelName(d);
    out.path = source;
    if (source.empty()) {
        out.message = "Our six levels: built-in tables (no " + std::string(tak::ai::kEngineProfileFile) + ").";
    } else if (fromMap) {
        out.from = AiFileChoice::From::Map;
        out.message = "All six levels (" + name + " shown) read " + source + " from this map, each over its own built-in table" +
                      (gameModFiles ? std::string(".")
                                    : ", unless a ticked game mod has " + source + ", which then replaces it entirely.");
    } else {
        out.from = AiFileChoice::From::GameMod;
        out.message = "All six levels (" + name + " shown) read " + source + " from the game mod, each over its own built-in table" +
                      (travel.count(source) ? "; it replaces this map's file entirely." : ".");
    }
    return out;
}

std::string compareAiFiles(const std::string& key, std::string_view before, std::string_view after,
                           const ClassicCatalogue& catalogue) {
    auto split=[](std::string_view text) {
        std::vector<std::string_view> lines;
        while(!text.empty()) {
            const auto end=text.find('\n');auto line=text.substr(0,end);
            if(!line.empty() && line.back()=='\r')line.remove_suffix(1);
            lines.push_back(line);
            if(end==std::string_view::npos)break;
            text.remove_prefix(end+1);
        }
        return lines;
    };
    auto quoted=[](std::string_view line) {
        std::string out(line.substr(0,56));if(line.size()>56)out+="...";
        return "\""+out+"\"";
    };
    const auto a=split(before),b=split(after);
    std::string out=key+": "+std::to_string(a.size())+" lines in the pack, "+std::to_string(b.size())+" to save";
    for(size_t i=0;i<std::max(a.size(),b.size());++i) {
        if(i<a.size() && i<b.size() && a[i]==b[i])continue;
        out+="; line "+std::to_string(i+1)+" "+(i<a.size()?quoted(a[i]):std::string("(none)"))+" becomes "+(i<b.size()?quoted(b[i]):std::string("(none)"));
        break;
    }
    std::vector<std::string> changed;
    if(aiKindOf(key)==AiKind::Level) {   // over each level's own table; a unit counts once
        std::set<std::string> units,differ;
        auto get=[](const std::unordered_map<std::string,int>& m,const std::string& unit) {
            const auto it=m.find(unit);return it==m.end()?std::optional<int>():std::optional<int>(it->second);
        };
        for(Difficulty d:kLevels) {
            auto x=tak::ai::builtinProfile(d),y=x;
            tak::ai::applyProfileText(before,x);tak::ai::applyProfileText(after,y);
            for(const auto* p:{&x,&y})for(const auto* m:{&p->weight,&p->limit})for(const auto& [unit,v]:*m)units.insert(unit);
            for(const auto& unit:units)
                if(get(x.weight,unit)!=get(y.weight,unit) || get(x.limit,unit)!=get(y.limit,unit))differ.insert(unit);
        }
        changed.assign(differ.begin(),differ.end());
    } else {
        tak::ai::ClassicProfile x(catalogue.size()),y(catalogue.size());
        x.execute(before,catalogue);y.execute(after,catalogue);
        for(size_t i=0;i<catalogue.size();++i)
            if(x.pref[i]!=y.pref[i] || x.limit[i]!=y.limit[i])changed.push_back(catalogue.names[i]);
    }
    if(changed.empty())return out+". Every unit plays the same.";
    out+=". "+std::to_string(changed.size())+(changed.size()==1?" unit plays":" units play")+" differently (";
    for(size_t i=0;i<changed.size() && i<4;++i)out+=(i?", ":"")+changed[i];
    return out+(changed.size()>4?", ...).":").");
}

// ---- DEFAULT group rules -----------------------------------------------------------------

std::vector<std::string> aiGroupWords(const ClassicCatalogue& catalogue) {
    std::vector<std::string> out;
    for (const auto& [word, units] : catalogue.categories)
        if (word != "all" && !word.empty() && !classicCanSingleOut(catalogue, word)) out.push_back(word);
    std::sort(out.begin(), out.end());
    if (catalogue.categories.count("all") && !classicCanSingleOut(catalogue, "all")) out.insert(out.begin(), "all");
    return out;
}

namespace {
template <class Locked>
std::vector<AiGroupUnit> groupUnits(const ClassicCatalogue& catalogue, const std::string& word, Locked locked) {
    std::vector<AiGroupUnit> out;
    const auto ct = catalogue.categories.find(word);
    if (ct == catalogue.categories.end() || classicCanSingleOut(catalogue, word)) return out;
    std::map<uint16_t, int> times;
    for (uint16_t i : ct->second) ++times[i];
    for (const auto& [index, n] : times) out.push_back({index, locked(index), n});
    return out;
}
} // namespace

std::vector<AiGroupUnit> aiGroupRuleUnits(const AiDraft& draft, size_t rule, const ClassicCatalogue& catalogue) {
    if (rule >= draft.groups.size()) return {};
    const auto& g = draft.groups[rule];
    return groupUnits(catalogue, g.word, [&](uint16_t i) {
        if (!classicCanSingleOut(catalogue, catalogue.names[i])) return false;   // never has a row
        const auto it = draft.units.find(catalogue.names[i]);
        return it != draft.units.end() && (g.weight ? it->second.chance.has_value() : it->second.limit.has_value());
    });
}

std::vector<AiGroupUnit> aiGroupLineUnits(const std::vector<AiLine>& lines, size_t line, const ClassicCatalogue& catalogue) {
    if (line >= lines.size() || lines[line].what != AiLine::What::Group) return {};
    const auto& g = lines[line];
    std::vector<char> locked(catalogue.size());
    for (size_t i = 0; i < line; ++i)
        if (lines[i].what == AiLine::What::Exact && lines[i].weight == g.weight) locked[lines[i].unit] = 1;
    return groupUnits(catalogue, g.word, [&](uint16_t i) { return locked[i] != 0; });
}

int aiGroupRuleChance(const AiGroupRule& g) {
    return tak::ai::classicScaledPref(100, g.value.empty() ? 0.0f : tak::ai::classicAtof(g.value));
}
int aiGroupRuleLimit(const AiGroupRule& g) { return g.value.empty() ? 0 : tak::ai::classicAtoi(g.value); }

// ---- our levels' limits in play ------------------------------------------------------------

int aiLevelLimitScale(Difficulty d) {
    switch (d) {
        case Difficulty::Easy: case Difficulty::Passive: return 50;
        case Difficulty::Hard: case Difficulty::Absurd: return 150;
        default: return 100;
    }
}
bool aiLevelIgnoresLimits(Difficulty d) { return d == Difficulty::Defensive || d == Difficulty::AbsurdlyDefensive; }

AiLevelLimit aiEffectiveLimit(Difficulty d, std::optional<int> limit, const tak::sim::UnitType* type,
                              const tak::sim::TypeRegistry* registry) {
    AiLevelLimit out;
    const int lim = limit ? *limit : -1;
    if (lim < 0) { out.text = "no cap"; return out; }
    if (lim == 0) { out.limit = 0; out.text = "never built"; return out; }
    if (aiLevelIgnoresLimits(d)) {
        out.changed = true;
        out.text = "no cap in play (" + std::string(aiLevelName(d)) + " ignores limits except 0)";
        return out;
    }
    const bool aggressive = d == Difficulty::Hard || d == Difficulty::Absurd;
    int n = std::max(1, lim * aiLevelLimitScale(d) / 100);
    // The AI's own sort of the unit (ai::Controller::categoryOf), for the raises.
    enum class Cat { Economy, Factory, Builder, Army, Defense } cat = Cat::Army;
    const bool mobileBuilder = type && !type->isStructure() && type->isBuilder;
    if (type && registry) {
        if (type->isStructure()) {
            cat = !registry->buildable(type->id).empty() ? Cat::Factory
                  : type->income > 0 || type->storage > 0 ? Cat::Economy : Cat::Defense;
        } else if (type->isBuilder) {
            cat = Cat::Builder;
            if (!type->commander) {
                int combat = 0, structs = 0;
                for (const auto& id : registry->buildable(type->id))
                    if (const auto* b = registry->find(id)) {
                        if (b->isStructure()) ++structs;
                        else if (!b->isBuilder) ++combat;
                    }
                if (combat > structs && combat > 0) cat = Cat::Factory;
            }
        }
    } else if (type && type->isStructure()) {
        cat = type->income > 0 || type->storage > 0 ? Cat::Economy : Cat::Defense;
    } else if (mobileBuilder) {
        cat = Cat::Builder;
    }
    if (mobileBuilder) {
        n = std::min(n, lim);   // a builder never passes its own number
        if (aggressive && cat == Cat::Factory && registry && !type->side.empty()) {
            // Zhon-style production (no factory buildings on the side): the factory raise.
            bool factories = false;
            for (const auto& [id, t] : registry->types())
                if (t.side == type->side && t.isStructure() && !registry->buildable(id).empty()) { factories = true; break; }
            out.mayRise = !factories;
        }
    } else {
        out.mayRise = aggressive && (cat == Cat::Army || cat == Cat::Factory || cat == Cat::Defense);
        if (aggressive && cat == Cat::Economy && type && type->income > 0) {
            out.changed = true;
            out.text = "no cap in play (" + std::string(aiLevelName(d)) + " lifts limits on income buildings)";
            return out;
        }
    }
    out.limit = n;
    out.changed = n != lim;
    out.text = std::to_string(n) + " in play";
    if (out.mayRise) out.text += ", more in long games";
    return out;
}

} // namespace cart
