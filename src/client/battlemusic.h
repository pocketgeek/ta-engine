#pragma once

// In-game music by battle status, as Total Annihilation plays it (docs/retail-music.md).
// Pure logic, no audio: the host feeds it combat events and the clock, and it says
// which track to start, when to fade and when to stop. Display only -- nothing here
// reaches the simulation, so its random track pick need not be deterministic.
//
// Retail evidence (TotalA.exe):
//   - 16 logical tracks; default categories L1-7 Battle, L8-16 Building (table 0x490fe0).
//   - Battle monitor 0x494e70: a 30-slot ring of one-second buckets; +1 per damage
//     event where the local player is attacker or victim (0x489de6), +5 per kill
//     credited to the local player (0x4869eb). Sampled when more than 30 ticks have
//     passed; nothing happens until the sample counter exceeds 10.
//     Building -> Battle: (sum > 50 or last-5 > 30) and the player owns > 30 units.
//     Battle -> Building: sum < 10 and last-5 == 0 and counter > 60. Counter resets on
//     every switch. (Retail never resets the ring or counter between games -- a game
//     that ended in battle leaves the next one unable to switch; we reset at start.)
//   - Category change 0x4ce690: acts in Custom mode, or for Victory/Defeat; fades out
//     over 18 steps of 2 ticks, waits 120 ticks more before Building, then picks a
//     track (0x4cdb40 / 0x4cde96) and starts it at full volume.
//   - Pick: k = max(rand() & 15, 1); walk forward cyclically from the current track and
//     take the k-th track of the wanted category.
//   - Modes (registry cdmode): PlayAll 1, Random 2, Repeat 3, Custom 4 (default 4).

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>

namespace ta::battlemusic {

enum class Category : uint8_t { Building = 0, Battle = 1, Victory = 2, Defeat = 3, Silence = 4, Unused = 5 };
enum class Mode : uint8_t { PlayAll = 1, Random = 2, Repeat = 3, Custom = 4 };

inline constexpr int kTracks = 16;              // logical tracks 1..16 -> music/(L+1).mp3
inline constexpr int kRingSlots = 30;           // one-second buckets
inline constexpr int kSampleTicks = 30;         // "more than 30 ticks" since the last sample
inline constexpr int kFadeSteps = 18, kFadeStepTicks = 2;
inline constexpr int kBuildingDelayTicks = 120; // 4 s extra wait before Building music

inline std::array<Category, kTracks + 1> defaultCategories() {
    std::array<Category, kTracks + 1> c{};
    c[0] = Category::Unused;
    for (int l = 1; l <= kTracks; ++l) c[l] = l <= 7 ? Category::Battle : Category::Building;
    return c;
}

// What the host should do this frame.
struct Action {
    enum class Kind : uint8_t { None, Play, Stop } kind = Kind::None;
    int track = 0;        // Play: logical track 1..16
    float gain = 1.0f;    // current fade gain (apply every frame)
};

class BattleMusic {
public:
    BattleMusic() : cats_(defaultCategories()) {}

    void setMode(Mode m) { mode_ = m; }
    void setRepeatTrack(int l) { repeatTrack_ = (l >= 1 && l <= kTracks) ? l : 1; }
    void setCategory(int track, Category c) { if (track >= 1 && track <= kTracks) cats_[track] = c; }
    Mode mode() const { return mode_; }
    Category requested() const { return req_; }
    int currentTrack() const { return cur_; }

    // A game begins: clear the monitor (retail's missing reset), request Building, pick.
    Action startGame(uint32_t now) {
        ring_.fill(0);
        slot_ = 0;
        counter_ = 0;
        lastSample_ = now;
        fade_ = Fade::None;
        req_ = Category::Building;
        return pick();
    }
    // Game over / back to the menu: stop and go silent.
    Action stop() {
        req_ = Category::Silence;
        fade_ = Fade::None;
        cur_ = 0;
        return {Action::Kind::Stop, 0, 1.0f};
    }

    void onDamage() { ring_[slot_] += 1; }   // local player attacker or victim, not repair
    void onKill() { ring_[slot_] += 5; }     // kill credited to the local player

