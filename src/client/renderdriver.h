#pragma once

// The RENDERER option: which SDL render driver the client asks for at startup.
//
// Settings::renderer stores SDL's own driver id ("opengl", "direct3d11", "metal",
// "software", ...). "" means AUTO: on Windows and macOS an ordered try, OpenGL first
// (see autoOrder below); on Linux SDL's own choice (OpenGL), as every build made it
// before this option existed. The choice is applied ONCE, where main() creates the renderer;
// changing it takes effect after a restart.
//
// The planning half (normalise / label / plan / verify, AUTO's order and judge) is
// plain logic over strings so renderdriver_test can cover it without a GPU. The runtime half (which drivers this
// SDL build has, and what actually got created) is a few SDL queries and one
// process-wide record that the Options screen, the stats panel and the benchmark read.

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace tak::renderdriver {

// Canonical stored form of a saved value: trimmed, lower-case, "auto" -> "". Anything
// that cannot be an SDL driver id (empty after trimming, over 32 chars, or holding
// characters outside [a-z0-9_]) also becomes "" (AUTO), so a mangled file can never
// feed garbage to SDL_SetHint. A well-formed id that this build does not have is KEPT:
// startup reports it and falls back to AUTO for that launch (see plan()).
inline std::string normalize(std::string_view raw) {
    size_t a = raw.find_first_not_of(" \t\r\n"), b = raw.find_last_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    std::string id(raw.substr(a, b - a + 1));
    if (id.size() > 32) return {};
    for (char& c : id) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return {};
    }
    return id == "auto" ? std::string() : id;
}

// Plain name for a driver id, as the Options list shows it. Unknown ids show as-is.
inline std::string label(std::string_view id) {
    if (id.empty() || id == "auto") return "Auto";
    struct Name { std::string_view id, label; };
    static constexpr Name kNames[] = {
        {"opengl", "OpenGL"},        {"opengles2", "OpenGL ES 2"}, {"opengles", "OpenGL ES"},
        {"direct3d", "Direct3D 9"},  {"direct3d11", "Direct3D 11"},
        {"direct3d12", "Direct3D 12"}, {"metal", "Metal"},          {"software", "Software"},
    };
    for (const Name& n : kNames) if (n.id == id) return std::string(n.label);
    return std::string(id);
}

