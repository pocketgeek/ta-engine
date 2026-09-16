#include "video/audiodec.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <cstdio>
#include <cstring>

namespace ta::audiodec {
namespace {

// Feed avformat from memory rather than a path: music comes through the VFS,
// which may serve it out of an archive. The buffer must be av_malloc'd and is
// owned by the AVIOContext afterwards (FFmpeg may reallocate it).
struct MemSource {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
};

int memRead(void* opaque, uint8_t* buf, int want) {
    auto* m = static_cast<MemSource*>(opaque);
    if (m->pos >= m->size) return AVERROR_EOF;
    size_t n = m->size - m->pos;
    if (n > size_t(want)) n = size_t(want);
    std::memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return int(n);
}

int64_t memSeek(void* opaque, int64_t offset, int whence) {
    auto* m = static_cast<MemSource*>(opaque);
    if (whence == AVSEEK_SIZE) return int64_t(m->size);
    int64_t base = whence == SEEK_CUR ? int64_t(m->pos)
                 : whence == SEEK_END ? int64_t(m->size)
                                      : 0;
    int64_t p = base + offset;
    if (p < 0 || p > int64_t(m->size)) return -1;
    m->pos = size_t(p);
    return p;
}

}  // namespace

Pcm decodeToS16(const std::vector<uint8_t>& bytes, const std::string& origin) {
    Pcm out;
    if (bytes.empty()) return out;

    constexpr int kIoBuf = 32768;
    auto* ioBuf = static_cast<uint8_t*>(av_malloc(kIoBuf));
    if (!ioBuf) return out;
    MemSource src{bytes.data(), bytes.size(), 0};
    AVIOContext* avio = avio_alloc_context(ioBuf, kIoBuf, 0, &src, memRead, nullptr, memSeek);
    if (!avio) { av_free(ioBuf); return out; }

    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) { av_free(avio->buffer); avio_context_free(&avio); return out; }
    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    AVCodecContext* ctx = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int stream = -1;

    auto cleanup = [&] {
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        if (swr) swr_free(&swr);
        if (ctx) avcodec_free_context(&ctx);
        if (fmt) {
            // Custom IO: free our buffer and context ourselves after closing.
            avformat_close_input(&fmt);
        }
        if (avio) { av_free(avio->buffer); avio_context_free(&avio); }
    };

    if (avformat_open_input(&fmt, origin.c_str(), nullptr, nullptr) < 0) {
        // open_input frees fmt on failure; don't double-free it below.
        fmt = nullptr;
        cleanup();
        return out;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) { cleanup(); return out; }

    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { stream = int(i); break; }
    if (stream < 0) { cleanup(); return out; }

    const AVCodec* dec = avcodec_find_decoder(fmt->streams[stream]->codecpar->codec_id);
    if (!dec) { cleanup(); return out; }          // decoder not built into this FFmpeg
    ctx = avcodec_alloc_context3(dec);
    if (!ctx || avcodec_parameters_to_context(ctx, fmt->streams[stream]->codecpar) < 0
        || avcodec_open2(ctx, dec, nullptr) < 0) { cleanup(); return out; }

    const int rate = ctx->sample_rate;
    const int ch = ctx->ch_layout.nb_channels > 0 ? ctx->ch_layout.nb_channels : 1;
    if (rate <= 0) { cleanup(); return out; }

    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, ch);
    bool ok = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, rate,
                                  &ctx->ch_layout, ctx->sample_fmt, rate, 0, nullptr) >= 0
              && swr_init(swr) >= 0;
    av_channel_layout_uninit(&outLayout);
    if (!ok) { cleanup(); return out; }

    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !pkt) { cleanup(); return out; }

    std::vector<int16_t> pcm;
    // A minute of 44.1k stereo is ~5.3M samples; reserve a second so the first
    // few appends don't reallocate from nothing.
    pcm.reserve(size_t(rate) * size_t(ch));
    std::vector<uint8_t> conv;

    auto drain = [&] {
        while (avcodec_receive_frame(ctx, frame) >= 0) {
            int maxOut = int(av_rescale_rnd(swr_get_delay(swr, rate) + frame->nb_samples,
                                            rate, rate, AV_ROUND_UP));
            if (maxOut <= 0) continue;
            size_t need = size_t(maxOut) * size_t(ch) * sizeof(int16_t);
            if (conv.size() < need) conv.resize(need);
            uint8_t* dst[1] = {conv.data()};
            int got = swr_convert(swr, dst, maxOut,
                                  const_cast<const uint8_t**>(frame->data), frame->nb_samples);
            if (got > 0) {
                const auto* s = reinterpret_cast<const int16_t*>(conv.data());
                pcm.insert(pcm.end(), s, s + size_t(got) * size_t(ch));
            }
        }
    };

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == stream && avcodec_send_packet(ctx, pkt) >= 0) drain();
        av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, nullptr);   // flush
    drain();

    cleanup();
    if (pcm.empty()) return out;
    out.samples = std::move(pcm);
    out.rate = rate;
    out.channels = ch;
    return out;
}

}  // namespace ta::audiodec
