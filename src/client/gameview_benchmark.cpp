#include "client/gameview.h"
#include "client/gamespeed.h"
#include "client/renderdriver.h"
#include "util/jpeg.h"

#include <ctime>
#include <fstream>


// Out-of-line GameView method definitions (benchmark concern), split from the
// class body in gameview.h so editing a body recompiles only this translation
// unit. Trivial getters, ctors, static, template, constexpr and default-arg
// methods stay inline in the header. Grouping is by name heuristic.

    void GameView::benchmarkBaseline() {
        benchPrevWallMs_ = SDL_GetTicks64();
        benchCliPrev_ = tak::proc::sample(0);
        benchSrvPrev_ = localServerPid_ ? tak::proc::sample(localServerPid_) : tak::proc::Sample{};
        benchStartMs_ = 0; benchNextSec_ = 10; benchSamples_.clear(); benchStatsShown_ = false;
        benchSavePending_ = benchSaved_ = benchSaveFailed_ = false;   // a new run, a new picture
        benchSaveNote_.clear(); benchSavePath_.clear();
    }

    int GameView::benchmarkElapsedSec() const {
        return benchStartMs_ ? int((SDL_GetTicks64() - benchStartMs_) / 1000) : 0;
    }

    void GameView::pushBenchSample(int gameSec) {
        uint64_t nowMs = SDL_GetTicks64();
        double wallSec = benchPrevWallMs_ ? double(nowMs - benchPrevWallMs_) / 1000.0 : 0;
        tak::proc::Sample cli = tak::proc::sample(0);
        tak::proc::Sample srv = localServerPid_ ? tak::proc::sample(localServerPid_) : tak::proc::Sample{};
        const int logicalCpus=tak::proc::numCpus();
        auto pct = [&](const tak::proc::Sample& n, const tak::proc::Sample& p) {
            return tak::proc::processCpuPercent(p,n,wallSec,logicalCpus);
        };
        BenchSample s;
        s.gameSec = gameSec;
        s.liveUnits = int(framedAliveUnits());
        s.clientCpuPct = pct(cli, benchCliPrev_);
        s.serverCpuPct = pct(srv, benchSrvPrev_);
        s.clientRss = cli.rssBytes;
        s.serverRss = srv.rssBytes;
        s.fps = fps_;
        s.simSpeed = actualSpeed_;
        s.gpuBytes = gpuvram::bytes();       // accounted client GPU memory (see gpuvram.h)
        tak::proc::GpuSample gpu = tak::proc::gpuSample();   // whole-device util % + VRAM
        s.gpuPct = tak::proc::systemGpuPercent(gpu);
        if (gpu.ok) {
            s.gpuSysUsed = gpu.memUsed;
            if (benchGpuName_.empty()) { benchGpuName_ = gpu.name; benchGpuTotal_ = gpu.memTotal; }
        }
        benchSamples_.push_back(s);
        benchCliPrev_ = cli; benchSrvPrev_ = srv; benchPrevWallMs_ = nowMs;
    }

    void GameView::benchmarkSample() {
        if (!benchmarkMode_ || !worldTables().benchmarkMode() || benchStatsShown_) return;
        // A sample every 10 real seconds of the run, from its first tick; the last when the
        // server ends it (Cmd::BenchClock, 60 real seconds in), whatever the game speed.
        const uint32_t gt = front().gameTick;
        if (gt > 0 && !benchStartMs_) benchStartMs_ = SDL_GetTicks64();
        if (front().benchEnded) {
            pushBenchSample(tak::sim::kBenchmarkSeconds);
            benchStatsShown_ = true;   // run complete -> show the stats overlay
            return;
        }
        while (benchStartMs_ && benchNextSec_ < tak::sim::kBenchmarkSeconds && benchmarkElapsedSec() >= benchNextSec_) {
            pushBenchSample(benchNextSec_);
            benchNextSec_ += 10;
        }
#ifndef NDEBUG
        // TAK_BENCH_STOP_TICK=N: Esc at tick N, with the results filled out to a full run's
        // size (all six samples, every row), so a short run lays out the tallest panel.
        if (const char* st = tak::devEnv("TAK_BENCH_STOP_TICK"); st && !benchStatsShown_ && gt >= uint32_t(std::atoi(st))) {
            stopBenchmark();
            BenchSample full = benchSamples_.back();
            full.gpuSysUsed = std::max<size_t>(full.gpuSysUsed, 1u << 30);
            benchSamples_.resize(6, full);
            if (benchGpuName_.empty()) benchGpuName_ = "HARNESS";
        }
#endif
    }

    void GameView::stopBenchmark() {
        if (benchStatsShown_ || !benchmarkMode_) return;
        if (!worldTables().benchmarkMode()) { menuRequested_ = true; return; }   // still in setup -> abort
        const int sec = benchmarkElapsedSec();   // the run's real seconds
        if (benchSamples_.empty() || benchSamples_.back().gameSec != sec) pushBenchSample(sec);
        benchStatsShown_ = true;
    }

    void GameView::benchmarkCamera(float dt, int winW, int winH) {
        if (!benchmarkMode_ || benchStatsShown_ || !worldTables().benchmarkMode()) return;
        int leg = benchmarkElapsedSec() * 8 / tak::sim::kBenchmarkSeconds;   // 0..7, one per faction / 7.5 s leg (60 s / 8)
        if (leg < 0) leg = 0;
        if (leg > 7) leg = 7;
        if (leg != benchCamLeg_) { benchCamLeg_ = leg; benchLegT_ = 0.0f; }   // new leg -> reset
        benchLegT_ += dt;
        float p = benchLegT_ / 7.5f;              // progress through the 7.5s leg
        if (p > 1.0f) p = 1.0f;
        // Zoom: all the way out at the leg start, zooming in over the leg.
        const int vw = mapViewW(winW), vh = mapViewH(winH);
        float zOut = mapView_.minZoom(vw, vh);
        float zIn = std::max(zOut * 2.5f, 1.6f);  // a clear close-up, comfortably above zOut
        float z = zOut + (zIn - zOut) * p;
        mapView_.setZoom(z);
        // Track this leg's AI monarch (player == leg) from the render snapshot.
        for (const UnitR* u : front().live)
            if (u && u->player == leg && isMonarchType(u->type)) {
                mapView_.setOffset(u->x - vw * 0.5f / z, u->z - vh * 0.5f / z);
                break;
            }
        mapView_.clampOffset(vw, vh);
    }

    void GameView::saveBenchmarkJpg(int winW, int winH) {
        namespace fs = std::filesystem;
        const auto text = [](const fs::path& p) { const auto t = p.u8string(); return std::string(t.begin(), t.end()); };
        benchSaveFailed_ = true;   // until the file is in place
        benchSavePath_.clear();
        const std::string profile = tak::replayDir();
        if (profile.empty()) { benchSaveNote_ = "COULD NOT SAVE: NO FOLDER FOR THIS PLAYER'S FILES"; return; }
        const fs::path dir = tak::u8path(profile) / "benchmarks";
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (!fs::is_directory(dir, ec)) {
            benchSaveNote_ = "COULD NOT SAVE: CANNOT MAKE THE FOLDER ";
            benchSavePath_ = text(dir);
            return;
        }
        // The whole frame at the size it is drawn (an explicit rect, as main()'s screenshot
        // reads), as a JPEG in memory.
        std::vector<uint8_t> jpg;
        try {
            std::vector<uint8_t> px(size_t(winW) * winH * 4);
            const SDL_Rect area{0, 0, winW, winH};
            if (SDL_RenderReadPixels(ren_, &area, SDL_PIXELFORMAT_RGBA32, px.data(), winW * 4) != 0)
                throw std::runtime_error(std::string("CANNOT READ THE SCREEN (") + SDL_GetError() + ")");
            jpg = tak::jpeg::encode(winW, winH, px, 92);
        } catch (const std::exception& e) {
            benchSaveNote_ = std::string("COULD NOT SAVE: ") + e.what();
            return;
        }
        // benchmark-2026-11-10-143059.jpg, then -2, -3, ...: never over a file that is there.
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char stamp[40];
        std::strftime(stamp, sizeof stamp, "benchmark-%Y-%m-%d-%H%M%S", &local);
        fs::path file = dir / (std::string(stamp) + ".jpg");
        for (int n = 2; n < 10000 && fs::exists(file, ec); ++n) file = dir / (std::string(stamp) + "-" + std::to_string(n) + ".jpg");
        // Under a temporary name first, renamed when complete: a write that fails leaves
        // no half picture behind.
        fs::path part = file;
        part += ".part";
        bool written = false;
        if (!fs::exists(file, ec)) {
            std::ofstream out(part, std::ios::binary);
            out.write(reinterpret_cast<const char*>(jpg.data()), std::streamsize(jpg.size()));
            out.close();
            written = bool(out);
            if (written) { fs::rename(part, file, ec); written = !ec; }
            if (!written) fs::remove(part, ec);
        }
        if (!written) {
            benchSaveNote_ = "COULD NOT SAVE: CANNOT WRITE IN ";
            benchSavePath_ = text(dir);
            return;
        }
        benchSaved_ = true;
        benchSaveFailed_ = false;
        benchSaveNote_ = "SAVED: ";
        benchSavePath_ = text(file);
        std::fprintf(stderr, "benchmark: saved %s (%d x %d, %zu bytes)\n", benchSavePath_.c_str(), winW, winH, jpg.size());
    }

    void GameView::renderBenchmarkStats(int winW, int winH) {
        // Every SETTINGS line first: the panel's height depends on how many there are.
        std::vector<std::string> settings;
        char b[64];
        // What the run's world really used, not what was asked for.
        settings.push_back(std::string("PATHFINDING ") + tak::benchmarkPathLabel(worldTables().pathfindingMode()));
        // The room's game speed, as the run was paced.
        if (mp_) settings.push_back(std::string("SPEED       ") + tak::speedLevelText(int(mp_->gameSpeed()) - 10));
        // SIMULATION THREADS as this run latched it (latchSimThreads): what its own
        // simulation and its local server's each ran on, then the setting they are the
        // halves of. Not the Options value now: a change in the Esc menu is for the next game.
        if (ccdGame_) {   // PIN TO CCDS: each simulation a thread per core of its CCD
            if (ccdServer_)
                std::snprintf(b, sizeof b, "SIM THREADS CLIENT %u  SERVER %u  (CCDS)", simSession_.game, ccdServerThreads_);
            else std::snprintf(b, sizeof b, "SIM THREADS CLIENT %u  (CCD)", simSession_.game);
            settings.push_back(b);
            if (ccdServer_) std::snprintf(b, sizeof b, "PIN TO CCDS  GAME CCD %d  SERVER CCD %d", ccdGame_, ccdServer_);
            else std::snprintf(b, sizeof b, "PIN TO CCDS  GAME CCD %d", ccdGame_);
            settings.push_back(b);
        } else {
            char set[16] = "AUTO";
            if (simThreads_ > 0) std::snprintf(set, sizeof set, "%u", simSession_.total);
            if (simSession_.server)
                std::snprintf(b, sizeof b, "SIM THREADS CLIENT %u  SERVER %u  (%s)", simSession_.game, simSession_.server, set);
            else std::snprintf(b, sizeof b, "SIM THREADS CLIENT %u  (%s)", simSession_.game, set);
            settings.push_back(b);
        }
        // PIN SIMULATION THREADS as this run latched it: ON only if its threads really
        // were pinned (the setting on, and a system that can pin).
        if (!ccdGame_) settings.push_back(std::string("PIN SIM THREADS ") + (simPin_ ? "ON" : "OFF"));
        std::snprintf(b, sizeof b, "RESOLUTION  %d X %d", winW, winH); settings.push_back(b);
        if (!benchGpuName_.empty()) settings.push_back(std::string("GPU         ") + benchGpuName_);
        {   // the backend that produced these numbers, and whether it was AUTO's pick
            SDL_RendererInfo ri{};
            std::string id = SDL_GetRendererInfo(ren_, &ri) == 0 && ri.name ? tak::renderdriver::normalize(ri.name) : std::string();
            settings.push_back("RENDERER    " + tak::renderdriver::label(id) +
                               (tak::renderdriver::active().inEffect.empty() ? " (AUTO)" : ""));
        }
        settings.push_back(std::string("FULLSCREEN  ") + (settings_ && settings_->fullscreen ? "ON" : "OFF"));
        settings.push_back(std::string("VSYNC       ") + (settings_ && settings_->vsync ? "ON" : "OFF"));
        if (settings_ && !settings_->vsync && settings_->maxFps <= 0) settings.push_back("MAX FPS     UNLIMITED");
        else if (settings_ && !settings_->vsync) { std::snprintf(b, sizeof b, "MAX FPS     %d", settings_->maxFps); settings.push_back(b); }
        else settings.push_back("MAX FPS     (VSYNC)");
        {   const int tf = settings_ ? std::clamp(settings_->textureFilter, 0, 2) : 0;
            settings.push_back(std::string("TEXFILTER   ") + (tf == 2 ? "EVERYTHING" : tf == 1 ? "UNITS" : "OFF")); }
        // GPU texture memory: the cap in force (Options limit, or a tightening after a
        // failed create), and this run's peak accounted usage.
        size_t gpuPeak = 0, sysPeak = 0;
        for (const auto& s : benchSamples_) {
            if (s.gpuBytes > gpuPeak) gpuPeak = s.gpuBytes;
            if (s.gpuSysUsed > sysPeak) sysPeak = s.gpuSysUsed;
        }
        if (gpuvram::cap() == gpuvram::kUnlimited) std::snprintf(b, sizeof b, "GPU TEX CAP UNLIMITED");
        else std::snprintf(b, sizeof b, "GPU TEX CAP %zuMB", gpuvram::cap() >> 20);
        settings.push_back(b);
        std::snprintf(b, sizeof b, "GPU PEAK    %zuMB", gpuPeak >> 20); settings.push_back(b);
        if (sysPeak) {
            if (benchGpuTotal_) std::snprintf(b, sizeof b, "SYS VRAM    %zu / %zuMB PEAK", sysPeak >> 20, benchGpuTotal_ >> 20);
            else std::snprintf(b, sizeof b, "SYS VRAM    %zuMB PEAK", sysPeak >> 20);
            settings.push_back(b);
        }

        const char* title = "BENCHMARK RESULTS";
        char subBuf[160];
        std::snprintf(subBuf, sizeof subBuf,
                      "8-AI FFA -- ULASEM ARENA -- %s: 1 UNIT/FACTION EVERY %s FOR 60S",
                      tak::sim::benchmarkLevelName(benchmarkLevel_),
                      tak::sim::benchmarkLevelInterval(benchmarkLevel_));
        const char* sub = subBuf;

        // One scale factor drives the whole panel, so it stays big + legible at 4K and
        // all of it, the buttons and the note under them included, still fits a short or
        // narrow window.
        constexpr float titleU = 4.0f, subU = 1.8f, cellU = 2.2f, setU = 2.0f, noteU = 1.8f;   // text sizes
        constexpr float padU = 44, rowU = 34, setRowU = 28;
        constexpr float colU[10] = {82, 96, 122, 122, 122, 70, 122, 122, 70, 96};
        // The gaps below, the buttons, and the note's line (kept free until there is one).
        constexpr float gapsU = 22 + 26 + 10 + 12 + 30 + 24 + 20 + 56 + 16 + 7 * noteU;
        float tableU = 0; for (float c : colU) tableU += c;
        const int rows = int(benchSamples_.size());
        const float baseW = 2 * padU + std::max(tableU, blockWidth(sub, subU));
        const float baseH = 2 * padU + 7 * (titleU + subU + cellU + setU) + rows * rowU +
                            float(settings.size()) * setRowU + gapsU;
        float k = float(winH) / 1000.0f; k = k < 1.0f ? 1.0f : (k > 2.4f ? 2.4f : k);
        k = std::min({k, float(winW) / baseW, float(winH) / baseH});
        const float titlePx = titleU * k, subPx = subU * k, cellPx = cellU * k, setPx = setU * k;
        const float pad = padU * k, rowH = rowU * k, tableW = tableU * k;
        float colW[10]; for (int c = 0; c < 10; ++c) colW[c] = colU[c] * k;
        const float panelW = baseW * k, panelH = baseH * k;
        float px0 = (winW - panelW) * 0.5f, py0 = (winH - panelH) * 0.5f;
        if (px0 < 0) px0 = 0;
        if (py0 < 0) py0 = 0;

        // Dim the frozen game, then the centered panel.
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 220);
        SDL_FRect dim{0, 0, float(winW), float(winH)}; SDL_RenderFillRectF(ren_, &dim);
        SDL_FRect panel{px0, py0, panelW, panelH};
        SDL_SetRenderDrawColor(ren_, 26, 28, 36, 245); SDL_RenderFillRectF(ren_, &panel);
        SDL_SetRenderDrawColor(ren_, 120, 128, 150, 255); SDL_RenderDrawRectF(ren_, &panel);

        const float cxL = px0 + pad;
        float y = py0 + pad;
        blockText(title, px0 + (panelW - blockWidth(title, titlePx)) * 0.5f, y, titlePx, {235, 225, 180, 255});
        y += 7 * titlePx + 22 * k;
        blockText(sub, px0 + (panelW - blockWidth(sub, subPx)) * 0.5f, y, subPx, {165, 170, 185, 255});
        y += 7 * subPx + 26 * k;

        const char* hdr[10] = {"TIME", "UNITS", "CLI CPU", "CLI MEM", "GPU MEM", "GPU%", "SRV CPU", "SRV MEM", "FPS", "SIM"};
        float cx = cxL;
        for (int c = 0; c < 10; ++c) { blockText(hdr[c], cx, y, cellPx, {205, 210, 225, 255}); cx += colW[c]; }
        y += 7 * cellPx + 10 * k;
        SDL_SetRenderDrawColor(ren_, 90, 95, 120, 255);
        SDL_FRect ln{cxL, y, tableW, 2 * k}; SDL_RenderFillRectF(ren_, &ln);
        y += 12 * k;
        for (const auto& s : benchSamples_) {
            cx = cxL;
            auto cell = [&](const char* t) { blockText(t, cx, y, cellPx, {228, 231, 240, 255}); };
            std::snprintf(b, sizeof b, "%dS", s.gameSec); cell(b); cx += colW[0];
            std::snprintf(b, sizeof b, "%d", s.liveUnits); cell(b); cx += colW[1];
            if (s.clientCpuPct >= 0) std::snprintf(b, sizeof b, "%.0f%%", s.clientCpuPct);
            else std::snprintf(b, sizeof b, "N/A");
            cell(b); cx += colW[2];
            std::snprintf(b, sizeof b, "%zuMB", s.clientRss / (1024 * 1024)); cell(b); cx += colW[3];
            std::snprintf(b, sizeof b, "%zuMB", s.gpuBytes / (1024 * 1024)); cell(b); cx += colW[4];
            if (s.gpuPct >= 0) std::snprintf(b, sizeof b, "%.0f%%", s.gpuPct); else std::snprintf(b, sizeof b, "N/A");
            cell(b); cx += colW[5];
            if (s.serverCpuPct >= 0) std::snprintf(b, sizeof b, "%.0f%%", s.serverCpuPct); else std::snprintf(b, sizeof b, "N/A");
            cell(b); cx += colW[6];
            if (s.serverRss) std::snprintf(b, sizeof b, "%zuMB", s.serverRss / (1024 * 1024)); else std::snprintf(b, sizeof b, "N/A");
            cell(b); cx += colW[7];
            std::snprintf(b, sizeof b, "%.0f", s.fps); cell(b); cx += colW[8];
            cell(tak::speedLevelText(tak::speedLevelOf(s.simSpeed)).c_str());
            y += rowH;
        }
        y += 30 * k;
        blockText("SETTINGS", cxL, y, setPx, {205, 210, 225, 255});
        y += 7 * setPx + 24 * k;
        for (const std::string& t : settings) { blockText(t, cxL, y, setPx, {200, 205, 215, 255}); y += setRowU * k; }
        y += 20 * k;

        // SAVE AS JPG and DONE, side by side.
        const float dw = 240 * k, dh = 56 * k, gap = 24 * k;
        const SDL_FRect save{px0 + (panelW - 2 * dw - gap) * 0.5f, y, dw, dh};
        const SDL_FRect done{save.x + dw + gap, y, dw, dh};
        benchSaveRect_ = save; benchDoneRect_ = done; benchPanelRect_ = panel;
