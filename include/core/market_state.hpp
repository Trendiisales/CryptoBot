#pragma once
// =============================================================================
// market_state.hpp — Lock-free market data store
//
// Stores the latest Ticker, OrderBook, CandleBuffer, and FundingRate for each
// symbol. Multiple reader threads see consistent snapshots with no mutex.
//
// Pattern: per-symbol SeqLock
//   - Writer increments sequence to odd before write, even after
//   - Reader spins if sequence is odd (writer in progress)
//   - On x86, single-writer seqlock is effectively free (TSO memory model)
//   - On ARM, uses explicit fences (already in AtomicPortfolio pattern)
//
// CandleBuffer: circular array of the last N closed candles per symbol.
// Reader gets a snapshot copy (cheap — 200 * 128 bytes = 25 KB max).
// =============================================================================

#include "models.hpp"
#include "spsc_ring_buffer.hpp"
#include <array>
#include <unordered_map>
#include <string>
#include <mutex>

namespace bot {

// ---------------------------------------------------------------------------
// SeqLock-protected slot for a single value T
// ---------------------------------------------------------------------------
template<typename T>
struct SeqLocked {
    static_assert(std::is_trivially_copyable_v<T>);

    alignas(CACHE_LINE_SIZE) std::atomic<std::uint64_t> seq{0};
    alignas(CACHE_LINE_SIZE) T value{};

    // Writer (single writer assumed — no CAS needed)
    void store(const T& v) noexcept {
        seq.fetch_add(1, std::memory_order_release);   // mark dirty (odd)
        std::atomic_thread_fence(std::memory_order_seq_cst);
        value = v;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        seq.fetch_add(1, std::memory_order_release);   // mark clean (even)
    }

    // Reader — returns copy on clean read
    [[nodiscard]] T load() const noexcept {
        for (;;) {
            const std::uint64_t s1 = seq.load(std::memory_order_acquire);
            if (BOT_UNLIKELY(s1 & 1u)) {
                // Writer in progress — yield and retry
                #if defined(__x86_64__) || defined(_M_X64)
                    __asm__ volatile("pause" ::: "memory");
                #else
                    std::atomic_thread_fence(std::memory_order_acquire);
                #endif
                continue;
            }
            T copy = value;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == s1) return copy;
        }
    }

    [[nodiscard]] bool has_data() const noexcept {
        return seq.load(std::memory_order_relaxed) > 0;
    }
};

// ---------------------------------------------------------------------------
// CandleBuffer — circular array of fixed capacity, one writer (feed thread)
// Readers take a full snapshot copy
// ---------------------------------------------------------------------------
template<std::size_t CAPACITY = 256>
struct CandleBuffer {
    static_assert(IsPowerOfTwo<CAPACITY>::value);
    static constexpr std::size_t MASK = CAPACITY - 1;

    // Write pointer (incremented by feed thread only)
    alignas(CACHE_LINE_SIZE) std::atomic<std::uint64_t> write_seq{0};
    alignas(CACHE_LINE_SIZE) std::array<Candle, CAPACITY> candles{};

    void push(const Candle& c) noexcept {
        const std::uint64_t idx = write_seq.load(std::memory_order_relaxed);
        candles[idx & MASK] = c;
        write_seq.store(idx + 1, std::memory_order_release);
    }

    // Get last N closed candles in chronological order into dst[]
    // Returns number of candles actually copied
    std::size_t get_last(Candle* dst, std::size_t n) const noexcept {
        const std::uint64_t head = write_seq.load(std::memory_order_acquire);
        if (head == 0) return 0;
        const std::size_t available = static_cast<std::size_t>(
            std::min(head, static_cast<std::uint64_t>(CAPACITY)));
        const std::size_t count = std::min(n, available);
        const std::uint64_t start = head - count;
        for (std::size_t i = 0; i < count; ++i) {
            dst[i] = candles[(start + i) & MASK];
        }
        return count;
    }

    std::uint64_t count() const noexcept {
        return write_seq.load(std::memory_order_acquire);
    }
};

// ---------------------------------------------------------------------------
// SymbolState — all data for one symbol
// Allocated in a flat array (no heap per symbol)
// ---------------------------------------------------------------------------
struct alignas(CACHE_LINE_SIZE) SymbolState {
    Symbol               symbol{};
    SeqLocked<Ticker>    ticker{};
    SeqLocked<OrderBook> book{};
    SeqLocked<FundingRate> funding{};
    CandleBuffer<256>    candles{};

    // Last signal timestamp per symbol (for rate limiting)
    std::atomic<std::int64_t> last_signal_ns{0};
    std::atomic<std::uint8_t> last_signal_type{
        static_cast<std::uint8_t>(SignalType::NONE)
    };

    // Per-pair position tracking (atomic double via int64 bitcast)
    std::atomic<std::int64_t> position_usd_bits{0};

    void set_position_usd(double usd) noexcept {
        std::int64_t bits;
        static_assert(sizeof(double) == sizeof(std::int64_t));
        std::memcpy(&bits, &usd, sizeof(double));
        position_usd_bits.store(bits, std::memory_order_release);
    }

    [[nodiscard]] double get_position_usd() const noexcept {
        const std::int64_t bits = position_usd_bits.load(std::memory_order_acquire);
        double usd;
        std::memcpy(&usd, &bits, sizeof(double));
        return usd;
    }
};

// ---------------------------------------------------------------------------
// MarketState — flat array of SymbolState indexed by symbol slot
// No heap allocation. Max symbols hard-coded (expand if needed).
// ---------------------------------------------------------------------------
class MarketState {
public:
    static constexpr std::size_t MAX_SYMBOLS = 32;

    MarketState() = default;

    // Register a symbol at startup (single-threaded init phase)
    int register_symbol(std::string_view sym) {
        if (n_symbols_ >= MAX_SYMBOLS)
            throw std::overflow_error("Too many symbols");
        const int idx = static_cast<int>(n_symbols_++);
        slots_[idx].symbol = Symbol(sym);
        symbol_to_idx_[std::string(sym)] = idx;
        return idx;
    }

    [[nodiscard]] int index_of(std::string_view sym) const noexcept {
        auto it = symbol_to_idx_.find(std::string(sym));
        return it != symbol_to_idx_.end() ? it->second : -1;
    }

    [[nodiscard]] SymbolState* get(int idx) noexcept {
        if (idx < 0 || idx >= static_cast<int>(n_symbols_)) return nullptr;
        return &slots_[idx];
    }

    [[nodiscard]] const SymbolState* get(int idx) const noexcept {
        if (idx < 0 || idx >= static_cast<int>(n_symbols_)) return nullptr;
        return &slots_[idx];
    }

    [[nodiscard]] SymbolState* get(std::string_view sym) noexcept {
        return get(index_of(sym));
    }

    [[nodiscard]] const SymbolState* get(std::string_view sym) const noexcept {
        return get(index_of(sym));
    }

    [[nodiscard]] std::size_t n_symbols() const noexcept { return n_symbols_; }

    // Iterate all registered slots
    template<typename Fn>
    void for_each(Fn&& fn) {
        for (std::size_t i = 0; i < n_symbols_; ++i) fn(slots_[i]);
    }

private:
    alignas(CACHE_LINE_SIZE) SymbolState slots_[MAX_SYMBOLS]{};
    std::size_t  n_symbols_{0};
    std::unordered_map<std::string, int> symbol_to_idx_;
};

} // namespace bot
