#include <cstdio>
#include "video/bink.h"

#include <algorithm>
#include <cstring>

#ifdef TAK_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace tak::video {

#ifdef TAK_HAVE_FFMPEG

struct BinkVideo::Impl {
    std::vector<uint8_t> data;      // owned .bik bytes, demuxed in place
    size_t pos = 0;

    AVIOContext* avio = nullptr;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* ctx = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int stream = -1;
    int w = 0, h = 0;
    double fps = 30.0;
    int frames = 0;        // total frames (0 = the container did not say)
    bool eofSent = false;

    // ---- audio (optional) -----------------------------------------------------
    AVCodecContext* actx = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* aframe = nullptr;
    int aStream = -1;
    int aRate = 0, aCh = 0;
    std::vector<uint8_t> aBuf;   // decoded interleaved S16 PCM, drained by the caller

    // ---- custom in-memory AVIO ------------------------------------------------
    static int readPacket(void* opaque, uint8_t* buf, int bufSize) {
        auto* s = static_cast<Impl*>(opaque);
        size_t remain = s->data.size() - s->pos;
        if (remain == 0) return AVERROR_EOF;
        int n = int(std::min<size_t>(size_t(bufSize), remain));
        std::memcpy(buf, s->data.data() + s->pos, size_t(n));
        s->pos += size_t(n);
        return n;
    }
    static int64_t seekPacket(void* opaque, int64_t offset, int whence) {
        auto* s = static_cast<Impl*>(opaque);
        if (whence & AVSEEK_SIZE) return int64_t(s->data.size());
        whence &= ~AVSEEK_FORCE;
        int64_t base = whence == SEEK_CUR ? int64_t(s->pos)
                     : whence == SEEK_END ? int64_t(s->data.size()) : 0;
        int64_t p = std::clamp<int64_t>(base + offset, 0, int64_t(s->data.size()));
        s->pos = size_t(p);
        return p;
    }

    // Decode an audio packet (nullptr flushes at EOF): resample every frame it yields
    // to interleaved S16 and append to aBuf for the caller to play.
    void decodeAudio(AVPacket* p) {
        if (aStream < 0 || !actx || !swr || !aframe) return;
        if (avcodec_send_packet(actx, p) < 0) return;
        while (avcodec_receive_frame(actx, aframe) == 0) {
            int maxOut = int(av_rescale_rnd(swr_get_delay(swr, actx->sample_rate) + aframe->nb_samples,
                                            aRate, actx->sample_rate, AV_ROUND_UP));
            if (maxOut > 0) {
                size_t prev = aBuf.size(), cap = size_t(maxOut) * size_t(aCh) * 2;
                aBuf.resize(prev + cap);
                uint8_t* out[1] = {aBuf.data() + prev};
                int got = swr_convert(swr, out, maxOut,
                                      const_cast<const uint8_t**>(aframe->extended_data),
                                      aframe->nb_samples);
                aBuf.resize(prev + (got > 0 ? size_t(got) * size_t(aCh) * 2 : 0));
            }
            av_frame_unref(aframe);
        }
        // Bound the buffer if the caller never drains it (a silent-to-them looping
        // clip): keep at most ~12s so a long hover-loop can't grow it without limit.
        // A caller that plays the audio (the intro) drains every frame, well under this.
        constexpr size_t kCap = size_t(1) << 21;   // 2 MiB
        if (aBuf.size() > kCap) aBuf.erase(aBuf.begin(), aBuf.end() - kCap);
    }

    void teardown() {
        if (sws) { sws_freeContext(sws); sws = nullptr; }
        if (swr) { swr_free(&swr); }
        if (frame) av_frame_free(&frame);
        if (aframe) av_frame_free(&aframe);
        if (pkt) av_packet_free(&pkt);
        if (ctx) avcodec_free_context(&ctx);
        if (actx) avcodec_free_context(&actx);
        if (fmt) avformat_close_input(&fmt);   // does NOT free custom pb
        if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); }
        stream = aStream = -1; w = h = 0; pos = 0; eofSent = false;
        aRate = aCh = 0; aBuf.clear();
    }
    ~Impl() { teardown(); }
};

BinkVideo::BinkVideo() : d_(std::make_unique<Impl>()) {}
BinkVideo::~BinkVideo() = default;
BinkVideo::BinkVideo(BinkVideo&&) noexcept = default;
BinkVideo& BinkVideo::operator=(BinkVideo&&) noexcept = default;

bool BinkVideo::available() { return true; }

