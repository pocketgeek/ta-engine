// Test doubles for vashim_test (Linux). Built twice:
//  - TAK_FAKE_LIBVA: a "libva.so.2" / "libva-drm.so.2" that loads but has none of libva's functions, so the
//    shim must treat VA-API as unavailable.
//  - TAK_FAKE_DRIVER: a VA driver exporting ONLY __vaDriverInit_1_23. A libva older than
//    2.23 (like the 2.22 loader the game used to bundle) cannot find it; the system libva
//    the shim forwards to does. Its init records the call and fails, so no device opens.
#include <cstdlib>
#ifdef TAK_FAKE_LIBVA
extern "C" __attribute__((visibility("default"))) int tak_fake_libva_marker() { return 1; }
#endif
#ifdef TAK_FAKE_DRIVER
extern "C" __attribute__((visibility("default"))) int __vaDriverInit_1_23(void*) {
    setenv("VASHIM_FAKE_DRIVER_INIT", "1_23", 1);
    return 1; // VA_STATUS_ERROR_OPERATION_FAILED
}
#endif
