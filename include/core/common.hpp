#pragma once
// =============================================================================
// common.hpp — Shared constants, CRTP bases, cache-line tooling, atomics
//
// Design philosophy:
//   - CACHE_LINE_SIZE = 64 bytes (x86/ARM standard)
//   - All hot-path structs are alignas(CACHE_LINE_SIZE)
//   - Atomic head/tail indices are each on their own cache line to prevent
//     false sharing between producer and consumer threads
//   - CRTP used for zero-overhead strategy dispatch and mixin injection
//   - No virtual functions in the hot path
//   - No std::mutex in market data path; std::atomic with acquire/release
// =============================================================================

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <chrono>
#include <type_traits>

namespace bot {

// ---------------------------------------------------------------------------
// Cache line size — pad all hot structs to avoid false sharing
// ---------------------------------------------------------------------------
static constexpr std::size_t CACHE_LINE_SIZE = 64;

// Pad a type T to fill complete cache lines
template<typename T>
struct alignas(CACHE_LINE_SIZE) CacheLinePadded {
    T value;
    char _pad[CACHE_LINE_SIZE - (sizeof(T) % CACHE_LINE_SIZE == 0
                                   ? CACHE_LINE_SIZE
                                   : sizeof(T) % CACHE_LINE_SIZE)];
};

// Shorthand: an atomic index on its own cache line
struct alignas(CACHE_LINE_SIZE) AtomicIndex {
    std::atomic<std::size_t> idx{0};
    char _pad[CACHE_LINE_SIZE - sizeof(std::atomic<std::size_t>)];
};
static_assert(sizeof(AtomicIndex) == CACHE_LINE_SIZE);

// ---------------------------------------------------------------------------
// Timestamp helpers (nanosecond precision)
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

[[nodiscard]] inline std::int64_t now_us() noexcept {
    return now_ns() / 1'000;
}

[[nodiscard]] inline std::int64_t now_ms() noexcept {
    return now_ns() / 1'000'000;
}

[[nodiscard]] inline std::int64_t epoch_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ---------------------------------------------------------------------------
// Fixed-width symbol type (avoids heap allocation for symbol strings)
// Max Binance symbol length is 12 chars (e.g. "BTCUSDT" = 7, plenty of room)
// ---------------------------------------------------------------------------
struct Symbol {
    static constexpr std::size_t MAX_LEN = 16;
    char data[MAX_LEN]{};

    Symbol() = default;
    explicit Symbol(std::string_view sv) {
        std::size_t n = std::min(sv.size(), MAX_LEN - 1);
        std::memcpy(data, sv.data(), n);
        data[n] = '\0';
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return {data};
    }

    bool operator==(const Symbol& o) const noexcept {
        return std::memcmp(data, o.data, MAX_LEN) == 0;
    }
    bool operator!=(const Symbol& o) const noexcept { return !(*this == o); }
};

// ---------------------------------------------------------------------------
// CRTP base — provides static_cast dispatch with zero virtual overhead
//
// Usage:
//   template<typename Derived>
//   struct MyBase : CrtpBase<Derived, MyBase> {
//       void interface() { self().implementation(); }
//   };
//   struct Concrete : MyBase<Concrete> {
//       void implementation() { /* ... */ }
//   };
// ---------------------------------------------------------------------------
template<typename Derived, template<typename> class Base>
struct CrtpBase {
protected:
    [[nodiscard]] Derived& self() noexcept {
        return static_cast<Derived&>(*this);
    }
    [[nodiscard]] const Derived& self() const noexcept {
        return static_cast<const Derived&>(*this);
    }
};

// ---------------------------------------------------------------------------
// CRTP Mixin: Timestamped — injects created_ns() into any struct
// ---------------------------------------------------------------------------
template<typename Derived>
struct Timestamped : CrtpBase<Derived, Timestamped> {
    std::int64_t ts_ns{now_ns()};
    [[nodiscard]] std::int64_t age_us() const noexcept {
        return (now_ns() - ts_ns) / 1'000;
    }
};

// ---------------------------------------------------------------------------
// CRTP Mixin: Identifiable — injects a monotonic uint64 ID
// ---------------------------------------------------------------------------
template<typename Derived>
struct Identifiable : CrtpBase<Derived, Identifiable> {
private:
    static inline std::atomic<std::uint64_t> s_counter{1};
public:
    std::uint64_t id{s_counter.fetch_add(1, std::memory_order_relaxed)};
};

// ---------------------------------------------------------------------------
// CRTP Strategy interface
// Every trading strategy inherits from this and implements:
//   std::optional<Signal> on_candle_impl(const CandleEvent&)
//   std::optional<Signal> on_book_impl(const BookEvent&)
//   const char* name_impl() const
// ---------------------------------------------------------------------------
struct Signal;   // forward declaration — defined in models.hpp

template<typename Derived>
struct StrategyBase : CrtpBase<Derived, StrategyBase> {
    // Called on every closed 1m candle
    template<typename CandleEvent>
    [[nodiscard]] auto on_candle(const CandleEvent& e) {
        return this->self().on_candle_impl(e);
    }

    // Called on every order book update
    template<typename BookEvent>
    [[nodiscard]] auto on_book(const BookEvent& e) {
        return this->self().on_book_impl(e);
    }

    [[nodiscard]] const char* name() const noexcept {
        return this->self().name_impl();
    }
};

// ---------------------------------------------------------------------------
// CRTP OrderGateway interface
// Concrete implementations: LiveGateway, ShadowGateway
// ---------------------------------------------------------------------------
template<typename Derived>
struct GatewayCrtp : CrtpBase<Derived, GatewayCrtp> {
    template<typename OrderReq>
    [[nodiscard]] auto place(const OrderReq& req) {
        return this->self().place_impl(req);
    }

    template<typename CancelReq>
    bool cancel(const CancelReq& req) {
        return this->self().cancel_impl(req);
    }

    [[nodiscard]] bool is_shadow() const noexcept {
        return this->self().is_shadow_impl();
    }
};

// ---------------------------------------------------------------------------
// Compile-time power-of-two check (required for ring buffer sizing)
// ---------------------------------------------------------------------------
template<std::size_t N>
struct IsPowerOfTwo : std::bool_constant<(N > 0) && ((N & (N - 1)) == 0)> {};

// ---------------------------------------------------------------------------
// Prefetch hint (non-mandatory, hints L1 prefetch for sequential access)
// ---------------------------------------------------------------------------
#if defined(__GNUC__) || defined(__clang__)
  #define BOT_PREFETCH(ptr)  __builtin_prefetch((ptr), 0, 3)
  #define BOT_LIKELY(x)      __builtin_expect(!!(x), 1)
  #define BOT_UNLIKELY(x)    __builtin_expect(!!(x), 0)
  #define BOT_NOINLINE       __attribute__((noinline))
  #define BOT_ALWAYS_INLINE  __attribute__((always_inline)) inline
#else
  #define BOT_PREFETCH(ptr)
  #define BOT_LIKELY(x)    (x)
  #define BOT_UNLIKELY(x)  (x)
  #define BOT_NOINLINE
  #define BOT_ALWAYS_INLINE inline
#endif

} // namespace bot
