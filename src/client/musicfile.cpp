#include "client/musicfile.h"

#include "hpi/hpi.h"
#include "tdf/tdf.h"
#include "util/u8path.h"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>

// The decoders: single-file, public domain / MIT-0 (src/vendor/dr_libs, src/vendor/stb).
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "vendor/dr_libs/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "vendor/dr_libs/dr_flac.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "vendor/stb/stb_vorbis.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
// stb_vorbis leaves single-letter helper macros defined.
#undef L
#undef C
#undef R

namespace tak::music {

namespace {

// Down to maxRate (SDL's converter), keeping the channels. Never up: the mixers step
// through a track at its own rate.
bool capRate(Pcm16& pcm, int maxRate, std::string* why) {
    if (pcm.rate <= maxRate) return true;
    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, AUDIO_S16SYS, Uint8(pcm.channels), pcm.rate, AUDIO_S16SYS,
                          Uint8(pcm.channels), maxRate) < 0) {
        if (why) *why = SDL_GetError();
        return false;
    }
    const size_t bytes = pcm.samples.size() * 2;
    std::vector<uint8_t> work(bytes * size_t(std::max(cvt.len_mult, 1)));
    std::memcpy(work.data(), pcm.samples.data(), bytes);
    cvt.buf = work.data();
    cvt.len = int(bytes);
    if (cvt.needed && SDL_ConvertAudio(&cvt) != 0) {
        if (why) *why = SDL_GetError();
        return false;
    }
    const size_t out = cvt.needed ? size_t(cvt.len_cvt) : bytes;
    pcm.samples.assign(out / 2 / size_t(pcm.channels) * size_t(pcm.channels), 0);
    std::memcpy(pcm.samples.data(), work.data(), pcm.samples.size() * 2);
    pcm.rate = maxRate;
    return true;
}

std::optional<Pcm16> fromFrames(const int16_t* data, uint64_t frames, int channels, int rate) {
    if (!data || frames == 0 || channels < 1 || channels > 8 || rate <= 0) return std::nullopt;
    Pcm16 pcm;
    pcm.samples.assign(data, data + frames * uint64_t(channels));
    pcm.rate = rate;
    pcm.channels = channels;
    return pcm;
}

std::optional<Pcm16> decodeWav(const std::vector<uint8_t>& bytes, std::string* why) {
    SDL_AudioSpec spec{};
    Uint8* buf = nullptr;
    Uint32 len = 0;
    SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), int(bytes.size()));
    if (!rw || !SDL_LoadWAV_RW(rw, 1, &spec, &buf, &len)) {
        if (why) *why = SDL_GetError();
        return std::nullopt;
    }
    SDL_AudioCVT cvt;
    if (spec.channels < 1 || spec.channels > 8 || spec.freq <= 0 ||
        SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16SYS, spec.channels, spec.freq) < 0) {
        SDL_FreeWAV(buf);
        if (why) *why = "unsupported WAV format";
        return std::nullopt;
    }
    std::vector<uint8_t> work(size_t(len) * size_t(std::max(cvt.len_mult, 1)));
    std::memcpy(work.data(), buf, len);
    SDL_FreeWAV(buf);
    cvt.buf = work.data();
    cvt.len = int(len);
    if (cvt.needed && SDL_ConvertAudio(&cvt) != 0) {
        if (why) *why = SDL_GetError();
        return std::nullopt;
    }
    const size_t out = cvt.needed ? size_t(cvt.len_cvt) : len;
    Pcm16 pcm;
    pcm.samples.assign(out / 2 / size_t(spec.channels) * size_t(spec.channels), 0);
    std::memcpy(pcm.samples.data(), work.data(), pcm.samples.size() * 2);
    pcm.rate = spec.freq;
    pcm.channels = spec.channels;
    return pcm;
}

