#pragma once
// Decode a whole compressed audio file to interleaved 16-bit PCM.
//
// This exists for TA's MUSIC. Kingdoms shipped its soundtrack as
// music/track<N>.wav, which SDL loads directly; TA ships music/<N>.mp3 (18
// tracks, 0..17 on a GOG install), which SDL cannot decode at all -- so the
// music player found no tracks and the game was silent.
//
// It reuses the FFmpeg that is already vendored and static-linked for the Bink
// menu videos (src/video/bink.cpp), rather than adding a second media
// dependency: the build enables the mp3 demuxer/decoder alongside bink, and the
// avformat -> avcodec -> swresample path here is the same one BinkVideo already
// uses for binkaudio.
//
// Whole-file decode, deliberately: a TA track is a couple of minutes of stereo,
// a few tens of MB as PCM, decoded once when the track starts. Streaming would
// buy memory this does not need at the cost of a decode thread feeding the
// mixer callback.

#include <cstdint>
#include <string>
#include <vector>

namespace ta::audiodec {

struct Pcm {
    std::vector<int16_t> samples;   // interleaved
    int rate = 0;
    int channels = 0;
    bool ok() const { return !samples.empty() && rate > 0 && channels > 0; }
};

// Decode `bytes` (a complete file image) to interleaved S16. `origin` only
// names the source in error messages. Returns an empty Pcm on any failure --
// an unsupported format, a truncated file, a decoder that was not built in.
Pcm decodeToS16(const std::vector<uint8_t>& bytes, const std::string& origin);

}  // namespace ta::audiodec