// At most seven characters, for the stats panel's value column.
inline std::string shortLabel(std::string_view id) {
    struct Name { std::string_view id, label; };
    static constexpr Name kNames[] = {
        {"opengl", "GL"},       {"opengles2", "GLES2"}, {"opengles", "GLES"},
        {"direct3d", "D3D9"},   {"direct3d11", "D3D11"}, {"direct3d12", "D3D12"},
        {"metal", "METAL"},     {"software", "SW"},
    };
    if (id.empty()) return "?";
    for (const Name& n : kNames) if (n.id == id) return std::string(n.label);
    std::string s(id.substr(0, 7));
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// SDL's render-driver names compare case-insensitively (SDL_CreateRenderer matches the
// hint with SDL_strcasecmp), so verification does the same.
inline bool sameId(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// What startup should ask SDL for.
struct Plan {
    std::string hint;      // SDL_HINT_RENDER_DRIVER value; "" = leave the hint alone (AUTO)
    Uint32 flags = 0;      // SDL_CreateRenderer flags
    bool fallback = false; // the saved choice cannot be honoured: running AUTO instead
    std::string reason;    // why, when fallback is set (for the log)
};

// saved: the normalised Settings::renderer. available: this SDL build's driver ids
// (SDL_GetRenderDriverInfo order). autoFlags: the flags today's AUTO path uses.
inline Plan plan(const std::string& saved, const std::vector<std::string>& available,
                 Uint32 autoFlags) {
    Plan p;
    p.flags = autoFlags;
    if (saved.empty()) return p;   // AUTO: main() runs autoOrder(), then SDL's own pick
    bool have = std::any_of(available.begin(), available.end(),
                            [&](const std::string& d) { return sameId(d, saved); });
    if (!have) {
        p.fallback = true;
        p.reason = "'" + saved + "' is not a renderer this build of SDL provides";
        return p;
    }
    p.hint = saved;
    // SDL's software renderer never claims ACCELERATED, so ask for it by the SOFTWARE
    // flag alone: any flag it does not advertise would make SDL's flag match reject it.
    // VSync is still requested afterwards through SDL_RenderSetVSync, which SDL honours
    // on the window surface or simulates by pacing Present.
    if (sameId(saved, "software")) p.flags = SDL_RENDERER_SOFTWARE;
    else p.flags = autoFlags & ~Uint32(SDL_RENDERER_SOFTWARE);
    return p;
}

// Did SDL hand back the driver that was asked for? An empty hint (AUTO) accepts
// anything. SDL quietly falls through to its default list when a hinted driver fails
// to create, so the created renderer's name is the only proof.
inline bool honoured(const std::string& hint, const char* createdName) {
    if (hint.empty()) return true;
    return createdName && sameId(hint, createdName);
}

// ---- AUTO ------------------------------------------------------------------------
//
// On Windows AUTO tries, in order, OpenGL, Direct3D 11 and Direct3D 9, keeping the
// first one that creates and is a real hardware driver; if none does, SDL's own pick.
// OpenGL comes first because every native fast path (client/nativepass.h,
// geometrysubmit.h, paletteindexed.h) is OpenGL-only; SDL's own Windows order starts
// at Direct3D 9. Windows ships a software OpenGL
// ("GDI Generic", vendor Microsoft) for a machine with no GPU driver, and Microsoft's
// OpenGL compatibility pack layers OpenGL over Direct3D 12 (also vendor Microsoft);
// either is slower than Direct3D 11 on the same machine, so AUTO moves on.
// On macOS AUTO tries OpenGL, then Metal, for the same fast paths (SDL's own macOS
// pick is Metal; in a 2026-10-25 test on a MacBook Neo
// OpenGL drew a 2,000-unit view 2.6x faster than Metal). Apple's software OpenGL ("Apple Software
// Renderer") is passed over like Windows' one.
// On Linux AUTO is unchanged: no hint, SDL's own pick (OpenGL), and an empty order.
enum class Platform { Windows, MacOS, Other };

// AUTO's candidates, in the order startup tries them, from those this build has;
// `skip` (a saved choice that already failed this launch) is left out. Empty = just
// SDL's own pick. SDL's own pick always follows the list as the last resort.
inline std::vector<std::string> autoOrder(Platform platform, const std::vector<std::string>& available,
                                          std::string_view skip = {}) {
    std::vector<std::string> out;
    std::vector<std::string_view> order;
    if (platform == Platform::Windows) order = {"opengl", "direct3d11", "direct3d"};
    else if (platform == Platform::MacOS) order = {"opengl", "metal"};
    for (std::string_view id : order) {
        if (!skip.empty() && sameId(id, skip)) continue;
        if (std::any_of(available.begin(), available.end(),
                        [&](const std::string& d) { return sameId(d, id); }))
            out.emplace_back(id);
    }
    return out;
}

inline bool containsNoCase(std::string_view hay, std::string_view needle) {
    if (needle.empty() || needle.size() > hay.size()) return needle.empty();
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (sameId(hay.substr(i, needle.size()), needle)) return true;
    return false;
}

// Whether AUTO keeps a renderer it created. created: SDL's driver name; accelerated:
// SDL_RENDERER_ACCELERATED; glVendor/glRenderer: GL_VENDOR/GL_RENDERER for an OpenGL
// renderer ("" for others, or when unreadable). A chosen (non-AUTO) driver is never
// judged: the player asked for it.
struct Verdict {
    bool keep = true;
    std::string reason;   // why not, for the log
};
inline Verdict judge(std::string_view created, bool accelerated, std::string_view glVendor,
                     std::string_view glRenderer) {
    Verdict v;
    if (!accelerated) {
        v.keep = false;
        v.reason = "SDL reports it is not hardware accelerated";
        return v;
    }
    if (!sameId(created.substr(0, 6), "opengl")) return v;
    for (std::string_view soft : {"gdi generic", "llvmpipe", "softpipe", "swiftshader",
                                  "software rasterizer", "software renderer"})
        if (containsNoCase(glRenderer, soft)) {
            v.keep = false;
            v.reason = "software OpenGL (" + std::string(glRenderer) + ")";
            return v;
        }
    if (containsNoCase(glVendor, "microsoft")) {
        v.keep = false;
        v.reason = "Microsoft's OpenGL (" + std::string(glVendor) + ", " + std::string(glRenderer) + ")";
    }
    return v;
}

// The RENDERER list's AUTO entry: "Auto (OpenGL)" when this launch runs AUTO and so
// knows what it picked, plain "Auto" otherwise.
inline std::string autoLabel(bool runningAuto, std::string_view created) {
    if (!runningAuto || created.empty()) return "Auto";
    return "Auto (" + label(created) + ")";
}

// ---- runtime record --------------------------------------------------------------

// This SDL build's render drivers, in SDL's own order.
inline std::vector<std::string> available() {
    std::vector<std::string> out;
    int n = SDL_GetNumRenderDrivers();
    for (int i = 0; i < n; ++i) {
        SDL_RendererInfo ri{};
        if (SDL_GetRenderDriverInfo(i, &ri) == 0 && ri.name && *ri.name) {
            std::string id = normalize(ri.name);
            if (!id.empty() && std::find(out.begin(), out.end(), id) == out.end())
                out.push_back(std::move(id));
        }
    }
    return out;
}

// What this launch is running, set once by main() after the renderer is created.
struct Active {
    std::string inEffect;   // the setting honoured this launch ("" = AUTO, incl. fallback)
    std::string created;    // the driver actually created (SDL_GetRendererInfo name)
    bool accelerated = false;
    std::string failed;     // the saved choice that could not be used ("" = none)
    std::string autoNote;   // AUTO only: what it passed over and why ("" = took the first)
    bool noticeShown = false;   // the one-time "unavailable, using Auto" notice was shown
};
inline Active& active() { static Active a; return a; }

// The one-time notice text, or "" if there is nothing to say (or it was already shown).
inline std::string pendingNotice() {
    const Active& a = active();
    if (a.failed.empty() || a.noticeShown) return {};
    return label(a.failed) + " renderer unavailable - using Auto";
}

}  // namespace tak::renderdriver
