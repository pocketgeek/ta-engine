// The VA-API forwarder (tools/vashim.c, linked as the static FFmpeg's libva.a) on Linux.
//   vashim_test unavailable   -- run with a libva.so.2 that cannot be used (CTest points
//                                LD_LIBRARY_PATH at vashim_fake_va): every VA entry fails
//                                cleanly and a stream asked for h264_vaapi records on x264.
//   vashim_test system        -- the system libva is loaded and reaches a driver (skip 77
//                                when the host has no libva or no DRM render node).
//   vashim_test driver <dir>  -- with LIBVA_DRIVERS_PATH=<dir> and the fake driver there,
//                                vaInitialize reaches its __vaDriverInit_1_23: proof that
//                                the SYSTEM loader runs, not a bundled older one (skip 77
//                                as above, or when the system libva predates 2.23).
#include "video/stream.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <va/va.h>
#include <va/va_drm.h>
}
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
static void require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static int openRenderNode() {
    for (int i = 128; i < 144; ++i) {
        int fd = open(("/dev/dri/renderD" + std::to_string(i)).c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
    return -1;
}
static bool systemLibvaPresent() {
    void* va = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
    void* drm = dlopen("libva-drm.so.2", RTLD_NOW | RTLD_LOCAL);
    return va && drm && dlsym(va, "vaInitialize") && dlsym(drm, "vaGetDisplayDRM");
}
static int unavailable() {
    require(vaGetDisplayDRM(-1) == nullptr, "no display without libva");
    int fd = openRenderNode();
    if (fd >= 0) { require(vaGetDisplayDRM(fd) == nullptr, "no display on a render node without libva"); close(fd); }
    int major = 0, minor = 0;
    require(vaInitialize(nullptr, &major, &minor) == VAStatus(VA_STATUS_ERROR_UNKNOWN), "vaInitialize reports an error");
    require(vaTerminate(nullptr) != VA_STATUS_SUCCESS, "vaTerminate reports an error");
    require(vaErrorStr(VA_STATUS_ERROR_UNKNOWN) && *vaErrorStr(VA_STATUS_ERROR_UNKNOWN), "error string");
    require(vaMaxNumProfiles(nullptr) == 0, "no profiles");
    void* buffer = nullptr;
    require(vaMapBuffer2(nullptr, 0, &buffer, 0) != VA_STATUS_SUCCESS && !buffer, "no mapping");
    require(vaSyncBuffer(nullptr, 0, 0) != VA_STATUS_SUCCESS, "no sync");
    require(vaQueryVendorString(nullptr) == nullptr, "no vendor");
    AVBufferRef* device = nullptr;
    require(av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", nullptr, 0) < 0,
            "FFmpeg cannot open a VA-API device");
    av_buffer_unref(&device);

    // A stream asked for h264_vaapi falls through to the CPU encoder, as on a machine
    // without VA-API hardware.
    tak::video::StreamConfig config;
    config.encoder = "h264_vaapi";
    config.width = 640; config.height = 360; config.fps = 30;
    auto path = std::filesystem::temp_directory_path() /
        ("tak-vashim-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".flv");
    std::string encoder;
    {
        tak::video::Stream stream;
        require(stream.startRecording(config, path.string()), "start recording");
        std::vector<uint8_t> pixels(640 * 360 * 4, 90);
        std::array<int16_t, 256 * 2> audio{};
        const auto begin = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - begin < 1500ms) {
            require(stream.active(), stream.status().state.c_str());
            stream.video(pixels.data(), 640, 360, 640 * 4);
            stream.audio(audio.data(), 256, 2);
            std::this_thread::sleep_for(30ms);
        }
        auto status = stream.status();
        encoder = status.encoder;
        std::printf("encoder=%s frames=%llu bytes=%llu\n", status.encoder.c_str(),
                    (unsigned long long)status.frames, (unsigned long long)status.bytes);
        require(status.frames >= 10 && status.bytes > 1000, "frames recorded");
        stream.stop();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (stream.active() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    require(encoder == "libx264", "h264_vaapi fell back to libx264");
    std::puts("OK: VA-API unavailable is reported cleanly and streaming falls back to libx264");
    return 0;
}
// Through FFmpeg, as streaming uses it: open the device, make an NV12 surface pool and
// send a frame up and back down. Exercises surface, image and sync forwarding.
static bool surfaceRoundTrip() {
    AVBufferRef* device = nullptr;
    for (int i = 128; i < 144 && !device; ++i) {
        const std::string node = "/dev/dri/renderD" + std::to_string(i);
        if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, node.c_str(), nullptr, 0) < 0) device = nullptr;
        else std::printf("FFmpeg opened VA-API on %s\n", node.c_str());
    }
    if (!device) return false;
    AVBufferRef* pool = av_hwframe_ctx_alloc(device);
    require(pool, "frame pool");
    auto* f = reinterpret_cast<AVHWFramesContext*>(pool->data);
    f->format = AV_PIX_FMT_VAAPI; f->sw_format = AV_PIX_FMT_NV12;
    f->width = 640; f->height = 360; f->initial_pool_size = 4;
    require(av_hwframe_ctx_init(pool) >= 0, "VA surface pool");
    AVFrame* up = av_frame_alloc(); AVFrame* hw = av_frame_alloc(); AVFrame* down = av_frame_alloc();
    up->format = AV_PIX_FMT_NV12; up->width = 640; up->height = 360;
    require(av_frame_get_buffer(up, 32) >= 0, "upload frame");
    for (int y = 0; y < 360; ++y) for (int x = 0; x < 640; ++x) up->data[0][y * up->linesize[0] + x] = uint8_t(x + y);
    for (int y = 0; y < 180; ++y) for (int x = 0; x < 640; ++x) up->data[1][y * up->linesize[1] + x] = uint8_t(x * 3);
    require(av_hwframe_get_buffer(pool, hw, 0) >= 0, "VA surface");
    require(av_hwframe_transfer_data(hw, up, 0) >= 0, "upload to the surface");
    down->format = AV_PIX_FMT_NV12;
    require(av_hwframe_transfer_data(down, hw, 0) >= 0, "download from the surface");
    bool same = true;
    for (int y = 0; y < 360 && same; ++y)
        same = std::memcmp(up->data[0] + y * up->linesize[0], down->data[0] + y * down->linesize[0], 640) == 0;
    require(same, "the frame comes back unchanged");
    av_frame_free(&up); av_frame_free(&hw); av_frame_free(&down);
    av_buffer_unref(&pool); av_buffer_unref(&device);
    std::puts("OK: an NV12 frame went through a VA surface and back");
    return true;
}
static int systemLibrary(const char* driverDir) {
    if (!systemLibvaPresent()) { std::puts("SKIP: no system libva.so.2/libva-drm.so.2"); return 77; }
    int fd = openRenderNode();
    if (fd < 0) { std::puts("SKIP: no DRM render node"); return 77; }
    if (driverDir) {
        setenv("LIBVA_DRIVERS_PATH", driverDir, 1);
        setenv("LIBVA_DRIVER_NAME", "takfake", 1);
        unsetenv("VASHIM_FAKE_DRIVER_INIT");
    }
    VADisplay display = vaGetDisplayDRM(fd);
    require(display != nullptr, "the shim forwards vaGetDisplayDRM to the system libva-drm");
    int major = 0, minor = 0;
    const VAStatus status = vaInitialize(display, &major, &minor);
    std::printf("vaInitialize: %s (VA-API %d.%d)\n", vaErrorStr(status), major, minor);
    if (driverDir) {
        const char* called = getenv("VASHIM_FAKE_DRIVER_INIT");
        if (!called && (major < 1 || (major == 1 && minor < 23))) {
            std::puts("SKIP: the system libva predates VA-API 1.23"); return 77;
        }
        require(called && std::strcmp(called, "1_23") == 0, "the system loader called __vaDriverInit_1_23");
        require(status != VA_STATUS_SUCCESS, "the fake driver's failure is reported");
        std::puts("OK: the system libva loader found the fake driver's __vaDriverInit_1_23");
    } else {
        require(major >= 1, "the system libva reports its API version");
        if (status == VA_STATUS_SUCCESS) std::printf("driver: %s\n", vaQueryVendorString(display));
        std::puts("OK: the shim reaches the system libva");
        if (status == VA_STATUS_SUCCESS && !surfaceRoundTrip())
            std::puts("note: FFmpeg could not open a VA-API device (no usable driver)");
    }
    if (status == VA_STATUS_SUCCESS) vaTerminate(display);
    close(fd);
    return 0;
}
int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "unavailable") return unavailable();
    if (mode == "system") return systemLibrary(nullptr);
    if (mode == "driver" && argc > 2) return systemLibrary(argv[2]);
    std::fprintf(stderr, "usage: vashim_test unavailable | system | driver <dir>\n");
    return 2;
}