bool BinkVideo::open(std::vector<uint8_t> data) {
    close();
    // Silence FFmpeg's own chatter (e.g. libswscale's per-frame "No accelerated colorspace
    // conversion found from yuv420p to rgba" -- our static build has no asm scaler, and the
    // C path is fine for these tiny door clips). Keep genuine errors.
    av_log_set_level(AV_LOG_ERROR);
    d_->data = std::move(data);
    d_->pos = 0;
    if (d_->data.empty()) return false;

    constexpr int kBuf = 1 << 15;
    unsigned char* buffer = static_cast<unsigned char*>(av_malloc(kBuf));
    if (!buffer) return false;
    d_->avio = avio_alloc_context(buffer, kBuf, 0, d_.get(),
                                  &Impl::readPacket, nullptr, &Impl::seekPacket);
    if (!d_->avio) { av_free(buffer); return false; }

    d_->fmt = avformat_alloc_context();
    if (!d_->fmt) { close(); return false; }
    d_->fmt->pb = d_->avio;
    d_->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&d_->fmt, nullptr, nullptr, nullptr) < 0) {
        // avformat_open_input frees fmt on failure; drop our avio too.
        d_->fmt = nullptr;
        close();
        return false;
    }
    if (avformat_find_stream_info(d_->fmt, nullptr) < 0) { close(); return false; }

    for (unsigned i = 0; i < d_->fmt->nb_streams; ++i)
        if (d_->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            d_->stream = int(i); break;
        }
    if (d_->stream < 0) { close(); return false; }

    AVStream* st = d_->fmt->streams[d_->stream];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        // The container parsed but the installed libavcodec has no decoder for this
        // codec -- the usual reason menu/door videos won't play. On Fedora the stock
        // "libavcodec-free" can lack the Bink decoder; the full FFmpeg (RPM Fusion)
        // has it. Log ONCE so a static-doors report is self-diagnosing.
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr, "video: installed FFmpeg has no '%s' decoder -- menu/door "
                         "videos disabled. Install the full FFmpeg (e.g. RPM Fusion's "
                         "'ffmpeg', or 'libavcodec-freeworld').\n",
                         avcodec_get_name(st->codecpar->codec_id));
        }
        close(); return false;
    }
    d_->ctx = avcodec_alloc_context3(dec);
    if (!d_->ctx || avcodec_parameters_to_context(d_->ctx, st->codecpar) < 0
        || avcodec_open2(d_->ctx, dec, nullptr) < 0) { close(); return false; }

    d_->w = d_->ctx->width;
    d_->h = d_->ctx->height;
    AVRational r = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    d_->fps = (r.num && r.den) ? double(r.num) / double(r.den) : 30.0;
    // Total frames: the stream's own count when it has one, else derive it from
    // the duration. Bink files from the retail install carry nb_frames.
    d_->frames = int(st->nb_frames);
    if (d_->frames <= 0 && st->duration > 0 && st->time_base.den > 0)
        d_->frames = int(double(st->duration) * av_q2d(st->time_base) * d_->fps);
    if (d_->frames < 0) d_->frames = 0;
    d_->frame = av_frame_alloc();
    d_->pkt = av_packet_alloc();
    if (!d_->frame || !d_->pkt || d_->w <= 0 || d_->h <= 0) { close(); return false; }

    // Optional audio: open its decoder + a resampler to interleaved S16 at the source
    // rate. Any failure just disables audio (aStream = -1) -- the video still plays.
    for (unsigned i = 0; i < d_->fmt->nb_streams; ++i)
        if (d_->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            d_->aStream = int(i); break;
        }
    if (d_->aStream >= 0) {
        AVStream* as = d_->fmt->streams[d_->aStream];
        const AVCodec* adec = avcodec_find_decoder(as->codecpar->codec_id);
        bool ok = adec != nullptr;
        if (ok) d_->actx = avcodec_alloc_context3(adec);
        if (!d_->actx || avcodec_parameters_to_context(d_->actx, as->codecpar) < 0
            || avcodec_open2(d_->actx, adec, nullptr) < 0)
            ok = false;
        if (ok) {
            d_->aRate = d_->actx->sample_rate;
            d_->aCh = d_->actx->ch_layout.nb_channels > 0 ? d_->actx->ch_layout.nb_channels : 1;
            d_->aframe = av_frame_alloc();
            AVChannelLayout out;
            av_channel_layout_default(&out, d_->aCh);
            if (!d_->aframe
                || swr_alloc_set_opts2(&d_->swr, &out, AV_SAMPLE_FMT_S16, d_->aRate,
                                       &d_->actx->ch_layout, d_->actx->sample_fmt,
                                       d_->actx->sample_rate, 0, nullptr) < 0
                || swr_init(d_->swr) < 0)
                ok = false;
            av_channel_layout_uninit(&out);
        }
        if (!ok) {
            if (d_->swr) swr_free(&d_->swr);
            if (d_->aframe) av_frame_free(&d_->aframe);
            if (d_->actx) avcodec_free_context(&d_->actx);
            d_->aStream = -1; d_->aRate = d_->aCh = 0;
        }
    }
    return true;
}

