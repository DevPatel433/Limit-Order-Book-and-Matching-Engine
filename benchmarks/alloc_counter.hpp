#pragma once

#include <cstdint>

// Counting replacement for global operator new/delete. Linked ONLY into the
// lob_bench executable (alloc_counter.cpp), so unit tests are unaffected.
// Single-threaded by design: plain counters, no atomics, so the cost while
// disabled is one predictable branch per allocation.
namespace lob::bench::alloc {

struct Stats {
    std::uint64_t allocations{0};
    std::uint64_t bytes_requested{0};
    std::int64_t live_bytes{0};  // usable bytes currently allocated (glibc only, else 0)
    std::int64_t peak_live_bytes{0};
};

void set_enabled(bool on);
void reset();
Stats stats();

}  // namespace lob::bench::alloc
