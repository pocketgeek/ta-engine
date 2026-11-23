#include "video/stream.h"
#include "video/rgbascale.h"
#include "video/streamcursor.h"
#include "video/youtubebitrate.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include "net/tls.h"
#include <unistd.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace tak::video {
namespace {
using Clock = std::chrono::steady_clock;
int64_t micros() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }
// FFmpeg's protocol errors may contain the URL (including the key). Suppress
// logging only on the streaming worker; Bink diagnostics remain available.
thread_local bool privateStreamLog = false;
// Set when the TLS layer rejected the server's certificate during this attempt, so
// the panel can say so. Only the "tls" protocol's own error text is inspected (it
// never holds the URL), in a stack buffer that is wiped and never stored.
thread_local bool tlsCertificateRejected = false;
bool certificateError(const char* text) {
    std::string lower(text);
    for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    // OpenSSL: "...:certificate verify failed"; Schannel names the certificate too.
    const bool found = lower.find("certificate") != std::string::npos;
    std::fill(lower.begin(), lower.end(), '\0');
    return found;
}
void logCallback(void* obj, int level, const char* fmt, va_list args) {
    if (!privateStreamLog) { av_log_default_callback(obj, level, fmt, args); return; }
    if (level > AV_LOG_ERROR || !obj) return;
    const AVClass* cls = *static_cast<AVClass**>(obj);
    if (!cls || !cls->item_name || std::strcmp(cls->item_name(obj), "tls") != 0) return;
    char text[256];
    va_list copy;
    va_copy(copy, args);
    std::vsnprintf(text, sizeof text, fmt, copy);
    va_end(copy);
    if (certificateError(text)) tlsCertificateRejected = true;
#ifdef TAK_STREAM_TESTING
    std::fprintf(stderr, "TLS ERROR %s", text); // the test prints why a handshake failed
#endif
    std::fill(std::begin(text), std::end(text), '\0');
}
void check(int result) { if (result < 0) throw result; }
#ifdef __APPLE__
// FFmpeg's OpenSSL TLS takes trust anchors only from a file, and OpenSSL never reads
// the keychain: each connection attempt writes the current system anchors to a
// private (0600) temporary file, deleted once the attempt has connected or failed.
// Public certificates only; nothing about the stream goes in it.
class SystemRootsFile {
public:
    SystemRootsFile() {
        const std::string pem = tak::net::systemRootsPem();
        if (pem.empty()) return;
        std::error_code error;
        std::string name = (std::filesystem::temp_directory_path(error) / "tak-roots-XXXXXX").string();
        if (error) return;
        const int fd = mkstemp(name.data());
        if (fd < 0) return;
        size_t done = 0;
        while (done < pem.size()) {
            const ssize_t n = ::write(fd, pem.data() + done, pem.size() - done);
            if (n <= 0) break;
            done += size_t(n);
        }
        ::close(fd);
        if (done == pem.size()) path_ = name; else ::unlink(name.c_str());
    }
    ~SystemRootsFile() { if (!path_.empty()) ::unlink(path_.c_str()); }
    SystemRootsFile(const SystemRootsFile&) = delete;
    SystemRootsFile& operator=(const SystemRootsFile&) = delete;
    const std::string& path() const { return path_; }
private:
    std::string path_;
};
#endif
#ifdef _WIN32
// A condition-variable timeout here sleeps whole 15.6 ms scheduler quanta, even
// under timeBeginPeriod(1): 1 ms takes ~15 ms and 16 ms ~31 ms, the length of
// two 60 FPS frame slots. A high-resolution waitable timer (Windows 10 1803+)
// wakes within ~0.5 ms. Without one, pacing falls back to the condition variable.
class PaceTimer {
public:
    PaceTimer(): timer_(CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS)) {}
    ~PaceTimer() { if (timer_) CloseHandle(timer_); }
    PaceTimer(const PaceTimer&) = delete;
    PaceTimer& operator=(const PaceTimer&) = delete;
    bool wait(int64_t us) {
        LARGE_INTEGER due; due.QuadPart = -us * 10; // relative, 100 ns units
        return timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) &&
            WaitForSingleObject(timer_, 1000) == WAIT_OBJECT_0;
    }
