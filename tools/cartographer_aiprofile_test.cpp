// The Cartographer AI-profile model (src/cartographer/aiprofiles.h): the chance
// encoding, the canonical form, byte-identical hand files, line attribution checked
// against the engine's own parsers (ai::ClassicProfile::execute, ai::applyProfileText),
// verification that can fail, Make editable, the 6.2 checks, the aiprofile name rules
// which file plays, the game-mod writer (folders, overwrite, backups), the DEFAULT group
// rules' covered units and the levels' limits in play (against ai::paramsFor). Every corpus here is synthetic: no retail text or data.
#include "cartographer/aiprofiles.h"
#include "ai/ai.h"
#include "ai/builtinprofile.h"
#include "net/mappackage.h"
#include "sim/sim.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using tak::ai::ClassicCatalogue;
using tak::ai::Difficulty;
using cart::AiKind;
using cart::AiLine;

static void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
static std::vector<uint8_t> bytesOf(std::string_view s) { return {s.begin(), s.end()}; }

static ClassicCatalogue sample() {
    return ClassicCatalogue::synthetic({
        {"araarch", {"ARA", "LAND"}}, {"arabuild", {"ARA", "builder"}}, {"aracastl", {"ARA"}},
        {"aradrag", {"ARA", "fly"}}, {"araldy", {"ARA", "fly"}}, {"tararch", {"TAR", "LAND"}},
        {"tarbuild", {"TAR", "builder"}}, {"tarnecr2", {"TAR"}}, {"vercut", {"VER", "ship"}},
        {"verwyv", {"VER", "fly", "fly"}},   // a repeated category word: the engine scales it twice
    });
}

// ---- the chance encoding -------------------------------------------------------------

static void chanceEncoding() {
    int naiveWrong = 0;
    for (int p = 0; p <= 100; ++p) {
        const auto text = cart::classicChanceText(p);
        check(tak::ai::classicScaledPref(100, tak::ai::classicAtof(text)) == p, "chance " + std::to_string(p) + " -> " + text);
        char naive[16];
        std::snprintf(naive, sizeof naive, "%d.%02d", p / 100, p % 100);
        if (tak::ai::classicScaledPref(100, tak::ai::classicAtof(naive)) != p) ++naiveWrong;
    }
    check(cart::classicChanceText(100) == "1" && cart::classicChanceText(0) == "0" && cart::classicChanceText(29) == "0.295" &&
          cart::classicChanceText(5) == "0.055" && cart::classicChanceText(1) == "0.015" && cart::classicChanceText(99) == "0.995" &&
          cart::classicChanceText(150) == "1" && cart::classicChanceText(-3) == "0",
          "p=100 -> 1, p=0 -> 0, else 0. + three digits of p*10+5");
    check(naiveWrong > 0, "plain p/100 loses values to the float store (the reason for the half step)");
    std::cout << "chance: all 101 round-trip; p/100 would get " << naiveWrong << " wrong\n";
}

// ---- attribution agrees with the engine ------------------------------------------------

static std::string reducedText(const std::vector<AiLine>& lines, std::initializer_list<AiLine::What> keep) {
    std::string out;
    for (const auto& l : lines)
        if (std::find(keep.begin(), keep.end(), l.what) != keep.end()) { out += l.text; out += '\n'; }
    return out;
}
static size_t lineCount(std::string_view text) {
    size_t n = 0;
    for (size_t start = 0; start < text.size(); ++n) {
        const auto end = text.find('\n', start);
        if (end == std::string_view::npos) { ++n; break; }
        start = end + 1;
    }
    return n;
}
static bool sameClassic(const tak::ai::ClassicProfile& a, const tak::ai::ClassicProfile& b, bool locks) {
    return a.pref == b.pref && a.limit == b.limit && (!locks || (a.prefLocked == b.prefLocked && a.limitLocked == b.limitLocked));
}

static void defaultAgrees(const std::string& text, const ClassicCatalogue& c) {
    const auto lines = cart::attributeAiLines(text, AiKind::Default, c);
    check(lines.size() == lineCount(text), "attribution covers every line");
    tak::ai::ClassicProfile engine(c.size());
    engine.execute(text, c);
    // Replaying the attribution gives the engine's table, locks included.
    tak::ai::ClassicProfile replay(c.size());
    std::vector<int> prefBy(c.size()), limitBy(c.size());
    for (const auto& l : lines) {
        const float w = l.hasValue ? tak::ai::classicAtof(l.value) : 0.0f;
        const int32_t n = l.hasValue ? tak::ai::classicAtoi(l.value) : 0;
        if (l.what == AiLine::What::Exact) {
            auto& by = l.weight ? prefBy : limitBy;
            check(l.unit != 0 && !by[l.unit], "an exact line sets an unlocked unit: " + text);
            by[l.unit] = l.number;
            if (l.weight) { replay.pref[l.unit] = tak::ai::classicScaledPref(replay.pref[l.unit], w); replay.prefLocked[l.unit] = 1; }
            else { replay.limit[l.unit] = n; replay.limitLocked[l.unit] = 1; }
        } else if (l.what == AiLine::What::Shadowed) {
            check(l.unit != 0 && (l.weight ? prefBy : limitBy)[l.unit] == l.by && l.by < l.number, "a shadowed line names its winner");
        } else if (l.what == AiLine::What::Group) {
            for (uint16_t i : c.categories.at(l.word)) {
                if (l.weight && !replay.prefLocked[i]) replay.pref[i] = tak::ai::classicScaledPref(replay.pref[i], w);
                if (!l.weight && !replay.limitLocked[i]) replay.limit[i] = n;
            }
        }
    }
    check(sameClassic(engine, replay, true), "attribution replay differs from ClassicProfile::execute:\n" + text);
    // Every line it calls a no-op is one: dropping them changes nothing.
    tak::ai::ClassicProfile reduced(c.size());
    reduced.execute(reducedText(lines, {AiLine::What::Exact, AiLine::What::Group}), c);
    check(sameClassic(engine, reduced, true), "a line marked as doing nothing does something:\n" + text);
}

static void levelAgrees(const std::string& text) {
    static const ClassicCatalogue none;
    const auto lines = cart::attributeAiLines(text, AiKind::Level, none);
    check(lines.size() == lineCount(text), "level attribution covers every line");
    tak::ai::Profile engine, replay, everySet;
    tak::ai::applyProfileText(text, engine);
    for (const auto& l : lines) {
        if (l.what == AiLine::What::Exact) {
            auto& table = l.weight ? replay.weight : replay.limit;
            check(!table.count(l.word), "one winning line per unit and keyword");
            table[l.word] = l.levelValue;
        }
        if (l.what == AiLine::What::Exact || l.what == AiLine::What::Overridden)
            (l.weight ? everySet.weight : everySet.limit)[l.word] = l.levelValue;
        if (l.what == AiLine::What::Overridden)
            check(l.by > l.number && lines[size_t(l.by - 1)].what == AiLine::What::Exact &&
                  lines[size_t(l.by - 1)].word == l.word && lines[size_t(l.by - 1)].weight == l.weight,
                  "an overridden line names the later line that wins");
    }
    check(engine.weight == replay.weight && engine.limit == replay.limit, "level attribution differs from applyProfileText:\n" + text);
    check(engine.weight == everySet.weight && engine.limit == everySet.limit, "overridden lines are sets the engine reads");
    tak::ai::Profile reduced;
    tak::ai::applyProfileText(reducedText(lines, {AiLine::What::Exact}), reduced);
    check(engine.weight == reduced.weight && engine.limit == reduced.limit, "a level line marked as doing nothing does something:\n" + text);
}

// Make editable: equal effect by the engine, then canonical and idempotent.
static void editableAgrees(const std::string& text, AiKind kind, const ClassicCatalogue& c) {
    auto draft = cart::parseAiDraft(text, kind, c);
    check(cart::writeAiDraft(draft) == text, "an untouched file re-saves byte for byte");
    std::string why;
    check(cart::makeEditable(draft, c, &why), "Make editable refused (" + why + "):\n" + text);
    check(!draft.asWritten, "converted draft is editable");
    std::string bytes, error;
    check(cart::saveAiDraft(draft, c, bytes, error), error);
    if (kind == AiKind::Default) {
        tak::ai::ClassicProfile before(c.size()), after(c.size());
        before.execute(text, c);
        after.execute(bytes, c);
        check(sameClassic(before, after, false), "Make editable changed the DEFAULT effect:\n" + text + "\n--\n" + bytes);
    } else {
        tak::ai::Profile before, after;
        tak::ai::applyProfileText(text, before);
        tak::ai::applyProfileText(bytes, after);
        check(before.weight == after.weight && before.limit == after.limit, "Make editable changed the level effect:\n" + text);
    }
    if (bytes.empty()) return;   // nothing but blank or shadowed lines
    const auto again = cart::parseAiDraft(bytes, kind, c);
    check(!again.asWritten && cart::writeAiDraft(again) == bytes, "canonical idempotence: write(parse(x)) == x\n" + bytes);
    check(bytes.find('\r') == std::string::npos && bytes.back() == '\n', "canonical form: LF endings and a final newline");
}

