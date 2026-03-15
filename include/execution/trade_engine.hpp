#pragma once
// =============================================================================
// trade_engine.hpp — Trade lifecycle engine
//
// Thread model:
//   Thread A (Feed):    WS messages → parse → push to market_events_ ring buffer
//   Thread B (Signal):  drain market_events_ → update MarketState → run signals
//                       → push to signal_queue_ ring buffer
//   Thread C (Exec):    drain signal_queue_ → fee gate → risk → gateway
//                       → open trades map (std::array, no heap)
//
// Communication between threads: SpscRingBuffer (zero mutex).
// Portfolio state: AtomicPortfolio (seqlock — zero mutex).
// Kill switch: atomic<bool> in RiskManager.
// Open trades: fixed std::array<Trade, MAX_TRADES> + atomic count.
// =============================================================================

#include "core/models.hpp"
#include "core/market_state.hpp"
#include "core/spsc_ring_buffer.hpp"
#include "core/fee_gate.hpp"
#include "core/config_parser.hpp"
#include "strategies/strategies.hpp"
#include "risk/risk_manager.hpp"
#include "execution/gateway.hpp"

#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>

namespace bot {

// ---------------------------------------------------------------------------
// Open trade slot — fixed array, no dynamic allocation
// ---------------------------------------------------------------------------
struct alignas(CACHE_LINE_SIZE) TradeSlot {
    Trade          trade{};
    std::atomic<bool> active{false};
};

// ---------------------------------------------------------------------------
// Statistics counters — all relaxed atomics
// ---------------------------------------------------------------------------
struct alignas(CACHE_LINE_SIZE) EngineStats {
    std::atomic<std::uint64_t> signals_generated{0};
    std::atomic<std::uint64_t> signals_blocked_fee{0};
    std::atomic<std::uint64_t> signals_blocked_risk{0};
    std::atomic<std::uint64_t> trades_entered{0};
    std::atomic<std::uint64_t> trades_closed_stop{0};
    std::atomic<std::uint64_t> trades_closed_tp{0};
    std::atomic<std::uint64_t> trades_closed_signal{0};
    std::atomic<std::uint64_t> shadow_fills{0};
    std::atomic<std::uint64_t> live_fills{0};
    std::atomic<std::int64_t>  total_pnl_bps_x1000{0};  // bps * 1000 for precision
};

// ---------------------------------------------------------------------------
// TradeEngine — single class, templated on gateway type
// ---------------------------------------------------------------------------
template<typename GatewayT>
class TradeEngine {
public:
    static constexpr std::size_t MAX_TRADES = 32;
    static constexpr std::size_t SIG_QUEUE_SIZE = 512;

    TradeEngine(
        MarketState&   state,
        GatewayT&      gateway,
        SignalComposer& composer,
        FeeGate&        fee_gate,
        RiskManager&    risk,
        AtomicPortfolio& portfolio,
        const Settings& cfg)
        : state_(state)
        , gateway_(gateway)
        , composer_(composer)
        , fee_gate_(fee_gate)
        , risk_(risk)
        , portfolio_(portfolio)
        , cfg_(cfg)
    {
        // TradeSlot contains atomic — init via placement new default construction
        for (auto& s : trades_) { s.trade = Trade{}; s.active.store(false, std::memory_order_relaxed); }
    }

    // -------------------------------------------------------------------------
    // Called by signal thread when a candle closes for a symbol
    // -------------------------------------------------------------------------
    void on_closed_candle(int sym_idx) noexcept {
        if (BOT_UNLIKELY(risk_.is_halted())) return;

        SymbolState* ss = state_.get(sym_idx);
        if (!ss) return;

        const auto sig_opt = composer_.generate(ss->symbol, *ss);
        if (!sig_opt) return;

        stats_.signals_generated.fetch_add(1, std::memory_order_relaxed);

        // Push to exec queue — non-blocking; if full, drop (exec thread busy)
        if (!sig_queue_.push(*sig_opt)) {
            // Queue full — rare; just drop the signal
        }
    }

    // -------------------------------------------------------------------------
    // Called by exec thread — processes one batch of pending signals
    // -------------------------------------------------------------------------
    void process_signals() noexcept {
        sig_queue_.drain([this](const Signal& sig) {
            handle_signal(sig);
        });
    }

    // -------------------------------------------------------------------------
    // Called by exec thread — check all open trades for stops/TP
    // -------------------------------------------------------------------------
    void monitor_open_trades() noexcept {
        for (std::size_t i = 0; i < MAX_TRADES; ++i) {
            auto& slot = trades_[i];
            if (!slot.active.load(std::memory_order_acquire)) continue;

            Trade& t = slot.trade;
            if (t.state != TradeState::OPEN) continue;

            const SymbolState* ss = state_.get(t.signal.symbol.view());
            if (!ss) continue;

            const Ticker ticker = ss->ticker.load();
            const double price  = ticker.mid();
            if (price <= 0.0) continue;

            // Update trailing stop
            t.trailing_stop = risk_.update_trailing_stop(
                price, t.entry_price, t.trailing_stop, t.side);
            t.stop_price = t.trailing_stop;

            // Check stop/TP
            if (risk_.should_stop_out(price, t.stop_price, t.take_profit_price, t.side)) {
                close_trade(i, price, "stop_hit");
            }
        }
    }

    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }

    [[nodiscard]] int open_trade_count() const noexcept {
        return open_count_.load(std::memory_order_relaxed);
    }

    void print_stats() const noexcept {
        printf("\n=== Engine Stats ===\n");
        printf("  Signals generated : %llu\n",
               (unsigned long long)stats_.signals_generated.load(std::memory_order_relaxed));
        printf("  Blocked (fee gate): %llu\n",
               (unsigned long long)stats_.signals_blocked_fee.load(std::memory_order_relaxed));
        printf("  Blocked (risk)    : %llu\n",
               (unsigned long long)stats_.signals_blocked_risk.load(std::memory_order_relaxed));
        printf("  Trades entered    : %llu\n",
               (unsigned long long)stats_.trades_entered.load(std::memory_order_relaxed));
        printf("  Trades closed     : %llu (stop) + %llu (tp)\n",
               (unsigned long long)stats_.trades_closed_stop.load(std::memory_order_relaxed),
               (unsigned long long)stats_.trades_closed_tp.load(std::memory_order_relaxed));
        printf("  Open now          : %d\n", open_trade_count());
        printf("  Gateway avg lat   : %.0f µs\n", gateway_.latency().avg_us());
        printf("  Gateway peak lat  : %llu µs\n",
               (unsigned long long)gateway_.latency().peak_us());
        printf("  Mode              : %s\n", gateway_.is_shadow() ? "SHADOW" : "LIVE");
        const double total_bps = static_cast<double>(
            stats_.total_pnl_bps_x1000.load(std::memory_order_relaxed)) / 1000.0;
        printf("  Total P&L         : %.2f bps\n", total_bps);
        printf("  Fee gate pass %%   : %.1f%%\n", fee_gate_.pass_rate());
    }

private:
    MarketState&     state_;
    GatewayT&        gateway_;
    SignalComposer&  composer_;
    FeeGate&         fee_gate_;
    RiskManager&     risk_;
    AtomicPortfolio& portfolio_;
    const Settings&  cfg_;

    // Signal SPSC queue — feed threads produce, exec thread consumes
    SpscRingBuffer<Signal, SIG_QUEUE_SIZE> sig_queue_;

    // Open trade slots — fixed array, no heap
    std::array<TradeSlot, MAX_TRADES> trades_;
    alignas(CACHE_LINE_SIZE) std::atomic<int> open_count_{0};

    EngineStats stats_;
    static inline std::atomic<std::uint64_t> trade_id_counter_{1};

    // -------------------------------------------------------------------------