private:
    HANDLE timer_;
};
#endif
struct HardwareUnavailable {};
struct EncodeFailure {};
// The optional local copy: a second muxer fed references to the encoder's own packets
// (nothing is encoded twice). Fragmented MP4 -- an empty moov, then one moof+mdat per
// two-second keyframe interval -- so a crash or a power cut still leaves a playable
// file holding every finished fragment. Never throws: the stream must not depend on it.
struct LocalCopy {
    AVFormatContext* mux = nullptr;
    AVStream *vs = nullptr, *as = nullptr;
    AVPacket* packet = nullptr;
    std::function<void(bool)> closed; // a finished copy: whether the trailer reached the disk
    void close(bool finish) {
        if (mux) {
            bool ok = finish && av_write_trailer(mux) >= 0 && mux->pb && !mux->pb->error;
            if (mux->pb && avio_closep(&mux->pb) < 0) ok = false;
            avformat_free_context(mux); mux = nullptr;
            if (finish && closed) closed(ok);
        }
        av_packet_free(&packet);
    }
    ~LocalCopy() { close(true); }
};
struct Encoder {
    AVFormatContext* mux = nullptr;
    AVCodecContext *v = nullptr, *a = nullptr;
    AVStream *vs = nullptr, *as = nullptr;
    AVFrame *vf = nullptr, *af = nullptr, *hw = nullptr;
    AVBufferRef *device = nullptr, *pool = nullptr;
    SwsContext* scale = nullptr;
    SwrContext* resample = nullptr;
    AVAudioFifo* fifo = nullptr;
    bool header = false;
    LocalCopy copy;
    ~Encoder() {
        copy.close(true);
        sws_freeContext(scale); swr_free(&resample);
        av_audio_fifo_free(fifo);
        av_frame_free(&vf); av_frame_free(&af); av_frame_free(&hw);
        avcodec_free_context(&v); avcodec_free_context(&a);
        av_buffer_unref(&pool); av_buffer_unref(&device);
        if (mux) { if (mux->pb) avio_closep(&mux->pb); avformat_free_context(mux); }
    }
};
bool valid(const StreamConfig& c) {
    return (c.width >= 2 && c.height >= 2 && c.width <= 8192 && c.height <= 8192 &&
            c.width % 2 == 0 && c.height % 2 == 0) &&
        (c.fps == 30 || c.fps == 60) && c.bitrateKbps >= 1000 && c.bitrateKbps <= 80000 &&
        c.localCopyDir.find("://") == std::string::npos &&
        (c.encoder.empty() || c.encoder == "libx264" || c.encoder == "h264_nvenc" ||
         c.encoder == "h264_amf" || c.encoder == "h264_qsv" ||
         c.encoder == "h264_vaapi" || c.encoder == "h264_videotoolbox" ||
         c.encoder == "hevc_nvenc" || c.encoder == "hevc_amf" || c.encoder == "hevc_qsv" ||
         c.encoder == "hevc_vaapi" || c.encoder == "hevc_videotoolbox");
}
}
struct Stream::Impl {
#ifdef TAK_STREAM_TESTING
    std::string testCaFile;
#endif
    std::atomic<bool> running{false}, cancel{false};
    std::atomic<int64_t> deadline{0};
    std::atomic<uint64_t> dropped{0}, replaced{0};
#ifdef TAK_STREAM_TESTING
    std::atomic<int> testDelay{0};
    std::atomic<bool> testDelaying{false};
    std::atomic<int> testCopyFailKeys{-1}, testCopyFail{-1};   // testFailLocalAfter
#endif
    mutable std::mutex stateMutex;
    StreamStatus state;
    std::thread worker;
    std::mutex inputMutex;
    std::condition_variable wake;
    std::vector<uint8_t> pixels;
    int sourceW = 0, sourceH = 0;
    bool fresh = false;
    // The hardware cursor (video/streamcursor.h): `nextCursor` is the latest one the
    // client set; each submitted frame takes a copy (`pixelsCursor`, under inputMutex),
    // so the cursor always travels with the picture it was captured beside.
    std::mutex cursorMutex;
    StreamCursor nextCursor, pixelsCursor;
#ifdef TAK_STREAM_TESTING
    mutable std::mutex testPictureMutex;
    std::vector<uint8_t> testPicture;
    int testPictureW = 0, testPictureH = 0;
#endif
    // Fixed two-second ring, stereo S16 at the game's 44100 Hz mix rate.
    static constexpr size_t kRing = 2 * kAudioRate;
    std::mutex audioMutex;
    std::array<int16_t, kRing * 2> pcm{};
    size_t read = 0, count = 0;
    // How far the worker's current attempt got, for the failure reason. Worker only.
    enum class Stage { Setup, Connect, Live } stage = Stage::Setup;
    bool youtube = false; // the destination is a YouTube ingest. Worker only.

#ifdef TAK_STREAM_TESTING
    void testKeep(const uint8_t* picture, ptrdiff_t pitch, int w, int h) {
        std::lock_guard lock(testPictureMutex);
        testPicture.resize(size_t(w)*h*4);
        for (int y=0;y<h;++y) std::memcpy(testPicture.data()+size_t(y)*w*4, picture+ptrdiff_t(y)*pitch, size_t(w)*4);
        testPictureW = w; testPictureH = h;
    }
#endif
    void report(const std::string& s, const std::string& encoder = {}) {
        std::lock_guard lock(stateMutex); state.state = s;
        if (!encoder.empty()) state.encoder = encoder;
    }
    void reportCopy(const std::string& s, const std::string& file = {}) {
        std::lock_guard lock(stateMutex); state.local = s;
        if (!file.empty()) state.localFile = file;
    }
    // TAK-YYYYMMDD-HHMMSS.mp4 in `dir` (local time), "-2", "-3"... if that name exists
    // (a CPU-fallback restart can begin a second file within the same second).
    static std::string copyName(std::string dir) {
        if (dir.back() != '/' && dir.back() != '\\') dir += '/';
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "TAK-%Y%m%d-%H%M%S", &tm);
        std::string file = dir + stamp + ".mp4";
        for (int n = 2; n < 100 && avio_check(("file:" + file).c_str(), 0) >= 0; ++n)
            file = dir + stamp + "-" + std::to_string(n) + ".mp4";
        return file;
    }
    // Opened once the stream's own header is out: a copy only of what is going live.
    void openCopy(Encoder& e, const StreamConfig& c) {
        if (c.localCopyDir.empty()) return;
        auto& l = e.copy;
        std::string file;
        std::error_code ec;
        try {
            const std::filesystem::path dir(std::u8string(c.localCopyDir.begin(), c.localCopyDir.end()));
            std::filesystem::create_directories(dir, ec);
            if (!std::filesystem::is_directory(dir, ec)) { reportCopy("LOCAL COPY FAILED - CANNOT CREATE THE VIDEOS FOLDER"); return; }
            file = copyName(c.localCopyDir);
        } catch (...) { reportCopy("LOCAL COPY FAILED - CANNOT CREATE THE VIDEOS FOLDER"); return; }
        auto fail = [&](const char* why, bool created) {
            l.close(false);
            if (created) {
                try { std::filesystem::remove(std::filesystem::path(std::u8string(file.begin(), file.end())), ec); } catch (...) {}
            }
            reportCopy(why);
        };
        if (avformat_alloc_output_context2(&l.mux, nullptr, "mp4", nullptr) < 0 || !l.mux) {
            l.mux = nullptr; return fail("LOCAL COPY FAILED - MP4 WRITER MISSING", false);
        }
        l.mux->max_interleave_delta = 250000;
        l.vs = avformat_new_stream(l.mux, nullptr); l.as = avformat_new_stream(l.mux, nullptr);
        l.packet = av_packet_alloc();
        if (!l.vs || !l.as || !l.packet || avcodec_parameters_from_context(l.vs->codecpar, e.v) < 0 ||
            avcodec_parameters_from_context(l.as->codecpar, e.a) < 0)
            return fail("LOCAL COPY FAILED - OUT OF MEMORY", false);
        l.vs->time_base = e.v->time_base; l.as->time_base = e.a->time_base;
        // hvc1 (parameter sets in the sample entry) is the HEVC tag Apple players accept.
        if (e.v->codec_id == AV_CODEC_ID_HEVC) l.vs->codecpar->codec_tag = MKTAG('h','v','c','1');
        if (avio_open(&l.mux->pb, ("file:" + file).c_str(), AVIO_FLAG_WRITE) < 0)
            return fail("LOCAL COPY FAILED - VIDEOS FOLDER NOT WRITABLE", false);
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
        const int result = avformat_write_header(l.mux, &opts); av_dict_free(&opts);
        if (result < 0 || l.mux->pb->error) return fail("LOCAL COPY FAILED - CANNOT WRITE THE FILE", true);
        l.closed = [this](bool ok) { reportCopy(ok ? "LOCAL COPY SAVED" : "LOCAL COPY STOPPED - WRITE FAILED"); };
        reportCopy("SAVING LOCAL COPY", file);
    }
    // A reference to the stream's packet, not a copy of its data. A failed write (a
    // full disk, a removed drive) closes the copy and leaves the stream running.
    void copyPacket(Encoder& e, AVCodecContext* codec, AVStream* stream, const AVPacket* packet) {
        auto& l = e.copy;
        if (!l.mux) return;
        AVStream* out = stream == e.vs ? l.vs : l.as;
        bool ok = av_packet_ref(l.packet, packet) >= 0;
#ifdef TAK_STREAM_TESTING
        if (ok && testCopyFailKeys > 0 && stream == e.vs && (packet->flags & AV_PKT_FLAG_KEY)) --testCopyFailKeys;
        if (ok && testCopyFailKeys == 0 && testCopyFail >= 0 && testCopyFail-- == 0) ok = false;
#endif
        if (ok) {
            av_packet_rescale_ts(l.packet, codec->time_base, out->time_base);
            l.packet->stream_index = out->index;
            ok = av_interleaved_write_frame(l.mux, l.packet) >= 0 && !l.mux->pb->error;
        }
        av_packet_unref(l.packet);
        if (!ok) { l.close(false); reportCopy("LOCAL COPY STOPPED - WRITE FAILED (DISK FULL?)"); }
    }
    static int interrupted(void* arg) {
        auto& p = *static_cast<Impl*>(arg);
        return p.cancel.load() || (p.deadline.load() && micros() > p.deadline.load());
    }
    void timeout() { deadline = micros() + 5000000; }
    bool openVideo(Encoder& e, const StreamConfig& c, const std::string& name, const char* node = nullptr) {
        avcodec_free_context(&e.v); av_buffer_unref(&e.pool); av_buffer_unref(&e.device);
        auto* codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec) return false;
        e.v = avcodec_alloc_context3(codec);
        if (!e.v) return false;
        e.v->width = c.width; e.v->height = c.height;
        e.v->time_base = {1, c.fps}; e.v->framerate = {c.fps, 1};
        // Never more than YouTube takes for this codec (video/youtubebitrate.h): the panel
        // already keeps to it, so this only guards a caller that does not.
        const int kbps = youtube ? std::min(c.bitrateKbps,
            youtubeRecommendedKbps(c.width, c.height, c.fps, streamCodec(name, c.width, c.height))) : c.bitrateKbps;
        e.v->bit_rate = int64_t(kbps) * 1000;
        e.v->rc_max_rate = e.v->bit_rate; e.v->rc_buffer_size = int(e.v->bit_rate * 2);
        e.v->gop_size = c.fps * 2; e.v->max_b_frames = 0;
        e.v->thread_count = 4;
        e.v->pix_fmt = name == "libx264" ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NV12;
        // NVENC accepts packed RGB and performs its own GPU color conversion.
        // Avoid a CPU RGB->NV12 pass over every pixel of a native ultrawide frame.
        if (name == "h264_nvenc" || name == "hevc_nvenc") e.v->pix_fmt = AV_PIX_FMT_RGBA;
        e.v->color_range = AVCOL_RANGE_MPEG; e.v->colorspace = AVCOL_SPC_BT709;
        e.v->color_primaries = AVCOL_PRI_BT709; e.v->color_trc = AVCOL_TRC_BT709;
        e.v->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (name == "h264_vaapi" || name == "hevc_vaapi") {
            if (av_hwdevice_ctx_create(&e.device, AV_HWDEVICE_TYPE_VAAPI, node, nullptr, 0) < 0) return false;
            e.pool = av_hwframe_ctx_alloc(e.device);
            if (!e.pool) return false;
            auto* f = reinterpret_cast<AVHWFramesContext*>(e.pool->data);
            f->format = AV_PIX_FMT_VAAPI; f->sw_format = AV_PIX_FMT_NV12;
            f->width = c.width; f->height = c.height; f->initial_pool_size = 8;
            if (av_hwframe_ctx_init(e.pool) < 0) return false;
            e.v->hw_frames_ctx = av_buffer_ref(e.pool); e.v->pix_fmt = AV_PIX_FMT_VAAPI;
        }
        AVDictionary* opts = nullptr;
        if (name == "libx264") {
            av_dict_set(&opts, "preset", "veryfast", 0); av_dict_set(&opts, "tune", "zerolatency", 0);
            av_dict_set(&opts, "x264-params", "nal-hrd=cbr:force-cfr=1:scenecut=0", 0);
        } else if (name == "h264_nvenc" || name == "hevc_nvenc") {
            av_dict_set(&opts, "preset", "p4", 0); av_dict_set(&opts, "tune", "ll", 0);
            av_dict_set(&opts, "rc", "cbr", 0);
        } else if (name == "h264_amf" || name == "hevc_amf") {
            av_dict_set(&opts, "usage", "ultralowlatency", 0); av_dict_set(&opts, "rc", "cbr", 0);
        } else if (name == "h264_videotoolbox" || name == "hevc_videotoolbox") {
            av_dict_set(&opts, "realtime", "1", 0); av_dict_set(&opts, "allow_sw", "0", 0);
        }
        int result = avcodec_open2(e.v, codec, &opts); av_dict_free(&opts);
        return result >= 0;
    }
    std::string chooseVideo(Encoder& e, const StreamConfig& c) {
        std::vector<std::string> names;
        if (!c.encoder.empty()) names.push_back(c.encoder);
        else {
#ifdef __APPLE__
            names = {"h264_videotoolbox"};
#elif defined(_WIN32)
            names = {"h264_nvenc", "h264_amf", "h264_qsv"};
#else
            names = {"h264_nvenc", "h264_vaapi"};
#endif
        }
        // H.264 hardware commonly stops at 4096 pixels per axis. Preserve
        // native dimensions by trying hardware HEVC before considering the CPU.
        const bool large = c.width > 4096 || c.height > 4096;
        if (large && c.encoder.empty()) {
#ifdef __APPLE__
            names.push_back("hevc_videotoolbox");
#elif defined(_WIN32)
            names.insert(names.end(), {"hevc_nvenc", "hevc_amf", "hevc_qsv"});
#else
            names.insert(names.end(), {"hevc_nvenc", "hevc_vaapi"});
#endif
        }
        if (!large || c.encoder == "libx264") names.push_back("libx264");
        for (const auto& name : names) {
            if (cancel) throw AVERROR_EXIT;
            if (name == "h264_vaapi" || name == "hevc_vaapi") {
                for (int i = 128; i < 144; ++i) {
                    std::string node = "/dev/dri/renderD" + std::to_string(i);
                    if (openVideo(e, c, name, node.c_str())) return name;
                }
            } else if (openVideo(e, c, name)) return name;
        }
        if (large) throw HardwareUnavailable{};
        throw AVERROR_ENCODER_NOT_FOUND;
    }
    void open(Encoder& e, const StreamConfig& c, const std::string& dest, bool local) {
        check(avformat_alloc_output_context2(&e.mux, nullptr, "flv", nullptr));
        e.mux->max_interleave_delta = 250000;
        e.mux->interrupt_callback = local ? AVIOInterruptCB{nullptr,nullptr} : AVIOInterruptCB{interrupted, this};
        const auto name = chooseVideo(e, c);
        report("CONNECTING", name);
        auto* ac = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!ac) throw AVERROR_ENCODER_NOT_FOUND;
        e.a = avcodec_alloc_context3(ac); if (!e.a) throw AVERROR(ENOMEM);
        e.a->sample_rate = 44100; e.a->sample_fmt = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&e.a->ch_layout, 2);
        e.a->bit_rate = 128000; e.a->time_base = {1, 44100}; e.a->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(avcodec_open2(e.a, ac, nullptr));
        e.vs = avformat_new_stream(e.mux, nullptr); e.as = avformat_new_stream(e.mux, nullptr);
        if (!e.vs || !e.as) throw AVERROR(ENOMEM);
        e.vs->time_base = e.v->time_base; e.as->time_base = e.a->time_base;
        check(avcodec_parameters_from_context(e.vs->codecpar, e.v));
        check(avcodec_parameters_from_context(e.as->codecpar, e.a));
        e.vf = av_frame_alloc(); e.af = av_frame_alloc(); e.hw = av_frame_alloc();
        if (!e.vf || !e.af || !e.hw) throw AVERROR(ENOMEM);
        e.vf->width = c.width; e.vf->height = c.height;
        e.vf->format = e.pool ? AV_PIX_FMT_NV12 : e.v->pix_fmt;
        check(av_frame_get_buffer(e.vf, 32));
        e.af->format = e.a->sample_fmt; e.af->sample_rate = e.a->sample_rate;
        e.af->nb_samples = e.a->frame_size;
        check(av_channel_layout_copy(&e.af->ch_layout, &e.a->ch_layout));
        check(av_frame_get_buffer(e.af, 0));
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        check(swr_alloc_set_opts2(&e.resample, &stereo, AV_SAMPLE_FMT_FLTP, 44100,
                                &stereo, AV_SAMPLE_FMT_S16, kAudioRate, 0, nullptr));
        check(swr_init(e.resample));
        e.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, 4096);
        if (!e.fifo) throw AVERROR(ENOMEM);
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "rw_timeout", "5000000", 0);
        av_dict_set(&opts, "tls_verify", "1", 0);