#ifndef NDEBUG
        std::string saveDrawn;   // TAK_BENCH_REPORT: SAVE AS JPG as button() last drew it
#endif
        // `live` false is the greyed button (Options' colours for one); `hover` false
        // leaves the pointer's highlight off, for the picture.
        auto button = [&](const SDL_FRect& r, const char* label, bool live, bool hover) {
#ifndef NDEBUG
            if (&r == &save) saveDrawn = std::string(live ? "LIVE " : "GREYED ") + label;
#endif
            const bool hot = live && hover && mouseX_ >= r.x && mouseX_ <= r.x + r.w && mouseY_ >= r.y && mouseY_ <= r.y + r.h;
            if (live) SDL_SetRenderDrawColor(ren_, hot ? 90 : 60, hot ? 110 : 66, hot ? 150 : 86, 255);
            else SDL_SetRenderDrawColor(ren_, 34, 38, 46, 255);
            SDL_RenderFillRectF(ren_, &r);
            if (live) SDL_SetRenderDrawColor(ren_, hot ? 180 : 100, hot ? 200 : 110, hot ? 240 : 140, 255);
            else SDL_SetRenderDrawColor(ren_, 70, 76, 92, 255);
            SDL_RenderDrawRectF(ren_, &r);
            blockText(label, r.x + (dw - blockWidth(label, setPx)) * 0.5f, r.y + (dh - 7 * setPx) * 0.5f, setPx,
                      live ? SDL_Color{230, 234, 244, 255} : SDL_Color{110, 115, 125, 255});
        };
        // A click on SAVE AS JPG is served here, by this draw: the screen is complete but
        // for the buttons, so they go on plain (no highlight under the pointer), the frame
        // is read back and saved, and the buttons are then drawn over as they now are.
        // The note is not on yet, and the cursor goes on after the whole draw (main()).
        if (benchSavePending_) {
            benchSavePending_ = false;
            button(save, "SAVE AS JPG", true, false);
            button(done, "DONE", true, false);
            saveBenchmarkJpg(winW, winH);
        }
        button(save, benchSaved_ ? "SAVED" : "SAVE AS JPG", !benchSaved_, true);
        button(done, "DONE", true, true);
        // Where the picture went, or why there is none: until DONE.
        std::string note = benchSaveNote_;
        const float notePx = noteU * k, noteY = y + dh + 16 * k;
        if (!note.empty()) {
            if (!benchSavePath_.empty()) note += tak::uiTextFitStart(benchSavePath_, notePx, tableW - blockWidth(note, notePx));
            else note = tak::uiTextFit(note, notePx, tableW);
            blockText(note, px0 + (panelW - blockWidth(note, notePx)) * 0.5f, noteY, notePx,
                      benchSaveFailed_ ? SDL_Color{240, 130, 120, 255} : SDL_Color{165, 170, 185, 255});
        }
