#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace lob {

inline constexpr std::size_t kCacheLine = 64;

// Bounded, lock-free, wait-free single-producer / single-consumer FIFO ring buffer.
//
//   * Exactly ONE thread may call try_push and exactly ONE (other) thread may call try_pop. Anything
//     else is a data race. (That contract is what makes it this cheap; it is not enforced at run time.)
//   * Capacity is rounded up to a power of two (minimum 2); indices increase monotonically and are
//     masked, so no slot is wasted and full/empty are told apart by subtraction.
//   * Memory ordering: the producer writes the slot, THEN release-stores `tail_`; the consumer
//     acquire-loads `tail_`, THEN reads the slot. The mirror pair (`head_`) hands the freed slot back.
//     Each side keeps a plain cached copy of the other side's index and only re-reads the shared atomic
//     when the cache says full/empty, so in steady state a push or pop touches only its own cache lines.
//   * The four hot fields live on separate cache lines to avoid false sharing.
//
// T must be default-constructible and copy-assignable; slots are overwritten, never destroyed, so it is
// intended for small trivially-copyable messages.
template <class T>
class SpscQueue {
    static_assert(std::is_default_constructible_v<T> && std::is_copy_assignable_v<T>,
                  "SpscQueue<T> needs a default-constructible, copy-assignable T");

public:
    explicit SpscQueue(std::size_t min_capacity)
        : capacity_(round_up_pow2(min_capacity < 2 ? 2 : min_capacity)),
          mask_(capacity_ - 1),
          buffer_(new T[capacity_]) {}

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    std::size_t capacity() const noexcept { return capacity_; }

    // Producer thread only. Returns false (and does nothing) if the queue is full.
    bool try_push(const T& value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - cached_head_ == capacity_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail - cached_head_ == capacity_) return false;
        }
        buffer_[tail & mask_] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only. Returns false (and leaves `out` untouched) if the queue is empty.
    bool try_pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == cached_tail_) return false;
        }
        out = buffer_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Approximate occupancy; exact only when called from the producer or consumer with the other idle.
    std::size_t size_approx() const noexcept {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }

private:
    static std::size_t round_up_pow2(std::size_t n) {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<T[]> buffer_;

    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // written by the consumer
    alignas(kCacheLine) std::size_t cached_tail_{0};        // consumer-private copy of tail_
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // written by the producer
    alignas(kCacheLine) std::size_t cached_head_{0};        // producer-private copy of head_
    char pad_[kCacheLine - sizeof(std::size_t)]{};          // keep the last field off the next object's line
};

}  // namespace lob