#if !defined(_WIN32) && !defined(__APPLE__)
        // Common distro trust bundles; static OpenSSL's build prefix isn't the
        // machine's certificate directory. Never ship a frozen trust bundle.
        for (const char* path : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem"}) {
            if (std::filesystem::exists(path)) { av_dict_set(&opts, "ca_file", path, 0); break; }
        }
#endif
#ifdef __APPLE__
        // Without it OpenSSL trusts nothing, and every RTMPS connection fails as
        // TLS CERTIFICATE NOT TRUSTED.
        const SystemRootsFile roots;
        if (!roots.path().empty()) av_dict_set(&opts, "ca_file", roots.path().c_str(), 0);
#endif
#ifdef TAK_STREAM_TESTING
        if (!testCaFile.empty()) av_dict_set(&opts, "ca_file", testCaFile.c_str(), 0);
#endif
        stage = Stage::Connect;
        timeout();
        int result = avio_open2(&e.mux->pb, dest.c_str(), AVIO_FLAG_WRITE, &e.mux->interrupt_callback, &opts);
        av_dict_free(&opts); check(result);
        av_dict_set(&opts, "flvflags", local ? "0" : "no_duration_filesize", 0);
        timeout(); result = avformat_write_header(e.mux, &opts); av_dict_free(&opts); check(result);
        e.header = true; deadline = 0;
        openCopy(e, c);
    }
    void packets(Encoder& e, AVCodecContext* codec, AVStream* stream, AVFrame* frame) {
        if (avcodec_send_frame(codec, frame) < 0) throw EncodeFailure{};
        AVPacket* packet = av_packet_alloc(); if (!packet) throw AVERROR(ENOMEM);
        int result = 0;
        bool writeFailure = false;
        while ((result = avcodec_receive_packet(codec, packet)) >= 0) {
            copyPacket(e, codec, stream, packet);
            av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            const int size = packet->size;
            const int64_t sending = micros();
            timeout(); result = av_interleaved_write_frame(e.mux, packet); deadline = 0;
            const int64_t waited = micros() - sending;
            if (result < 0) { writeFailure = true; break; }
            { std::lock_guard lock(stateMutex); state.bytes += size; state.sendMicros += uint64_t(std::max<int64_t>(waited, 0)); }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            if (writeFailure) check(result);
            throw EncodeFailure{};
        }
    }
    void audioFrame(Encoder& e, int64_t pts) {
        while (av_audio_fifo_size(e.fifo) < e.af->nb_samples) {
            std::array<int16_t, 512> input{};
            { std::lock_guard lock(audioMutex);
              for (int i = 0; i < 256 && count; ++i) {
                  input[i*2] = pcm[read*2]; input[i*2+1] = pcm[read*2+1];
                  read = (read + 1) % kRing; --count;
              }
            }
            float left[1152], right[1152];
            uint8_t* out[] = {reinterpret_cast<uint8_t*>(left), reinterpret_cast<uint8_t*>(right)};
            const uint8_t* in[] = {reinterpret_cast<const uint8_t*>(input.data())};
            int n = swr_convert(e.resample, out, 1152, in, 256); check(n);
            check(av_audio_fifo_write(e.fifo, reinterpret_cast<void**>(out), n));
        }
        check(av_frame_make_writable(e.af));
        check(av_audio_fifo_read(e.fifo, reinterpret_cast<void**>(e.af->data), e.af->nb_samples));
        e.af->pts = pts; packets(e, e.a, e.as, e.af);
    }
    void run(StreamConfig c, std::string dest, bool local) {
        privateStreamLog = true;
        youtube = isYouTubeIngest(dest);
        int attempts = 0;
        int64_t start = 0;
        RgbaScaler resize; // outlives reconnects: its helper threads start once
        std::vector<uint8_t> resized;
#ifdef _WIN32
        PaceTimer pace;
#endif
        while (!cancel) {
            try {
                Encoder e;
                stage = Stage::Setup; tlsCertificateRejected = false;
                open(e, c, dest, local);
                { std::lock_guard lock(audioMutex); count = read = 0; }
                report(local ? "RECORDING" : c.encoder.empty() && e.v->codec_id == AV_CODEC_ID_H264 &&
                    std::string(e.v->codec->name) == "libx264" ? "LIVE - CPU FALLBACK" : "LIVE");
                stage = Stage::Live;
                start = micros();
                int64_t vpts = 0, apts = 0, nextKey = 0;
                bool haveVideo = false;
                std::vector<uint8_t> frame;
                int width = 0, height = 0;
                StreamCursor frameCursor;
                CursorStamper stamper;
                while (!cancel) {
#ifdef TAK_STREAM_TESTING
                    if (int delay = testDelay.exchange(0)) {
                        std::unique_lock lock(inputMutex);
                        testDelaying = true;
                        wake.wait_for(lock, std::chrono::milliseconds(delay), [&]{return cancel.load();});
                        testDelaying = false;
                    }
#endif
                    if (cancel) break;
                    int64_t elapsed = micros() - start;
                    // A busy encoder is not a broken connection. Skip expired video
                    // slots rather than trying to encode a growing backlog.
                    const int64_t due = elapsed * c.fps / 1000000;
                    if (due > vpts) { dropped += due - vpts; vpts = due; }
                    // Catch audio up before the next expensive video conversion. A
                    // long I/O stall discards old sound instead of delaying it forever.
                    const int64_t audioDue = elapsed * 44100 / 1000000;
                    if (audioDue - apts > 44100) {
                        apts = (audioDue / e.af->nb_samples) * e.af->nb_samples;
                        av_audio_fifo_reset(e.fifo);
                        std::lock_guard lock(audioMutex);
                        read = count = 0;
                    }
                    while (apts <= audioDue && !cancel) {
                        audioFrame(e, apts); apts += e.af->nb_samples;
                    }
                    // Stop may interrupt audio catch-up. Do not submit a newer
                    // video timestamp after the audio timeline has stopped.
                    if (cancel) break;
                    if (vpts * 1000000 / c.fps <= elapsed) {
                        check(av_frame_make_writable(e.vf));
                        { std::lock_guard lock(inputMutex);
                          if (fresh) { frame.swap(pixels); width = sourceW; height = sourceH; fresh = false;
                                      frameCursor = std::move(pixelsCursor); pixelsCursor = {}; }
                        }
                        if (!frame.empty()) {
                            check(av_frame_make_writable(e.vf));
                            // Letterbox instead of distorting non-16:9 windows.
                            int dw = c.width, dh = int(int64_t(height) * dw / width) & ~1;
                            if (dh > c.height) { dh = c.height; dw = int(int64_t(width) * dh / height) & ~1; }
                            dw = std::max(dw, 2); dh = std::max(dh, 2);
                            ptrdiff_t strides[4] = {e.vf->linesize[0], e.vf->linesize[1], e.vf->linesize[2], e.vf->linesize[3]};
                            if (dw != c.width || dh != c.height)
                                check(av_image_fill_black(e.vf->data, strides, static_cast<AVPixelFormat>(e.vf->format), AVCOL_RANGE_MPEG, c.width, c.height));
                            const int x = ((c.width-dw)/2)&~1, y = ((c.height-dh)/2)&~1;
                            const bool packed = e.vf->format == AV_PIX_FMT_RGBA;
                            uint8_t* out[4] = {e.vf->data[0] + y*e.vf->linesize[0]+x*(packed?4:1),
                                e.vf->data[1] ? e.vf->data[1]+y/2*e.vf->linesize[1]+(e.vf->format==AV_PIX_FMT_NV12 ? x : x/2) : nullptr,
                                e.vf->data[2] ? e.vf->data[2]+y/2*e.vf->linesize[2]+x/2 : nullptr, nullptr};
                            // Resize with the threaded scaler (straight into the frame for
                            // NVENC's RGBA); swscale then only converts colour, at output size.
                            // The hardware cursor goes in after the resize, at output
                            // resolution, into the picture (never the letterbox bars).
                            if (packed) {
                                resize.scale(frame.data(), width, height, width*4, out[0], dw, dh, e.vf->linesize[0]);
                                stamper.stamp(out[0], e.vf->linesize[0], dw, dh, frameCursor);
#ifdef TAK_STREAM_TESTING
                                testKeep(out[0], e.vf->linesize[0], dw, dh);
#endif
                            } else {
                                uint8_t* source = frame.data();
                                if (dw != width || dh != height) {
                                    resized.resize(size_t(dw)*dh*4);
                                    resize.scale(frame.data(), width, height, width*4, resized.data(), dw, dh, dw*4);
                                    source = resized.data();
                                }
                                stamper.stamp(source, ptrdiff_t(dw)*4, dw, dh, frameCursor);
#ifdef TAK_STREAM_TESTING
                                testKeep(source, ptrdiff_t(dw)*4, dw, dh);
#endif
                                e.scale = sws_getCachedContext(e.scale, dw, dh, AV_PIX_FMT_RGBA,
                                    dw, dh, static_cast<AVPixelFormat>(e.vf->format), SWS_BILINEAR, nullptr, nullptr, nullptr);
                                if (!e.scale) throw AVERROR(ENOMEM);
                                const int* coeff = sws_getCoefficients(SWS_CS_ITU709);
                                check(sws_setColorspaceDetails(e.scale, coeff, 1, coeff, 0, 0, 1<<16, 1<<16));
                                const uint8_t* in[] = {source, nullptr, nullptr, nullptr}; int pitch[] = {dw*4,0,0,0};
                                check(sws_scale(e.scale, in, pitch, 0, dh, out, e.vf->linesize));
                            }
                            haveVideo = true;
                            frame.clear();
                        }
                        if (!haveVideo) { // valid black frame until first capture
                            ptrdiff_t stride[4] = {e.vf->linesize[0],e.vf->linesize[1],e.vf->linesize[2],0};
                            check(av_image_fill_black(e.vf->data, stride, static_cast<AVPixelFormat>(e.vf->format), AVCOL_RANGE_MPEG,c.width,c.height));
                        }
                        e.vf->pts = vpts++;
                        e.vf->pict_type = e.vf->pts >= nextKey ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
                        if (e.vf->pict_type == AV_PICTURE_TYPE_I) nextKey = e.vf->pts + c.fps * 2;
                        if (e.pool) {
                            av_frame_unref(e.hw); check(av_hwframe_get_buffer(e.pool, e.hw, 0));
                            check(av_hwframe_transfer_data(e.hw, e.vf, 0)); e.hw->pts = e.vf->pts;
                            e.hw->pict_type = e.vf->pict_type;
                            packets(e, e.v, e.vs, e.hw);
                        } else packets(e, e.v, e.vs, e.vf);
                        { std::lock_guard lock(stateMutex); ++state.frames; }
                    }
                    const int64_t next = std::min(vpts*1000000/c.fps, apts*1000000/44100);
                    const int64_t wait = std::max<int64_t>(0, start+next-micros());
                    // Behind: go straight on. Even a zero condition-variable timeout
                    // sleeps a 15.6 ms quantum on Windows, a whole frame slot lost.
                    if (wait == 0) continue;
#ifdef _WIN32
                    // At most one frame slot, so stop still lands within ~33 ms.
                    if (pace.wait(wait)) continue;
#endif
                    std::unique_lock lock(inputMutex);
                    wake.wait_for(lock, std::chrono::microseconds(wait), [&]{return cancel.load();});
                }
                if (local) { // files can be finalized even after stop interrupts network
                    deadline = 0; e.mux->interrupt_callback = {nullptr,nullptr};
                    packets(e,e.v,e.vs,nullptr); packets(e,e.a,e.as,nullptr);
                    av_write_trailer(e.mux);
                }
            } catch (HardwareUnavailable) {
                report("FAILED - NO GPU ENCODER FOR THIS SIZE"); break;
            } catch (EncodeFailure) {
                if (cancel) break;
                if (c.encoder == "libx264") { report("FAILED - ENCODER ERROR"); break; }
                if (c.width > 4096 || c.height > 4096) {
                    report("FAILED - GPU ENCODER ERROR; TRY 4K"); break;
                }
                c.encoder = "libx264";
                report("RESTARTING WITH CPU ENCODER");
            } catch (int) {
                if (cancel) break;
                // Three retries per outage: a connection that stayed up a minute
                // earned a fresh set, so drops hours apart never end the stream.
                if (stage == Stage::Live && micros() - start > 60000000) attempts = 0;
                const bool certificate = tlsCertificateRejected;
                if (local || ++attempts > 3) {
                    report(local || stage == Stage::Setup ? "FAILED - CHECK KEY, CONNECTION OR ENCODER" :
                           certificate ? "FAILED - TLS CERTIFICATE NOT TRUSTED" :
                           stage == Stage::Connect ? "FAILED - CANNOT CONNECT - CHECK KEY, NETWORK" :
                                                     "FAILED - CONNECTION DROPPED - CHECK NETWORK");
                    break;
                }
                report("RECONNECTING " + std::to_string(attempts) + "/3" +
                       (stage == Stage::Setup ? "" : certificate ? " - TLS CERTIFICATE" :
                        stage == Stage::Connect ? " - CANNOT CONNECT" : " - CONNECTION DROPPED"));
                std::unique_lock lock(inputMutex);
                wake.wait_for(lock, std::chrono::seconds(1 << attempts), [&]{return cancel.load();});
            } catch (...) { report("FAILED - STREAMING RESOURCE ERROR"); break; }
        }
        std::fill(c.key.begin(), c.key.end(), '\0'); std::fill(dest.begin(),dest.end(),'\0');
        if (cancel) report("OFF");
        {
            std::lock_guard lock(inputMutex);
            std::vector<uint8_t>().swap(pixels);
            fresh = false; sourceW = sourceH = 0;
            running = false;
        }
    }
    bool start(const StreamConfig& c, std::string destination, bool local) {
        if (running || !valid(c)) return false;
        if (worker.joinable()) worker.join();
        { std::lock_guard lock(stateMutex); state = {}; state.state = "STARTING"; state.width=c.width; state.height=c.height; state.fps=c.fps; }
        { std::lock_guard lock(inputMutex); pixels.clear(); fresh = false; }
        { std::lock_guard lock(audioMutex); read = count = 0; }
        cancel = false; dropped = 0; replaced = 0; running = true;
        try { worker = std::thread([this,c,d=std::move(destination),local]() mutable {run(c,std::move(d),local);}); }
        catch (...) { running=false; report("FAILED - CANNOT START WORKER"); return false; }
        return true;
    }
};
Stream::Stream(): p_(std::make_unique<Impl>()) {
    static std::once_flag once;
    std::call_once(once, []{ av_log_set_callback(logCallback); avformat_network_init(); });
}
Stream::~Stream() { stop(); if (p_->worker.joinable()) p_->worker.join(); }
bool Stream::start(const StreamConfig& c) {
    if (c.key.empty() || c.key.size() > 256 || c.key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos) return false;
    return p_->start(c, kYouTubeIngest + c.key, false);
}
bool Stream::startRecording(const StreamConfig& c, const std::string& path) {
    if (path.empty() || path.find("://") != std::string::npos) return false;
    return p_->start(c, "file:" + path, true);
}
#ifdef TAK_STREAM_TESTING
void Stream::testDelayOnce(int milliseconds) { p_->testDelay = milliseconds; }
void Stream::testFailLocalAfter(int keyframes, int packets) {
    p_->testCopyFail = packets;
    p_->testCopyFailKeys = std::max(0, keyframes);
}
bool Stream::testDelayInProgress() const { return p_->testDelaying; }
std::vector<uint8_t> Stream::testLastPicture(int& w, int& h) const {
    std::lock_guard lock(p_->testPictureMutex); w = p_->testPictureW; h = p_->testPictureH; return p_->testPicture;
}
size_t Stream::testRetainedVideoBytes() const {
    std::lock_guard lock(p_->inputMutex); return p_->pixels.capacity();
}
bool Stream::startTestEndpoint(const StreamConfig& c, const std::string& url, const std::string& caFile) {
    if (active() || !url.starts_with("rtmps://localhost:")) return false;
    p_->testCaFile = caFile;
    return p_->start(c,url,false);
}
#endif
void Stream::stop() { if (active()) p_->report("STOPPING"); p_->cancel = true; p_->wake.notify_all(); }
bool Stream::active() const { return p_->running; }
StreamStatus Stream::status() const {
    std::lock_guard lock(p_->stateMutex); auto s = p_->state;
    s.active = p_->running; s.dropped = p_->dropped; s.replaced = p_->replaced; return s;
}
bool Stream::video(const uint8_t* rgba, int w, int h, int pitch) {
    if (!active() || !rgba || w <= 0 || h <= 0 || w > 8192 || h > 8192 || pitch < w*4) return false;
    std::unique_lock lock(p_->inputMutex, std::try_to_lock);
    if (!lock.owns_lock()) { ++p_->replaced; return false; }
    if (!p_->running || p_->cancel) return false;
    if (p_->fresh) ++p_->replaced;
    p_->pixels.resize(size_t(w)*h*4);
    for (int y=0;y<h;++y) std::memcpy(p_->pixels.data()+size_t(y)*w*4, rgba+size_t(y)*pitch, size_t(w)*4);
    p_->sourceW=w; p_->sourceH=h; p_->fresh=true;
    { std::lock_guard cursorLock(p_->cursorMutex); p_->pixelsCursor = p_->nextCursor; }
    return true;
}
bool Stream::video(std::vector<uint8_t>& rgba, int w, int h) {
    if (!active() || w <= 0 || h <= 0 || w > 8192 || h > 8192 || rgba.size() != size_t(w)*h*4) return false;
    std::unique_lock lock(p_->inputMutex, std::try_to_lock);
    if (!lock.owns_lock()) { ++p_->replaced; return false; }
    if (!p_->running || p_->cancel) return false;
    if (p_->fresh) ++p_->replaced;
    p_->pixels.swap(rgba);
    p_->sourceW=w; p_->sourceH=h; p_->fresh=true;
    { std::lock_guard cursorLock(p_->cursorMutex); p_->pixelsCursor = p_->nextCursor; }
    return true;
}
void Stream::cursor(const StreamCursor& c) {
    std::lock_guard lock(p_->cursorMutex); p_->nextCursor = c;
}
void Stream::audio(const int16_t* samples, int frames, int channels) {
    if (!active() || !samples || channels < 1 || channels > 8) return;
    std::unique_lock lock(p_->audioMutex,std::try_to_lock); if (!lock.owns_lock()) return;
    // Any device layout (SDL order) folds to stereo, ITU-style: centre and surrounds
    // at -3 dB, the LFE dropped, each side weighted to unity (util/audiolayout.h).
    const auto fold=tak::audio::downmixMatrix(channels,2);
    for (int f=0;f<frames;++f) {
        // Bounded latency: when the upload stalls, retain the newest audio.
        if (p_->count == Impl::kRing) { p_->read=(p_->read+1)%Impl::kRing; --p_->count; }
        const int16_t* in = samples + f*channels;
        float l=in[0], r=in[0];
        if (channels>1) {
            l=r=0;
            for (int k=0;k<channels;++k) { l+=fold[0][size_t(k)]*in[k]; r+=fold[1][size_t(k)]*in[k]; }
        }
        size_t pos=(p_->read+p_->count)%Impl::kRing;
        p_->pcm[pos*2]=int16_t(l); p_->pcm[pos*2+1]=int16_t(r); ++p_->count;
    }
}
}
