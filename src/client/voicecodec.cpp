#include "client/voicecodec.h"

#include <opus.h>

#include <algorithm>

namespace tak::voice {

namespace {
OpusEncoder* enc(void* p) { return static_cast<OpusEncoder*>(p); }
OpusDecoder* dec(void* p) { return static_cast<OpusDecoder*>(p); }
}  // namespace

Encoder::Encoder() {
    int err = 0;
    OpusEncoder* e = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
    if (!e || err != OPUS_OK) return;
    opus_encoder_ctl(e, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(e, OPUS_SET_VBR(1));
    opus_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(5));
    // In-band FEC: each packet carries a low-rate copy of the previous one, which the
    // receiver uses when a frame went missing (the server drops voice under load).
    opus_encoder_ctl(e, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(e, OPUS_SET_PACKET_LOSS_PERC(5));
    // No DTX: push-to-talk already sends nothing while the key is up.
    opus_encoder_ctl(e, OPUS_SET_DTX(0));
    st_ = e;
}

Encoder::~Encoder() { if (st_) opus_encoder_destroy(enc(st_)); }

std::vector<uint8_t> Encoder::encode(const float* pcm) {
    if (!st_) return {};
    std::vector<uint8_t> out(kMaxPacket);
    const opus_int32 n = opus_encode_float(enc(st_), pcm, kFrame, out.data(), opus_int32(out.size()));
    if (n <= 0) return {};
    out.resize(size_t(n));
    return out;
}

void Encoder::reset() { if (st_) opus_encoder_ctl(enc(st_), OPUS_RESET_STATE); }

Decoder::Decoder() {
    int err = 0;
    OpusDecoder* d = opus_decoder_create(kRate, 1, &err);
    if (d && err == OPUS_OK) st_ = d;
}

Decoder::~Decoder() { if (st_) opus_decoder_destroy(dec(st_)); }

bool Decoder::decode(const uint8_t* data, size_t n, float* out) {
    std::fill(out, out + kFrame, 0.0f);
    if (!st_ || !data || n == 0 || n > kMaxPacket) return false;
    return opus_decode_float(dec(st_), data, opus_int32(n), out, kFrame, 0) == kFrame;
}

bool Decoder::decodeFec(const uint8_t* next, size_t n, float* out) {
    std::fill(out, out + kFrame, 0.0f);
    if (!st_) return false;
    if (!next || n == 0 || n > kMaxPacket) return conceal(out);
    return opus_decode_float(dec(st_), next, opus_int32(n), out, kFrame, 1) == kFrame;
}

bool Decoder::conceal(float* out) {
    std::fill(out, out + kFrame, 0.0f);
    if (!st_) return false;
    return opus_decode_float(dec(st_), nullptr, 0, out, kFrame, 0) == kFrame;
}

void Decoder::reset() { if (st_) opus_decoder_ctl(dec(st_), OPUS_RESET_STATE); }

}  // namespace tak::voice
