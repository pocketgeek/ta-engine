#pragma once

// Cartographer's restart arguments, for applying a new GAME DATA FOLDER
// (client/datafolder.h; View > Preferences). Pure, tested in tools/datafolder_test.cpp.

#include <string>
#include <vector>

namespace cart {

// The launch's arguments that still mean something in the new process (--out, and
// --data, which a restart never meets because it greys the setting), with the map
// named at launch replaced by `document` -- the open map's saved file, or the
// installed map's name, or nothing for an unsaved new map. The one-shot options go:
// --new/--world/--random made the map that is open now, and --save/--bundle/--stamp/
// --shot/--shot-tool are headless runs that never reach Preferences. The value counts
// mirror Session::parseArguments.
inline std::vector<std::string> restartArguments(const std::vector<std::string>& args, const std::string& document) {
    struct Opt { const char* name; int values; bool keep; };
    static constexpr Opt kOptions[] = {
        {"--data", 1, true},  {"--out", 1, true},    {"--save", 1, false}, {"--bundle", 1, false},
        {"--stamp", 3, false}, {"--new", 1, false},  {"--world", 1, false}, {"--random", 0, false},
        {"--shot", 1, false},  {"--shot-tool", 1, false},
    };
    std::vector<std::string> out;
    for (size_t i = 0; i < args.size(); ++i) {
        const Opt* opt = nullptr;
        for (const auto& o : kOptions) if (args[i] == o.name) opt = &o;
        if (!opt) {
            if (args[i].empty() || args[i][0] != '-') continue;   // the launch's map: replaced below
            out.push_back(args[i]);                                  // an unknown flag: pass it on
            continue;
        }
        // parseArguments only consumes an option when all its values are there.
        if (i + size_t(opt->values) >= args.size()) { if (opt->keep) out.push_back(args[i]); continue; }
        if (opt->keep) out.insert(out.end(), args.begin() + std::ptrdiff_t(i), args.begin() + std::ptrdiff_t(i + 1 + size_t(opt->values)));
        i += size_t(opt->values);
    }
    if (!document.empty()) out.push_back(document);
    return out;
}

}  // namespace cart
