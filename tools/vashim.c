/* VA-API forwarding shim for the static streaming FFmpeg (Linux only).
 *
 * FFmpeg's VA-API code calls libva directly. Linking a static libva would freeze its
 * driver loader at the bundled version: a driver built against a newer libva exports
 * only a newer __vaDriverInit_1_N and cannot be initialised, and the loader's driver
 * folders are fixed at build time (Fedora's dri-nonfree/dri-freeworld were missing).
 * Instead, tools/build-stream-deps.sh compiles this file as the prefix's libva.a: each
 * function FFmpeg references forwards to the SYSTEM libva.so.2 / libva-drm.so.2, loaded
 * with dlopen on first use. Without them every call fails with a VA error
 * (vaGetDisplayDRM returns NULL), so FFmpeg cannot open a VA-API device and the game
 * moves on to the next encoder exactly as it does on a machine without VA-API hardware.
 *
 * libva's 2.x ABI is stable and backward compatible. Functions newer than libva 2.0 are
 * optional: vaSyncBuffer (2.9) reports VA_STATUS_ERROR_UNIMPLEMENTED, which FFmpeg
 * probes for, and vaMapBuffer2 (2.21) falls back to vaMapBuffer.
 *
 * With libva's own LIBVA_MESSAGING_LEVEL set, the shim says on stderr which libraries it
 * loaded (or why not) and leaves libva's messages on stderr instead of routing them into
 * FFmpeg's log, so a user can see libva's version and driver search (see
 * docs/streaming.md). Everything here is compiled with hidden visibility: the program
 * never exports va* symbols that a driver or the system libva could bind to. */
#define _GNU_SOURCE /* dlinfo */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <link.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_str.h>

/* Symbols FFmpeg 7.1 references (nm -u on libavutil.a/libavcodec.a). X(name, required). */
#define TAK_VA_FUNCS(X) \
    X(vaInitialize, 1) X(vaTerminate, 1) X(vaQueryVendorString, 1) X(vaErrorStr, 1) \
    X(vaMaxNumProfiles, 1) X(vaMaxNumEntrypoints, 1) X(vaMaxNumImageFormats, 1) \
    X(vaQueryConfigProfiles, 1) X(vaQueryConfigEntrypoints, 1) X(vaGetConfigAttributes, 1) \
    X(vaCreateConfig, 1) X(vaDestroyConfig, 1) X(vaQuerySurfaceAttributes, 1) \
    X(vaCreateSurfaces, 1) X(vaDestroySurfaces, 1) X(vaCreateContext, 1) X(vaDestroyContext, 1) \
    X(vaCreateBuffer, 1) X(vaMapBuffer, 1) X(vaUnmapBuffer, 1) X(vaDestroyBuffer, 1) \
    X(vaBeginPicture, 1) X(vaRenderPicture, 1) X(vaEndPicture, 1) X(vaSyncSurface, 1) \
    X(vaQueryImageFormats, 1) X(vaCreateImage, 1) X(vaDestroyImage, 1) X(vaGetImage, 1) \
    X(vaPutImage, 1) X(vaDeriveImage, 1) X(vaProfileStr, 1) X(vaEntrypointStr, 1) \
    X(vaSetDriverName, 1) X(vaSetErrorCallback, 1) X(vaSetInfoCallback, 1) \
    X(vaSyncBuffer, 0) X(vaMapBuffer2, 0)

#define TAK_VA_PTR(name, required) static __typeof__(&name) p_##name;
TAK_VA_FUNCS(TAK_VA_PTR)
static __typeof__(&vaGetDisplayDRM) p_vaGetDisplayDRM;

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int verbose;

static const char* libraryPath(void* handle) {
    struct link_map* map = NULL;
    if (handle && dlinfo(handle, RTLD_DI_LINKMAP, &map) == 0 && map && map->l_name && *map->l_name)
        return map->l_name;
    return "?";
}

static void unavailable(const char* why, const char* detail) {
#define TAK_VA_CLEAR(name, required) p_##name = NULL;
    TAK_VA_FUNCS(TAK_VA_CLEAR)
    p_vaGetDisplayDRM = NULL;
    if (verbose) fprintf(stderr, "TAK VA-API: system libva unavailable: %s%s%s\n", why,
                         detail ? ": " : "", detail ? detail : "");
}

