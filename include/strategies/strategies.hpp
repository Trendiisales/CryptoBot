#pragma once
// =============================================================================
// strategies.hpp — CRTP-based signal generation engine
//
// Each strategy is a CRTP mixin that implements on_candle_impl / on_book_impl.
// The SignalComposer instantiates all strategies as value members (no heap)
// and combines their outputs into a composite Signal.
//
// Indicator math uses fixed std::array<double, N> — no std::vector, no heap.
// All functions are [[nodiscard]] and noexcept where possible.
//
// CRTP dispatch: zero virtual function overhead on the hot path.
// =============================================================================

#include "core/common.hpp"
#include "core/models.hpp"
#include "core/market_state.hpp"
#include "core/config_parser.hpp"
#include <array>
#include <cmath>
#include <optional>
#include <numeric>
#include <algorithm>

namespace bot {

// ---------------------------------------------------------------------------
// Indicator math (operates on raw double arrays — no containers)
// All take pointer + length; caller provides stack or arena storage.
// ---------------------------------------------------------------------------
namespace indicators {

// Exponential moving average of last `period` values in arr[0..n-1]
[[nodiscard]] inline double ema(
    const double* arr, std::size_t n, int period) noexcept
{
    if (static_cast<int>(n) < period) return 0.0;
    const double k = 2.0 / (period + 1.0);
    double result  = arr[0];
    for (std::size_t i = 1; i < n; ++i)
        result = arr[i] * k + result * (1.0 - k);
    return result;
}

// RSI using standard Wilder's smoothing
[[nodiscard]] inline double rsi(
    const double* closes, std::size_t n, int period) noexcept
{
    if (static_cast<int>(n) < period + 1) return 50.0;
    double avg_gain = 0.0, avg_loss = 0.0;
    const std::size_t start = n - period - 1;
    for (std::size_t i = start; i < n - 1; ++i) {
        const double d = closes[i + 1] - closes[i];
        if (d >= 0.0) avg_gain += d; else avg_loss -= d;
    }
    avg_gain /= period;
    avg_loss /= period;
    if (avg_loss < 1e-12) return 100.0;
    return 100.0 - 100.0 / (1.0 + avg_gain / avg_loss);
}

struct BollingerBands { double upper, mid, lower; };

[[nodiscard]] inline std::optional<BollingerBands> bollinger(
    const double* closes, std::size_t n, int period, double std_mult) noexcept
{
    if (static_cast<int>(n) < period) return std::nullopt;
    const double* w = closes + (n - period);
    double sum = 0.0;
    for (int i = 0; i < period; ++i) sum += w[i];
    const double mid = sum / period;
    double var = 0.0;
    for (int i = 0; i < period; ++i) {
        const double d = w[i] - mid;
        var += d * d;
    }
    const double s = std::sqrt(var / period);
    return BollingerBands{ mid + std_mult * s, mid, mid - std_mult * s };
}

// Z-score of the last value relative to the window
[[nodiscard]] inline double zscore(
    const double* arr, std::size_t n, int window) noexcept
{
    if (static_cast<int>(n) < window || window < 2) return 0.0;
    const double* w = arr + (n - window);
    double sum = 0.0;
    for (int i = 0; i < window; ++i) sum += w[i];
    const double mean = sum / window;
    double var = 0.0;
    for (int i = 0; i < window; ++i) { const double d = w[i]-mean; var += d*d; }
    const double std_dev = std::sqrt(var / window);
    if (std_dev < 1e-12) return 0.0;
    return (arr[n - 1] - mean) / std_dev;
}

// Session VWAP from candle array
[[nodiscard]] inline double vwap(const Candle* candles, std::size_t n) noexcept {
    double sum_pv = 0.0, sum_v = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double tp = (candles[i].high + candles[i].low + candles[i].close) / 3.0;
        sum_pv += tp * candles[i].volume;
        sum_v  += candles[i].volume;
    }
    return sum_v > 0.0 ? sum_pv / sum_v : 0.0;
}

} // namespace indicators

// ---------------------------------------------------------------------------
// Working buffer for indicator computations (stack-allocated, no heap)
// ---------------------------------------------------------------------------
template<std::size_t N = 256>
struct IndicatorBuf {
    std::array<double, N> closes{};
    std::array<double, N> volumes{};
    std::array<Candle,  N> raw_candles{};
    std::size_t count{0};

    void load(const CandleBuffer<N>& cbuf) noexcept {
        count = cbuf.get_last(raw_candles.data(), N);
        for (std::size_t i = 0; i < count; ++i) {
            closes[i]  = raw_candles[i].close;
            volumes[i] = raw_candles[i].volume;
        }
    }
};

