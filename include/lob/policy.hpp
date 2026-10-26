#pragma once

#include <memory>

#include "lob/arena.hpp"

namespace lob {

// A policy decides how the book's containers get their memory. The matching and
// book logic are identical for every policy (they are templates over it), so the
// only difference between implementations is allocation: this keeps the Phase 5
// optimisation isolated and the baseline available for fair comparison.

// Baseline: plain std::allocator (global operator new/delete).
struct StdPolicy {
    struct Resource {};
    template <class T>
    using Allocator = std::allocator<T>;
    template <class T>
    static Allocator<T> make_allocator(Resource&) noexcept {
        return {};
    }
    static constexpr const char* kName = "baseline";
};

// Optimised: all nodes come from one per-book SlabArena.
struct PoolPolicy {
    using Resource = SlabArena;
    template <class T>
    using Allocator = ArenaAllocator<T>;
    template <class T>
    static Allocator<T> make_allocator(Resource& r) noexcept {
        return Allocator<T>(&r);
    }
    static constexpr const char* kName = "pooled";
};

enum class EngineKind { Baseline, Pooled };

constexpr const char* to_string(EngineKind k) noexcept {
    return k == EngineKind::Baseline ? StdPolicy::kName : PoolPolicy::kName;
}

}  // namespace lob