static void load(void) {
    verbose = getenv("LIBVA_MESSAGING_LEVEL") != NULL;
    /* Never closed: the driver libva loads stays in use until the process exits. */
    void* va = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!va) { unavailable("cannot load libva.so.2", dlerror()); return; }
    void* drm = dlopen("libva-drm.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!drm) { unavailable("cannot load libva-drm.so.2", dlerror()); return; }
    int missing = 0;
#define TAK_VA_LOAD(name, required) \
    p_##name = (__typeof__(&name))dlsym(va, #name); \
    if (!p_##name && required) { \
        if (verbose) fprintf(stderr, "TAK VA-API: libva.so.2 lacks %s\n", #name); \
        missing = 1; \
    }
    TAK_VA_FUNCS(TAK_VA_LOAD)
    p_vaGetDisplayDRM = (__typeof__(&vaGetDisplayDRM))dlsym(drm, "vaGetDisplayDRM");
    if (!p_vaGetDisplayDRM) missing = 1;
    if (missing) { unavailable("incomplete libva", NULL); return; }
    if (verbose) fprintf(stderr, "TAK VA-API: using system %s and %s\n", libraryPath(va), libraryPath(drm));
}

static int ready(void) { pthread_once(&once, load); return p_vaInitialize != NULL; }

/* An unavailable library: display calls fail. A missing optional function: unimplemented. */
#define TAK_VA_FORWARD(name, params, args) \
    VAStatus name params { \
        if (!ready()) return VA_STATUS_ERROR_UNKNOWN; \
        return p_##name ? p_##name args : VA_STATUS_ERROR_UNIMPLEMENTED; \
    }

VADisplay vaGetDisplayDRM(int fd) { return ready() ? p_vaGetDisplayDRM(fd) : NULL; }

TAK_VA_FORWARD(vaInitialize, (VADisplay d, int* major, int* minor), (d, major, minor))
TAK_VA_FORWARD(vaTerminate, (VADisplay d), (d))
TAK_VA_FORWARD(vaQueryConfigProfiles, (VADisplay d, VAProfile* l, int* n), (d, l, n))
TAK_VA_FORWARD(vaQueryConfigEntrypoints, (VADisplay d, VAProfile p, VAEntrypoint* l, int* n), (d, p, l, n))
TAK_VA_FORWARD(vaGetConfigAttributes, (VADisplay d, VAProfile p, VAEntrypoint e, VAConfigAttrib* a, int n),
               (d, p, e, a, n))
TAK_VA_FORWARD(vaCreateConfig, (VADisplay d, VAProfile p, VAEntrypoint e, VAConfigAttrib* a, int n, VAConfigID* id),
               (d, p, e, a, n, id))
TAK_VA_FORWARD(vaDestroyConfig, (VADisplay d, VAConfigID id), (d, id))
TAK_VA_FORWARD(vaQuerySurfaceAttributes, (VADisplay d, VAConfigID id, VASurfaceAttrib* a, unsigned int* n),
               (d, id, a, n))
TAK_VA_FORWARD(vaCreateSurfaces, (VADisplay d, unsigned int format, unsigned int width, unsigned int height,
                                  VASurfaceID* s, unsigned int n, VASurfaceAttrib* a, unsigned int na),
               (d, format, width, height, s, n, a, na))
TAK_VA_FORWARD(vaDestroySurfaces, (VADisplay d, VASurfaceID* s, int n), (d, s, n))
TAK_VA_FORWARD(vaCreateContext, (VADisplay d, VAConfigID id, int w, int h, int flag, VASurfaceID* s, int n,
                                 VAContextID* c),
               (d, id, w, h, flag, s, n, c))
TAK_VA_FORWARD(vaDestroyContext, (VADisplay d, VAContextID c), (d, c))
TAK_VA_FORWARD(vaCreateBuffer, (VADisplay d, VAContextID c, VABufferType t, unsigned int size, unsigned int n,
                                void* data, VABufferID* b),
               (d, c, t, size, n, data, b))
TAK_VA_FORWARD(vaMapBuffer, (VADisplay d, VABufferID b, void** p), (d, b, p))
TAK_VA_FORWARD(vaUnmapBuffer, (VADisplay d, VABufferID b), (d, b))
TAK_VA_FORWARD(vaDestroyBuffer, (VADisplay d, VABufferID b), (d, b))
TAK_VA_FORWARD(vaBeginPicture, (VADisplay d, VAContextID c, VASurfaceID s), (d, c, s))
TAK_VA_FORWARD(vaRenderPicture, (VADisplay d, VAContextID c, VABufferID* b, int n), (d, c, b, n))
TAK_VA_FORWARD(vaEndPicture, (VADisplay d, VAContextID c), (d, c))
TAK_VA_FORWARD(vaSyncSurface, (VADisplay d, VASurfaceID s), (d, s))
TAK_VA_FORWARD(vaSyncBuffer, (VADisplay d, VABufferID b, uint64_t timeout), (d, b, timeout))
TAK_VA_FORWARD(vaQueryImageFormats, (VADisplay d, VAImageFormat* f, int* n), (d, f, n))
TAK_VA_FORWARD(vaCreateImage, (VADisplay d, VAImageFormat* f, int w, int h, VAImage* i), (d, f, w, h, i))
TAK_VA_FORWARD(vaDestroyImage, (VADisplay d, VAImageID i), (d, i))
TAK_VA_FORWARD(vaGetImage, (VADisplay d, VASurfaceID s, int x, int y, unsigned int w, unsigned int h, VAImageID i),
               (d, s, x, y, w, h, i))
TAK_VA_FORWARD(vaPutImage, (VADisplay d, VASurfaceID s, VAImageID i, int sx, int sy, unsigned int sw,
                            unsigned int sh, int dx, int dy, unsigned int dw, unsigned int dh),
               (d, s, i, sx, sy, sw, sh, dx, dy, dw, dh))
TAK_VA_FORWARD(vaDeriveImage, (VADisplay d, VASurfaceID s, VAImage* i), (d, s, i))
TAK_VA_FORWARD(vaSetDriverName, (VADisplay d, char* name), (d, name))

/* libva < 2.21: vaMapBuffer2's flags are only a hint, so plain vaMapBuffer is equivalent. */
VAStatus vaMapBuffer2(VADisplay d, VABufferID b, void** p, uint32_t flags) {
    if (!ready()) return VA_STATUS_ERROR_UNKNOWN;
    return p_vaMapBuffer2 ? p_vaMapBuffer2(d, b, p, flags) : p_vaMapBuffer(d, b, p);
}

int vaMaxNumProfiles(VADisplay d) { return ready() ? p_vaMaxNumProfiles(d) : 0; }
int vaMaxNumEntrypoints(VADisplay d) { return ready() ? p_vaMaxNumEntrypoints(d) : 0; }
int vaMaxNumImageFormats(VADisplay d) { return ready() ? p_vaMaxNumImageFormats(d) : 0; }

const char* vaQueryVendorString(VADisplay d) { return ready() ? p_vaQueryVendorString(d) : NULL; }
const char* vaErrorStr(VAStatus s) { return ready() ? p_vaErrorStr(s) : "VA-API unavailable (no system libva)"; }
const char* vaProfileStr(VAProfile p) { return ready() ? p_vaProfileStr(p) : "<unknown profile>"; }
const char* vaEntrypointStr(VAEntrypoint e) { return ready() ? p_vaEntrypointStr(e) : "<unknown entrypoint>"; }

/* LIBVA_MESSAGING_LEVEL asks for libva's own stderr output: keep FFmpeg's callbacks out. */
VAMessageCallback vaSetErrorCallback(VADisplay d, VAMessageCallback cb, void* user) {
    return ready() && !verbose ? p_vaSetErrorCallback(d, cb, user) : NULL;
}
VAMessageCallback vaSetInfoCallback(VADisplay d, VAMessageCallback cb, void* user) {
    return ready() && !verbose ? p_vaSetInfoCallback(d, cb, user) : NULL;
}