static std::string randomScript(std::mt19937& rng, const ClassicCatalogue& c) {
    auto pick = [&](std::initializer_list<const char*> list) { return std::string(*(list.begin() + rng() % list.size())); };
    std::string out;
    const int lines = int(rng() % 14);
    for (int i = 0; i < lines; ++i) {
        const auto roll = rng() % 100;
        std::string line;
        if (roll < 6) line = pick({"", "   ", "\t", "// note", "/x", "# weight araarch 0", "x", "plan attack 3", "//weight araarch 0"});
        else {
            line = pick({"weight", "weight", "weight", "limit", "limit", "WEIGHT", "Weight", "Limit", "plan", "w"});
            const auto kind = rng() % 10;
            std::string word;
            if (kind < 5) word = c.names[1 + rng() % (c.size() - 1)];
            else if (kind < 8) word = pick({"ara", "tar", "ver", "fly", "land", "builder", "ship", "all", "ALL"});
            else word = pick({"araboman", "araärch", "#araarch", "", "zzz"});
            if (rng() % 4 == 0 && !word.empty()) word[0] = char(std::toupper(static_cast<unsigned char>(word[0])));
            const std::string sep = pick({" ", " ", "\t", "  ", " \v"});
            if (!word.empty()) line += sep + word;
            if (rng() % 8 != 0)
                line += sep + pick({"0", "1", "2", "5", "-1", "0.5", "0.295", "0.505", "0.25", "1e2", "3abc", "+4", "#5",
                                    "0.5#x", "99999999999", "0.0099", "25", "12"});
            if (rng() % 10 == 0) line += pick({" extra", " # comment", "#", " 7"});
            if (rng() % 12 == 0) line = pick({" ", "  ", "\t"}) + line;
        }
        out += line;
        if (i + 1 < lines || rng() % 3) out += rng() % 4 == 0 ? "\r\n" : "\n";
    }
    return out;
}

static const std::vector<std::string>& handCorpus() {
    static const std::vector<std::string> corpus = {
        "weight araarch 0.5\r\nlimit arabuild 3\r\nweight all 0.5",                     // CRLF, no final newline
        "// my profile\nweight araarch 0.25 # a quarter\n#weight aradrag 0\nlimit fly 2 # flyers\n",
        "WEIGHT\tAraArch\t0.5\nweight araarch 1\nLimit aradrag\nplan attack 3\n// caf\xc3\xa9 \xc3\xbcnits\nweight verwyv\n",
        "\n\n   \nweight tararch 0\n\n",
        "weight all 0.5\nweight araarch 0.5\nweight ara 2\nlimit ALL 4\nlimit tarnecr2 -1\nlimit ship 0\n",
        "weight araarch 5\nlimit araarch 3\n/comment\nweight araarch 7\nWeight tararch 2\nweight tararch 0.5\nweight vercut #3\n",
        "weight fly 0.5\nweight verwyv 0.5\nweight araboman 0\nweight\nlimit\n  weight aradrag 0.019\n",
        std::string(cart::kAiProfileMagic) + "\n// Units not listed are built at full chance with no limit.\nweight verwyv 0.5\n",
        std::string(cart::kAiProfileMagic) + "\n// Units not listed are built at full chance with no limit.\nweight verwyv 0.505\n"
            "// kept from the original file (no effect)\nweight araarch 0\n",                // an effective "kept" line
        "weight araarch 0.5\x0b\x0c\nlimit\x0b" "arabuild 2\n",
        "plan\rattack\r\nweight\raraarch\r0.5\r\n\rweight tararch 2\n",                     // a CR inside a line
    };
    return corpus;
}

static void attribution() {
    const auto c = sample();
    for (const auto& text : handCorpus()) {
        defaultAgrees(text, c);
        levelAgrees(text);
        editableAgrees(text, AiKind::Default, c);
        editableAgrees(text, AiKind::Level, c);
        for (AiKind kind : {AiKind::Default, AiKind::Level}) {
            const auto draft = cart::parseAiDraft(text, kind, c);
            check(draft.asWritten && draft.original == text, "a hand-written file opens as written");
            std::string bytes, error;
            check(cart::saveAiDraft(draft, c, bytes, error) && bytes == text, "an as-written file saves its bytes unchanged");
        }
    }
    std::mt19937 rng(20261023);
    for (int i = 0; i < 10000; ++i) {
        const auto text = randomScript(rng, c);
        defaultAgrees(text, c);
        levelAgrees(text);
        editableAgrees(text, AiKind::Default, c);
        editableAgrees(text, AiKind::Level, c);
    }
    // The attribution itself, on one file.
    const auto lines = cart::attributeAiLines(
        "weight all 0.5\nweight araarch 0.5\nweight araarch 1\nlimit fly 2\nplan x\nweight bogus 1\n// c\n\n", AiKind::Default, c);
    using W = AiLine::What;
    check(lines.size() == 8 && lines[0].what == W::Group && lines[1].what == W::Exact && lines[1].combines == std::vector<int>{1} &&
          lines[2].what == W::Shadowed && lines[2].by == 2 && lines[3].what == W::Group && lines[4].what == W::Other &&
          lines[5].what == W::Unknown && lines[6].what == W::Comment && lines[7].what == W::Blank,
          "DEFAULT attribution: group, exact (combines with line 1), shadowed by line 2, other, unknown, comment, blank");
    const auto level = cart::attributeAiLines("weight a 1\nWeight a 2\nweight a 0.5\nweight b #1\n/c\nweight a 3\n", AiKind::Level, c);
    check(level.size() == 6 && level[0].what == W::Overridden && level[0].by == 6 && level[1].what == W::Other &&
          level[2].what == W::Overridden && level[2].levelValue == 0 && level[3].what == W::Skipped && level[4].what == W::Comment &&
          level[5].what == W::Exact && level[5].levelValue == 3,
          "level attribution: later line wins, case-sensitive keyword, 0.5 reads 0, '#' value skipped, '/' comment");
    std::cout << "attribution: corpus + 10000 random scripts agree with both parsers\n";
}

// ---- the canonical form, verification, Make editable --------------------------------------

static void canonical() {
    const auto c = sample();
    cart::AiDraft d;
    d.header = "DEFAULT AI profile for map \"Islands\" (ai/islands.txt). Edit in Cartographer: SCENARIO > AI Profiles.";
    d.units["araldy"] = {0, {}, 0};
    d.units["verwyv"] = {50, {}, 2};
    d.groups.push_back({false, "fly", "2"});
    d.kept.push_back("plan attack 3");
    const std::string golden =
        "// TAK Cartographer AI profile 1\n"
        "// DEFAULT AI profile for map \"Islands\" (ai/islands.txt). Edit in Cartographer: SCENARIO > AI Profiles.\n"
        "// Units not listed are built at full chance with no limit.\n"
        "weight araldy 0\nlimit araldy 0\nweight verwyv 0.505\nlimit verwyv 2\n"
        "// group rules\nlimit fly 2\n"
        "// kept from the original file (no effect)\nplan attack 3\n";
    std::string bytes, error;
    check(cart::saveAiDraft(d, c, bytes, error) && bytes == golden, "the canonical form (design 5.3):\n" + bytes);
    const auto back = cart::parseAiDraft(bytes, AiKind::Default, c);
    check(!back.asWritten && back.header == d.header && back.units.size() == 2 && back.units.at("verwyv").chance == 50 &&
          back.units.at("verwyv").limit == 2 && back.groups.size() == 1 && back.kept == d.kept && cart::writeAiDraft(back) == golden,
          "a canonical file opens editable and re-writes to the same bytes");
    {
        tak::ai::ClassicProfile p(c.size());
        p.execute(bytes, c);
        const auto at = [&](const char* n) { return size_t(c.byName.at(n)); };
        check(p.pref[at("verwyv")] == 50 && p.limit[at("verwyv")] == 2 && p.limit[at("aradrag")] == 2 && p.pref[at("araldy")] == 0 &&
              p.limit[at("araldy")] == 0, "the engine reads the rows exactly; the group rule reaches only units without a row");
    }
    // Every chance value through a whole file.
    for (int p = 0; p <= 100; ++p) {
        cart::AiDraft one;
        one.units["tararch"].chance = p;
        one.groups.push_back({true, "all", "0.5"});
        check(cart::saveAiDraft(one, c, bytes, error), error);
        tak::ai::ClassicProfile prof(c.size());
        prof.execute(bytes, c);
        check(prof.pref[c.byName.at("tararch")] == p && prof.pref[c.byName.at("tarbuild")] == 50,
              "a typed chance means exactly that value, measured from 100");
    }
    // Levels: whole numbers, no group section.
    cart::AiDraft lv;
    lv.kind = AiKind::Level;
    lv.units["araarch"] = {{}, 0, {}};
    lv.units["verwyv"] = {{}, 12, 3};
    check(cart::saveAiDraft(lv, c, bytes, error) &&
          bytes == "// TAK Cartographer AI profile 1\n// Units not listed keep the built-in values.\n"
                   "weight araarch 0\nweight verwyv 12\nlimit verwyv 3\n", "the level canonical form:\n" + bytes);
    check(!cart::parseAiDraft(bytes, AiKind::Level, c).asWritten, "a canonical level file opens editable");
    // Empty means deleted.
    cart::AiDraft empty;
    empty.header = "only a header";
    empty.units["araarch"];
    check(empty.empty() && cart::writeAiDraft(empty).empty() && cart::saveAiDraft(empty, c, bytes, error) && bytes.empty(),
          "an empty draft writes nothing (the file is removed, never written empty)");
    check(!cart::parseAiDraft("", AiKind::Default, c).asWritten, "no file opens as an empty editable draft");
}

