#include "client/alloccount.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif

// The replaceable operators. The array, nothrow and sized forms all forward to these
// four, and the aligned pair has to be here too: a Unit is alignas(64).
namespace {

thread_local uint64_t* counted = nullptr;

template <class Allocate>
void* allocate(std::size_t n, Allocate&& get) {
    for (;;) {
        if (void* p = get()) {
            if (counted) *counted += n;
            return p;
        }
        const std::new_handler handler = std::get_new_handler();
        if (!handler) throw std::bad_alloc();
        handler();
    }
}

}  // namespace

namespace tak::client {

AllocCount::AllocCount() { counted = &bytes_; }
AllocCount::~AllocCount() { counted = nullptr; }

}  // namespace tak::client

void* operator new(std::size_t n) {
    return allocate(n, [&] { return std::malloc(n ? n : 1); });
}

void operator delete(void* p) noexcept { std::free(p); }
// The sized form, which the compiler calls when it knows the size: the same release.
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

void* operator new(std::size_t n, std::align_val_t align) {
    const std::size_t alignment = std::max(std::size_t(align), sizeof(void*));
    return allocate(n, [&]() -> void* {
#if defined(_WIN32)
        return _aligned_malloc(n ? n : 1, alignment);
#else
        void* p = nullptr;
        return posix_memalign(&p, alignment, n ? n : 1) == 0 ? p : nullptr;
#endif
    });
}

void operator delete(void* p, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
