#pragma once
// =============================================================================
// risk_manager.hpp — Position sizing, drawdown enforcement, kill switches
//
// All gate decisions are branchless-friendly.
// Kill-switch flag is a single atomic<bool> — reads are acquire, writes release.
// Per-symbol signal rate-limiting uses atomic<int64_t> timestamps.
// No mutex anywhere in this file.
// =============================================================================

#include "core/models.hpp"
#include "core/config_parser.hpp"
#include <atomic>
#include <cmath>
#include <tuple>
#include <ctime>

namespace bot {

struct RiskDecision {
    bool   allowed{false};
    double position_usd{0.0};
    double stop_price{0.0};
    double take_profit_price{0.0};
    const char* reason{""};
};

class RiskManager {
public:
    explicit RiskManager(const RiskSettings& cfg)
        : cfg_(cfg)
        , halted_(false)
        , peak_equity_usd_(0.0)
        , day_start_equity_usd_(0.0)
        , day_start_date_(today_utc())
    {}

    // -------------------------------------------------------------------------
    // Primary gate: returns RiskDecision with allowed=true/false
    // -------------------------------------------------------------------------
    [[nodiscard]] RiskDecision evaluate(
        const Signal&             sig,
        const PortfolioSnapshot&  port,
        double                    symbol_exposure_usd,
        std::int64_t              last_signal_ns) const noexcept
    {
        RiskDecision d;

        // 1. Kill switch
        if (BOT_UNLIKELY(halted_.load(std::memory_order_acquire))) {
            d.reason = "HALTED";
            return d;
        }

        // 2. Update peak equity (relaxed — only writer is this function context)
        update_peak(port.equity_usd);

        // 3. Daily reset
        maybe_reset_daily(port.equity_usd);

        // 4. Total drawdown check
        const double peak = peak_equity_usd_.load(std::memory_order_relaxed);
        if (peak > 0.0) {
            const double dd = ((peak - port.equity_usd) / peak) * 100.0;
            if (BOT_UNLIKELY(dd >= cfg_.total_dd_limit_pct)) {
                trigger_halt("Total drawdown limit breached");
                d.reason = "KILL: total DD";
                return d;
            }
        }

        // 5. Daily drawdown check
        const double day_start = day_start_equity_usd_.load(std::memory_order_relaxed);
        if (day_start > 0.0) {
            const double daily_dd = ((day_start - port.equity_usd) / day_start) * 100.0;
            if (BOT_UNLIKELY(daily_dd >= cfg_.daily_dd_limit_pct)) {
                d.reason = "daily DD limit";
                return d;
            }
        }

        // 6. Max open trades
        if (BOT_UNLIKELY(port.open_trade_count >= cfg_.max_open_trades)) {
            d.reason = "max open trades";
            return d;
        }

        // 7. Per-pair exposure
        if (symbol_exposure_usd >= cfg_.max_position_usd) {
            d.reason = "pair exposure cap";
            return d;
        }

        // 8. Total exposure
        if (port.total_exposure_usd >= cfg_.max_total_usd) {
            d.reason = "total exposure cap";
            return d;
        }

        // 9. Signal rate limit per symbol
        const std::int64_t elapsed_ms =
            (now_ns() - last_signal_ns) / 1'000'000;
        if (elapsed_ms < static_cast<std::int64_t>(cfg_.min_signal_interval_sec) * 1000) {
            d.reason = "signal rate limit";
            return d;
        }

        // 10. Compute size
        const double size = compute_size(sig, port, symbol_exposure_usd);
        if (size < 10.0) {
            d.reason = "size too small";
            return d;
        }

        // 11. Stops
        const auto [stop, tp] = compute_stops(0.0 /* set after fill */, sig.side);
        // stop/tp are set relative to fill price later; return as-is (0 = unset)

        d.allowed        = true;
        d.position_usd   = size;
        d.stop_price     = stop;
        d.take_profit_price = tp;
        return d;
    }