static void verification() {
    const auto c = sample();
    cart::AiDraft d;
    d.units["verwyv"].chance = 29;
    d.units["araarch"].limit = -1;
    d.groups.push_back({true, "fly", "0.5"});
    d.groups.push_back({false, "all", "3"});
    const auto good = cart::writeAiDraft(d);
    std::string unit;
    check(cart::verifyAiDraft(d, good, c, &unit), "the written file verifies: " + unit);
    auto seeded = [&](std::string bytes, const char* what) {
        unit.clear();
        check(!cart::verifyAiDraft(d, bytes, c, &unit), std::string("verify missed a seeded mismatch: ") + what);
        return unit;
    };
    // A plain p/100 encoder (one of the 50 values it truncates).
    auto naive = good;
    naive.replace(naive.find("0.295"), 5, "0.29");
    check(seeded(naive, "p/100 encoding").starts_with("verwyv"), "the mismatch names the unit");
    // Group rules written before the rows: they would scale the rows too.
    auto reordered = good;
    reordered.replace(reordered.find("// group rules\n"), std::string::npos, "");
    reordered.insert(reordered.find("limit araarch"), "weight fly 0.5\nlimit all 3\n");
    seeded(reordered, "group lines before unit rows");
    // "no limit" must still be written: it locks the unit against `limit all 3`.
    auto dropped = good;
    dropped.erase(dropped.find("limit araarch -1\n"), 17);
    check(seeded(dropped, "a missing -1 limit row").starts_with("araarch"), "the unlocked unit is named");
    // An extra effective line anywhere.
    seeded(good + "weight tararch 0\n", "an effective kept line");
    std::string bytes, error;
    cart::AiDraft bad;
    bad.units["nosuchunit"].chance = 10;
    check(!cart::saveAiDraft(bad, c, bytes, error) && error.starts_with("AI profile could not be written exactly") &&
          error.find("nosuchunit") != std::string::npos, "save refuses a draft the engine does not read back: " + error);
    // Levels.
    cart::AiDraft lv;
    lv.kind = AiKind::Level;
    lv.units["araarch"] = {{}, 5, 2};
    const auto lvBytes = cart::writeAiDraft(lv);
    check(cart::verifyAiDraft(lv, lvBytes, c), "level file verifies");
    auto lvBad = lvBytes;
    lvBad.replace(lvBad.find("weight araarch 5"), 16, "weight araarch 6");
    check(!cart::verifyAiDraft(lv, lvBad, c, &unit) && unit.starts_with("araarch"), "level mismatch caught");
    check(!cart::verifyAiDraft(lv, lvBytes + "weight other 1\n", c, &unit) && unit.starts_with("other"), "level extra unit caught");
    // As written: only the same bytes.
    const auto hand = cart::parseAiDraft("weight araarch 0.5\r\n", AiKind::Default, c);
    check(cart::verifyAiDraft(hand, "weight araarch 0.5\r\n", c) && !cart::verifyAiDraft(hand, "weight araarch 0.5\n", c),
          "an as-written draft verifies only its own bytes");
    // Make editable: an exact conversion, and a clean refusal.
    auto conv = cart::parseAiDraft("weight all 0.5\nweight araarch 0.5\nlimit fly 2\nweight araarch 1\nplan x\nlimit aradrag 1\n",
                                   AiKind::Default, c);
    check(cart::makeEditable(conv, c) && conv.units.at("araarch").chance == 25 && conv.units.at("aradrag").limit == 1 &&
          !conv.units.count("arabuild") && conv.groups.size() == 2 && conv.kept == std::vector<std::string>{"plan x"},
          "Make editable: a row from the engine's value (25 %), group rules in order, shadowed line dropped, plan kept");
    auto huge = cart::parseAiDraft(std::string(cart::kAiFileMaxBytes + 1, '\n'), AiKind::Default, c);
    std::string why;
    check(!cart::makeEditable(huge, c, &why) && huge.asWritten && huge.original.size() == cart::kAiFileMaxBytes + 1 && !why.empty(),
          "Make editable refuses cleanly and leaves the file as written");
    std::cout << "verify: seeded mismatches caught\n";
}

// ---- index 0 ----------------------------------------------------------------------

static void indexZero() {
    // The design's fact 3 assumed the alphabetically first unit is index 0. In this
    // engine index 0 is retail's empty sentinel: the first unit is index 1 and an
    // exact line sets it like any other.
    auto c = sample();
    check(c.names[0].empty() && c.byName.at("araarch") == 1 && cart::classicCanSingleOut(c, "araarch") &&
          cart::classicCanSingleOut(c, "ARAARCH") && !cart::classicCanSingleOut(c, ""), "the first unit is index 1");
    tak::ai::ClassicProfile p(c.size());
    p.execute("weight araarch 0\n", c);
    check(p.pref[1] == 0 && p.prefLocked[1], "an exact line sets the alphabetically first unit");
    // The engine's `index != 0` rule itself: a name that resolved to slot 0 falls
    // through to the category lookup. The model follows it.
    c.byName["ghost"] = 0;
    c.byName["fly"] = 0;
    check(!cart::classicCanSingleOut(c, "ghost"), "a slot-0 name cannot be singled out");
    const auto lines = cart::attributeAiLines("weight ghost 0\nweight fly 0.5\n", AiKind::Default, c);
    check(lines[0].what == AiLine::What::Unknown && lines[1].what == AiLine::What::Group, "a slot-0 name acts as its category word");
    defaultAgrees("weight ghost 0\nweight fly 0.5\nweight araarch 1\n", c);
    const auto issues = cart::checkAiFile("ai/islands.txt", "weight ghost 0\n", {&c, nullptr});
    check(issues.size() == 1 && issues[0].message.find("cannot single out ghost") != std::string::npos, "the slot-0 line is flagged");
    cart::AiDraft d;
    d.units["ghost"].chance = 5;
    std::string bytes, error;
    check(!cart::saveAiDraft(d, c, bytes, error), "a row for a slot-0 name is refused");
}

// ---- checks -------------------------------------------------------------------------

static tak::sim::TypeRegistry syntheticRegistry() {
    auto files = std::make_shared<tak::hpi::Vfs::Files>();
    auto file = [&](const std::string& path, const std::string& text) {
        std::string lines;
        for (char ch : text) { lines += ch; if (ch == ';' || ch == '{' || ch == '}') lines += '\n'; }
        (*files)[path] = {lines.begin(), lines.end()};
    };
    auto type = [&](const char* id, const char* side, const char* fields) {
        file(std::string("units/") + id + ".fbi", std::string("[UNITINFO]{unitname=") + id + ";objectname=" + id + ";side=" + side +
             ";name=" + id + ";maxdamage=100;footprintx=2;footprintz=2;buildtime=30;buildcost=10;" + fields + "}");
    };
    type("aramon", "ARA", "commander=1;builder=1;workertime=30;canmove=1;maxvelocity=1;category=ARA LAND;");
    type("arabuild", "ARA", "builder=1;workertime=30;canmove=1;maxvelocity=1;category=ARA builder;");
    type("aralode", "ARA", "yardmap=S;category=ARA;");
    type("araarch", "ARA", "canmove=1;maxvelocity=1;category=ARA LAND;");
    type("aradrag", "ARA", "canmove=1;maxvelocity=1;canfly=1;category=ARA fly;");
    type("aramod", "ARA", "canmove=1;maxvelocity=1;category=ARA LAND;");      // in no built-in level table
    type("araorphan", "ARA", "canmove=1;maxvelocity=1;category=ARA LAND;");   // no builder makes it
    type("tarmon", "TAR", "commander=1;builder=1;workertime=30;canmove=1;maxvelocity=1;category=TAR;");
    type("tararch", "TAR", "canmove=1;maxvelocity=1;category=TAR LAND;");
    for (const char* edge : {"aramon/arabuild", "aramon/araarch", "aramon/aradrag", "aramon/aramod", "arabuild/aralode", "tarmon/tararch"})
        file(std::string("canbuild/") + edge + ".tdf", "[CANBUILD]{sortbias=0;}");
    tak::hpi::Vfs vfs;
    vfs.setMapFiles(files);
    tak::sim::TypeRegistry reg;
    reg.loadDir(vfs, "units");
    reg.loadBuildTree(vfs, "canbuild");
    return reg;
}