bool BinkVideo::isOpen() const { return d_->ctx != nullptr; }
void BinkVideo::close() { d_->teardown(); d_->data.clear(); }
int BinkVideo::width() const { return d_->w; }
int BinkVideo::height() const { return d_->h; }
double BinkVideo::fps() const { return d_->fps; }
int BinkVideo::frameCount() const { return d_->frames; }
int BinkVideo::audioRate() const { return d_->aRate; }
int BinkVideo::audioChannels() const { return d_->aCh; }
void BinkVideo::drainAudio(std::vector<uint8_t>& pcm) {
    if (d_->aBuf.empty()) return;
    pcm.insert(pcm.end(), d_->aBuf.begin(), d_->aBuf.end());
    d_->aBuf.clear();
}

bool BinkVideo::nextFrame(std::vector<uint8_t>& rgba) {
    if (!isOpen()) return false;
    for (;;) {
        int r = avcodec_receive_frame(d_->ctx, d_->frame);
        if (r == 0) {
            const int fw = d_->frame->width, fh = d_->frame->height;
            d_->sws = sws_getCachedContext(
                d_->sws, fw, fh, AVPixelFormat(d_->frame->format),
                fw, fh, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!d_->sws) { av_frame_unref(d_->frame); return false; }
            // COLOUR RANGE: swscale's default, which expands the 16..235 the stream
            // is tagged with out to 0..255. Three independent measurements agree:
            //   * FFmpeg reports color_range=tv on these clips.
            //   * Least-squares fitting a free affine YCbCr->RGB decode against the
            //     retail art recovers a luma gain of ~1.16 -- i.e. 255/219.
            //   * Over 12k NEUTRAL pixels (no chroma to confound it) where a door
            //     clip overlaps MainBG 1:1, mean error is 4.1 expanding vs 11.2 not.
            //     Re-fitting a residual gamma on top lands at 0.97, i.e. nothing.
            //
            // Do not "fix" this from a histogram, and do not re-litigate it from how
            // a clip LOOKS against the static art. The decoded luma sits inside
            // 16..235, which argues for full-range content, and a single dark frame
            // can dip to 12, which argues the other way; both are inconclusive, and I
            // changed this twice on them. A seam across a clip's border is not a test
            // either -- I used one and it pointed the right way for the wrong reason.
            // Bink is lossy, so a clip is NEVER a pixel match for art painted behind
            // it: thin high-contrast detail comes back low (the menu's carved gold
            // loses ~24 luma) while flat neutrals land within ~2. A clip that looks
            // wrong over the background is evidence about the CLIP, not the decode.
            // Only compare neutrals, and only at 1:1 with alignment confirmed.
            // sws SIMD over-writes past a tightly-packed row when the width isn't
            // aligned (odd door widths like 155/221), so scale into a properly
            // aligned + padded image, then copy the rows out tightly (pitch fw*4).
            uint8_t* dst[4] = {nullptr, nullptr, nullptr, nullptr};
            int dstLines[4] = {0, 0, 0, 0};
            if (av_image_alloc(dst, dstLines, fw, fh, AV_PIX_FMT_RGBA, 32) < 0) {
                av_frame_unref(d_->frame); return false;
            }
            // Zero it FIRST. av_image_alloc hands back uninitialized heap, and at the
            // doors' ODD widths (GIRL 155, KNIGHT 221) swscale's unscaled yuv420p->rgba
            // converter leaves the final column untouched -- measured against the
            // bundled FFmpeg n7.1.1 the releases ship: exactly one column per frame,
            // 194 of 194 rows on GIRL4, 250 of 250 on KNIGHT4. The copy below then put
            // that heap garbage straight into the texture, which is the corrupt right
            // edge reported on macOS and Windows. It never showed on a Linux dev build
            // because a system FFmpeg (9.x here) writes the column.
            std::memset(dst[0], 0, size_t(dstLines[0]) * size_t(fh));
            sws_scale(d_->sws, d_->frame->data, d_->frame->linesize, 0, fh, dst, dstLines);
            // ...then clamp the edge if this build really did skip that column. Detected
            // rather than assumed, so a build that DOES write it keeps its own pixels.
            //
            // The obvious-looking fix is SWS_ACCURATE_RND, which does make the column
            // get written -- but it is not a rounding tweak here, and it is NOT a colour
            // fix either. Recomputing BT.601 by hand from the raw yuv420p planes of
            // KNIGHT4 frame 0 and scoring both variants against it settles what each one
            // actually is:
            //
            //            vs hand BT.601 + NEAREST chroma   vs hand BT.601 + BILINEAR chroma
            //   BILINEAR          mean 1.00, max 2                  mean 1.48, max 51
            //   +ACCURATE_RND     mean 1.26, max 62                 mean 0.97, max 25
            //
            // Both are correct BT.601 with the SAME matrix and the same expanding range
            // (the frames report color_range=MPEG, matching the note above). The only
            // thing the flag changes is chroma UPSAMPLING: nearest vs interpolated. Our
            // current path is an essentially exact nearest-chroma decode -- max delta 2
            // from hand-computed truth -- so there is no colour error here to fix, and
            // nearest is what a 1999 software Bink decoder did. If smoother chroma is
            // ever wanted it is a deliberate look change belonging behind the SMOOTH
            // MOVIES option, not something to switch on while chasing an edge artifact.
            if (fw >= 2) {
                const size_t last = size_t(fw - 1) * 4, prev = size_t(fw - 2) * 4;
                bool skipped = true;
                for (int y = 0; y < fh && skipped; ++y) {
                    const uint8_t* px = dst[0] + size_t(y) * size_t(dstLines[0]) + last;
                    if (px[0] || px[1] || px[2] || px[3]) skipped = false;
                }
                if (skipped)
                    for (int y = 0; y < fh; ++y) {
                        uint8_t* row = dst[0] + size_t(y) * size_t(dstLines[0]);
                        std::memcpy(row + last, row + prev, 4);
                    }
            }
            rgba.assign(size_t(fw) * size_t(fh) * 4, 0);
            for (int y = 0; y < fh; ++y)
                std::memcpy(rgba.data() + size_t(y) * size_t(fw) * 4,
                            dst[0] + size_t(y) * size_t(dstLines[0]), size_t(fw) * 4);
            av_freep(&dst[0]);
            d_->w = fw; d_->h = fh;
            av_frame_unref(d_->frame);
            return true;
        }
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) return false;
        if (r == AVERROR_EOF) return false;

        // Need more input: pull the next video packet (or flush at EOF).
        int rr = d_->eofSent ? AVERROR_EOF : av_read_frame(d_->fmt, d_->pkt);
        if (rr == AVERROR_EOF || d_->eofSent) {
            if (!d_->eofSent) {
                d_->eofSent = true;
                avcodec_send_packet(d_->ctx, nullptr);
                d_->decodeAudio(nullptr);   // flush the audio decoder too
            }
            continue;   // drain remaining frames, then receive_frame yields EOF
        }
        if (rr < 0) return false;
        if (d_->pkt->stream_index == d_->stream)
            avcodec_send_packet(d_->ctx, d_->pkt);
        else if (d_->pkt->stream_index == d_->aStream)
            d_->decodeAudio(d_->pkt);
        av_packet_unref(d_->pkt);
    }
}

