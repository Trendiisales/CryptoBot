#pragma once
// =============================================================================
// models.hpp — Core data models
//
// All hot-path structs are:
//   1. Trivially copyable (safe for ring buffers, no constructor in hot path)
//   2. Cache-line aligned (no false sharing between threads)
//   3. Fixed-size (no heap allocation in the critical path)
//   4. Use integer price representation where possible (avoids FP comparison)
//
// Price encoding: all prices stored as double (Binance precision is float64).
// Quantities: double (crypto can have fractional coins).
// Fees/BPS: double (computed in signal engine, not in inner loop).
// =============================================================================

#include "common.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <atomic>

namespace bot {

// ---------------------------------------------------------------------------
// Enumerations (plain enum for trivial copyability)
// ---------------------------------------------------------------------------
enum class Side    : std::uint8_t { BUY = 0, SELL = 1 };
enum class OrdType : std::uint8_t { LIMIT = 0, MARKET = 1, LIMIT_MAKER = 2, IOC = 3 };
enum class OrdStatus : std::uint8_t {
    PENDING = 0, OPEN, PARTIALLY_FILLED, FILLED, CANCELLED, REJECTED, EXPIRED
};
enum class SignalType : std::uint8_t {
    NONE = 0, MOMENTUM, MEAN_REVERSION, ORDER_FLOW, PERP_BASIS, COMPOSITE
};
enum class TradeState : std::uint8_t {
    NONE = 0, ENTERING, OPEN, EXITING, CLOSED, STOPPED, HALTED
};
enum class EventType : std::uint8_t {
    NONE = 0, TICKER, BOOK, CANDLE, FUNDING, TRADE_PRINT
};

constexpr Side opposite(Side s) noexcept {
    return s == Side::BUY ? Side::SELL : Side::BUY;
}

constexpr const char* side_str(Side s) noexcept {
    return s == Side::BUY ? "BUY" : "SELL";
}

constexpr const char* signal_type_str(SignalType t) noexcept {
    switch (t) {
        case SignalType::MOMENTUM:       return "momentum";
        case SignalType::MEAN_REVERSION: return "mean_reversion";
        case SignalType::ORDER_FLOW:     return "order_flow";
        case SignalType::PERP_BASIS:     return "perp_basis";
        case SignalType::COMPOSITE:      return "composite";
        default:                         return "none";
    }
}

constexpr const char* ord_status_str(OrdStatus s) noexcept {
    switch (s) {
        case OrdStatus::PENDING:           return "pending";
        case OrdStatus::OPEN:              return "open";
        case OrdStatus::PARTIALLY_FILLED:  return "partially_filled";
        case OrdStatus::FILLED:            return "filled";
        case OrdStatus::CANCELLED:         return "cancelled";
        case OrdStatus::REJECTED:          return "rejected";
        case OrdStatus::EXPIRED:           return "expired";
        default:                           return "unknown";
    }
}

// ---------------------------------------------------------------------------
// BookLevel — single price level in the order book
// ---------------------------------------------------------------------------
struct BookLevel {
    double price{0.0};
    double qty{0.0};
};
static_assert(sizeof(BookLevel) == 16);

// ---------------------------------------------------------------------------
// OrderBook — L2 snapshot (top 20 levels each side)
// Fixed array — NO heap allocation
// ---------------------------------------------------------------------------
struct alignas(CACHE_LINE_SIZE) OrderBook {
    static constexpr std::size_t MAX_LEVELS = 20;

    Symbol      symbol{};
    std::int64_t ts_ns{0};
    std::uint32_t n_bids{0};
    std::uint32_t n_asks{0};

    std::array<BookLevel, MAX_LEVELS> bids{};   // sorted desc
    std::array<BookLevel, MAX_LEVELS> asks{};   // sorted asc

    [[nodiscard]] double best_bid()  const noexcept { return n_bids ? bids[0].price : 0.0; }
    [[nodiscard]] double best_ask()  const noexcept { return n_asks ? asks[0].price : 0.0; }
    [[nodiscard]] double mid()       const noexcept { return (best_bid() + best_ask()) * 0.5; }

    [[nodiscard]] double spread_bps() const noexcept {
        const double b = best_bid();
        if (b <= 0.0) return 9999.0;
        return ((best_ask() - b) / b) * 10'000.0;
    }