static bool has(const std::vector<cart::AiIssue>& issues, int line, const std::string& text,
                cart::AiIssue::Severity severity = cart::AiIssue::Severity::Warning) {
    return std::any_of(issues.begin(), issues.end(), [&](const auto& i) {
        return i.line == line && i.severity == severity && i.message.find(text) != std::string::npos;
    });
}
static std::string dump(const std::vector<cart::AiIssue>& issues) {
    std::string out;
    for (const auto& i : issues) out += i.file + ":" + std::to_string(i.line) + " " + i.message + "\n";
    return out;
}

static void checks() {
    using S = cart::AiIssue::Severity;
    const auto reg = syntheticRegistry();
    check(reg.find("aramon") && reg.find("aramon")->commander && reg.find("aralode")->onMana, "synthetic registry loaded");
    const auto c = ClassicCatalogue::from(reg);
    const cart::AiRoster roster{&c, &reg};
    const auto def = cart::checkAiFile("ai/islands.txt",
        "weight araarc 0\nweight araarch 0.5\nweight araarch 1\nweight all 0.5\nweight arabuild 0.5\nlimit aradrag\n"
        "plan attack 3\nweight araorphan 0.5\nweight fly\n", roster);
    const auto d = dump(def);
    check(has(def, 1, "'araarc' matches nothing, so the line does nothing. Did you mean 'araarch'?"), "unknown word + suggestion\n" + d);
    check(has(def, 3, "line 3 is ignored: line 2 already set this unit"), "shadowed exact line\n" + d);
    check(has(def, 5, "line 5 combines with line 4 ('weight all 0.5'): result 25 %", S::Info), "combines with a group line\n" + d);
    check(has(def, 6, "no value: reads as 0 and locks the unit (never built)"), "missing value (exact)\n" + d);
    check(has(def, 9, "no value: reads as 0 for every unit 'fly' covers"), "missing value (group)\n" + d);
    check(has(def, 7, "'plan' does nothing", S::Info), "other command\n" + d);
    check(has(def, 8, "no builder of its side can make araorphan", S::Info), "unit no builder can make\n" + d);
    check(!has(def, 0, "cannot", S::Error), "nothing is switched off for a whole side\n" + d);

    check(has(cart::checkAiFile("ai/islands.txt", "weight ara 0\n", roster), 0, "this AI cannot build anything for ARA", S::Error),
          "every buildable unit of a side off");
    check(has(cart::checkAiFile("ai/islands.txt", "limit arabuild 0\n", roster), 0, "cannot grow its economy for ARA: every builder", S::Error),
          "every builder off");
    check(has(cart::checkAiFile("ai/default.txt", "weight aralode 0\n", roster), 0, "every lodestone is off", S::Error), "every lodestone off");
    check(cart::checkAiFile("ai/islands.txt", std::string(cart::kAiFileMaxBytes + 1, '\n'), roster).at(0).severity == S::Error,
          "a file over 256 KiB is an error");

    const auto lv = cart::checkAiFile("ai/takengine.txt",
        "Weight araarch 5\nweight araarch 0.5\nweight arabuild #5\nweight aralode\nweight all 3\nweight araarch 4\n"
        "weight araarc 2\nplan attack 3\nweight ARA 1\nlimit tararch 2\n", roster);
    const auto l = dump(lv);
    check(has(lv, 1, "levels need lowercase 'weight'; this line does nothing"), "capital keyword\n" + l);
    check(has(lv, 2, "'0.5' reads as 0: never built"), "decimal value\n" + l);
    check(has(lv, 2, "line 2 is replaced by line 6 (later line wins)"), "earlier line overridden\n" + l);
    check(has(lv, 3, "'#' is not a comment for levels; this line is skipped"), "'#' before the value\n" + l);
    check(has(lv, 4, "no value; the line is skipped"), "missing value (level)\n" + l);
    check(has(lv, 5, "levels read unit names only") && has(lv, 9, "levels read unit names only"), "category word or ALL\n" + l);
    check(has(lv, 7, "Did you mean 'araarch'?"), "unknown unit (level)\n" + l);
    check(has(lv, 8, "'plan' does nothing", S::Info), "other command (level)\n" + l);
    check(has(lv, 0, "aramod (aramod) is never built by our levels (not named)") && !has(lv, 0, "araarch (araarch) is never"),
          "a unit outside every built-in table and the file is never built\n" + l);
    check(!has(cart::checkAiFile("ai/takengine.txt", "weight aramod 3\n", roster), 0, "aramod (aramod) is never built"), "naming it fixes that");
    const auto off = cart::checkAiFile("ai/takengine.txt", "limit arabuild 0\n", roster);
    check(has(off, 0, "every builder is off (every level)", S::Error) &&
          std::count_if(off.begin(), off.end(), [](const cart::AiIssue& i) { return i.message.find("every builder") != std::string::npos; }) == 1,
          "the side checks read each level through applyProfileText on its own built-in table, named once\n" + dump(off));
    // Every ai/ file but ai/takengine.txt is a DEFAULT profile file, whatever its name:
    // the same checks as any other.
    for (const char* name : {"ai/n.txt", "AI/AD.TXT", "ai/d.txt"}) {
        const std::string bytes = "Weight araarch 5\nlimit arabuild 0\n";
        auto messages = [&](const std::string& key) {
            std::vector<std::string> out;
            for (const auto& issue : cart::checkAiFile(key, bytes, roster)) out.push_back(issue.message);
            return out;
        };
        check(cart::aiKindOf(name) == AiKind::Default && messages(name) == messages("ai/islands.txt") && !messages(name).empty(),
              std::string("checked as a DEFAULT profile file: ") + name);
    }

    // Map-level rows.
    tak::hpi::Vfs base;
    cart::AiFiles files{{"ai/notes.txt", bytesOf("plan\n")}, {"ai/default.txt", bytesOf("weight all 1\n")}};
    const auto map = cart::checkAiMap(files, "islands", base, roster);
    const auto m = dump(map);
    check(has(map, 0, "no ai/islands.txt in this map or the install; the AI falls back to ai/default.txt"), "aiprofile without a file\n" + m);
    check(has(map, 0, "ai/notes.txt is kept in the .kmp but not sent to other players or into Test Map"), "file outside the travel list\n" + m);
    check(has(map, 0, "every map without its own profile name also reads ai/default.txt", S::Info), "a map's ai/default.txt\n" + m);
    files["ai/islands.txt"] = bytesOf("weight all 1\n");
    check(!has(cart::checkAiMap(files, "islands", base, roster), 0, "no ai/islands.txt"), "the map's own file is found");
    files["ai/n.txt"] = bytesOf("weight araarch 2\n");
    const auto withN = cart::checkAiMap(files, "islands", base, roster);
    check(has(withN, 0, "ai/n.txt is kept in the .kmp but not sent to other players or into Test Map"),
          "ai/n.txt is a file like any other outside the travel list\n" + dump(withN));
    const auto namedN = cart::checkAiMap(files, "n", base, roster);
    check(!has(namedN, 0, "makes the DEFAULT AI read") && !has(namedN, 0, "no ai/n.txt") && !has(namedN, 0, "ai/n.txt is kept in the .kmp"),
          "aiprofile=n reads the map's ai/n.txt\n" + dump(namedN));
    files.erase("ai/n.txt");
    check(has(cart::checkAiMap({}, "takengine", base, roster), 0, "makes the DEFAULT AI read ai/takengine.txt, the file our levels read", S::Error) &&
          has(cart::checkAiMap({}, "a/b", base, roster), 0, "names a file in another folder", S::Error),
          "aiprofile names that collide with the levels' file or folders are errors");
    check(has(cart::checkAiMap({{"ai/my.txt", bytesOf("weight all 1\n")}}, "my.map", base, roster), 0, "the part after the last '.' is dropped") &&
          !has(cart::checkAiMap({{"ai/my.txt", bytesOf("weight all 1\n")}}, "my.map", base, roster), 0, "no ai/my.txt"),
          "a '.' name reads the extension-dropped file");
    check(cart::checkAiMap({}, "DEFAULT", base, roster).empty() && cart::checkAiMap({}, "", base, roster).empty(),
          "the standard profile (retail's aiprofile=DEFAULT) raises nothing");
    std::cout << "checks: every 6.2 row\n";
}

// ---- names --------------------------------------------------------------------------