    // Request a category directly (console/debug: Victory, Defeat).
    Action request(Category c) { return change(c); }

    // Once per frame. trackEnded: the playing track finished (or none is playing).
    Action tick(uint32_t now, int localUnits, bool trackEnded) {
        Action a;
        clock_ = now;   // a change this frame starts its fade now
        if (req_ != Category::Silence && now - lastSample_ > uint32_t(kSampleTicks)) {
            lastSample_ = now;
            ++counter_;
            if (counter_ > 10) {
                int total = 0, recent = 0;
                for (int v : ring_) total += v;
                for (int i = 1; i <= 5; ++i) recent += ring_[(slot_ - i + kRingSlots) % kRingSlots];
                Category want = req_;
                if (req_ == Category::Building && (total > 50 || recent > 30) && localUnits > 30)
                    want = Category::Battle;
                else if (req_ == Category::Battle && total < 10 && recent == 0 && counter_ > 60)
                    want = Category::Building;
                if (want != req_) {
                    counter_ = 0;
                    a = change(want);
                }
            }
            slot_ = (slot_ + 1) % kRingSlots;
            ring_[slot_] = 0;
        }
        // Fade/delay state machine.
        if (fade_ == Fade::Out) {
            int steps = int(now - fadeStart_) / kFadeStepTicks;
            if (steps >= kFadeSteps) {
                if (req_ == Category::Building) { fade_ = Fade::Delay; fadeStart_ = now; gain_ = 0; }
                else { fade_ = Fade::None; gain_ = 1; a = pick(); }
            } else {
                gain_ = 1.0f - float(steps) / kFadeSteps;
            }
        } else if (fade_ == Fade::Delay) {
            gain_ = 0;
            if (now - fadeStart_ >= uint32_t(kBuildingDelayTicks)) { fade_ = Fade::None; gain_ = 1; a = pick(); }
        } else if (trackEnded && a.kind == Action::Kind::None && req_ != Category::Silence) {
            a = pick();
        }
        a.gain = gain_;
        return a;
    }

    // Monitor state, for tests.
    int counter() const { return counter_; }

private:
    enum class Fade : uint8_t { None, Out, Delay };

    Action change(Category c) {
        Category old = req_;
        req_ = c;
        bool acts = mode_ == Mode::Custom || c == Category::Victory || c == Category::Defeat;
        if (!acts) return {};
        if (c == Category::Silence) return stop();
        if (old == Category::Silence || fade_ != Fade::None || cur_ == 0) {
            fade_ = Fade::None;
            gain_ = 1;
            return pick();
        }
        fade_ = Fade::Out;
        fadeStart_ = clock_;
        return {};
    }

    Action pick() {
        if (req_ == Category::Silence) return stop();
        int next = 0;
        if (mode_ == Mode::Custom || req_ == Category::Victory || req_ == Category::Defeat) {
            int k = std::max(int(rng_() & 15), 1);
            int found = 0, t = cur_ < 1 ? 0 : cur_;
            bool any = false;
            for (int l = 1; l <= kTracks; ++l) if (cats_[l] == req_) any = true;
            if (!any) return stop();
            while (found < k) {
                t = t % kTracks + 1;
                if (cats_[t] == req_) ++found;
            }
            next = t;
        } else if (mode_ == Mode::PlayAll) {
            next = cur_ % kTracks + 1;   // retail stops after 16; we wrap (spec 6)
        } else if (mode_ == Mode::Random) {
            next = int(rng_() % kTracks) + 1;
        } else {
            next = repeatTrack_;
        }
        cur_ = next;
        gain_ = 1;
        return {Action::Kind::Play, next, 1.0f};
    }

    std::array<Category, kTracks + 1> cats_;
    std::array<int, kRingSlots> ring_{};
    int slot_ = 0, counter_ = 0;
    uint32_t lastSample_ = 0, fadeStart_ = 0, clock_ = 0;
    Mode mode_ = Mode::Custom;
    int repeatTrack_ = 1;
    Category req_ = Category::Silence;
    Fade fade_ = Fade::None;
    float gain_ = 1;
    int cur_ = 0;
    std::minstd_rand rng_{std::random_device{}()};
};

}  // namespace ta::battlemusic
