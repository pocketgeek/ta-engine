#include "client/gameview.h"

// Out-of-line GameView method definitions (benchmark concern), split from the
// class body in gameview.h so editing a body recompiles only this translation
// unit. Trivial getters, ctors, static, template, constexpr and default-arg
// methods stay inline in the header. Grouping is by name heuristic.

    void GameView::benchmarkBaseline() {
        benchPrevWallMs_ = SDL_GetTicks64();
        benchCliPrev_ = ta::proc::sample(0);
        benchSrvPrev_ = benchServerPid_ ? ta::proc::sample(benchServerPid_) : ta::proc::Sample{};
        benchNextTick_ = 300; benchSamples_.clear(); benchStatsShown_ = false;
    }

    void GameView::pushBenchSample(int gameSec) {
        uint64_t nowMs = SDL_GetTicks64();
        double wallSec = benchPrevWallMs_ ? double(nowMs - benchPrevWallMs_) / 1000.0 : 0;
        ta::proc::Sample cli = ta::proc::sample(0);
        ta::proc::Sample srv = benchServerPid_ ? ta::proc::sample(benchServerPid_) : ta::proc::Sample{};
        auto pct = [&](const ta::proc::Sample& n, const ta::proc::Sample& p) {
            return (n.ok && wallSec > 0) ? (n.cpuSeconds - p.cpuSeconds) / wallSec * 100.0 : 0.0;
        };
        BenchSample s;
        s.gameSec = gameSec;
        s.liveUnits = int(front().live.size());
        s.clientCpuPct = pct(cli, benchCliPrev_);
        s.serverCpuPct = pct(srv, benchSrvPrev_);
        s.clientRss = cli.rssBytes;
        s.serverRss = srv.rssBytes;
        s.fps = fps_;
        s.simSpeed = actualSpeed_;
        s.gpuBytes = gpuvram::bytes();       // client GPU texture memory (bounded by the cap)
        ta::proc::GpuSample gpu = ta::proc::gpuSample();   // whole-device util % + VRAM
        if (gpu.ok) {
            s.gpuPct = gpu.utilPct;
            s.gpuSysUsed = gpu.memUsed;
            if (benchGpuName_.empty()) { benchGpuName_ = gpu.name; benchGpuTotal_ = gpu.memTotal; }
        }
        benchSamples_.push_back(s);
        benchCliPrev_ = cli; benchSrvPrev_ = srv; benchPrevWallMs_ = nowMs;
    }

    void GameView::benchmarkSample() {
        if (!benchmarkMode_ || !world_.benchmarkMode() || benchStatsShown_) return;
        uint32_t gt = front().gameTick, end = world_.benchmarkEndTick();
        while (benchNextTick_ <= gt && benchNextTick_ <= end) {
            pushBenchSample(int(benchNextTick_ / 30));
            benchNextTick_ += 300;
        }
        if (gt >= end) benchStatsShown_ = true;   // run complete -> show the stats overlay
    }

    void GameView::stopBenchmark() {
        if (benchStatsShown_ || !benchmarkMode_) return;
        if (!world_.benchmarkMode()) { menuRequested_ = true; return; }   // still in setup -> abort
        int sec = int(front().gameTick / 30);
        if (benchSamples_.empty() || benchSamples_.back().gameSec != sec) pushBenchSample(sec);
        benchStatsShown_ = true;
    }

    void GameView::benchmarkCamera(float dt, int winW, int winH) {
        if (!benchmarkMode_ || benchStatsShown_ || !world_.benchmarkMode()) return;
        int leg = int(front().gameTick / 225);   // 0..7, one per faction / 7.5s leg (60s / 8)
        if (leg < 0) leg = 0;
        if (leg > 7) leg = 7;
        if (leg != benchCamLeg_) { benchCamLeg_ = leg; benchLegT_ = 0.0f; }   // new leg -> reset
        benchLegT_ += dt;
        float p = benchLegT_ / 7.5f;              // progress through the 7.5s leg
        if (p > 1.0f) p = 1.0f;
        // Zoom: all the way out at the leg start, zooming in over the leg.
        float zOut = mapView_.minZoom(winW, winH);
        float zIn = std::max(zOut * 2.5f, 1.6f);  // a clear close-up, comfortably above zOut
        float z = zOut + (zIn - zOut) * p;
        mapView_.setZoom(z);
        // Track this leg's AI commander (player == leg) from the render snapshot.
        for (const UnitR* u : front().live)
            if (u && u->player == leg && isCommanderType(u->type)) {
                mapView_.setOffset(u->x - winW * 0.5f / z, u->z - winH * 0.5f / z);
                break;
            }
        mapView_.clampOffset(winW, winH);
    }

    void GameView::renderBenchmarkStats(int winW, int winH) {
        // One scale factor drives the whole panel, so it stays big + legible at 4K.
        float k = float(winH) / 1000.0f; k = k < 1.0f ? 1.0f : (k > 2.4f ? 2.4f : k);
        const float titlePx = 4.0f * k, subPx = 1.8f * k, cellPx = 2.2f * k, setPx = 2.0f * k;
        const float pad = 44 * k, rowH = 34 * k;
        const float colW[10] = {82*k, 96*k, 122*k, 122*k, 122*k, 70*k, 122*k, 122*k, 70*k, 96*k};
        float tableW = 0; for (float c : colW) tableW += c;
        const char* title = "BENCHMARK RESULTS";
        char subBuf[160];
        std::snprintf(subBuf, sizeof subBuf,
                      "8-AI FFA -- ULASEM ARENA -- %s: 1 UNIT/FACTION EVERY %s FOR 60S",
                      ta::sim::benchmarkLevelName(benchmarkLevel_),
                      ta::sim::benchmarkLevelInterval(benchmarkLevel_));
        const char* sub = subBuf;

        float contentW = tableW;
        if (blockWidth(sub, subPx) > contentW) contentW = blockWidth(sub, subPx);
        int rows = int(benchSamples_.size());
        float contentH = 7 * (titlePx + subPx + cellPx + setPx) + rows * rowH + 482 * k;
        float panelW = contentW + 2 * pad, panelH = contentH + 2 * pad;
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
        char b[64];
        for (const auto& s : benchSamples_) {
            cx = cxL;
            auto cell = [&](const char* t) { blockText(t, cx, y, cellPx, {228, 231, 240, 255}); };
            std::snprintf(b, sizeof b, "%dS", s.gameSec); cell(b); cx += colW[0];
            std::snprintf(b, sizeof b, "%d", s.liveUnits); cell(b); cx += colW[1];
            std::snprintf(b, sizeof b, "%.0f%%", s.clientCpuPct); cell(b); cx += colW[2];
            std::snprintf(b, sizeof b, "%zuMB", s.clientRss / (1024 * 1024)); cell(b); cx += colW[3];
            std::snprintf(b, sizeof b, "%zuMB", s.gpuBytes / (1024 * 1024)); cell(b); cx += colW[4];
            if (s.gpuPct >= 0) std::snprintf(b, sizeof b, "%.0f%%", s.gpuPct); else std::snprintf(b, sizeof b, "N/A");
            cell(b); cx += colW[5];
            if (s.serverRss) std::snprintf(b, sizeof b, "%.0f%%", s.serverCpuPct); else std::snprintf(b, sizeof b, "N/A");
            cell(b); cx += colW[6];
            if (s.serverRss) std::snprintf(b, sizeof b, "%zuMB", s.serverRss / (1024 * 1024)); else std::snprintf(b, sizeof b, "N/A");
            cell(b); cx += colW[7];
            std::snprintf(b, sizeof b, "%.0f", s.fps); cell(b); cx += colW[8];
            std::snprintf(b, sizeof b, "%.2fX", s.simSpeed); cell(b);
            y += rowH;
        }
        y += 30 * k;
        blockText("SETTINGS", cxL, y, setPx, {205, 210, 225, 255});
        y += 7 * setPx + 24 * k;
        auto sl = [&](const std::string& t) { blockText(t, cxL, y, setPx, {200, 205, 215, 255}); y += 28 * k; };
        std::snprintf(b, sizeof b, "RESOLUTION  %d X %d", winW, winH); sl(b);
        if (!benchGpuName_.empty()) sl(std::string("GPU         ") + benchGpuName_);
        else { SDL_RendererInfo ri; if (SDL_GetRendererInfo(ren_, &ri) == 0) { std::snprintf(b, sizeof b, "RENDERER    %s", ri.name); sl(b); } }
        sl(std::string("FULLSCREEN  ") + (settings_ && settings_->fullscreen ? "ON" : "OFF"));
        sl(std::string("VSYNC       ") + (settings_ && settings_->vsync ? "ON" : "OFF"));
        if (settings_ && !settings_->vsync) { std::snprintf(b, sizeof b, "MAX FPS     %d", settings_->maxFps); sl(b); }
        else sl("MAX FPS     (VSYNC)");
        sl(std::string("ANTI-ALIAS  ") + (settings_ && settings_->antiAlias ? "2X" : "OFF"));
        sl(std::string("BILINEAR    ") + (settings_ && settings_->bilinear ? "ON" : "OFF"));
        // GPU texture memory: the self-calibrating cap, and this run's peak usage/pages.
        size_t gpuPeak = 0, sysPeak = 0;
        for (const auto& s : benchSamples_) {
            if (s.gpuBytes > gpuPeak) gpuPeak = s.gpuBytes;
            if (s.gpuSysUsed > sysPeak) sysPeak = s.gpuSysUsed;
        }
        std::snprintf(b, sizeof b, "GPU TEX CAP %zuMB", gpuvram::cap() >> 20); sl(b);
        std::snprintf(b, sizeof b, "GPU PEAK    %zuMB", gpuPeak >> 20); sl(b);
        if (sysPeak) {
            if (benchGpuTotal_) std::snprintf(b, sizeof b, "SYS VRAM    %zu / %zuMB PEAK", sysPeak >> 20, benchGpuTotal_ >> 20);
            else std::snprintf(b, sizeof b, "SYS VRAM    %zuMB PEAK", sysPeak >> 20);
            sl(b);
        }
        y += 20 * k;

        const float dw = 240 * k, dh = 56 * k;
        SDL_FRect done{px0 + (panelW - dw) * 0.5f, y, dw, dh}; benchDoneRect_ = done;
        bool hot = mouseX_ >= done.x && mouseX_ <= done.x + done.w && mouseY_ >= done.y && mouseY_ <= done.y + done.h;
        SDL_SetRenderDrawColor(ren_, hot ? 90 : 60, hot ? 110 : 66, hot ? 150 : 86, 255); SDL_RenderFillRectF(ren_, &done);
        SDL_SetRenderDrawColor(ren_, hot ? 180 : 100, hot ? 200 : 110, hot ? 240 : 140, 255); SDL_RenderDrawRectF(ren_, &done);
        blockText("DONE", done.x + (dw - blockWidth("DONE", setPx)) * 0.5f, done.y + (dh - 7 * setPx) * 0.5f,
                  setPx, {230, 234, 244, 255});
    }