static void names() {
    check(cart::proposeAiProfileName("Islands") == "islands" && cart::proposeAiProfileName("My Map v1.2!") == "my map v12" &&
          cart::proposeAiProfileName("  Isle_of-Fire  ") == "isle_of-fire" && cart::proposeAiProfileName("Default") == "default-map" &&
          cart::proposeAiProfileName("N") == "n" && cart::proposeAiProfileName("TakEngine") == "takengine-map" &&
          cart::proposeAiProfileName("...") == "map" && cart::proposeAiProfileName("CON") == "con-map" &&
          cart::proposeAiProfileName(std::string(40, 'x')) == std::string(32, 'x') &&
          cart::proposeAiProfileName("\xc3\x9c" "ber Land") == "ber land",
          "proposed name: lowercased [a-z0-9 _-], at most 32 characters, never reserved");
    for (const char* bad : {"my.map", "a/b", "a\\b", "default", "DEFAULT", "takengine", "TakEngine", "", "con",
                            "thirty-three-characters-long-xxxx"})
        check(!cart::aiProfileNameError(bad).empty(), std::string("name accepted: ") + bad);
    for (const char* good : {"islands", "mission 19", "Isle_of-Fire", "d", "E", "n", "h", "a", "ad", "defaults", "takengine2"})
        check(cart::aiProfileNameError(good).empty(), std::string("name refused: ") + good + " " + cart::aiProfileNameError(good));
    // aiProfileFileKey follows the engine's own lookup.
    tak::hpi::Vfs none;
    for (const char* name : {"islands", "My.Map", "mission 19", "a.b.c", "x.", "DEFAULT", ".hidden", "ISLANDS.ota"}) {
        const auto key = cart::aiProfileFileKey(name);
        if (key.empty()) continue;
        tak::ai::ClassicFiles mapFiles{{key, bytesOf("weight all 1\n")}};
        tak::ai::ClassicProfileLayer layer;
        const auto script = tak::ai::classicProfileScript(none, name, nullptr, &mapFiles, &layer);
        check(layer == tak::ai::ClassicProfileLayer::Map && tak::hpi::MountSet::key(script.path) == key,
              std::string("aiProfileFileKey disagrees with classicProfileScript for ") + name);
    }
    check(cart::aiProfileFileKey("my.map") == "ai/my.txt" && cart::aiProfileFileKey("").empty() && cart::standardAiProfile("") &&
          cart::standardAiProfile("DEFAULT") && cart::standardAiProfile("default.ota") && !cart::standardAiProfile("islands"),
          "file keys and the standard profile");
    // Rename re-keys, empty deletes.
    cart::AiFiles files{{"ai/old.txt", bytesOf("weight all 1\n")}, {"ai/n.txt", bytesOf("weight x 1\n")}};
    check(cart::renameAiProfile(files, "old", "New") && files.count("ai/new.txt") && !files.count("ai/old.txt") &&
          files.at("ai/new.txt") == bytesOf("weight all 1\n"), "rename moves ai/<old>.txt to ai/<new>.txt");
    check(!cart::renameAiProfile(files, "new", "default") && !cart::renameAiProfile(files, "new", "takengine") && files.count("ai/new.txt"),
          "rename never moves onto ai/takengine.txt or ai/default.txt");
    cart::setAiFile(files, "AI/N.txt", "");
    cart::setAiFile(files, "ai/e.txt", "weight y 2\n");
    check(!files.count("ai/n.txt") && files.at("ai/e.txt") == bytesOf("weight y 2\n"), "an empty draft deletes its file");
    for (Difficulty d : {Difficulty::Defensive, Difficulty::Easy, Difficulty::Normal, Difficulty::Hard, Difficulty::Absurd,
                         Difficulty::AbsurdlyDefensive})
        check(cart::aiEngineFile(tak::ai::levelProfileFile(d)), "every level's file is ai/takengine.txt (ai::levelProfileFile)");
    check(cart::aiEngineFile("AI/TakEngine.TXT") && !cart::aiEngineFile("ai/n.txt") && !cart::aiEngineFile("ai/default.txt") &&
          cart::aiKindOf("ai/takengine.txt") == AiKind::Level && cart::aiKindOf("AI/TAKENGINE.TXT") == AiKind::Level &&
          cart::aiKindOf("ai/n.txt") == AiKind::Default && cart::aiKindOf("ai/default.txt") == AiKind::Default &&
          cart::aiKindOf("ai/islands.txt") == AiKind::Default,
          "ai/takengine.txt is the levels' file; every other ai/ file is DEFAULT");
}

// What travels agrees with the game's own import (net::maps, Test Map's --play-map).
static void travel(const fs::path& root) {
    tak::tnt::Map map;
    map.width = map.height = 32;
    map.blocksX = map.blocksY = 16;
    map.heights.assign(1024, 40);
    map.features.assign(1024, 0xffff);
    map.tileKeys.assign(256, 1);
    map.tileCols.assign(256, 0);
    map.tileRows.assign(256, 0);
    tak::hpi::Vfs base;
    auto art = std::make_shared<tak::hpi::Vfs::Files>();
    (*art)["terrain/00000001.jpg"] = {1};
    base.setModFiles(art);
    const cart::AiFiles files{{"ai/default.txt", bytesOf("a\n")}, {"ai/n.txt", bytesOf("b\n")}, {"ai/ad.txt", bytesOf("c\n")},
                              {"ai/takengine.txt", bytesOf("weight araarch 2\n")},
                              {"ai/islands.txt", bytesOf("d\n")}, {"ai/my.txt", bytesOf("e\n")}, {"ai/notes.txt", bytesOf("f\n")},
                              {"ai/mission 19.txt", bytesOf("g\n")}, {"ai/x.txt", {}}};
    int n = 0;
    for (const char* profile : {"DEFAULT", "", "islands", "ISLANDS", "my.map", "notes.txt", "mission 19", "x", "n"}) {
        tak::tnt::Scenario metadata;
        metadata.aiProfile = profile;
        const auto doc = cart::documentFiles(map, metadata, {}, {}, {}, files, "Travel");
        const auto path = root / ("travel" + std::to_string(n++) + ".kmp");
        std::string error;
        check(cart::writeDocumentBundle(path, doc, error), error);
        const auto package = tak::net::maps::importSnapshot(base, path);
        const auto travelling = cart::travellingAiFiles(files, profile);
        for (const auto& [key, bytes] : files)
            check(package->files->count(key) == travelling.count(key),
                  "aiFileTravels disagrees with the map package for " + key + " (aiprofile=" + profile + ")");
    }
}

// ---- which file plays ----------------------------------------------------------------

static void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

