#include "ai/ai.h"
#include "ai/taretail.h"
#include "ai/builtinprofile.h"
#include "ai/classicprofile.h"

#include <cstdlib>
#include <climits>

#include "hpi/hpi.h"
#include "sim/detmath.h"
#include "sim/footprint.h"
#include "sim/retailai.h"
#include "util/devenv.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace ta::ai {

Profile loadProfile(const ta::hpi::Vfs& vfs, const std::string& name) {
    Profile prof;
    // ai/<name>.txt, falling back to default.txt when the named one is absent.
    std::string path = "ai/" + name + ".txt";
    if (name.empty() || !vfs.has(path)) path = "ai/default.txt";
    if (!vfs.has(path)) return prof;
    auto b = vfs.read(path);
    applyProfileText(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()), prof);
    return prof;
}

DiffParams paramsFor(Difficulty d) {
    switch (d) {
        // Defensive: local interception without offensive waves, and production with
        // no AI-imposed limits -- every idle producer may act each think and spending
        // is held to income by the affordability gate and the low-mana throttle alone.
        // 256 bounds one think's orders for the wire frame, not the pace.
        case Difficulty::Defensive: return {30, 5, 256, 100, false, 0, true};
        // Defensive's behaviour, plus Absurd's 2x income cheat (incomeMultFor).
        case Difficulty::AbsurdlyDefensive: return {30, 5, 256, 100, false, 0, true};
        // Sluggish: reacts slowly, builds up slowly, and only commits once it has
        // gathered a sizeable group -- so it's passive and beatable. No raiding.
        case Difficulty::Easy:
        case Difficulty::Passive: return {90, 4, 1, 50,  true,  0, false};
        // Fast, army-heavy, and aggressive: reacts often, musters a large army before
        // the big push, harasses with raids meanwhile, and pushes bigger unit limits.
        case Difficulty::Hard:   return {20, 6, 8, 150, true,  4, false};
        // Hard's behaviour, plus a 2x income cheat applied to the sim (incomeMultFor).
        case Difficulty::Absurd: return {20, 6, 8, 150, true,  4, false};
        // Classic takes Normal's (unused) knobs: its ClassicBrain has none.
        case Difficulty::Normal:
        default:                 return {30, 5, 3, 100, true,  3, false};
    }
}

Controller::Controller(int player, const ta::sim::TypeRegistry& registry,
                       const Profile&, uint32_t seed, Difficulty difficulty,
                       std::vector<std::pair<float, float>>)
    : player_(player),
      diff_(difficulty == Difficulty::Passive ? Difficulty::Easy : difficulty),
      stayHome_(difficulty == Difficulty::Passive) {
    // Every level runs Total Annihilation's own AI (TotalA.exe port, ai/taretail.h):
    // RETAIL EASY/RETAIL/RETAIL HARD exactly; the engine's levels map onto its plans
    // (Easy -> easy, Normal -> medium, Hard/Absurd -> hard; Absurd keeps its 2x income)
    // and the defensive levels never launch strike groups.
    const int d = retailLevel(diff_) ? retailDifficulty(diff_)
                : diff_ == Difficulty::Easy ? 0
                : (diff_ == Difficulty::Hard || diff_ == Difficulty::Absurd) ? 2 : 1;
    retail_ = std::make_unique<TaRetailBrain>(player, seed, registry, d);
    retail_->setAggressive(!(diff_ == Difficulty::Defensive || diff_ == Difficulty::AbsurdlyDefensive ||
                             stayHome_));
}

void Controller::setClassicProfile(const ClassicProfileScript& script) {
    retail_->useProfile(script.text);
}

Controller::Controller(Controller&&) noexcept = default;
Controller::~Controller() = default;

void Controller::tick(const ta::sim::World& world, uint32_t simTick, const CommandSink& sink) {
    retail_->tick(world, simTick, sink);
}

}  // namespace ta::ai