    [[nodiscard]] double bid_depth_usd(std::size_t levels = 5) const noexcept {
        double total = 0.0;
        const std::size_t n = std::min(levels, static_cast<std::size_t>(n_bids));
        for (std::size_t i = 0; i < n; ++i) total += bids[i].price * bids[i].qty;
        return total;
    }

    [[nodiscard]] double ask_depth_usd(std::size_t levels = 5) const noexcept {
        double total = 0.0;
        const std::size_t n = std::min(levels, static_cast<std::size_t>(n_asks));
        for (std::size_t i = 0; i < n; ++i) total += asks[i].price * asks[i].qty;
        return total;
    }

    // OFI in [-1, 1]: positive = buy-side dominant
    [[nodiscard]] double order_flow_imbalance(std::size_t levels = 10) const noexcept {
        const double bid_usd = bid_depth_usd(levels);
        const double ask_usd = ask_depth_usd(levels);
        const double total   = bid_usd + ask_usd;
        if (total <= 0.0) return 0.0;
        return (bid_usd - ask_usd) / total;
    }
};

// ---------------------------------------------------------------------------
// Ticker — best bid/ask snapshot (low-latency bookTicker stream)
// ---------------------------------------------------------------------------
struct alignas(CACHE_LINE_SIZE) Ticker {
    Symbol      symbol{};
    double      bid{0.0};
    double      ask{0.0};
    double      last{0.0};
    double      volume_24h{0.0};
    std::int64_t ts_ns{0};

    [[nodiscard]] double mid()        const noexcept { return (bid + ask) * 0.5; }
    [[nodiscard]] double spread_bps() const noexcept {
        if (bid <= 0.0) return 9999.0;
        return ((ask - bid) / bid) * 10'000.0;
    }
};

// ---------------------------------------------------------------------------
// Candle — OHLCV
// ---------------------------------------------------------------------------
struct alignas(CACHE_LINE_SIZE) Candle {
    Symbol      symbol{};
    double      open{0.0}, high{0.0}, low{0.0}, close{0.0}, volume{0.0};
    std::int64_t open_time_ms{0};
    std::int64_t close_time_ms{0};
    bool         is_closed{false};
};

// ---------------------------------------------------------------------------
// FundingRate — from perp stream (read-only signal input)
// ---------------------------------------------------------------------------
struct alignas(32) FundingRate {
    Symbol      symbol{};
    double      rate{0.0};              // decimal, e.g. 0.0001 = 1 bp per 8h
    std::int64_t next_funding_ms{0};
    std::int64_t ts_ns{0};

    [[nodiscard]] double rate_bps() const noexcept { return rate * 10'000.0; }
};

// ---------------------------------------------------------------------------
// Signal — output of the strategy engine
// ---------------------------------------------------------------------------
struct alignas(CACHE_LINE_SIZE) Signal {
    Symbol      symbol{};
    SignalType  type{SignalType::NONE};
    Side        side{Side::BUY};
    float       confidence{0.0f};         // 0..1
    float       expected_edge_bps{0.0f};  // estimated gross edge
    std::int64_t ts_ns{0};
    std::uint64_t id{0};

    // Compact sub-signal bitmask (which strategies voted)
    std::uint8_t  sub_signals{0};         // bit 0=MOM, 1=MR, 2=OFI, 3=PERP
    std::uint8_t  n_aligned{0};

    [[nodiscard]] bool valid() const noexcept {
        return type != SignalType::NONE && expected_edge_bps > 0.0f;
    }
};

// ---------------------------------------------------------------------------
// OrderRequest — submitted to the gateway
// ---------------------------------------------------------------------------
struct alignas(64) OrderRequest {
    Symbol      symbol{};
    Side        side{Side::BUY};
    OrdType     type{OrdType::LIMIT_MAKER};
    double      qty{0.0};
    double      price{0.0};      // 0.0 for MARKET
    double      iceberg_qty{0.0};
    std::uint64_t client_id{0};
    bool        post_only{true};
};

// ---------------------------------------------------------------------------
// OrderResult — returned from gateway (sync/async)
// ---------------------------------------------------------------------------
struct alignas(64) OrderResult {
    Symbol      symbol{};
    std::uint64_t exchange_id{0};
    std::uint64_t client_id{0};
    OrdStatus   status{OrdStatus::PENDING};
    double      filled_qty{0.0};
    double      avg_price{0.0};
    double      fee_paid{0.0};
    std::int64_t latency_us{0};   // round-trip from placement to ack
    bool        is_shadow{false};
};

// ---------------------------------------------------------------------------
// Trade — full round-trip lifecycle
// ---------------------------------------------------------------------------
struct alignas(CACHE_LINE_SIZE) Trade {
    Symbol       symbol{};
    std::uint64_t id{0};
    TradeState   state{TradeState::NONE};
    Side         side{Side::BUY};

