#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

namespace lob {

// Size-class free-list allocator over large chunks. It exists to remove the
// per-node malloc/free cost that profiling identified (docs/results_baseline.md):
// every resting order costs one std::list node and one hash-map node.
//
//   * Requests up to kMaxPooledBytes (and alignment <= kGranule) are rounded up to a
//     multiple of kGranule and served from a per-size-class LIFO free list, or carved
//     from the current 64 KiB chunk. No per-block header, no locking, no syscalls
//     on the hot path.
//   * Freed blocks go back on their size class's free list and are reused. Memory is
//     returned to the system only when the arena is destroyed (see docs/phase5_design.md
//     for the trade-off).
//   * Larger or over-aligned requests (e.g. hash bucket arrays) fall through to
//     ::operator new / ::operator delete.
//
// NOT thread-safe, like the rest of the engine. Not copyable or movable: allocators
// hold a raw pointer to it.
class SlabArena {
public:
    static constexpr std::size_t kGranule = 16;
    static constexpr std::size_t kMaxPooledBytes = 256;
    static constexpr std::size_t kChunkBytes = 64 * 1024;

    SlabArena() noexcept = default;
    ~SlabArena();
    SlabArena(const SlabArena&) = delete;
    SlabArena& operator=(const SlabArena&) = delete;

    void* allocate(std::size_t bytes, std::size_t align) {
        if (bytes <= kMaxPooledBytes && align <= kGranule) {
            const std::size_t cls = size_class(bytes);
            if (FreeNode* n = free_[cls]) {
                free_[cls] = n->next;
                return n;
            }
            return carve((cls + 1) * kGranule);
        }
        return allocate_large(bytes, align);
    }

    void deallocate(void* p, std::size_t bytes, std::size_t align) noexcept {
        if (bytes <= kMaxPooledBytes && align <= kGranule) {
            const std::size_t cls = size_class(bytes);
            auto* n = static_cast<FreeNode*>(p);
            n->next = free_[cls];
            free_[cls] = n;
        } else {
            deallocate_large(p, align);
        }
    }

    // Bytes obtained from the system for pooled chunks (high-water mark; never shrinks).
    std::size_t bytes_reserved() const noexcept { return chunks_ * kChunkBytes; }
    std::size_t chunk_count() const noexcept { return chunks_; }

private:
    struct FreeNode {
        FreeNode* next;
    };
    struct ChunkHeader {
        ChunkHeader* next;
        std::size_t pad;  // keeps the first block 16-byte aligned
    };
    static_assert(sizeof(ChunkHeader) == kGranule);
    static_assert(kMaxPooledBytes % kGranule == 0);
    static constexpr std::size_t kClasses = kMaxPooledBytes / kGranule;

    static constexpr std::size_t size_class(std::size_t bytes) noexcept {
        return bytes == 0 ? 0 : (bytes + kGranule - 1) / kGranule - 1;
    }

    void* carve(std::size_t size) {
        if (static_cast<std::size_t>(end_ - cur_) < size) refill();
        void* p = cur_;
        cur_ += size;
        return p;
    }
    void refill();  // out of line: obtains a new chunk

    static void* allocate_large(std::size_t bytes, std::size_t align) {
        if (align > __STDCPP_DEFAULT_NEW_ALIGNMENT__) return ::operator new(bytes, std::align_val_t(align));
        return ::operator new(bytes);
    }
    static void deallocate_large(void* p, std::size_t align) noexcept {
        if (align > __STDCPP_DEFAULT_NEW_ALIGNMENT__) ::operator delete(p, std::align_val_t(align));
        else ::operator delete(p);
    }

    FreeNode* free_[kClasses] = {};
    char* cur_{nullptr};
    char* end_{nullptr};
    ChunkHeader* chunk_list_{nullptr};
    std::size_t chunks_{0};
};

// Standard-conforming allocator handle; copies (and rebinds) share one SlabArena.
template <class T>
class ArenaAllocator {
public:
    using value_type = T;

    explicit ArenaAllocator(SlabArena* arena) noexcept : arena_(arena) {}
    template <class U>
    ArenaAllocator(const ArenaAllocator<U>& other) noexcept : arena_(other.arena()) {}

    T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
        return static_cast<T*>(arena_->allocate(n * sizeof(T), alignof(T)));
    }
    void deallocate(T* p, std::size_t n) noexcept { arena_->deallocate(p, n * sizeof(T), alignof(T)); }

    SlabArena* arena() const noexcept { return arena_; }

    template <class U>
    friend bool operator==(const ArenaAllocator& a, const ArenaAllocator<U>& b) noexcept {
        return a.arena_ == b.arena();
    }

private:
    SlabArena* arena_;
};

}  // namespace lob
