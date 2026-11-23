// renderdriver_test -- the Options RENDERER choice, without a GPU.
//
// Covers the parts that decide what startup asks SDL for: the stored-id normaliser,
// the id -> plain-name mapping, the plan (AUTO untouched, Software's flags, an id this
// build lacks), and the "did SDL actually honour the hint" check that drives the
// fallback to AUTO. Then a real SDL query of this build's driver list, which needs no
// window or GPU.

#include "client/renderdriver.h"

#include <cstdio>
#include <string>
#include <vector>

namespace rd = tak::renderdriver;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

int main() {
    std::printf("renderdriver_test\n");

    // ---- normalise: what a settings file value becomes ----
    check(rd::normalize("") == "", "empty -> auto");
    check(rd::normalize("auto") == "", "'auto' -> auto");
    check(rd::normalize("  AUTO \r") == "", "'AUTO' with whitespace -> auto");
    check(rd::normalize("OpenGL") == "opengl", "case folded");
    check(rd::normalize(" direct3d11 ") == "direct3d11", "trimmed");
    check(rd::normalize("vulkan") == "vulkan", "unknown but well-formed id kept");
    check(rd::normalize("open gl") == "", "embedded space -> auto");
    check(rd::normalize("gl;rm -rf") == "", "punctuation -> auto");
    check(rd::normalize(std::string(33, 'a')) == "", "over-long -> auto");

    // ---- labels ----
    check(rd::label("") == "Auto", "label: auto");
    check(rd::label("opengl") == "OpenGL", "label: opengl");
    check(rd::label("opengles2") == "OpenGL ES 2", "label: opengles2");
    check(rd::label("opengles") == "OpenGL ES", "label: opengles");
    check(rd::label("direct3d") == "Direct3D 9", "label: direct3d");
    check(rd::label("direct3d11") == "Direct3D 11", "label: direct3d11");
    check(rd::label("direct3d12") == "Direct3D 12", "label: direct3d12");
    check(rd::label("metal") == "Metal", "label: metal");
    check(rd::label("software") == "Software", "label: software");
    check(rd::label("vulkan") == "vulkan", "label: unknown id shown as-is");
    for (const char* id : {"opengl", "opengles2", "opengles", "direct3d", "direct3d11",
                           "direct3d12", "metal", "software", "averyveryverylongid"})
        if (rd::shortLabel(id).size() > 7) { check(false, "short label <= 7 chars"); break; }
    check(rd::shortLabel("software") == "SW" && rd::shortLabel("opengl") == "GL", "short labels");

    // ---- plan ----
    const std::vector<std::string> linuxDrivers{"opengl", "opengles2", "opengles", "software"};
    const Uint32 vs = SDL_RENDERER_PRESENTVSYNC, acc = SDL_RENDERER_ACCELERATED;
    {
        auto p = rd::plan("", linuxDrivers, vs);
        check(p.hint.empty() && p.flags == vs && !p.fallback, "auto: no hint, today's flags");
    }
    {
        auto p = rd::plan("opengles2", linuxDrivers, vs);
        check(p.hint == "opengles2" && p.flags == vs && !p.fallback, "chosen: hint + today's flags");
    }
    {
        auto p = rd::plan("software", linuxDrivers, vs);
        check(p.hint == "software" && p.flags == SDL_RENDERER_SOFTWARE && !p.fallback,
              "software (vsync on): SOFTWARE flag only");
        p = rd::plan("software", linuxDrivers, acc);
        check(p.flags == SDL_RENDERER_SOFTWARE, "software (vsync off): never ACCELERATED");
    }
    {
        auto p = rd::plan("direct3d11", linuxDrivers, vs);
        check(p.fallback && p.hint.empty() && p.flags == vs && !p.reason.empty(),
              "missing from this build: fall back to auto");
        p = rd::plan("vulkan", linuxDrivers, vs);
        check(p.fallback && p.hint.empty(), "unknown id: fall back to auto");
        p = rd::plan("opengl", {}, vs);
        check(p.fallback, "no drivers listed: fall back to auto");
    }
    check(!rd::plan("OPENGL", linuxDrivers, vs).fallback, "case-insensitive availability");

    // ---- honoured: the created renderer is the proof ----
    check(rd::honoured("", "software"), "auto accepts whatever SDL made");
    check(rd::honoured("", nullptr), "auto accepts a null name");
    check(rd::honoured("opengles2", "opengles2"), "hint honoured");
    check(rd::honoured("opengl", "OpenGL"), "hint honoured (case-insensitive)");
    check(!rd::honoured("opengles2", "opengl"), "SDL fell through to another driver -> fallback");
    check(!rd::honoured("software", nullptr), "creation failed -> fallback");

    // ---- AUTO: the Windows order and what it keeps (pure; the same on every platform) ----
    {
        const std::vector<std::string> win{"direct3d", "direct3d11", "direct3d12", "opengl",
                                           "opengles2", "software"};
        auto o = rd::autoOrder(rd::Platform::Windows, win);
        check(o == std::vector<std::string>({"opengl", "direct3d11", "direct3d"}),
              "windows auto: OpenGL, Direct3D 11, Direct3D 9");
        check(rd::autoOrder(rd::Platform::Other, win).empty() &&
                  rd::autoOrder(rd::Platform::Other, linuxDrivers).empty(),
              "linux auto: no order, SDL's own pick");
        const std::vector<std::string> mac{"metal", "opengl", "opengles2", "software"};
        check(rd::autoOrder(rd::Platform::MacOS, mac) == std::vector<std::string>({"opengl", "metal"}),
              "macOS auto: OpenGL, then Metal");
        check(rd::autoOrder(rd::Platform::MacOS, mac, "opengl") == std::vector<std::string>({"metal"}),
              "macOS auto: a failed saved OpenGL is not retried");
        check(rd::autoOrder(rd::Platform::MacOS, mac, "metal") == std::vector<std::string>({"opengl"}),
              "macOS auto: a failed saved Metal is not retried");
        check(rd::autoOrder(rd::Platform::MacOS, {"metal", "software"}) ==
                  std::vector<std::string>({"metal"}),
              "macOS auto: a build without OpenGL starts at Metal");
        check(rd::autoOrder(rd::Platform::MacOS, {"software"}).empty(), "macOS auto: neither -> SDL");
        check(rd::autoOrder(rd::Platform::MacOS, {"direct3d11", "opengl"}) ==
                  std::vector<std::string>({"opengl"}),
              "macOS auto: never tries Direct3D");
        check(rd::autoOrder(rd::Platform::Windows, {"direct3d", "direct3d11", "software"}) ==
                  std::vector<std::string>({"direct3d11", "direct3d"}),
              "windows auto: a build without OpenGL starts at Direct3D 11");
        check(rd::autoOrder(rd::Platform::Windows, {"software"}).empty(), "windows auto: none of the three -> SDL");
        check(rd::autoOrder(rd::Platform::Windows, win, "opengl") ==
                  std::vector<std::string>({"direct3d11", "direct3d"}),
              "a chosen driver that failed is not retried");
        check(rd::autoOrder(rd::Platform::Windows, win, "DIRECT3D11") ==
                  std::vector<std::string>({"opengl", "direct3d"}),
              "skip is case-insensitive");
        check(rd::autoOrder(rd::Platform::Windows, {"OpenGL", "Direct3D11"}) ==
                  std::vector<std::string>({"opengl", "direct3d11"}),
              "availability is case-insensitive");

        check(rd::judge("opengl", true, "NVIDIA Corporation", "NVIDIA GeForce RTX 5070/PCIe/SSE2").keep,
              "judge: NVIDIA OpenGL kept");
        check(rd::judge("opengl", true, "ATI Technologies Inc.", "AMD Radeon RX 7800 XT").keep,
              "judge: AMD OpenGL kept");
        check(rd::judge("opengl", true, "Intel", "Intel(R) Arc(TM) Graphics").keep,
              "judge: Intel OpenGL kept");
        auto v = rd::judge("opengl", true, "Microsoft Corporation", "GDI Generic");
        check(!v.keep && v.reason.find("GDI Generic") != std::string::npos,
              "judge: GDI Generic rejected, named in the reason");
        check(!rd::judge("opengl", true, "", "GDI Generic").keep, "judge: GDI Generic without vendor");
        check(!rd::judge("opengl", true, "Mesa", "llvmpipe (LLVM 19.1.0, 256 bits)").keep,
              "judge: llvmpipe rejected");
        check(!rd::judge("opengl", true, "Mesa", "softpipe").keep, "judge: softpipe rejected");
        check(!rd::judge("opengl", true, "Google Inc.", "Google SwiftShader").keep,
              "judge: SwiftShader rejected");
        auto va = rd::judge("opengl", true, "Apple Computer, Inc.", "Apple Software Renderer");
        check(!va.keep && va.reason.find("Apple Software Renderer") != std::string::npos,
              "judge: Apple's software OpenGL rejected, named in the reason");
        check(rd::judge("opengl", true, "Apple", "Apple M2").keep, "judge: Apple M-series OpenGL kept");
        check(rd::judge("opengl", true, "Apple", "Apple A18 Pro").keep, "judge: Apple A18 Pro OpenGL kept");
        check(rd::judge("opengl", true, "Intel Inc.", "Intel(R) Iris(TM) Plus Graphics 655").keep,
              "judge: Intel Mac OpenGL kept");
        check(rd::judge("metal", true, "", "").keep, "judge: accelerated Metal kept");
        check(!rd::judge("opengl", true, "Microsoft Corporation",
                         "D3D12 (Qualcomm(R) Adreno(TM) X1-85 GPU)").keep,
              "judge: Microsoft's OpenGL-over-D3D12 rejected");
        check(!rd::judge("opengl", true, "microsoft corporation", "x").keep,
              "judge: vendor match is case-insensitive");
        check(!rd::judge("opengl", false, "NVIDIA Corporation", "NVIDIA GeForce").keep,
              "judge: not accelerated rejected");
        check(!rd::judge("direct3d11", false, "", "").keep, "judge: unaccelerated Direct3D rejected");
        check(rd::judge("direct3d11", true, "", "").keep && rd::judge("direct3d", true, "", "").keep,
              "judge: accelerated Direct3D kept (no GL strings)");
        check(rd::judge("opengl", true, "", "").keep,
              "judge: unreadable GL strings are not proof of software");
        check(rd::judge("direct3d11", true, "Microsoft Corporation", "GDI Generic").keep,
              "judge: GL strings only matter for an OpenGL renderer");

        check(rd::autoLabel(true, "opengl") == "Auto (OpenGL)", "auto label names the pick");
        check(rd::autoLabel(true, "direct3d11") == "Auto (Direct3D 11)", "auto label: Direct3D 11");
        check(rd::autoLabel(false, "opengl") == "Auto", "auto label plain when a choice runs");
        check(rd::autoLabel(true, "") == "Auto", "auto label plain before a renderer exists");
    }

    // ---- one-time notice ----
    {
        auto& a = rd::active();
        a = {};
        check(rd::pendingNotice().empty(), "no failure, no notice");
        a.failed = "opengles2";
        check(rd::pendingNotice() == "OpenGL ES 2 renderer unavailable - using Auto",
              "notice names the failed renderer");
        a.noticeShown = true;
        check(rd::pendingNotice().empty(), "notice shown only once");
        a = {};
    }

    // ---- this SDL build's real list (no window or GPU needed) ----
    {
        SDL_version v{};
        SDL_GetVersion(&v);
        std::printf("  linked SDL %d.%d.%d\n", v.major, v.minor, v.patch);
        auto ids = rd::available();
        std::printf("  SDL %d render driver(s):", int(ids.size()));
        for (auto& id : ids) std::printf(" %s(%s)", id.c_str(), rd::label(id).c_str());
        std::printf("\n");
        bool sane = !ids.empty();
        for (auto& id : ids) sane = sane && rd::normalize(id) == id;
        check(sane, "every SDL driver id survives normalise unchanged");
        bool sw = false;
        for (auto& id : ids) sw = sw || id == "software";
        check(sw, "software is always available");
    }

    std::printf(g_fail ? "renderdriver_test: %d FAILED\n" : "renderdriver_test: all ok\n", g_fail);
    return g_fail ? 1 : 0;
}
