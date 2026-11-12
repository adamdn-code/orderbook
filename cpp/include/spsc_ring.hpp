// Lock-free single-producer / single-consumer ring buffer.
//
// Exactly one thread may push and exactly one (other) thread may pop.
// No locks, no allocation after construction: the producer writes a slot and
// publishes it by bumping `head_`; the consumer reads it and frees it by
// bumping `tail_`. Each index lives on its own cache line so the two threads
// don't fight over the same memory, and each side keeps a cached copy of the
// other side's index so it only touches the shared one when it looks full or
// empty.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace ob {

inline constexpr std::size_t kCacheLine = 64;

template <class T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

public:
    // Producer side. Returns false if the ring is full.
    bool try_push(const T& value) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head - cached_tail_ == Capacity) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head - cached_tail_ == Capacity) return false;
        }
        slots_[head & kMask] = value;
        head_.store(head + 1, std::memory_order_release);  // publish the slot
        return true;
    }

    // Consumer side. Oldest item, or nullptr if empty. The slot stays reserved
    // until pop(), so the consumer can finish working before freeing it.
    T* front() {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail == cached_head_) return nullptr;
        }
        return &slots_[tail & kMask];
    }

    void pop() { tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

    // Safe from any thread; exact only when nobody is pushing.
    bool empty() const {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() { return Capacity; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // written by producer
    alignas(kCacheLine) std::size_t cached_tail_ = 0;       // producer's private copy
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // written by consumer
    alignas(kCacheLine) std::size_t cached_head_ = 0;       // consumer's private copy
    alignas(kCacheLine) std::array<T, Capacity> slots_{};
};

}  // namespace ob