    double       entry_price{0.0};
    double       exit_price{0.0};
    double       qty{0.0};
    double       stop_price{0.0};
    double       take_profit_price{0.0};
    double       trailing_stop{0.0};
    double       entry_fee{0.0};
    double       exit_fee{0.0};

    std::int64_t opened_ns{0};
    std::int64_t closed_ns{0};
    std::int64_t opened_epoch_ms{0};
    std::int64_t closed_epoch_ms{0};
    std::int64_t entry_latency_us{0};
    std::int64_t exit_latency_us{0};
    OrdStatus    entry_order_status{OrdStatus::PENDING};
    OrdStatus    exit_order_status{OrdStatus::PENDING};

    Signal       signal{};   // copy of originating signal

    [[nodiscard]] double gross_pnl_usd() const noexcept {
        if (entry_price <= 0.0 || qty <= 0.0) return 0.0;
        const double sign = (side == Side::BUY) ? 1.0 : -1.0;
        return sign * (exit_price - entry_price) * qty;
    }

    [[nodiscard]] double pnl_usd() const noexcept {
        return gross_pnl_usd() - entry_fee - exit_fee;
    }

    [[nodiscard]] double pnl_bps() const noexcept {
        const double notional = entry_price * qty;
        if (notional <= 0.0) return 0.0;
        return (pnl_usd() / notional) * 10'000.0;
    }

    [[nodiscard]] double duration_sec() const noexcept {
        const std::int64_t end = closed_ns > 0 ? closed_ns : now_ns();
        return static_cast<double>(end - opened_ns) / 1e9;
    }
};

// ---------------------------------------------------------------------------
// PortfolioState — atomic-safe summary (written by trade manager, read by risk)
// Wrapped in its own struct to allow atomic load/store of the whole thing
// via a generation counter (seqlock pattern)
// ---------------------------------------------------------------------------
struct PortfolioSnapshot {
    double  equity_usd{0.0};
    double  available_usd{0.0};
    double  total_pnl_usd{0.0};
    double  daily_pnl_usd{0.0};
    double  peak_equity_usd{0.0};
    double  total_exposure_usd{0.0};
    int     open_trade_count{0};
    bool    is_halted{false};

    [[nodiscard]] double drawdown_pct() const noexcept {
        if (peak_equity_usd <= 0.0) return 0.0;
        return ((peak_equity_usd - equity_usd) / peak_equity_usd) * 100.0;
    }
};

// Seqlock-protected portfolio state (lock-free reads, exclusive writes)
struct alignas(CACHE_LINE_SIZE) AtomicPortfolio {
    std::atomic<std::uint64_t> seq{0};   // even = stable, odd = being written
    PortfolioSnapshot snap{};

    // Writer: increment seq (odd), write, increment again (even)
    void store(const PortfolioSnapshot& s) noexcept {
        seq.fetch_add(1, std::memory_order_release);   // odd
        std::atomic_thread_fence(std::memory_order_seq_cst);
        snap = s;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        seq.fetch_add(1, std::memory_order_release);   // even
    }

    // Reader: spin until seq is even and stable
    [[nodiscard]] PortfolioSnapshot load() const noexcept {
        for (;;) {
            const std::uint64_t s1 = seq.load(std::memory_order_acquire);
            if (s1 & 1u) { continue; }   // writer in progress — spin
            PortfolioSnapshot copy = snap;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_acquire) == s1) return copy;
        }
    }
};

// ---------------------------------------------------------------------------
// MarketEvent — variant-style tagged union pushed into ring buffers
// Trivially copyable, fixed size, no heap
// ---------------------------------------------------------------------------
struct alignas(CACHE_LINE_SIZE) MarketEvent {
    EventType type{EventType::NONE};
    char      _align[7]{};

    union {
        Ticker      ticker;
        FundingRate funding;
        // Candle and OrderBook are too large for inline storage — carry index
        // into a preallocated slab instead
        struct { Symbol symbol; std::uint32_t slab_idx; } ref;
    } data{};
};

} // namespace bot