static void whichFileMatrix(const fs::path& root) {
    using F = cart::AiFileChoice::From;
    writeFile(root / "base" / "ai" / "default.txt", "weight all 1\n");
    writeFile(root / "base" / "ai" / "installed.txt", "weight all 1\n");
    writeFile(root / "base" / "ai" / "n.txt", "weight araarch 1\n");   // the install never supplies the levels' file
    writeFile(root / "base" / "ai" / "takengine.txt", "weight araarch 1\n");
    writeFile(root / "mods" / "game" / "pack" / "ai" / "default.txt", "weight all 0.5\n");
    writeFile(root / "mods" / "game" / "pack" / "ai" / "h.txt", "weight araarch 2\n");   // not the levels' file: not read by them
    writeFile(root / "mods" / "game" / "pack3" / "ai" / "takengine.txt", "weight araarch 2\n");
    writeFile(root / "mods" / "game" / "pack2" / "ai" / "islands.txt", "weight all 0.25\n");
    writeFile(root / "mods" / "game" / "pack2" / "ai" / "takengine.txt", "weight araarch 3\n");
    tak::hpi::Vfs base;
    base.addLayer(tak::hpi::MountSet(root / "base"));
    tak::hpi::Vfs nothing;
    const auto pack = tak::hpi::modFiles(root, tak::hpi::ModKind::Game, {"pack"});
    const auto pack2 = tak::hpi::modFiles(root, tak::hpi::ModKind::Game, {"pack2"});
    const auto pack3 = tak::hpi::modFiles(root, tak::hpi::ModKind::Game, {"pack3"});
    check(pack.count("ai/default.txt") && pack2.count("ai/islands.txt"), "game-mod fixtures mounted");
    const cart::AiFiles map{{"ai/islands.txt", bytesOf("weight all 0.75\n")}, {"ai/takengine.txt", bytesOf("weight araarch 4\n")},
                            {"ai/notes.txt", bytesOf("plan\n")}};
    const cart::AiFiles otherMap{{"ai/n.txt", bytesOf("weight araarch 4\n")}, {"ai/h.txt", bytesOf("weight araarch 5\n")}};
    auto is = [](const cart::AiFileChoice& c, F from, const std::string& path, const std::string& text) {
        check(c.from == from && c.path == path && c.message == text, "whichFile: got [" + c.path + "] " + c.message + "\nwant [" + path + "] " + text);
    };
    is(cart::whichFile(Difficulty::Classic, base, map, "islands", &pack), F::Map, "ai/islands.txt",
       "DEFAULT AI on this map reads ai/islands.txt from this map.");   // the map's ai/<key> beats a mod's ai/default.txt
    is(cart::whichFile(Difficulty::Classic, base, map, "islands"), F::Map, "ai/islands.txt",
       "DEFAULT AI on this map reads ai/islands.txt from this map. A ticked game mod's ai/islands.txt would replace it; "
       "a game mod's ai/default.txt would not.");
    is(cart::whichFile(Difficulty::Classic, base, map, "islands", &pack2), F::GameMod, "ai/islands.txt",
       "DEFAULT AI reads ai/islands.txt from the game mod, which replaces this map's.");
    is(cart::whichFile(Difficulty::Classic, base, {}, "islands"), F::Base, "ai/default.txt",
       "DEFAULT AI reads the installed ai/default.txt (no ai/islands.txt in this map or the install).");
    is(cart::whichFile(Difficulty::Classic, base, {}, "islands", &pack), F::GameMod, "ai/default.txt",
       "DEFAULT AI reads ai/default.txt from the game mod.");
    is(cart::whichFile(Difficulty::Classic, base, {}, "installed"), F::Base, "ai/installed.txt",
       "DEFAULT AI reads the installed ai/installed.txt.");
    is(cart::whichFile(Difficulty::Classic, base, map, "DEFAULT"), F::Base, "ai/default.txt",
       "DEFAULT AI reads the installed ai/default.txt (this map has no profile name).");
    is(cart::whichFile(Difficulty::Classic, base, {{"ai/default.txt", bytesOf("weight all 1\n")}}, ""), F::Map, "ai/default.txt",
       "DEFAULT AI on this map reads ai/default.txt from this map. A ticked game mod's ai/default.txt would replace it.");
    is(cart::whichFile(Difficulty::Classic, nothing, map, "other"), F::None, "",
       "DEFAULT AI: no profile file, so every unit is built at full chance with no limit.");   // ai/notes.txt does not travel
    is(cart::whichFile(Difficulty::Classic, nothing, map, "notes"), F::Map, "ai/notes.txt",
       "DEFAULT AI on this map reads ai/notes.txt from this map. A ticked game mod's ai/notes.txt would replace it; "
       "a game mod's ai/default.txt would not.");   // ... until the map names it
    is(cart::whichFile(Difficulty::Classic, nothing, {{"ai/islands.txt", {}}}, "islands"), F::None, "",
       "DEFAULT AI: no profile file, so every unit is built at full chance with no limit.");   // an empty file is dropped
    const std::string none = "Our six levels: built-in tables (no ai/takengine.txt).";
    is(cart::whichFile(Difficulty::Normal, base, map, "islands"), F::Map, "ai/takengine.txt",
       "All six levels (Normal shown) read ai/takengine.txt from this map, each over its own built-in table, unless a ticked "
       "game mod has ai/takengine.txt, which then replaces it entirely.");
    is(cart::whichFile(Difficulty::Hard, base, map, "islands", &pack), F::Map, "ai/takengine.txt",
       "All six levels (Hard shown) read ai/takengine.txt from this map, each over its own built-in table.");   // pack's h.txt unread
    is(cart::whichFile(Difficulty::Normal, base, map, "islands", &pack2), F::GameMod, "ai/takengine.txt",
       "All six levels (Normal shown) read ai/takengine.txt from the game mod, each over its own built-in table; it replaces "
       "this map's file entirely.");
    is(cart::whichFile(Difficulty::Easy, base, {}, "islands", &pack3), F::GameMod, "ai/takengine.txt",
       "All six levels (Easy shown) read ai/takengine.txt from the game mod, each over its own built-in table.");
    is(cart::whichFile(Difficulty::Hard, base, otherMap, "islands", &pack), F::None, "", none);   // other files: never read by the levels
    is(cart::whichFile(Difficulty::Normal, base, {}, ""), F::None, "", none);   // never the install's
}

// ---- the game-mod writer -------------------------------------------------------------

static std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static std::set<std::string> namesIn(const fs::path& dir) {
    std::set<std::string> out;
    for (const auto& e : fs::directory_iterator(dir)) out.insert(e.path().filename().string());
    return out;
}

