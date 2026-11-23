#include "client/gameview.h"

// Out-of-line GameView methods for the power codes (sim/powercodes.h): what a +CODE
// chat line does on this client, and MakePoster's whole-map picture. The sim codes
// leave here as Cmd::PowerCode and change the game only when the server sequences
// them (it drops them while the room has POWER CODES off); Radar and MakePoster are
// local display.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "util/u8path.h"

namespace {
    // Poster writer threads outlive the match that made them (leaving to the menu
    // does not wait for a 100 MB PNG), but not the process: the destructor of this
    // static joins any still writing, so quitting finishes the file instead of
    // killing the thread mid-write. Each writes to a .part name and renames it when
    // complete, so a file that is somehow cut short never looks like a poster.
    struct PosterWriters {
        std::mutex m;
        std::vector<std::thread> threads;
        void start(std::function<void()> work) {
            std::lock_guard<std::mutex> lock(m);
            threads.emplace_back(std::move(work));
        }
        ~PosterWriters() {
            std::lock_guard<std::mutex> lock(m);
            for (auto& t : threads)
                if (t.joinable()) t.join();
        }
    };
    PosterWriters& posterWriters() {
        static PosterWriters writers;
        return writers;
    }
}

    void GameView::submitChat(const std::string& line) {
        // Retail's chat handler 0x4b4fb0 runs a '+' line through the command table and
        // then sends it as chat anyway, to every human (not the chosen recipients) --
        // so everyone sees a code typed. Here only a code that can act goes to all:
        // with POWER CODES off a +line is ordinary chat to the chosen recipients.
        const auto code = tak::sim::parsePowerCode(line);
        const bool acts = code && powerCodesOn();
        if (acts) {
            if (chatEchoMs_) flushSystemChat();   // an earlier code's echo is overdue
            chatEcho_ = line;
            chatEchoMs_ = std::max<uint64_t>(SDL_GetTicks64(), 1);
            powerCode(*code);
            if (heldSystemChat_.empty()) { chatEchoMs_ = 0; chatEcho_.clear(); }   // nothing to hold
        }
        mp_->chat(line, acts ? uint8_t(0xff) : chatRecipients_, acts || chatSpectators_);
    }

    void GameView::powerScriptStep() {
        static const char* script = tak::devEnv("TAK_POWER_SCRIPT");
        static size_t at = 0;
        if (!script || !mp_) return;
        const std::string all(script);
        while (at < all.size()) {
            const size_t end = std::min(all.find(';', at), all.size());
            const std::string item = all.substr(at, end - at);
            const size_t colon = item.find(':');
            const uint32_t tick = colon == std::string::npos ? 0 : uint32_t(std::atoi(item.c_str()));
            if (netTick_ < tick) return;
            const std::string line = colon == std::string::npos ? item : item.substr(colon + 1);
            if (const auto code = tak::sim::parsePowerCode(line); code && code->code == tak::sim::PowerCode::ManaMe) {
                selection_.clear();
                for (const UnitR* u : front().live)
                    if (u->alive() && u->player == localPlayer_) selection_.push_back(u->id);
            }
            std::fprintf(stderr, "power script: tick %u types '%s'\n", netTick_, line.c_str());
            submitChat(line);
            at = end + 1;
        }
    }

    bool GameView::powerCodesOn() const {
        if (!mp_ || replayMode_ || spectating_ || !missionStem_.empty() || inLobbyPhase()) return false;
        const auto& room = mp_->room();
        return room.opts.powerCodes != 0 && room.mission.empty() && room.mySlot >= 0;
    }

    void GameView::powerCode(const tak::sim::PowerCodeInput& in) {
        using tak::sim::PowerCode;
        // Retail 0x428f10 skips a group-2 handler unless the match allows cheating:
        // with POWER CODES off the line is ordinary chat and does nothing else.
        if (!powerCodesOn()) return;
        switch (in.code) {
            case PowerCode::Radar: radarCode_ = !radarCode_; return;
            case PowerCode::MakePoster: startPoster(); return;
            case PowerCode::Los:
                if (in.arg == tak::sim::PowerArg::Invalid) return;   // retail ignores any other word
                break;
            default: break;
        }
        if (!tak::sim::simPowerCode(in.code)) return;
        tak::net::Command c;
        c.kind = tak::net::Cmd::PowerCode;
        c.targetId = int32_t(in.code);
        c.queue = uint8_t(in.arg);
        std::lock_guard<std::mutex> lock(simMutex_);
        if (in.code == PowerCode::ManaMe) {
            // 426390 acts on the typer's selection: one command per selected unit of
            // this player's (the sim checks ownership and the pool again).
            for (int id : selection_) {
                const UnitR* u = frameUnitP(id);
                if (!u || !u->alive() || u->player != localPlayer_) continue;
                c.unitId = id;
                issue(c);
            }
            return;
        }
        issue(c);
    }

    // Retail writes <game dir>\screenshots\BIGSHOT%04i.bmp. Ours goes beside it as PNG,
    // in the data folder; a read-only install falls back to the profile folder.
    std::string GameView::posterFolder() const {
        namespace fs = std::filesystem;
        std::vector<fs::path> candidates;
        if (const char* dir = tak::devEnv("TAK_POSTER_DIR")) candidates.push_back(tak::u8path(dir));   // tests
        if (!installRoot_.empty()) candidates.push_back(tak::u8path(installRoot_) / "screenshots");
        if (const std::string profile = tak::replayDir(); !profile.empty())
            candidates.push_back(tak::u8path(profile) / "screenshots");
        for (const auto& dir : candidates) {
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (!fs::is_directory(dir, ec)) continue;
            const fs::path probe = dir / ".tak-write-test";
            if (FILE* f = std::fopen(probe.string().c_str(), "wb")) {
                std::fclose(f);
                fs::remove(probe, ec);
                const auto text = dir.u8string();
                return std::string(text.begin(), text.end());
            }
        }
        return {};
    }

    void GameView::startPoster() {
        if (poster_) return;   // one at a time
        const int width = int(mapView_.map().blocksX) * 32, height = int(mapView_.map().blocksY) * 32;
        if (width <= 0 || height <= 0) return;
        const std::string folder = posterFolder();
        if (folder.empty()) { systemChat("POSTER NOT SAVED: NO WRITABLE SCREENSHOTS FOLDER"); return; }
        // The number after the highest existing BIGSHOT, as retail's 0x526650 picks it.
        namespace fs = std::filesystem;
        int highest = 0;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(tak::u8path(folder), ec)) {
            const auto name = entry.path().filename().string();
            int n = 0;
            // BIGSHOTnnnn.png, or .png.part while an earlier poster is still writing.
            if ((name.size() == 15 || name.size() == 20) && name.rfind("BIGSHOT", 0) == 0 &&
                std::sscanf(name.c_str() + 7, "%4d", &n) == 1)
                highest = std::max(highest, n);
        }
        char file[32];
        std::snprintf(file, sizeof file, "BIGSHOT%04d.png", std::min(highest + 1, 9999));
        auto job = std::make_unique<PosterJob>();
        job->width = width; job->height = height;
        try {
            job->rgb = std::make_shared<std::vector<uint8_t>>(size_t(width) * size_t(height) * 3, 0);
        } catch (const std::bad_alloc&) {
            systemChat("POSTER NOT SAVED: THE MAP IS TOO LARGE FOR MEMORY");
            return;
        }
        const auto path = tak::u8path(folder) / file;
        const auto text = path.u8string();
        job->path.assign(text.begin(), text.end());
        job->startMs = SDL_GetTicks64();
        poster_ = std::move(job);
        systemChat("MAKING POSTER " + std::to_string(width) + "X" + std::to_string(height) + "...");
    }

    void GameView::posterStep(int winW, int winH) {
        {   // results of finished writers, as chat lines
            std::lock_guard<std::mutex> lock(posterNotes_->m);
            for (auto& line : posterNotes_->lines) systemChat(line);
            posterNotes_->lines.clear();
        }
        if (!poster_ || inLobbyPhase() || loadScreen_) return;
        PosterJob& job = *poster_;
        const int mvw = mapViewW(winW);
        if (mvw <= 0 || winH <= 0) return;   // minimised: wait for a window to draw in
        if (!job.tileW) {
            // At most 2048 x 1152 a tile, whatever the window: reading pixels back is
            // the cost of a tile (a 7475 x 2160 one read in about 42 ms, a hitch every
            // frame of the poster), and a smaller read keeps each frame close to normal.
            job.tileW = std::min({mvw, job.width, 2048});
            job.tileH = std::min({winH, job.height, 1152});
            job.cols = (job.width + job.tileW - 1) / job.tileW;
            job.rows = (job.height + job.tileH - 1) / job.tileH;
            job.next = 0;   // a new grid starts over: its tiles are not the old ones
        }
        if (mvw < job.tileW || winH < job.tileH) { job.tileW = 0; return; }   // the window shrank: retile
        // One tile: the world at native scale (retail draws the poster 1:1), with the
        // selection, health bars and HUD left out; everything else as this player sees it.
        const uint64_t tileStart = SDL_GetPerformanceCounter();
        const int tx = job.next % job.cols, ty = job.next / job.cols;
        const int x0 = tx * job.tileW, y0 = ty * job.tileH;
        const int x1 = std::min(job.width, x0 + job.tileW), y1 = std::min(job.height, y0 + job.tileH);
        const float camX = mapView_.offX(), camY = mapView_.offY(), camZoom = mapView_.zoom();
        auto selection = std::move(selection_);
        selection_.clear();
        const int bars = healthBars_;
        healthBars_ = 0;
        mapView_.setZoom(1.0f);
        mapView_.setOffset(float(x0), float(y0));
        mapView_.ensureChunks(mvw, winH);   // may clamp the edge tiles back onto the map
        mapView_.finishChunks();
        const int ox = int(std::lround(mapView_.offX())), oy = int(std::lround(mapView_.offY()));
        posterFrame_ = true;
        draw(winW, winH);
        posterFrame_ = false;
        // Read back only the tile's own rectangle (the edge tiles are smaller than the
        // window), into a buffer kept from tile to tile.
        const int rx0 = std::clamp(x0 - ox, 0, mvw), ry0 = std::clamp(y0 - oy, 0, winH);
        const int rx1 = std::clamp(x1 - ox, 0, mvw), ry1 = std::clamp(y1 - oy, 0, winH);
        const int rw = rx1 - rx0, rh = ry1 - ry0;
        bool read = false;
        if (rw > 0 && rh > 0) {
            job.readback.resize(size_t(rw) * rh * 4);
            const SDL_Rect area{rx0, ry0, rw, rh};
            read = SDL_RenderReadPixels(ren_, &area, SDL_PIXELFORMAT_RGBA32, job.readback.data(), rw * 4) == 0;
        }
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
        SDL_RenderClear(ren_);   // the frame's own draw follows; nothing of the tile shows
        mapView_.setZoom(camZoom);
        mapView_.setOffset(camX, camY);
        selection_ = std::move(selection);
        healthBars_ = bars;
        if (read) {
            auto& rgb = *job.rgb;
            for (int y = y0; y < y1; ++y) {
                const int sy = y - oy;
                if (sy < ry0 || sy >= ry1) continue;
                for (int x = x0; x < x1; ++x) {
                    const int sx = x - ox;
                    if (sx < rx0 || sx >= rx1) continue;
                    const uint8_t* s = job.readback.data() + (size_t(sy - ry0) * rw + (sx - rx0)) * 4;
                    uint8_t* d = rgb.data() + (size_t(y) * job.width + x) * 3;
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                }
            }
        }
        job.maxTileMs = std::max(job.maxTileMs, double(SDL_GetPerformanceCounter() - tileStart) * 1000.0 /
                                                    double(SDL_GetPerformanceFrequency()));
        if (++job.next < job.cols * job.rows) return;
        // Every tile is in: compress and write on a worker, report through posterNotes_.
        const uint64_t renderMs = SDL_GetTicks64() - job.startMs;
        std::fprintf(stderr, "poster: %dx%d in %d tiles of %dx%d, rendered in %llu ms, worst tile %.1f ms\n",
                     job.width, job.height, job.cols * job.rows, job.tileW, job.tileH, (unsigned long long)renderMs,
                     job.maxTileMs);
        posterWriters().start([rgb = job.rgb, path = job.path, w = job.width, h = job.height, notes = posterNotes_] {
            std::vector<std::string> lines;
            const auto file = tak::u8path(path);
            auto part = file;
            part += ".part";
            try {
                const auto t0 = std::chrono::steady_clock::now();
                tak::png::writeRgb(part, w, h, *rgb);
                std::filesystem::rename(part, file);
                std::fprintf(stderr, "poster: wrote %s in %lld ms\n", path.c_str(), (long long)
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
                // The name, then the folder on a line of its own (a full path is wider
                // than the chat overlay).
                const auto name = file.filename().u8string(), folder = file.parent_path().u8string();
                lines.push_back("POSTER SAVED: " + std::string(name.begin(), name.end()));
                lines.push_back("IN " + std::string(folder.begin(), folder.end()));
            } catch (const std::exception& e) {
                std::error_code ec;
                std::filesystem::remove(part, ec);
                lines.push_back(std::string("POSTER NOT SAVED: ") + e.what());
            }
            std::lock_guard<std::mutex> lock(notes->m);
            for (auto& line : lines) notes->lines.push_back(std::move(line));
        });
        poster_.reset();
    }