    // Compute stops from actual entry price
    [[nodiscard]] std::pair<double, double> compute_stops(
        double entry_price, Side side) const noexcept
    {
        if (entry_price <= 0.0) return {0.0, 0.0};

        const double sign   = (side == Side::BUY) ? 1.0 : -1.0;
        const double stop   = entry_price * (1.0 - sign * cfg_.stop_loss_bps / 10'000.0);
        const double tp     = 0.0;   // trailing stop used; TP auto-disabled
        return {stop, tp};
    }

    // Update trailing stop — branchless, no mutex
    [[nodiscard]] double update_trailing_stop(
        double current_price,
        double entry_price,
        double current_stop,
        Side   side) const noexcept
    {
        const double sign    = (side == Side::BUY) ? 1.0 : -1.0;
        const double pnl_bps = sign * ((current_price - entry_price) / entry_price) * 10'000.0;

        if (pnl_bps < cfg_.trailing_activate_bps) return current_stop;

        const double new_stop = current_price
                              * (1.0 - sign * cfg_.trailing_distance_bps / 10'000.0);

        // Only move stop in favourable direction
        if (side == Side::BUY)
            return std::max(current_stop, new_stop);
        else
            return (current_stop <= 0.0) ? new_stop : std::min(current_stop, new_stop);
    }

    // Check stop/TP hit
    [[nodiscard]] bool should_stop_out(
        double current_price,
        double stop_price,
        double tp_price,
        Side   side) const noexcept
    {
        if (stop_price > 0.0) {
            if (side == Side::BUY  && current_price <= stop_price) return true;
            if (side == Side::SELL && current_price >= stop_price) return true;
        }
        if (tp_price > 0.0) {
            if (side == Side::BUY  && current_price >= tp_price) return true;
            if (side == Side::SELL && current_price <= tp_price) return true;
        }
        return false;
    }

    void reset_halt() noexcept {
        halted_.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool is_halted() const noexcept {
        return halted_.load(std::memory_order_acquire);
    }

private:
    const RiskSettings& cfg_;

    // Atomic kill switch — written once on breach, read on every evaluate()
    alignas(CACHE_LINE_SIZE) mutable std::atomic<bool> halted_;

    // Peak and daily-start equity — written by evaluate (single thread), read atomically
    alignas(CACHE_LINE_SIZE) mutable std::atomic<double> peak_equity_usd_;
    alignas(CACHE_LINE_SIZE) mutable std::atomic<double> day_start_equity_usd_;
    mutable int day_start_date_;

    void trigger_halt(const char* reason) const noexcept {
        halted_.store(true, std::memory_order_release);
        // Caller logs the reason
        (void)reason;
    }

    void update_peak(double equity) const noexcept {
        double current = peak_equity_usd_.load(std::memory_order_relaxed);
        while (equity > current) {
            if (peak_equity_usd_.compare_exchange_weak(
                    current, equity,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed))
                break;
        }
    }

    void maybe_reset_daily(double equity) const noexcept {
        const int today = today_utc();
        if (today != day_start_date_) {
            day_start_date_ = today;
            day_start_equity_usd_.store(equity, std::memory_order_relaxed);
        }
        const double ds = day_start_equity_usd_.load(std::memory_order_relaxed);
        if (ds <= 0.0) {
            day_start_equity_usd_.store(equity, std::memory_order_relaxed);
        }
    }

    // Kelly/fixed-fraction hybrid
    [[nodiscard]] double compute_size(
        const Signal&            sig,
        const PortfolioSnapshot& port,
        double                   pair_exposure) const noexcept
    {
        const double equity = port.equity_usd;
        if (equity <= 0.0) return 0.0;

        // Fixed fraction
        const double fixed_usd = equity * (cfg_.position_size_pct / 100.0);

        // Kelly fraction
        const double p = static_cast<double>(sig.confidence);
        const double b = static_cast<double>(sig.expected_edge_bps) / cfg_.stop_loss_bps;
        double kelly_f = (b > 0.001)
            ? std::max(0.0, p - (1.0 - p) / b)
            : 0.0;
        kelly_f = std::min(kelly_f, cfg_.max_kelly_fraction);
        const double kelly_usd = equity * kelly_f;

        double size = std::min(fixed_usd, kelly_usd);
        // Cap by remaining pair capacity
        size = std::min(size, cfg_.max_position_usd - pair_exposure);
        // Cap by remaining total exposure
        size = std::min(size, cfg_.max_total_usd - port.total_exposure_usd);
        return std::max(0.0, size);
    }

    static int today_utc() noexcept {
        const std::time_t t = std::time(nullptr);
        const std::tm* tm   = std::gmtime(&t);
        return tm ? tm->tm_year * 10000 + tm->tm_mon * 100 + tm->tm_mday : 0;
    }
};

} // namespace bot
