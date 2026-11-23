#pragma once

// Decoder for the retail Total Annihilation: Kingdoms `.bik` (Bink Video) menu
// clips -- the animated doors on the front screen. Bink is proprietary (RAD Game
// Tools / binkw32.dll); this uses FFmpeg's clean-room reverse-engineered decoder
// (libavcodec's `binkvideo`), which is GPL-compatible.
//
// Built with real decoding only when TAK_HAVE_FFMPEG is defined (FFmpeg found at
// configure time). Otherwise every open() fails and callers fall back to the GAF
// door art. The header pulls in no FFmpeg types (pimpl), so it compiles either way.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tak::video {

class BinkVideo {
public:
    BinkVideo();
    ~BinkVideo();
    BinkVideo(const BinkVideo&) = delete;
    BinkVideo& operator=(const BinkVideo&) = delete;
    BinkVideo(BinkVideo&&) noexcept;
    BinkVideo& operator=(BinkVideo&&) noexcept;

    // True only when this build has FFmpeg compiled in.
    static bool available();

    // Open an in-memory .bik. Returns false if unavailable, unreadable, or not a
    // Bink video stream. The buffer is taken by value (moved) and kept for the
    // lifetime of the decoder (FFmpeg demuxes from it in place).
    bool open(std::vector<uint8_t> data);
    bool isOpen() const;
    void close();

    int width() const;
    int height() const;
    double fps() const;   // frames per second (from the stream; ~30 for the doors)
    // Total frames, or 0 if the container does not say. Lets a caller drive
    // playback position from something other than wall-clock -- the loading
    // screen ties it to load progress, so a fast load still shows the whole clip
    // instead of its first second.
    int frameCount() const;

    // Decode the next frame into `rgba` (resized to width*height*4, RGBA8888).
    // Returns false at end-of-stream (call rewind() to loop). Audio packets demuxed
    // along the way are decoded into the audio buffer (see drainAudio).
    bool nextFrame(std::vector<uint8_t>& rgba);

    // Audio (if the clip has an audio stream and FFmpeg is present). Decoded to
    // interleaved signed-16 PCM at the source rate/channel count. 0 => no audio.
    int audioRate() const;
    int audioChannels() const;
    // Move any audio decoded so far (during nextFrame) out into `pcm` (appended).
    // Feed it to an SDL audio queue to play the clip's sound alongside the video.
    void drainAudio(std::vector<uint8_t>& pcm);

    // Seek back to the first frame (for looping the hover-idle clip).
    void rewind();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace tak::video
