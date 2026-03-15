#pragma once
// =============================================================================
// spsc_ring_buffer.hpp — Single-Producer Single-Consumer lock-free ring buffer
//
// Design:
//   - Power-of-two capacity for bitmask wrapping (zero division cost)
//   - Producer (head) and consumer (tail) on separate cache lines
//     → eliminates false sharing between the feed thread and signal thread
//   - Only two atomics total: head_ and tail_
//   - Producer writes with memory_order_release; consumer reads with acquire
//     → correct happens-before without seq_cst overhead
//   - T must be trivially copyable (stack-allocated, no heap in hot path)
//
// Usage:
//   SpscRingBuffer<MarketEvent, 4096> q;
//
//   // Producer thread:
//   q.push(event);   // returns false if full (non-blocking)
//
//   // Consumer thread:
//   MarketEvent e;
//   if (q.pop(e)) { process(e); }
//
// Capacity is always (N - 1) usable slots (one slot reserved as sentinel).
// N must be a power of 2.
// =============================================================================

#include "common.hpp"
#include <optional>

namespace bot {

template<typename T, std::size_t N>
class SpscRingBuffer {
    static_assert(IsPowerOfTwo<N>::value, "SpscRingBuffer capacity must be power of two");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable for lock-free buffer");

    static constexpr std::size_t MASK = N - 1;

public:
    SpscRingBuffer() = default;

    // Non-copyable, non-movable (contains atomics)
    SpscRingBuffer(const SpscRingBuffer&)            = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    // -------------------------------------------------------------------------
    // push — called by producer thread only
    // Returns true on success, false if buffer is full (caller must retry/drop)
    // -------------------------------------------------------------------------
    BOT_ALWAYS_INLINE bool push(const T& item) noexcept {
        const std::size_t head = head_.idx.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & MASK;

        // Full check: next write position must not equal tail (consumer position)
        if (BOT_UNLIKELY(next == tail_.idx.load(std::memory_order_acquire))) {
            return false;   // buffer full — caller drops or spins
        }

        buffer_[head] = item;
        // Release: makes the write visible to consumer after this store
        head_.idx.store(next, std::memory_order_release);
        return true;
    }

    // -------------------------------------------------------------------------
    // push (move overload)
    // -------------------------------------------------------------------------
    BOT_ALWAYS_INLINE bool push(T&& item) noexcept {
        const std::size_t head = head_.idx.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & MASK;
        if (BOT_UNLIKELY(next == tail_.idx.load(std::memory_order_acquire))) {
            return false;
        }
        buffer_[head] = std::move(item);
        head_.idx.store(next, std::memory_order_release);
        return true;
    }

    // -------------------------------------------------------------------------
    // pop — called by consumer thread only
    // Returns true on success (item written into out), false if empty
    // -------------------------------------------------------------------------
    BOT_ALWAYS_INLINE bool pop(T& out) noexcept {
        const std::size_t tail = tail_.idx.load(std::memory_order_relaxed);

        // Empty check
        if (BOT_UNLIKELY(tail == head_.idx.load(std::memory_order_acquire))) {
            return false;
        }

        // Prefetch next slot to warm cache for the following pop
        BOT_PREFETCH(&buffer_[(tail + 1) & MASK]);

        out = buffer_[tail];
        // Release: advances consumer position, allowing producer to reuse slot
        tail_.idx.store((tail + 1) & MASK, std::memory_order_release);
        return true;
    }

    // -------------------------------------------------------------------------
    // try_pop — returns optional (useful in range-for style draining)
    // -------------------------------------------------------------------------
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        T item;
        if (pop(item)) return item;
        return std::nullopt;
    }

    // -------------------------------------------------------------------------
    // Drain all available items, calling f(item) for each
    // -------------------------------------------------------------------------
    template<typename Fn>
    std::size_t drain(Fn&& f) noexcept {
        std::size_t count = 0;
        T item;
        while (pop(item)) {
            f(item);
            ++count;
        }
        return count;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.idx.load(std::memory_order_acquire)
            == tail_.idx.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const auto h = head_.idx.load(std::memory_order_acquire);
        const auto t = tail_.idx.load(std::memory_order_acquire);
        return (h - t) & MASK;
    }

    static constexpr std::size_t capacity() noexcept { return N - 1; }

private:
    // Data array — not padded individually; hot structs are aligned at type level
    alignas(CACHE_LINE_SIZE) T buffer_[N]{};

    // Producer index — written by producer, read by both
    // Own cache line to avoid false sharing with tail_
    AtomicIndex head_;

    // Consumer index — written by consumer, read by both
    // Own cache line to avoid false sharing with head_
    AtomicIndex tail_;
};

// ---------------------------------------------------------------------------
// MPSC variant (Multi-Producer Single-Consumer)
// Uses a single CAS on the sequence counter for producer coordination.
// Suitable for multiple feed threads pushing into one signal thread.
// ---------------------------------------------------------------------------
template<typename T, std::size_t N>
class MpscRingBuffer {
    static_assert(IsPowerOfTwo<N>::value, "MpscRingBuffer capacity must be power of two");
    static_assert(std::is_trivially_copyable_v<T>);

    static constexpr std::size_t MASK = N - 1;

    struct alignas(CACHE_LINE_SIZE) Slot {
        T            data{};
        // sequence_ tracks whether this slot is ready to consume
        // Odd = being written, Even = ready or empty
        std::atomic<std::size_t> sequence{0};
    };

public:
    MpscRingBuffer() {
        for (std::size_t i = 0; i < N; ++i)
            slots_[i].sequence.store(i, std::memory_order_relaxed);
    }

    MpscRingBuffer(const MpscRingBuffer&)            = delete;
    MpscRingBuffer& operator=(const MpscRingBuffer&) = delete;

    // Producer: CAS on enqueue position — multiple producers safe
    bool push(const T& item) noexcept {
        std::size_t pos = enqueue_.idx.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & MASK];
            std::size_t seq = slot.sequence.load(std::memory_order_acquire);
            std::intptr_t diff = static_cast<std::intptr_t>(seq)
                               - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                // Slot is free — try to claim it
                if (enqueue_.idx.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed))
                {
                    slot.data = item;
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
                // CAS failed — another producer got it, retry
            } else if (diff < 0) {
                return false;   // buffer full
            } else {
                pos = enqueue_.idx.load(std::memory_order_relaxed);
            }
        }
    }

    // Consumer (single thread only)
    bool pop(T& out) noexcept {
        std::size_t pos = dequeue_.idx.load(std::memory_order_relaxed);
        Slot& slot      = slots_[pos & MASK];
        std::size_t seq = slot.sequence.load(std::memory_order_acquire);
        std::intptr_t diff = static_cast<std::intptr_t>(seq)
                           - static_cast<std::intptr_t>(pos + 1);
        if (diff != 0) return false;   // not yet published
        out = slot.data;
        slot.sequence.store(pos + N, std::memory_order_release);
        dequeue_.idx.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    template<typename Fn>
    std::size_t drain(Fn&& f) noexcept {
        std::size_t count = 0;
        T item;
        while (pop(item)) { f(item); ++count; }
        return count;
    }

private:
    alignas(CACHE_LINE_SIZE) Slot slots_[N];
    AtomicIndex enqueue_;
    AtomicIndex dequeue_;
};

} // namespace bot
