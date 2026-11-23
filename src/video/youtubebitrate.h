#pragma once
// YouTube's live bitrates, so the streaming panel never sends YouTube more than it
// takes: above its figure for the stream's size, frame rate and codec, YouTube can
// accept the connection and then never start the broadcast, while the encoder
// reports LIVE. Other destinations keep the panel's whole range.
//
// "Choose live encoder settings, bitrates, and resolutions",
// https://support.google.com/youtube/answer/2853702, fetched 2026-10-29 (Mbps):
//
//   Resolution / frame rate   AV1+H.265 min  AV1+H.265 rec  H.264 min  H.264 rec
//   4K / 2160p @60fps               10             35           14         50
//   4K / 2160p @30fps                8             30           11         42
//   1440p @60fps                     6             24            8         34
//   1440p @30fps                     5             15            7         21
//   1080p @60fps                     4             12            6         17
//   1080p @30fps                     4             10            5         14
//   720p @60fps                      2              6            3          8
//   720p @30fps                      2              6            3          8
//   480p @30fps                      0.3            3            0.4        4
//   360p @30fps                      0.3            3            0.4        4
//
// The page calls each minimum-to-recommended pair the "recommended bitrate setting
// range" and names no maximum (and no 240p row), so the top of that range, the
// recommended figure, is both the cap and what RECOMMENDED fills in.
//   * Row: the first one at or above the picture's SHORT side (its height, unless it
//     stands upright), the top row above 2160 and the bottom one below 360. An
//     ultrawide 3840x1080 is 1080p: YouTube labels a picture by its short side, and
//     1080p's figure is above every larger row's minimum, so it holds whichever row
//     YouTube files the stream under.
//   * 480p and 360p have no 60 fps row: their 30 fps figure serves both.
//   * Codec: what the encoder will produce. H.265 and AV1 share a column; no encoder
//     of ours makes AV1.
#include <iterator>
#include <string_view>

namespace tak::video {

enum class StreamCodec { H264, Hevc };

// What video::Stream opens for this size (stream.cpp chooseVideo): AUTOMATIC (an empty
// `encoder`) goes to hardware HEVC above 4096 pixels on either axis, else H.264; a named
// encoder is that codec. A CPU-fallback restart is libx264, whose cap is never lower.
inline StreamCodec streamCodec(std::string_view encoder, int width, int height) {
    if (encoder.starts_with("hevc_")) return StreamCodec::Hevc;
    if (encoder.empty() && (width > 4096 || height > 4096)) return StreamCodec::Hevc;
    return StreamCodec::H264;
}

// YouTube's recommended bitrate (Kbps), the top of its range: the cap and RECOMMENDED.
inline int youtubeRecommendedKbps(int width, int height, int fps, StreamCodec codec) {
    struct Row { int height, hevc30, h264at30, hevc60, h264at60; };
    static constexpr Row rows[] = { // 480p and 360p: the 30 fps figure at 60 too
        {360, 3000, 4000, 3000, 4000},       {480, 3000, 4000, 3000, 4000},
        {720, 6000, 8000, 6000, 8000},       {1080, 10000, 14000, 12000, 17000},
        {1440, 15000, 21000, 24000, 34000},  {2160, 30000, 42000, 35000, 50000},
    };
    const int side = width < height ? width : height;
    const Row* r = rows;
    while (r->height < side && r + 1 != std::end(rows)) ++r;
    const bool sixty = fps > 30, hevc = codec == StreamCodec::Hevc;
    return sixty ? (hevc ? r->hevc60 : r->h264at60) : (hevc ? r->hevc30 : r->h264at30);
}

// One of the table's figures (Kbps): a bitrate the panel can have set from it.
inline bool isYouTubeRecommendedKbps(int kbps) {
    for (int side : {360, 480, 720, 1080, 1440, 2160})
        for (int fps : {30, 60})
            for (StreamCodec codec : {StreamCodec::H264, StreamCodec::Hevc})
                if (youtubeRecommendedKbps(side, side, fps, codec) == kbps) return true;
    return false;
}

// A YouTube RTMP or RTMPS ingest: a.rtmp.youtube.com, b.rtmps.youtube.com:443 and the
// like (any host under youtube.com). The key after the host is never looked at.
inline bool isYouTubeIngest(std::string_view url) {
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    auto same = [&](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) if (lower(a[i]) != b[i]) return false;
        return true;
    };
    size_t at = 0;
    if (same(url.substr(0, 7), "rtmp://")) at = 7;
    else if (same(url.substr(0, 8), "rtmps://")) at = 8;
    else return false;
    std::string_view host = url.substr(at);
    host = host.substr(0, host.find_first_of(":/?#"));
    if (host.ends_with('.')) host.remove_suffix(1);
    constexpr std::string_view domain = "youtube.com";
    if (host.size() < domain.size() || !same(host.substr(host.size() - domain.size()), domain)) return false;
    return host.size() == domain.size() || host[host.size() - domain.size() - 1] == '.';
}

}