std::optional<Pcm16> decodeAny(const std::vector<uint8_t>& bytes, std::string* why) {
    const auto starts = [&](const char* magic) {
        const size_t n = std::strlen(magic);
        return bytes.size() >= n && std::memcmp(bytes.data(), magic, n) == 0;
    };
    if (starts("RIFF")) return decodeWav(bytes, why);
    if (starts("fLaC")) {
        unsigned channels = 0, rate = 0;
        drflac_uint64 frames = 0;
        drflac_int16* data = drflac_open_memory_and_read_pcm_frames_s16(bytes.data(), bytes.size(), &channels, &rate,
                                                                         &frames, nullptr);
        auto pcm = fromFrames(data, frames, int(channels), int(rate));
        drflac_free(data, nullptr);
        if (!pcm && why) *why = "not a FLAC file this can play";
        return pcm;
    }
    if (starts("OggS")) {
        int channels = 0, rate = 0;
        short* data = nullptr;
        const int frames = stb_vorbis_decode_memory(bytes.data(), int(bytes.size()), &channels, &rate, &data);
        auto pcm = frames > 0 ? fromFrames(data, uint64_t(frames), channels, rate) : std::nullopt;
        std::free(data);
        if (!pcm && why) *why = "not an Ogg Vorbis file this can play";
        return pcm;
    }
    // MP3 has no fixed magic (an ID3 tag, or straight into frames): try it last.
    drmp3_config config{};
    drmp3_uint64 frames = 0;
    drmp3_int16* data = drmp3_open_memory_and_read_pcm_frames_s16(bytes.data(), bytes.size(), &config, &frames, nullptr);
    auto pcm = fromFrames(data, frames, int(config.channels), int(config.sampleRate));
    drmp3_free(data, nullptr);
    if (!pcm && why) *why = "not a WAV, MP3, FLAC or Ogg Vorbis file";
    return pcm;
}

std::mutex customMutex;
std::map<int, std::string> customTracks;

}  // namespace

std::optional<Pcm16> decode(const std::vector<uint8_t>& bytes, int maxRate, std::string* why) {
    if (bytes.empty()) {
        if (why) *why = "empty file";
        return std::nullopt;
    }
    auto pcm = decodeAny(bytes, why);
    if (pcm && !capRate(*pcm, maxRate, why)) return std::nullopt;
    return pcm;
}

void setCustom(const std::map<int, std::string>& tracks) {
    std::lock_guard<std::mutex> lock(customMutex);
    customTracks = tracks;
}

std::string customFile(int track) {
    std::lock_guard<std::mutex> lock(customMutex);
    const auto it = customTracks.find(track);
    return it == customTracks.end() ? std::string() : it->second;
}

std::optional<Pcm16> loadTrack(const hpi::Vfs& vfs, int track, int maxRate, bool custom) {
    if (const std::string file = custom ? customFile(track) : std::string(); !file.empty()) {
        std::ifstream in(tak::u8path(file), std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string why;
        if (auto pcm = in.bad() || bytes.empty() ? std::nullopt : decode(bytes, maxRate, &why)) {
            std::fprintf(stderr, "music: track %d plays %s\n", track, file.c_str());
            return pcm;
        }
        std::fprintf(stderr, "music: custom track %d (%s) will not play: %s -- playing the original\n", track,
                     file.c_str(), bytes.empty() ? "cannot read the file" : why.c_str());
    }
    std::vector<uint8_t> bytes;
    try { bytes = vfs.read("music/track" + std::to_string(track) + ".wav"); } catch (...) { return std::nullopt; }
    std::string why;
    auto pcm = decode(bytes, maxRate, &why);
    if (!pcm) std::fprintf(stderr, "music: track %d will not play: %s\n", track, why.c_str());
    return pcm;
}

std::vector<SideTracks> sideTracks(const hpi::Vfs& vfs) {
    std::vector<SideTracks> out;
    try {
        const auto bytes = vfs.read("gamedata/sidedata.tdf");
        const auto root = tak::tdf::parseText(std::string(bytes.begin(), bytes.end()), "gamedata/sidedata.tdf");
        for (const auto& key : root.childOrder) {
            const tak::tdf::Node* side = root.child(key);
            if (!side) continue;
            SideTracks st;
            st.side = side->valueOr("name", "");
            std::transform(st.side.begin(), st.side.end(), st.side.begin(), [](unsigned char c) { return char(std::toupper(c)); });
            std::istringstream list(side->valueOr("musictracks", ""));
            for (int t; list >> t;) if (t >= 1 && t <= 99) st.tracks.push_back(t);
            if (!st.side.empty() && !st.tracks.empty()) out.push_back(std::move(st));
        }
    } catch (const std::exception&) {}
    return out;
}

}  // namespace tak::music
