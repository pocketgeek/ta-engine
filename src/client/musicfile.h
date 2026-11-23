#pragma once

// Music: decoding, and CUSTOM MUSIC (the AUDIO page). Every original track the game plays
// -- music/track<n>.wav: the title screen and lobby's, each side's in game -- is loaded
// through loadTrack(), which plays the player's own file for that track when one is
// chosen, and the original otherwise (or when the file will not play).
// Display only: nothing here reaches the simulation or another player.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tak::hpi { class Vfs; }

namespace tak::music {

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
// `custom` allows it), else music/track<n>.wav from the VFS. nullopt when neither plays.
std::optional<Pcm16> loadTrack(const hpi::Vfs& vfs, int track, int maxRate, bool custom = true);

// Each side's original tracks, as this install lists them (gamedata/sidedata.tdf
// musictracks), in that file's order: ARAMON, TAROS, ... .
struct SideTracks {
    std::string side;
    std::vector<int> tracks;
};
std::vector<SideTracks> sideTracks(const hpi::Vfs& vfs);

// The track the title screen and the lobby play. They always play the original:
// a custom file for it plays only in games.
constexpr int kMenuTrack = 15;

}  // namespace tak::music