#ifndef NDEBUG
        // TAK_BENCH_REPORT: the list and the buttons where they were drawn, for
        // benchmark_pathfinding and benchmark_save; SAVE AS JPG and the note under it
        // again whenever either changes. LIVE or GREYED and the label are what button()
        // drew this frame (saveDrawn), not a second reading of benchSaved_.
        static bool reported = false;
        if (tak::devFlag("TAK_BENCH_REPORT")) {
            if (!reported) {
                reported = true;
                for (const std::string& t : settings) std::printf("BENCHRESULT %s\n", t.c_str());
                std::printf("BENCHDONE %d %d %.1f %.1f %.1f %.1f\n", winW, winH, done.x, done.y, done.w, done.h);
            }
            char at[160];
            std::snprintf(at, sizeof at, "BENCHSAVE %d %d %.1f %.1f %.1f %.1f %s\n", winW, winH, save.x, save.y, save.w,
                          save.h, saveDrawn.c_str());
            std::string line = at;
            if (!note.empty()) {
                const float nw = blockWidth(note, notePx);
                std::snprintf(at, sizeof at, "BENCHNOTE %.1f %.1f %.1f %.1f %s ", px0 + (panelW - nw) * 0.5f, noteY, nw,
                              7 * notePx, benchSaveFailed_ ? "WARNING" : "PLAIN");
                line += at + note + "\n";
            }
            if (line != benchSaveReported_) {
                std::fputs((benchSaveReported_ = line).c_str(), stdout);
                std::fflush(stdout);
            }
        }
        // TAK_BENCH_CLICKS="save;shot:after.png;save": the pointer on this screen, one step
        // every few frames so each is handled and drawn before the next. "save" clicks the
        // middle of SAVE AS JPG where it was drawn, "hover" only moves the pointer there,
        // "miss" clicks the panel just left of the button, "away" moves the pointer off
        // the panel, "shot:<file>" writes this frame as a PNG (main(), with the cursor
        // on), and "until:<file>" waits for that file to exist (a test's go-ahead).
        // TAK_QUIT_TICK then waits for the last step (benchClicksDone).
        if (const char* script = tak::devEnv("TAK_BENCH_CLICKS"); script && !benchClicksDone_ && --benchClickWait_ < 0) {
            std::vector<std::string> steps;
            for (const char* p = script; *p;) {
                const char* e = std::strchr(p, ';');
                steps.emplace_back(p, e ? size_t(e - p) : std::strlen(p));
                p = e ? e + 1 : p + steps.back().size();
            }
            if (benchClickStep_ >= steps.size()) benchClicksDone_ = true;
            else {
                const std::string& step = steps[benchClickStep_++];
                benchClickWait_ = 3;
                SDL_Event ev{};
                auto point = [&](float x, float y, bool click) {
                    ev.type = SDL_MOUSEMOTION;
                    ev.motion.x = int(x); ev.motion.y = int(y);
                    SDL_PushEvent(&ev);
                    if (!click) return;
                    ev.type = SDL_MOUSEBUTTONDOWN;
                    ev.button.button = SDL_BUTTON_LEFT;
                    ev.button.x = int(x); ev.button.y = int(y);
                    SDL_PushEvent(&ev);
                    ev.type = SDL_MOUSEBUTTONUP;
                    SDL_PushEvent(&ev);
                };
                if (step == "save") point(save.x + save.w * 0.5f, save.y + save.h * 0.5f, true);
                else if (step == "hover") point(save.x + save.w * 0.5f, save.y + save.h * 0.5f, false);
                else if (step == "miss") point(save.x - gap * 0.5f, save.y + save.h * 0.5f, true);
                else if (step == "away") point(0, 0, false);
                else if (step.rfind("shot:", 0) == 0) benchShotPath_ = step.substr(5);
                else if (std::error_code ec; step.rfind("until:", 0) == 0 && !std::filesystem::exists(tak::u8path(step.substr(6)), ec)) {
                    --benchClickStep_;
                    benchClickWait_ = 0;
                }
            }
        }
#endif
    }
