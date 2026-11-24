#pragma once

// Music: decoding, and CUSTOM MUSIC (the AUDIO page). Every original track the game plays
// -- logical CD track L, music/(L+1).mp3 (docs/retail-music.md) -- is loaded
// through loadTrack(), which plays the player's own file for that track when one is
// chosen, and the original otherwise (or when the file will not play).
// Display only: nothing here reaches the simulation or another player.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ta::hpi { class Vfs; }

namespace ta::music {

// Interleaved signed 16-bit PCM at its own rate.
struct Pcm16 {
    std::vector<int16_t> samples;
    int rate = 0;
    int channels = 0;
};

// The audio a file holds -- WAV (anything SDL loads), MP3, FLAC or Ogg Vorbis, told apart
// by content -- as S16 at its own rate, converted down (never up) to at most maxRate, with
// 1..8 channels. On failure, why says what went wrong.
std::optional<Pcm16> decode(const std::vector<uint8_t>& bytes, int maxRate, std::string* why = nullptr);

// The replacements in use (Settings::customMusicTracks), handed over by the hosts when
// settings load and on every change. Safe from any thread.
void setCustom(const std::map<int, std::string>& tracks);
// The file playing for original track n, or "" for the original.
std::string customFile(int track);

// Original track n, ready to play: the custom file if one is set and decodes (and
// `custom` allows it), else music/(n+1).mp3 from the VFS. nullopt when neither plays.
std::optional<Pcm16> loadTrack(const hpi::Vfs& vfs, int track, int maxRate, bool custom = true);

// The original tracks this install has, by retail's default category (a group's
// `side` is its name: BATTLE then BUILDING), logical track numbers in order.
struct SideTracks {
    std::string side;
    std::vector<int> tracks;
};
std::vector<SideTracks> sideTracks(const hpi::Vfs& vfs);

// Retail's music library: 16 logical CD tracks (docs/retail-music.md section 1).
constexpr int kLogicalTracks = 16;

// The track the title screen and the lobby play. They always play the original:
// a custom file for it plays only in games.
constexpr int kMenuTrack = 15;

}  // namespace ta::music
