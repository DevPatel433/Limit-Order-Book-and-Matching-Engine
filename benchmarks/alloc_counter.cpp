#include "alloc_counter.hpp"

#include <cstdlib>
#include <new>

#if defined(__GLIBC__)
#include <malloc.h>
#define LOB_USABLE(p) static_cast<std::int64_t>(malloc_usable_size(p))
#else
#define LOB_USABLE(p) (static_cast<void>(p), std::int64_t{0})
#endif

namespace {
bool g_enabled = false;
lob::bench::alloc::Stats g_stats;

inline void* counted_alloc(std::size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    if (g_enabled) {
        ++g_stats.allocations;
        g_stats.bytes_requested += n;
        g_stats.live_bytes += LOB_USABLE(p);
        if (g_stats.live_bytes > g_stats.peak_live_bytes) g_stats.peak_live_bytes = g_stats.live_bytes;
    }
    return p;
}

inline void counted_free(void* p) noexcept {
    if (!p) return;
    if (g_enabled) g_stats.live_bytes -= LOB_USABLE(p);
    std::free(p);
}
}  // namespace

namespace lob::bench::alloc {
void set_enabled(bool on) { g_enabled = on; }
void reset() { g_stats = Stats{}; }
Stats stats() { return g_stats; }
}  // namespace lob::bench::alloc

void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return counted_alloc(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return counted_alloc(n); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::size_t) noexcept { counted_free(p); }
