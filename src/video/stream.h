#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "util/audiolayout.h"
#include "video/streamcursor.h"

namespace tak::video {
// Where Stream::start() sends the stream (the key follows it): YouTube's RTMPS ingest.
// Stream::audio() takes PCM at the game's mix rate (util/audiolayout.h kMixRate).
inline constexpr int kAudioRate = tak::audio::kMixRate;   // 44100
inline constexpr const char* kYouTubeIngest = "rtmps://a.rtmps.youtube.com:443/live2/";
struct StreamConfig {
    int width = 1280, height = 720, fps = 30, bitrateKbps = 6000;
    // Empty means automatic hardware selection with a CPU fallback.
    std::string encoder;
    std::string key; // Memory only. Never persisted or logged.
    // Non-empty: also save the SAME encoded packets to a fragmented MP4 in this folder
    // (created if missing), TAK-YYYYMMDD-HHMMSS.mp4, one file per connection. A local
    // failure stops only the copy; the stream continues.
    std::string localCopyDir;
};
struct StreamStatus {
    bool active = false;
    std::string state = "OFF", encoder;
    uint64_t frames = 0, dropped = 0, replaced = 0, bytes = 0;
    // Microseconds the worker has spent handing packets to the connection. It waits
    // there only when the upload cannot take them as fast as they are encoded.
    uint64_t sendMicros = 0;
    int width = 0, height = 0, fps = 30;
    // Local copy: "" when off, else "SAVING LOCAL COPY", "LOCAL COPY SAVED" or a
    // "LOCAL COPY STOPPED/FAILED - ..." reason; localFile is the newest file's path.
    std::string local, localFile;
};
// One application-owned encoder. All codec/network operations run on its worker.
// Video/audio producers never wait for that worker or allocate on the audio thread.
class Stream {
public:
    Stream();
    ~Stream();
    bool start(const StreamConfig&);
    // Local FLV integration test/recording sink, never exposed as a network URL.
    bool startRecording(const StreamConfig&, const std::string& path);
#ifdef TAK_STREAM_TESTING
    void testDelayOnce(int milliseconds);
    bool testDelayInProgress() const;
    size_t testRetainedVideoBytes() const;
    // The newest encoded picture as RGBA, after scaling and cursor stamping (the
    // letterboxed inner picture, before colour conversion). Empty before the first.
    std::vector<uint8_t> testLastPicture(int& w, int& h) const;
    bool startTestEndpoint(const StreamConfig&, const std::string& url, const std::string& caFile);
    // The local copy's writes fail, as a full disk would, `packets` packets after it has
    // written its `keyframes`-th video keyframe. Counting keyframes rather than packets
    // makes what is already saved independent of the machine's speed: past the second
    // keyframe the copy holds one finished fragment (a GOP, 2 s), however many video
    // frames a slow encoder dropped meanwhile.
    void testFailLocalAfter(int keyframes, int packets);
#endif
    void stop(); // nonblocking; destruction joins after interrupting network I/O
    bool active() const;
    StreamStatus status() const;
    bool video(const uint8_t* rgba, int width, int height, int pitch);
    // Transfer packed RGBA ownership, returning a reusable buffer to the producer.
    bool video(std::vector<uint8_t>& rgba, int width, int height);
    void audio(const int16_t* samples, int frames, int channels); // kAudioRate Hz
    // The hardware cursor for the NEXT video() submission (video/streamcursor.h); it
    // stays set until replaced, and a default StreamCursor shows none. The worker
    // stamps it in after scaling, so it costs the caller a copy of a shared_ptr.
    void cursor(const StreamCursor&);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