// ---------------------------------------------------------------------------
// CRTP Strategy base (common interface, zero virtual)
// ---------------------------------------------------------------------------
template<typename Derived>
struct StrategyMixin : StrategyBase<Derived> {
    // Default no-op for on_book (override in derived if needed)
    template<typename = void>
    std::optional<Signal> on_book_impl(const OrderBook&) noexcept {
        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// 1. MomentumStrategy — EMA cross + RSI + volume confirm
// ---------------------------------------------------------------------------
struct MomentumStrategy : StrategyMixin<MomentumStrategy> {
    const StrategySettings& cfg;
    explicit MomentumStrategy(const StrategySettings& c) : cfg(c) {}

    const char* name_impl() const noexcept { return "Momentum"; }

    template<std::size_t N = 256>
    [[nodiscard]] std::optional<Signal> evaluate(
        const Symbol& sym, const IndicatorBuf<N>& buf) const noexcept
    {
        if (static_cast<int>(buf.count) < cfg.ema_trend + 5) return std::nullopt;

        const double* c = buf.closes.data();
        const std::size_t n = buf.count;

        const double fast     = indicators::ema(c, n,     cfg.ema_fast);
        const double slow     = indicators::ema(c, n,     cfg.ema_slow);
        const double trend    = indicators::ema(c, n,     cfg.ema_trend);
        const double fast_1   = indicators::ema(c, n - 1, cfg.ema_fast);
        const double slow_1   = indicators::ema(c, n - 1, cfg.ema_slow);
        const double rsi_v    = indicators::rsi(c, n,     cfg.rsi_period);

        // Volume confirmation
        const std::size_t vol_start = n >= 20 ? n - 20 : 0;
        double avg_vol = 0.0;
        for (std::size_t i = vol_start; i < n; ++i) avg_vol += buf.volumes[i];
        avg_vol /= static_cast<double>(n - vol_start);
        const double curr_vol = buf.volumes[n - 1];
        if (curr_vol < avg_vol * cfg.volume_confirm_ratio) return std::nullopt;

        const double last = c[n - 1];
        const bool bull_cross = (fast_1 <= slow_1) && (fast > slow) && (last > trend);
        const bool bear_cross = (fast_1 >= slow_1) && (fast < slow) && (last < trend);

        auto make_sig = [&](Side side) -> Signal {
            const double ema_spread = std::abs(fast - slow) / last * 10'000.0;
            Signal s{};
            s.symbol           = sym;
            s.type             = SignalType::MOMENTUM;
            s.side             = side;
            s.confidence       = 0.65f;
            s.expected_edge_bps = static_cast<float>(std::min(ema_spread * 0.75 + 5.0, 90.0));
            s.ts_ns            = now_ns();
            s.sub_signals      = 0x01;  // bit 0
            return s;
        };

        if (bull_cross && rsi_v > cfg.rsi_oversold && rsi_v < cfg.rsi_overbought)
            return make_sig(Side::BUY);
        if (bear_cross && rsi_v > cfg.rsi_oversold && rsi_v < cfg.rsi_overbought)
            return make_sig(Side::SELL);
        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// 2. MeanReversionStrategy — Bollinger + VWAP zscore
// ---------------------------------------------------------------------------
struct MeanReversionStrategy : StrategyMixin<MeanReversionStrategy> {
    const StrategySettings& cfg;
    explicit MeanReversionStrategy(const StrategySettings& c) : cfg(c) {}

    const char* name_impl() const noexcept { return "MeanReversion"; }

    template<std::size_t N = 256>
    [[nodiscard]] std::optional<Signal> evaluate(
        const Symbol& sym, const IndicatorBuf<N>& buf) const noexcept
    {
        if (static_cast<int>(buf.count) < cfg.bb_period + 5) return std::nullopt;

        const double* c  = buf.closes.data();
        const std::size_t n = buf.count;
        const double last   = c[n - 1];

        const auto bb = indicators::bollinger(c, n, cfg.bb_period, cfg.bb_std);
        if (!bb) return std::nullopt;

        const double zs       = indicators::zscore(c, n, cfg.bb_period);
        const double rsi_v    = indicators::rsi(c, n, cfg.rsi_period);
        const double vwap_v   = indicators::vwap(buf.raw_candles.data(),
                                                  std::min(n, std::size_t{48}));
        const double vwap_dev = vwap_v > 0.0 ? ((last - vwap_v) / vwap_v) * 10'000.0 : 0.0;

        auto make_sig = [&](Side side, double reversion_bps) -> Signal {
            Signal s{};
            s.symbol            = sym;
            s.type              = SignalType::MEAN_REVERSION;
            s.side              = side;
            s.confidence        = 0.60f;
            s.expected_edge_bps = static_cast<float>(std::min(reversion_bps * 0.80 + 4.0, 110.0));
            s.ts_ns             = now_ns();
            s.sub_signals       = 0x02;  // bit 1
            return s;
        };

        // LONG entry
        if (last < bb->lower
            && zs < -cfg.bb_entry_zscore
            && rsi_v < 40.0
            && vwap_dev < -cfg.vwap_entry_bps)
        {
            const double rev = ((bb->mid - last) / last) * 10'000.0;
            return make_sig(Side::BUY, rev);
        }

        // SHORT entry
        if (last > bb->upper
            && zs > cfg.bb_entry_zscore
            && rsi_v > 60.0
            && vwap_dev > cfg.vwap_entry_bps)
        {
            const double rev = ((last - bb->mid) / last) * 10'000.0;
            return make_sig(Side::SELL, rev);
        }

        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// 3. OrderFlowStrategy — L2 imbalance from order book
// ---------------------------------------------------------------------------
struct OrderFlowStrategy : StrategyMixin<OrderFlowStrategy> {
    const StrategySettings& cfg;
    const FeeSettings&      fee;
    explicit OrderFlowStrategy(const StrategySettings& s, const FeeSettings& f)
        : cfg(s), fee(f) {}

    const char* name_impl() const noexcept { return "OrderFlow"; }

    [[nodiscard]] std::optional<Signal> evaluate(
        const Symbol& sym, const OrderBook& book) const noexcept
    {
        if (book.spread_bps() > fee.max_spread_bps) return std::nullopt;
        const double ofi = book.order_flow_imbalance(10);
        if (std::abs(ofi) < cfg.ofi_threshold) return std::nullopt;

        Signal s{};
        s.symbol            = sym;
        s.type              = SignalType::ORDER_FLOW;
        s.side              = ofi > 0.0 ? Side::BUY : Side::SELL;
        s.confidence        = static_cast<float>(std::min(std::abs(ofi), 0.95));
        s.expected_edge_bps = static_cast<float>(std::min(std::abs(ofi) * 20.0 + 4.0, 90.0));
        s.ts_ns             = now_ns();
        s.sub_signals       = 0x04;  // bit 2
        return s;
    }
};

// ---------------------------------------------------------------------------
// 4. VwapReversionStrategy — Chimera-style VWAP deviation + book confirmation
// ---------------------------------------------------------------------------
struct VwapReversionStrategy : StrategyMixin<VwapReversionStrategy> {
    const StrategySettings& cfg;
    const FeeSettings&      fee;
    explicit VwapReversionStrategy(const StrategySettings& c, const FeeSettings& f)
        : cfg(c), fee(f) {}

    const char* name_impl() const noexcept { return "VwapReversion"; }

    template<std::size_t N = 256>
    [[nodiscard]] std::optional<Signal> evaluate(
        const Symbol& sym,
        const IndicatorBuf<N>& buf,
        const OrderBook& book) const noexcept
    {
        if (static_cast<int>(buf.count) < 30) return std::nullopt;
        if (book.n_bids == 0 || book.n_asks == 0) return std::nullopt;
        if (book.spread_bps() > std::min(fee.max_spread_bps, 2.5)) return std::nullopt;

        const std::size_t n = buf.count;
        const double last = buf.closes[n - 1];
        const double session_vwap = indicators::vwap(
            buf.raw_candles.data(),
            std::min(n, std::size_t{48}));
        if (session_vwap <= 0.0) return std::nullopt;

        const double deviation_bps = ((last - session_vwap) / session_vwap) * 10'000.0;
        const double imbalance = book.order_flow_imbalance(10);

        auto make_sig = [&](Side side, double deviation_abs_bps) -> Signal {
            const double edge = deviation_abs_bps * 1.00 + std::abs(imbalance) * 16.0;
            const double confidence =
                std::clamp((deviation_abs_bps / 40.0) + std::abs(imbalance) * 0.35, 0.58, 0.92);
            Signal s{};
            s.symbol            = sym;
            s.type              = SignalType::VWAP_REVERSION;
            s.side              = side;
            s.confidence        = static_cast<float>(confidence);
            s.expected_edge_bps = static_cast<float>(std::min(edge, 110.0));
            s.ts_ns             = now_ns();
            s.sub_signals       = 0x08;  // bit 3
            return s;
        };

        if (deviation_bps <= -cfg.vwap_entry_bps
            && deviation_bps >= -80.0
            && imbalance >= 0.10)
        {
            return make_sig(Side::BUY, std::abs(deviation_bps));
        }

        if (deviation_bps >= cfg.vwap_entry_bps
            && deviation_bps <= 80.0
            && imbalance <= -0.10)
        {
            return make_sig(Side::SELL, std::abs(deviation_bps));
        }

        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// SignalComposer — aggregates all four strategies into one composite Signal
// All strategy instances are value members (stack-allocated, no heap/virtual)
// ---------------------------------------------------------------------------
class SignalComposer {
public:
    explicit SignalComposer(const Settings& s)
        : cfg_(s.strategy)
        , fee_(s.fee)
        , research_(s.research)
        , momentum_(s.strategy)
        , mean_rev_(s.strategy)
        , ofi_(s.strategy, s.fee)
        , vwap_rev_(s.strategy, s.fee)
    {}

    // Strategy weights (sum = 1.0)
    static constexpr float W_MOMENTUM = 0.30f;
    static constexpr float W_MEANREV  = 0.25f;
    static constexpr float W_OFI      = 0.20f;
    static constexpr float W_VWAP     = 0.25f;

    [[nodiscard]] std::optional<Signal> generate(
        const Symbol&     sym,
        const SymbolState& state,
        bool              research_mode = false) const noexcept
    {
        // Extract candle data onto stack
        IndicatorBuf<256> buf{};
        buf.load(state.candles);
        if (buf.count < 30) return std::nullopt;   // insufficient history

        const OrderBook book = state.book.load();

        // Run strategies
        std::optional<Signal> sigs[4];
        sigs[0] = momentum_.evaluate(sym, buf);
        sigs[1] = mean_rev_.evaluate(sym, buf);
        sigs[2] = ofi_.evaluate(sym, book);
        sigs[3] = vwap_rev_.evaluate(sym, buf, book);

        // Vote counting
        int buy_votes  = 0, sell_votes = 0;
        float buy_w    = 0.0f, sell_w = 0.0f;
        float buy_edge = 0.0f, sell_edge = 0.0f;
        const float weights[4] = {W_MOMENTUM, W_MEANREV, W_OFI, W_VWAP};

        for (int i = 0; i < 4; ++i) {
            if (!sigs[i]) continue;
            const float w = weights[i];
            if (sigs[i]->side == Side::BUY) {
                buy_votes++;
                buy_w    += w;
                buy_edge += sigs[i]->expected_edge_bps * w;
            } else {
                sell_votes++;
                sell_w    += w;
                sell_edge += sigs[i]->expected_edge_bps * w;
            }
        }

        if (buy_votes >= 2 || sell_votes >= 2) {
            const bool is_buy   = (buy_votes >= 2 && buy_votes > sell_votes);
            const int  votes    = is_buy ? buy_votes  : sell_votes;
            const float w_total = is_buy ? buy_w      : sell_w;
            const float raw_edge= is_buy ? (w_total > 0.0f ? buy_edge  / w_total : 0.0f)
                                         : (w_total > 0.0f ? sell_edge / w_total : 0.0f);

            // Alignment bonus: +2 bps per extra confirming strategy beyond first
            const float edge = raw_edge + static_cast<float>((votes - 1) * 2);

            // Build composite signal
            Signal composite{};
            composite.symbol            = sym;
            composite.type              = SignalType::COMPOSITE;
            composite.side              = is_buy ? Side::BUY : Side::SELL;
            composite.confidence        = std::min(w_total, 1.0f);
            composite.expected_edge_bps = std::min(edge, 150.0f);
            composite.ts_ns             = now_ns();
            composite.n_aligned         = static_cast<std::uint8_t>(votes);
            composite.sub_signals       = 0;
            for (int i = 0; i < 4; ++i)
                if (sigs[i] && sigs[i]->side == composite.side)
                    composite.sub_signals |= sigs[i]->sub_signals;

            return composite;
        }

        if (!research_mode || !research_.enable_shadow_relaxation) return std::nullopt;

        const Signal* best = nullptr;
        float best_score = 0.0f;
        int best_same_side = 0;
        int best_opp_side = 0;

        for (int i = 0; i < 4; ++i) {
            if (!sigs[i]) continue;
            const float score = sigs[i]->confidence * sigs[i]->expected_edge_bps;
            if (score <= best_score) continue;

            int same_side = 0;
            int opp_side = 0;
            for (const auto& candidate : sigs) {
                if (!candidate) continue;
                if (candidate->side == sigs[i]->side) ++same_side;
                else ++opp_side;
            }

            best = &*sigs[i];
            best_score = score;
            best_same_side = same_side;
            best_opp_side = opp_side;
        }

        if (!best) return std::nullopt;
        if (best_same_side <= best_opp_side) return std::nullopt;
        if (best->confidence < research_.min_single_signal_confidence) return std::nullopt;
        if (best->expected_edge_bps < research_.min_single_signal_edge_bps) return std::nullopt;

        Signal promoted = *best;
        promoted.n_aligned = static_cast<std::uint8_t>(best_same_side);
        return promoted;
    }

private:
    const StrategySettings& cfg_;
    const FeeSettings&      fee_;
    const ResearchSettings& research_;

    // Value members — no heap, no virtual
    MomentumStrategy     momentum_;
    MeanReversionStrategy mean_rev_;
    OrderFlowStrategy    ofi_;
    VwapReversionStrategy vwap_rev_;
};

} // namespace bot