    void handle_signal(const Signal& sig) noexcept {
        const PortfolioSnapshot port = portfolio_.load();

        // Already have a trade for this symbol?
        if (find_open_trade(sig.symbol.view()) >= 0) return;

        // Get market data
        SymbolState* ss = state_.get(sig.symbol.view());
        if (!ss) return;

        const Ticker     ticker = ss->ticker.load();
        const OrderBook  book   = ss->book.load();
        const double     sym_exp = ss->get_position_usd();

        // Fee gate
        const FeeGateResult fg = fee_gate_.evaluate(sig, &ticker, &book);
        if (!fg.is_viable) {
            stats_.signals_blocked_fee.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Signal rate limit check
        const std::int64_t last_sig = ss->last_signal_ns.load(std::memory_order_acquire);

        // Risk gate
        const RiskDecision rd = risk_.evaluate(sig, port, sym_exp, last_sig);
        if (!rd.allowed) {
            stats_.signals_blocked_risk.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Find free trade slot
        const int slot_idx = find_free_slot();
        if (slot_idx < 0) return;   // all slots full

        // Build order
        const double price = (sig.side == Side::BUY) ? ticker.ask : ticker.bid;
        const double qty   = rd.position_usd / price;

        if (price <= 0.0 || qty <= 0.0) return;

        OrderRequest req{};
        req.symbol    = sig.symbol;
        req.side      = sig.side;
        req.type      = cfg_.exec.use_post_only
                        ? OrdType::LIMIT_MAKER
                        : OrdType::LIMIT;
        req.qty       = qty;
        req.price     = price;
        req.post_only = cfg_.exec.use_post_only;
        req.client_id = trade_id_counter_.load(std::memory_order_relaxed);

        // Place order
        const PlaceResult pr = gateway_.place(req);
        if (!pr.ok || pr.result.filled_qty <= 0.0) return;

        // Record trade
        auto& slot       = trades_[slot_idx];
        Trade& t         = slot.trade;
        t.id             = trade_id_counter_.fetch_add(1, std::memory_order_relaxed);
        t.symbol         = sig.symbol;
        t.state          = TradeState::OPEN;
        t.side           = sig.side;
        t.entry_price    = pr.result.avg_price;
        t.qty            = pr.result.filled_qty;
        t.entry_fee      = pr.result.fee_paid;
        t.opened_ns      = now_ns();
        t.signal         = sig;

        const auto [stop, tp] = risk_.compute_stops(t.entry_price, t.side);
        t.stop_price          = stop;
        t.take_profit_price   = tp;
        t.trailing_stop       = stop;

        slot.active.store(true, std::memory_order_release);
        open_count_.fetch_add(1, std::memory_order_relaxed);
        stats_.trades_entered.fetch_add(1, std::memory_order_relaxed);
        ss->last_signal_ns.store(now_ns(), std::memory_order_release);

        // Update portfolio exposure
        ss->set_position_usd(sym_exp + (t.entry_price * t.qty));

        // Update portfolio snapshot
        PortfolioSnapshot np = port;
        np.total_exposure_usd += t.entry_price * t.qty;
        np.available_usd      -= t.entry_price * t.qty + t.entry_fee;
        np.open_trade_count    = open_count_.load(std::memory_order_relaxed);
        portfolio_.store(np);

        if (gateway_.is_shadow())
            stats_.shadow_fills.fetch_add(1, std::memory_order_relaxed);
        else
            stats_.live_fills.fetch_add(1, std::memory_order_relaxed);
    }

    void close_trade(std::size_t slot_idx, double exit_price, const char* reason) noexcept {
        auto& slot = trades_[slot_idx];
        Trade& t   = slot.trade;
        if (!slot.active.load(std::memory_order_acquire)) return;
        if (t.state != TradeState::OPEN) return;

        t.state = TradeState::EXITING;

        OrderRequest req{};
        req.symbol    = t.symbol;
        req.side      = opposite(t.side);
        req.type      = OrdType::MARKET;    // exit is always market for certainty
        req.qty       = t.qty;
        req.price     = exit_price;
        req.post_only = false;
        req.client_id = trade_id_counter_.load(std::memory_order_relaxed);

        const PlaceResult pr = gateway_.place(req);
        const double actual_exit = pr.ok ? pr.result.avg_price : exit_price;
        t.exit_price  = actual_exit;
        t.exit_fee    = pr.ok ? pr.result.fee_paid : 0.0;
        t.closed_ns   = now_ns();
        t.state       = TradeState::CLOSED;

        // Update stats
        const double pnl_bps = t.pnl_bps();
        const auto pnl_bps_int = static_cast<std::int64_t>(pnl_bps * 1000.0);
        stats_.total_pnl_bps_x1000.fetch_add(pnl_bps_int, std::memory_order_relaxed);

        if (std::strcmp(reason, "stop_hit") == 0)
            stats_.trades_closed_stop.fetch_add(1, std::memory_order_relaxed);
        else
            stats_.trades_closed_tp.fetch_add(1, std::memory_order_relaxed);

        // Update portfolio
        const PortfolioSnapshot old_port = portfolio_.load();
        PortfolioSnapshot np = old_port;
        const double notional_entry = t.entry_price * t.qty;
        np.total_exposure_usd = std::max(0.0, np.total_exposure_usd - notional_entry);
        np.total_pnl_usd     += t.pnl_usd();
        np.daily_pnl_usd     += t.pnl_usd();
        np.equity_usd        += t.pnl_usd();
        if (np.equity_usd > np.peak_equity_usd)
            np.peak_equity_usd = np.equity_usd;
        np.open_trade_count = std::max(0, np.open_trade_count - 1);
        portfolio_.store(np);

        // Clear slot
        SymbolState* ss = state_.get(t.symbol.view());
        if (ss) ss->set_position_usd(
            std::max(0.0, ss->get_position_usd() - notional_entry));

        slot.active.store(false, std::memory_order_release);
        open_count_.fetch_sub(1, std::memory_order_relaxed);
    }

    [[nodiscard]] int find_free_slot() const noexcept {
        for (std::size_t i = 0; i < MAX_TRADES; ++i)
            if (!trades_[i].active.load(std::memory_order_acquire))
                return static_cast<int>(i);
        return -1;
    }

    [[nodiscard]] int find_open_trade(std::string_view sym) const noexcept {
        for (std::size_t i = 0; i < MAX_TRADES; ++i) {
            if (!trades_[i].active.load(std::memory_order_acquire)) continue;
            if (trades_[i].trade.symbol.view() == sym) return static_cast<int>(i);
        }
        return -1;
    }
};

} // namespace bot
