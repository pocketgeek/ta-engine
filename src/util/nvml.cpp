#include "util/nvml.h"
#include <mutex>
#include <string>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace tak::proc {
GpuSample nvml_detail::sampleDevice(const Api& api) {
    GpuSample g;
    Device device = nullptr;
    // Preserve the previous nvidia-smi first-adapter policy. Do not combine
    // percentages or memory from different GPUs into one misleading sample.
    if (!api.device || api.device(0, &device) != 0 || !device) return g;
    Utilization utilization{};
    if (api.utilization && api.utilization(device, &utilization) == 0 && utilization.gpu <= 100) {
        g.utilPct = utilization.gpu;
        g.ok = true;
    }
    Memory memory{};
    if (api.memory && api.memory(device, &memory) == 0 && memory.total && memory.used <= memory.total) {
        g.memTotal = size_t(memory.total);
        g.memUsed = size_t(memory.used);
        g.ok = true;
    }
    char name[256]{};
    if (api.name && api.name(device, name, sizeof(name)) == 0) {
        name[sizeof(name)-1] = '\0';
        g.name = name;
    }
    g.systemWide = g.ok;
    return g;
}

namespace {
class Nvml {
    nvml_detail::Api api_;
    std::mutex mutex_;
    bool initialized_ = false;
#if defined(_WIN32)
    HMODULE library_ = nullptr;
    template<class T> void resolve(T& target, const char* name) {
        // Through void(*)(): FARPROC's own signature is not the function's.
        target = reinterpret_cast<T>(reinterpret_cast<void (*)()>(GetProcAddress(library_, name)));
    }
#elif !defined(__APPLE__)
    void* library_ = nullptr;
    template<class T> void resolve(T& target, const char* name) {
        target = reinterpret_cast<T>(dlsym(library_, name));
    }
#endif
public:
    Nvml() {
#if defined(_WIN32)
        // DCH drivers install in System32; standard drivers use NVSMI. Avoid
        // loading DLLs from the working directory or beside downloaded maps.
        library_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library_) {
            wchar_t path[32768];
            const DWORD n = ExpandEnvironmentStringsW(
                L"%ProgramW6432%\\NVIDIA Corporation\\NVSMI\\nvml.dll", path, 32768);
            if (n && n <= 32768 && path[0] != L'%')
                library_ = LoadLibraryExW(path, nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
#elif !defined(__APPLE__)
        library_ = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
#if !defined(__APPLE__)
        if (!library_) return;
        resolve(api_.init, "nvmlInit_v2");
        resolve(api_.shutdown, "nvmlShutdown");
        resolve(api_.device, "nvmlDeviceGetHandleByIndex_v2");
        resolve(api_.utilization, "nvmlDeviceGetUtilizationRates");
        resolve(api_.memory, "nvmlDeviceGetMemoryInfo");
        resolve(api_.name, "nvmlDeviceGetName");
#endif
    }
    ~Nvml() {
        if (initialized_) api_.shutdown();
#if defined(_WIN32)
        if (library_) FreeLibrary(library_);
#elif !defined(__APPLE__)
        if (library_) dlclose(library_);
#endif
    }
    GpuSample sample() {
        std::lock_guard lock(mutex_);
        if (!api_.init || !api_.shutdown || !api_.device) return {};
        if (!initialized_) {
            if (api_.init() != 0) return {};
            initialized_ = true;
        }
        return nvml_detail::sampleDevice(api_);
    }
};
}
GpuSample nvidiaGpuSample() {
    static Nvml nvml;
    return nvml.sample();
}
}