static void modWriter(const fs::path& root) {
    using A = cart::ModProfileChange::Action;
    // Names.
    check(cart::modPackNameError("harder-ai").empty(), "a plain pack name is accepted");
    for (const std::string& bad : std::vector<std::string>{"", ".hidden", "a/b", "a\\b", "con", "x.", std::string(129, 'p')})
        check(!cart::modPackNameError(bad).empty(), "pack name '" + bad + "' is refused");
    std::string error;
    cart::ModProfilePlan plan;
    check(!cart::planModProfiles(root / "absent", "p", {}, {}, plan, error), "a missing data folder is refused");

    // An existing pack in case-variant folders: Mods/Game/Harder/AI.
    const auto data = root / "moddata", pack = data / "Mods" / "Game" / "Harder", ai = pack / "AI";
    writeFile(ai / "N.TXT", "weight araarch 1\n");
    writeFile(ai / "e.txt", "weight araarch 5\n");
    writeFile(ai / "a.txt", "weight araarch 7\n");
    writeFile(ai / "notes.txt", "keep\n");
    writeFile(pack / "readme.txt", "keep\n");
    {
        const auto archive = tak::hpi::pack({{"ai/h.txt", bytesOf("weight araarch 9\n")}});
        std::ofstream(pack / "base.hpi", std::ios::binary).write(reinterpret_cast<const char*>(archive.data()), std::streamsize(archive.size()));
    }
    const cart::AiFiles files{{"ai/n.txt", bytesOf("weight araarch 2\n")}, {"ai/e.txt", bytesOf("weight araarch 5\n")},
                              {"ai/h.txt", bytesOf("weight araarch 3\n")}, {"ai/d.txt", {}}};
    check(cart::planModProfiles(data, "harder", files, {"ai/a.txt", "ai/ad.txt"}, plan, error), "plan: " + error);
    check(plan.pack == "Harder" && plan.folder == ai && plan.create.empty(), "the existing pack and folders are reused in their own case");
    check(plan.changes.size() == 4, "four changes planned (an empty draft and an absent delete add none)");
    const auto& del = plan.changes[0]; const auto& same = plan.changes[1]; const auto& create = plan.changes[2]; const auto& rep = plan.changes[3];
    check(del.key == "ai/a.txt" && del.action == A::Delete, "a ticked delete of an existing file");
    check(same.key == "ai/e.txt" && same.action == A::Same, "an identical file is skipped");
    check(create.key == "ai/h.txt" && create.action == A::Create && create.archive == "base.hpi", "a new loose file over the pack archive's copy");
    check(rep.key == "ai/n.txt" && rep.action == A::Replace && rep.path == ai / "N.TXT" && readText(rep.path) == "weight araarch 1\n",
          "a different file replaces the existing spelling");
    check(plan.writes(), "the plan writes");

    // A file changed after the compare: nothing is written.
    writeFile(ai / "N.TXT", "weight araarch 4\n");
    check(!cart::writeModProfiles(data, plan, root / "profile", error) && error.find("changed on disk") != std::string::npos,
          "a stale plan is refused: " + error);
    check(readText(ai / "N.TXT") == "weight araarch 4\n" && !fs::exists(ai / "h.txt") && fs::exists(ai / "a.txt"), "the stale plan changed nothing");
    check(!fs::exists(root / "profile" / "mod-backups"), "the stale plan made no backup");
    check(cart::planModProfiles(data, "Harder", files, {"ai/a.txt"}, plan, error), "re-plan: " + error);

    fs::path backup;
    const auto before = fs::last_write_time(ai / "e.txt");
    check(cart::writeModProfiles(data, plan, root / "profile", error, &backup), "write: " + error);
    check(readText(ai / "N.TXT") == "weight araarch 2\n" && readText(ai / "h.txt") == "weight araarch 3\n" && !fs::exists(ai / "a.txt"),
          "replaced, created and deleted");
    check(namesIn(ai) == std::set<std::string>{"N.TXT", "e.txt", "h.txt", "notes.txt"}, "no second spelling, staging or .bak in ai/");
    check(fs::last_write_time(ai / "e.txt") == before && readText(ai / "notes.txt") == "keep\n" && readText(pack / "readme.txt") == "keep\n",
          "identical and other pack files are untouched");
    check(namesIn(pack) == std::set<std::string>{"AI", "base.hpi", "readme.txt"}, "nothing else was added to the pack");
    check(namesIn(data / "Mods" / "Game") == std::set<std::string>{"Harder"}, "the staging folder is gone");
    check(backup.parent_path() == root / "profile" / "mod-backups" / "Harder", "the backup sits in the profile folder, per pack: " + backup.string());
    check(readText(backup / "ai" / "N.TXT") == "weight araarch 4\n" && readText(backup / "ai" / "a.txt") == "weight araarch 7\n" &&
          namesIn(backup / "ai").size() == 2, "the backup holds the replaced and deleted files only");
    const auto read = cart::readModProfiles(data, "Harder");
    check(read.size() == 4 && read.at("ai/n.txt") == bytesOf("weight araarch 2\n") && read.at("ai/h.txt") == bytesOf("weight araarch 3\n"),
          "Open from game mod reads the pack as the game does (loose over archive)");
    // Saving the same again writes nothing.
    check(cart::planModProfiles(data, "Harder", read, {}, plan, error) && !plan.writes(), "an unchanged pack plans no writes");
    check(cart::writeModProfiles(data, plan, root / "profile", error, &backup) && backup.empty(), "nothing to back up when nothing changes");

    // A data root with no mods folder: everything is created; no backup is needed.
    const auto fresh = root / "freshdata";
    fs::create_directories(fresh);
    check(cart::planModProfiles(fresh, "new pack", {{"ai/n.txt", bytesOf("weight araarch 2\n")}}, {}, plan, error), "fresh plan: " + error);
    check(plan.create.size() == 4 && plan.create.front() == fresh / "mods" && plan.folder == fresh / "mods" / "game" / "new pack" / "ai", "missing folders planned");
    check(cart::writeModProfiles(fresh, plan, root / "profile", error, &backup) && backup.empty(), "fresh write: " + error);
    check(tak::hpi::modPacks(fresh, tak::hpi::ModKind::Game) == std::vector<std::string>{"new pack"}, "the new pack is listed by the game");
    check(tak::hpi::modFiles(fresh, tak::hpi::ModKind::Game, {"new pack"}).count("ai/n.txt"), "the game reads the new file");

    // mods/game/always_enabled/ holds the packs the game always applies: it is not a
    // pack, and Cartographer edits only ordinary packs (an always-enabled one hides
    // a namesake folder, so a save there would never load).
    fs::create_directories(fresh / "mods" / "game" / "always_enabled" / "House AI" / "ai");
    std::ofstream(fresh / "mods" / "game" / "always_enabled" / "House AI" / "ai" / "takengine.txt") << "limit arabow 0\n";
    check(!cart::planModProfiles(fresh, "always_enabled", {{"ai/n.txt", bytesOf("x\n")}}, {}, plan, error) &&
          error.find("always_enabled") != std::string::npos, "a pack named always_enabled is refused: " + error);
    check(!cart::planModProfiles(fresh, "ALWAYS_ENABLED", {{"ai/n.txt", bytesOf("x\n")}}, {}, plan, error), "always_enabled is refused in any case");
    check(!cart::planModProfiles(fresh, "house ai", {{"ai/n.txt", bytesOf("x\n")}}, {}, plan, error) &&
          error.find("always_enabled/House AI") != std::string::npos, "an always-enabled pack's name is refused (any case): " + error);
    check(!cart::modPackReservedError(fresh, "House AI").empty() && cart::modPackReservedError(fresh, "new pack").empty(),
          "only always-enabled names are reserved");
    check(tak::hpi::modPacks(fresh, tak::hpi::ModKind::Game) == std::vector<std::string>{"new pack"}, "always_enabled and its packs are not listed for editing");
    bool refused = false;
    try { cart::readModProfiles(fresh, "House AI"); } catch (const std::exception&) { refused = true; }
    check(refused, "Open from game mod refuses an always-enabled pack");
    check(cart::alwaysEnabledGameNote(fresh).find("House AI") != std::string::npos && cart::alwaysEnabledGameNote(data).empty(),
          "TEST IN GAME names the always-enabled game packs");

#ifndef _WIN32
    // Symlinked components are refused: the game would never list the pack.
    const auto linked = root / "linkdata";
    fs::create_directories(linked / "mods" / "real");
    fs::create_directory_symlink(linked / "mods" / "real", linked / "mods" / "game");
    check(!cart::planModProfiles(linked, "p", {{"ai/n.txt", bytesOf("x\n")}}, {}, plan, error) && error.find("symbolic link") != std::string::npos,
          "a symlinked game folder is refused: " + error);
    fs::create_directories(fresh / "elsewhere");
    fs::create_directory_symlink(fresh / "elsewhere", fresh / "mods" / "game" / "linked");
    check(!cart::planModProfiles(fresh, "Linked", {{"ai/n.txt", bytesOf("x\n")}}, {}, plan, error) && error.find("symbolic link") != std::string::npos,
          "a symlinked pack is refused");
    fs::create_symlink(fresh / "elsewhere", fresh / "mods" / "game" / "new pack" / "ai" / "h.txt");
    check(!cart::planModProfiles(fresh, "new pack", {{"ai/h.txt", bytesOf("x\n")}}, {}, plan, error), "a symlinked file is not written through");
    // A read-only install: SAVE FAILED in plain words, nothing created.
    if (geteuid() != 0) {
        const auto locked = root / "lockeddata";
        fs::create_directories(locked / "mods" / "game");
        fs::permissions(locked / "mods" / "game", fs::perms::owner_read | fs::perms::owner_exec);
        check(cart::planModProfiles(locked, "p", {{"ai/n.txt", bytesOf("x\n")}}, {}, plan, error), "locked plan: " + error);
        check(!cart::writeModProfiles(locked, plan, root / "profile", error) && error.find("read-only") != std::string::npos,
              "a read-only data folder says so: " + error);
        fs::permissions(locked / "mods" / "game", fs::perms::owner_all);
        check(fs::is_empty(locked / "mods" / "game"), "the read-only failure left nothing behind");
    }
#endif

    // The overwrite compare names the first changed line and the units that change.
    const auto c = sample();
    const auto text = cart::compareAiFiles("ai/x.txt", "weight araarch 1\nlimit aradrag 2\n", "weight araarch 0.505\nlimit aradrag 2\n", c);
    check(text.find("line 1 \"weight araarch 1\" becomes \"weight araarch 0.505\"") != std::string::npos &&
          text.find("1 unit plays differently (araarch)") != std::string::npos, "DEFAULT compare: " + text);
    const auto level = cart::compareAiFiles("ai/takengine.txt", "weight araarch 5\n", "weight araarch 5\n// note\n", c);
    check(level.find("Every unit plays the same") != std::string::npos, "level compare: " + level);
    const auto levels = cart::compareAiFiles("ai/takengine.txt", "weight araarch 5\n", "weight araarch 6\nlimit tararch 0\n", c);
    check(levels.find("2 units play differently (araarch, tararch)") != std::string::npos, "takengine compare: " + levels);
    std::cout << "mod writer: case-variant folders reused, identical skipped, backups outside the pack, symlinks and read-only refused\n";
}

// ---- phase 5: group rules and the levels' limits in play -------------------------------

// The prefix of a file's lines up to `last`, with that line replaced.
static std::string prefixWith(const std::vector<AiLine>& lines, size_t last, const std::string& replacement) {
    std::string out;
    for (size_t i = 0; i < last; ++i) out += std::string(lines[i].text) + "\n";
    return out + replacement + "\n";
}

// What the engine does to the units a group line covers, checked unit by unit: with
// the line's value replaced by a sentinel, every covered unit not marked skipped takes
// it, and every skipped or uncovered unit stays as the lines before left it.
static void groupLineAgrees(const std::string& text, const ClassicCatalogue& c) {
    const auto lines = cart::attributeAiLines(text, AiKind::Default, c);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].what != AiLine::What::Group) continue;
        const auto units = cart::aiGroupLineUnits(lines, i, c);
        check(!units.empty(), "a group line covers no unit: " + std::string(lines[i].text));
        tak::ai::ClassicProfile before(c.size()), after(c.size());
        before.execute(prefixWith(lines, i, ""), c);
        after.execute(prefixWith(lines, i, std::string(lines[i].weight ? "weight " : "limit ") + lines[i].word + (lines[i].weight ? " 0" : " 4321")), c);
        std::vector<int> covered(c.size(), -1);
        for (const auto& u : units) covered[u.index] = u.skipped;
        for (size_t k = 1; k < c.size(); ++k) {
            const bool takes = covered[k] == 0;
            if (lines[i].weight)
                check(takes ? after.pref[k] == 0 : after.pref[k] == before.pref[k],
                      "group weight attribution for " + c.names[k] + " differs from the engine:\n" + text);
            else
                check(takes ? after.limit[k] == 4321 : after.limit[k] == before.limit[k],
                      "group limit attribution for " + c.names[k] + " differs from the engine:\n" + text);
        }
    }
}