void BinkVideo::rewind() {
    if (!isOpen()) return;
    av_seek_frame(d_->fmt, d_->stream, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(d_->ctx);
    d_->eofSent = false;
}

#else  // ---- no FFmpeg: stub so callers compile + fall back to GAF art ---------

struct BinkVideo::Impl {};
BinkVideo::BinkVideo() = default;
int BinkVideo::frameCount() const { return 0; }
BinkVideo::~BinkVideo() = default;
BinkVideo::BinkVideo(BinkVideo&&) noexcept = default;
BinkVideo& BinkVideo::operator=(BinkVideo&&) noexcept = default;
bool BinkVideo::available() { return false; }
bool BinkVideo::open(std::vector<uint8_t>) { return false; }
bool BinkVideo::isOpen() const { return false; }
void BinkVideo::close() {}
int BinkVideo::width() const { return 0; }
int BinkVideo::height() const { return 0; }
double BinkVideo::fps() const { return 0.0; }
int BinkVideo::audioRate() const { return 0; }
int BinkVideo::audioChannels() const { return 0; }
void BinkVideo::drainAudio(std::vector<uint8_t>&) {}
bool BinkVideo::nextFrame(std::vector<uint8_t>&) { return false; }
void BinkVideo::rewind() {}

#endif

}  // namespace tak::video
