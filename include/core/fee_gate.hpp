#pragma once
// =============================================================================
// fee_gate.hpp — 8 bps net edge enforcement
//
// Evaluated before every trade entry. Calculates true round-trip cost and
// rejects any signal where expected_edge < (fees + spread + slippage + 8 bps).
//
// All arithmetic is branchless-friendly double math.
// Counters use relaxed atomics (statistics, not synchronisation).
// =============================================================================

#include "core/models.hpp"
#include "core/config_parser.hpp"
#include <atomic>
#include <cmath>

namespace bot {

struct FeeGateResult {
    bool   is_viable{false};
    double entry_fee_bps{0.0};
    double exit_fee_bps{0.0};
    double spread_cost_bps{0.0};
    double slippage_est_bps{0.0};
    double total_cost_bps{0.0};
    double expected_edge_bps{0.0};
    double net_edge_bps{0.0};
    const char* rejection_reason{""};
};

class FeeGate {
public:
    explicit FeeGate(const FeeSettings& cfg) : cfg_(cfg) {}

    // -------------------------------------------------------------------------
    // Main evaluation — called on every candidate signal
    // -------------------------------------------------------------------------
    [[nodiscard]] FeeGateResult evaluate(
        const Signal&    sig,
        const Ticker*    ticker,
        const OrderBook* book) const noexcept
    {
        FeeGateResult r;
        r.expected_edge_bps = sig.expected_edge_bps;

        // 1. Data availability
        if (BOT_UNLIKELY(!ticker || !book)) {
            passed_counter_.fetch_add(0, std::memory_order_relaxed);  // no-op keep codepath
            blocked_counter_.fetch_add(1, std::memory_order_relaxed);
            r.rejection_reason = "no market data";
            return r;
        }

        // 2. Spread check
        const double spread_bps = ticker->spread_bps();
        if (BOT_UNLIKELY(spread_bps > cfg_.max_spread_bps)) {
            blocked_counter_.fetch_add(1, std::memory_order_relaxed);
            r.rejection_reason = "spread too wide";
            return r;
        }

        // 3. Fee calculation (maker-first assumption)
        const double entry_fee = cfg_.maker_bps;
        const double exit_fee  = cfg_.maker_bps;

        // 4. Spread cost: half the spread on each leg = spread per leg
        const double spread_cost = spread_bps;   // entry + exit legs combined

        // 5. Slippage estimate from order book depth
        const double slippage = estimate_slippage(sig, *book);

        // 6. Total round-trip cost
        const double total_cost = entry_fee + exit_fee + spread_cost + slippage;

        // 7. Net edge
        const double net_edge = sig.expected_edge_bps - total_cost;

        r.entry_fee_bps    = entry_fee;
        r.exit_fee_bps     = exit_fee;
        r.spread_cost_bps  = spread_cost;
        r.slippage_est_bps = slippage;
        r.total_cost_bps   = total_cost;
        r.net_edge_bps     = net_edge;

        // 8. Gate decision
        if (BOT_UNLIKELY(net_edge < cfg_.min_net_edge_bps)) {
            blocked_counter_.fetch_add(1, std::memory_order_relaxed);
            r.rejection_reason = "insufficient net edge";
            return r;
        }

        passed_counter_.fetch_add(1, std::memory_order_relaxed);
        r.is_viable = true;
        return r;
    }

    // Statistics (relaxed atomic reads — approximate)
    [[nodiscard]] std::uint64_t passed()  const noexcept {
        return passed_counter_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t blocked() const noexcept {
        return blocked_counter_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] double pass_rate() const noexcept {
        const double total = static_cast<double>(passed() + blocked());
        return total > 0.0 ? (static_cast<double>(passed()) / total) * 100.0 : 0.0;
    }

private:
    const FeeSettings& cfg_;

    // Statistics — relaxed atomics (no synchronisation needed, just counters)
    alignas(CACHE_LINE_SIZE) mutable std::atomic<std::uint64_t> passed_counter_{0};
    alignas(CACHE_LINE_SIZE) mutable std::atomic<std::uint64_t> blocked_counter_{0};

    [[nodiscard]] double estimate_slippage(
        const Signal&    sig,
        const OrderBook& book) const noexcept
    {
        const double depth_usd = (sig.side == Side::BUY)
            ? book.ask_depth_usd(5)
            : book.bid_depth_usd(5);

        if (depth_usd <= 0.0) return cfg_.slippage_buffer_bps;

        // Assume ~$5k order notional (conservative)
        constexpr double EST_NOTIONAL = 5'000.0;
        const double ratio = EST_NOTIONAL / depth_usd;

        if (ratio < 0.005) return 0.5;
        if (ratio < 0.010) return 1.0;
        if (ratio < 0.050) return cfg_.slippage_buffer_bps;
        return cfg_.slippage_buffer_bps * 2.0;
    }
};

} // namespace bot