static void groupRules() {
    const auto c = sample();
    check(cart::aiGroupWords(c) == std::vector<std::string>{"all", "ara", "builder", "fly", "land", "ship", "tar", "ver"},
          "group words: this catalogue's categories, all first");
    // A category word that is also a unit name is that unit to the engine: never offered.
    const auto clash = ClassicCatalogue::synthetic({{"araarch", {"fly"}}, {"fly", {"ARA"}}, {"verwyv", {"fly"}}});
    const auto words = cart::aiGroupWords(clash);
    check(std::find(words.begin(), words.end(), "fly") == words.end() && std::find(words.begin(), words.end(), "ara") != words.end(),
          "a word that names a unit is not offered as a group");

    // A canonical draft: rows lock their keyword only, a repeated category word counts twice.
    cart::AiDraft d;
    d.units["verwyv"].chance = 50;
    d.units["aradrag"].limit = 2;
    d.groups = {{true, "fly", cart::classicChanceText(50)}, {false, "fly", "3"}, {true, "all", "1"}};
    auto names = [&](const std::vector<cart::AiGroupUnit>& units, bool skipped) {
        std::vector<std::string> out;
        for (const auto& u : units) if (u.skipped == skipped) out.push_back(c.names[u.index]);
        return out;
    };
    const auto weightFly = cart::aiGroupRuleUnits(d, 0, c), limitFly = cart::aiGroupRuleUnits(d, 1, c);
    check(names(weightFly, false) == std::vector<std::string>{"aradrag", "araldy"} && names(weightFly, true) == std::vector<std::string>{"verwyv"},
          "weight fly: verwyv's own chance row skips it, aradrag's limit row does not");
    check(names(limitFly, false) == std::vector<std::string>{"araldy", "verwyv"} && names(limitFly, true) == std::vector<std::string>{"aradrag"},
          "limit fly: aradrag's own limit row skips it");
    for (const auto& u : weightFly) check(u.times == (c.names[u.index] == "verwyv" ? 2 : 1), "a repeated category word is counted");
    check(cart::aiGroupRuleUnits(d, 2, c).size() == c.size() - 1, "ALL covers every unit");
    check(cart::aiGroupRuleChance(d.groups[0]) == 50 && cart::aiGroupRuleLimit(d.groups[1]) == 3 &&
          cart::aiGroupRuleChance({true, "fly", ""}) == 0 && cart::aiGroupRuleLimit({false, "fly", "-1"}) == -1,
          "group values as the engine reads them");

    // The draft's view agrees with the written file's, and both with the engine.
    std::mt19937 rng(5150);
    const auto groupWords = cart::aiGroupWords(c);
    for (int round = 0; round < 3000; ++round) {
        cart::AiDraft r;
        for (size_t k = 1; k < c.size(); ++k) {
            if (rng() % 3) continue;
            auto& rule = r.units[c.names[k]];
            if (rng() % 2) rule.chance = int(rng() % 101);
            if (rng() % 2) rule.limit = int(rng() % 4) - 1;
            if (rule.empty()) r.units.erase(c.names[k]);
        }
        for (int g = int(rng() % 4); g > 0; --g)
            r.groups.push_back({rng() % 2 == 0, groupWords[rng() % groupWords.size()],
                                rng() % 2 ? cart::classicChanceText(int(rng() % 101)) : std::to_string(int(rng() % 5) - 1)});
        std::string bytes, error;
        check(cart::saveAiDraft(r, c, bytes, error), error);
        const auto lines = cart::attributeAiLines(bytes, AiKind::Default, c);
        size_t rule = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].what != AiLine::What::Group) continue;
            const auto a = cart::aiGroupRuleUnits(r, rule++, c), b = cart::aiGroupLineUnits(lines, i, c);
            check(a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
                      return x.index == y.index && x.skipped == y.skipped && x.times == y.times;
                  }), "a draft's group rule and its written line cover different units:\n" + bytes);
        }
        check(rule == r.groups.size(), "every group rule is written as a group line");
        groupLineAgrees(bytes, c);
        groupLineAgrees(randomScript(rng, c), c);
    }
    for (const auto& text : handCorpus()) groupLineAgrees(text, c);
    std::cout << "group rules: words, covered units and the lock agree with ClassicProfile::execute\n";
}

static void levelLimits() {
    // The scale and the defensive rule are paramsFor's.
    for (const auto d : {Difficulty::Defensive, Difficulty::Easy, Difficulty::Normal, Difficulty::Hard, Difficulty::Absurd,
                         Difficulty::AbsurdlyDefensive}) {
        const auto p = tak::ai::paramsFor(d);
        check(cart::aiLevelLimitScale(d) == p.limitScale && cart::aiLevelIgnoresLimits(d) == p.unbounded,
              std::string("limit scale differs from ai::paramsFor for ") + cart::aiLevelName(d));
    }
    tak::sim::UnitType army;army.id = "army";army.maxVel = tak::sim::Fixed::fromFloat(1.0f);
    tak::sim::UnitType builder = army;builder.id = "builder";builder.isBuilder = true;
    tak::sim::UnitType mine;mine.id = "mine";mine.income = 3;mine.maxVel = tak::sim::Fixed();   // a structure with income
    auto in = [](Difficulty d, std::optional<int> lim, const tak::sim::UnitType* t) { return cart::aiEffectiveLimit(d, lim, t); };
    check(in(Difficulty::Normal, 5, &army).limit == 5 && !in(Difficulty::Normal, 5, &army).changed, "Normal keeps a limit");
    check(in(Difficulty::Easy, 3, &army).limit == 1 && in(Difficulty::Easy, 1, &army).limit == 1 && in(Difficulty::Easy, 6, &army).limit == 3,
          "Easy halves a limit, minimum 1");
    const auto hard = in(Difficulty::Hard, 3, &army);
    check(hard.limit == 4 && hard.changed && hard.mayRise && hard.text == "4 in play, more in long games", "Hard raises by half: " + hard.text);
    check(in(Difficulty::Absurd, 2, &army).limit == 3, "Absurd raises by half");
    check(in(Difficulty::Hard, 4, &builder).limit == 4 && !in(Difficulty::Hard, 4, &builder).mayRise, "a builder never passes its own number");
    check(in(Difficulty::Easy, 4, &builder).limit == 2, "Easy halves a builder's limit");
    check(in(Difficulty::Hard, 2, &mine).limit == -1 && in(Difficulty::Normal, 2, &mine).limit == 2, "Hard lifts income buildings");
    for (const auto d : {Difficulty::Defensive, Difficulty::AbsurdlyDefensive}) {
        check(in(d, 5, &army).limit == -1 && in(d, 5, &army).changed, "defensive levels ignore limits");
        check(in(d, 0, &army).limit == 0, "defensive levels keep a limit of 0");
    }
    for (const auto d : {Difficulty::Easy, Difficulty::Normal, Difficulty::Hard})
        check(in(d, 0, &army).limit == 0 && in(d, std::nullopt, &army).limit == -1 && in(d, -1, &army).limit == -1, "0 is never, none is no cap");
    std::cout << "level limits in play: paramsFor's scale, builders, income buildings, defensive levels\n";
}

// Save as game mod, DEFAULT written as ai/default.txt: only the header line changes,
// naming the file it now is and the pack; a hand-written file keeps its bytes.
static void modDefaultHeader() {
    const auto c = sample();
    cart::AiDraft d;
    d.header = "DEFAULT AI profile for map \"Islands\" (ai/islands.txt). Edit in Cartographer: SCENARIO > AI Profiles.";
    d.units["verwyv"] = {50, {}, 2};
    d.groups.push_back({false, "fly", "2"});
    d.kept.push_back("plan attack 3");
    std::string source, error;
    check(cart::saveAiDraft(d, c, source, error), error);
    const auto header = cart::aiModDefaultHeader("Harder", "Islands", "ai/islands.txt");
    check(header == "DEFAULT AI profile for game mod \"Harder\" (ai/default.txt), saved from map \"Islands\" (ai/islands.txt). "
                    "Edit in Cartographer: SCENARIO > AI Profiles.", "the game mod default header: " + header);
    const auto out = cart::aiFileWithHeader(source, AiKind::Default, header, c);
    const auto lineEnd = [](const std::string& s) { return s.find('\n', s.find('\n') + 1) + 1; };   // past line 2
    check(out.substr(0, out.find('\n') + 1) == source.substr(0, source.find('\n') + 1) &&
          out.substr(out.find('\n') + 1, lineEnd(out) - out.find('\n') - 1) == "// " + header + "\n" &&
          out.substr(lineEnd(out)) == source.substr(lineEnd(source)), "only the header line changes:\n" + out);
    const auto back = cart::parseAiDraft(out, AiKind::Default, c);
    check(!back.asWritten && back.header == header && cart::verifyAiDraft(back, out, c), "the new file is canonical and verifies");
    tak::ai::ClassicProfile before(c.size()), after(c.size());
    before.execute(source, c);
    after.execute(out, c);
    check(sameClassic(before, after, true), "the header change changes no unit");
    for (const std::string hand : {"weight verwyv 0.5\n", "// TAK Cartographer AI profile 1\n// x\nweight verwyv 0.5\n", ""})
        check(cart::aiFileWithHeader(hand, AiKind::Default, header, c) == hand, "a hand-written file is saved unchanged: " + hand);
    std::cout << "game mod default.txt: the header names ai/default.txt and the pack; nothing else changes\n";
}

int main() try {
    const auto root = fs::temp_directory_path() /
                      ("tak-aiprofile-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{root};
    chanceEncoding();
    canonical();
    modDefaultHeader();
    verification();
    indexZero();
    attribution();
    checks();
    names();
    travel(root);
    whichFileMatrix(root);
    modWriter(root);
    groupRules();
    levelLimits();
    std::cout << "cartographer_aiprofile: all passed\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
