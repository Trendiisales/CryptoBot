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
#include "execution/trade_journal.hpp"

#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>

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
    std::atomic<std::uint64_t> signals_promoted_research{0};
    std::atomic<std::uint64_t> blocked_fee_spread{0};
    std::atomic<std::uint64_t> blocked_fee_edge{0};
    std::atomic<std::uint64_t> blocked_risk_rate_limit{0};
    std::atomic<std::uint64_t> blocked_risk_size{0};
    std::atomic<std::uint64_t> blocked_risk_exposure{0};
    std::atomic<std::uint64_t> blocked_gateway{0};
    std::atomic<std::uint64_t> trades_entered{0};
    std::atomic<std::uint64_t> trades_closed_stop{0};
    std::atomic<std::uint64_t> trades_closed_tp{0};
    std::atomic<std::uint64_t> trades_closed_signal{0};
    std::atomic<std::uint64_t> shadow_fills{0};
    std::atomic<std::uint64_t> live_fills{0};
    std::atomic<std::int64_t>  total_pnl_bps_x1000{0};  // bps * 1000 for precision
};

struct ClosedTradeRecord {
    Symbol      symbol{};
    SignalType  signal_type{SignalType::NONE};
    double      pnl_bps{0.0};
    double      entry_price{0.0};
    double      exit_price{0.0};
    std::int64_t closed_epoch_ms{0};
    std::int64_t hold_ms{0};
    char        reason[16]{};
};

template<std::size_t CAPACITY>
struct ClosedTradeLogSnapshot {
    std::array<ClosedTradeRecord, CAPACITY> entries{};
    std::size_t count{0};
};

struct alignas(CACHE_LINE_SIZE) PerformanceStats {
    std::atomic<std::uint64_t> closed_trades{0};
    std::atomic<std::uint64_t> winning_trades{0};
    std::atomic<std::uint64_t> losing_trades{0};
    std::atomic<std::int64_t>  gross_profit_usd_x1000{0};
    std::atomic<std::int64_t>  gross_loss_usd_x1000{0};
    std::atomic<std::int64_t>  total_hold_ms{0};
    std::atomic<std::int64_t>  last_close_epoch_ms{0};
};

// ---------------------------------------------------------------------------
// TradeEngine — single class, templated on gateway type
// ---------------------------------------------------------------------------
template<typename GatewayT>
class TradeEngine {
public:
    static constexpr std::size_t MAX_TRADES = 32;
    static constexpr std::size_t SIG_QUEUE_SIZE = 512;
    static constexpr std::size_t CLOSED_TRADE_LOG_CAP = 64;
    using TradeLogSnapshot = ClosedTradeLogSnapshot<CLOSED_TRADE_LOG_CAP>;

    TradeEngine(
        MarketState&   state,
        GatewayT&      gateway,
        SignalComposer& composer,
        FeeGate&        fee_gate,
        RiskManager&    risk,
        AtomicPortfolio& portfolio,
        TradeJournal&    trade_journal,
        const Settings& cfg)
        : state_(state)
        , gateway_(gateway)
        , composer_(composer)
        , fee_gate_(fee_gate)
        , risk_(risk)
        , portfolio_(portfolio)
        , trade_journal_(trade_journal)
        , cfg_(cfg)
    {
        // TradeSlot contains atomic — init via placement new default construction
        for (auto& s : trades_) { s.trade = Trade{}; s.active.store(false, std::memory_order_relaxed); }
        for (auto& ts : last_signal_eval_epoch_ms_) {
            ts.store(0, std::memory_order_relaxed);
        }
    }

    // -------------------------------------------------------------------------
    // Called by signal thread when a candle closes for a symbol
    // -------------------------------------------------------------------------
    void on_closed_candle(int sym_idx) noexcept {
        evaluate_symbol(sym_idx, true);
    }

    void on_market_tick(int sym_idx) noexcept {
        evaluate_symbol(sym_idx, false);
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

    [[nodiscard]] double gateway_latency_avg_us() const noexcept {
        return gateway_.latency().avg_us();
    }

    [[nodiscard]] double fee_gate_pass_rate_ratio() const noexcept {
        return fee_gate_.pass_rate() / 100.0;
    }

    [[nodiscard]] double win_rate_ratio() const noexcept {
        const double total = static_cast<double>(
            performance_.closed_trades.load(std::memory_order_relaxed));
        if (total <= 0.0) return 0.0;
        return static_cast<double>(
            performance_.winning_trades.load(std::memory_order_relaxed)) / total;
    }

    [[nodiscard]] double profit_factor() const noexcept {
        const double gross_profit = static_cast<double>(
            performance_.gross_profit_usd_x1000.load(std::memory_order_relaxed)) / 1000.0;
        const double gross_loss = static_cast<double>(
            performance_.gross_loss_usd_x1000.load(std::memory_order_relaxed)) / 1000.0;
        if (gross_loss <= 0.0) return gross_profit > 0.0 ? 999.0 : 0.0;
        return gross_profit / gross_loss;
    }

    [[nodiscard]] double avg_trade_pnl_bps() const noexcept {
        const double total = static_cast<double>(
            performance_.closed_trades.load(std::memory_order_relaxed));
        if (total <= 0.0) return 0.0;
        return static_cast<double>(
            stats_.total_pnl_bps_x1000.load(std::memory_order_relaxed)) / 1000.0 / total;
    }

    [[nodiscard]] double avg_hold_ms() const noexcept {
        const double total = static_cast<double>(
            performance_.closed_trades.load(std::memory_order_relaxed));
        if (total <= 0.0) return 0.0;
        return static_cast<double>(
            performance_.total_hold_ms.load(std::memory_order_relaxed)) / total;
    }

    [[nodiscard]] std::int64_t last_close_epoch_ms() const noexcept {
        return performance_.last_close_epoch_ms.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool shadow_research_mode_active() const noexcept {
        if (!cfg_.is_shadow_mode || !cfg_.research.enable_shadow_relaxation) return false;
        const std::int64_t idle_ms =
            static_cast<std::int64_t>(cfg_.research.idle_seconds_before_relaxation) * 1000;
        const std::int64_t last_close = last_close_epoch_ms();
        const std::int64_t anchor = last_close > 0 ? last_close : start_epoch_ms_;
        return epoch_ms() - anchor >= idle_ms;
    }

    [[nodiscard]] std::uint64_t total_trade_count() const noexcept {
        return stats_.trades_entered.load(std::memory_order_relaxed);
    }

    [[nodiscard]] TradeLogSnapshot closed_trade_log() const noexcept {
        return trade_log_.load();
    }

    void print_stats() const noexcept {
        printf("\n=== Engine Stats ===\n");
        printf("  Signals generated : %llu\n",
               (unsigned long long)stats_.signals_generated.load(std::memory_order_relaxed));
        printf("  Blocked (fee gate): %llu\n",
               (unsigned long long)stats_.signals_blocked_fee.load(std::memory_order_relaxed));
        printf("  Blocked (risk)    : %llu\n",
               (unsigned long long)stats_.signals_blocked_risk.load(std::memory_order_relaxed));
        printf("  Blocked (gateway) : %llu\n",
               (unsigned long long)stats_.blocked_gateway.load(std::memory_order_relaxed));
        printf("  Research promote  : %llu\n",
               (unsigned long long)stats_.signals_promoted_research.load(std::memory_order_relaxed));
        printf("  Trades entered    : %llu\n",
               (unsigned long long)stats_.trades_entered.load(std::memory_order_relaxed));
        printf("  Trades closed     : %llu (stop) + %llu (tp) + %llu (signal)\n",
               (unsigned long long)stats_.trades_closed_stop.load(std::memory_order_relaxed),
               (unsigned long long)stats_.trades_closed_tp.load(std::memory_order_relaxed),
               (unsigned long long)stats_.trades_closed_signal.load(std::memory_order_relaxed));
        printf("  Win rate          : %.1f%% | PF=%.2f | Avg=%.2f bp\n",
               win_rate_ratio() * 100.0,
               profit_factor(),
               avg_trade_pnl_bps());
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
    void evaluate_symbol(int sym_idx, bool force) noexcept {
        if (BOT_UNLIKELY(risk_.is_halted())) return;

        SymbolState* ss = state_.get(sym_idx);
        if (!ss) return;

        const std::int64_t now_epoch_ms = epoch_ms();
        if (!force) {
            const std::int64_t last_eval = last_signal_eval_epoch_ms_[sym_idx].load(std::memory_order_relaxed);
            if (now_epoch_ms - last_eval < 1000) return;
        }
        last_signal_eval_epoch_ms_[sym_idx].store(now_epoch_ms, std::memory_order_relaxed);

        const bool research_mode = shadow_research_mode_active();
        const auto sig_opt = composer_.generate(ss->symbol, *ss, research_mode);
        if (!sig_opt) {
            ss->last_signal_type.store(
                static_cast<std::uint8_t>(SignalType::NONE),
                std::memory_order_release);
            return;
        }

        ss->last_signal_type.store(
            static_cast<std::uint8_t>(sig_opt->type),
            std::memory_order_release);

        stats_.signals_generated.fetch_add(1, std::memory_order_relaxed);
        if (research_mode && sig_opt->type != SignalType::COMPOSITE) {
            stats_.signals_promoted_research.fetch_add(1, std::memory_order_relaxed);
        }

        // Push to exec queue — non-blocking; if full, drop (exec thread busy)
        if (!sig_queue_.push(*sig_opt)) {
            // Queue full — rare; just drop the signal
        }
    }
    MarketState&     state_;
    GatewayT&        gateway_;
    SignalComposer&  composer_;
    FeeGate&         fee_gate_;
    RiskManager&     risk_;
    AtomicPortfolio& portfolio_;
    TradeJournal&    trade_journal_;
    const Settings&  cfg_;
    const std::int64_t start_epoch_ms_{epoch_ms()};

    // Signal SPSC queue — feed threads produce, exec thread consumes
    SpscRingBuffer<Signal, SIG_QUEUE_SIZE> sig_queue_;
    std::array<std::atomic<std::int64_t>, MarketState::MAX_SYMBOLS> last_signal_eval_epoch_ms_{};

    // Open trade slots — fixed array, no heap
    std::array<TradeSlot, MAX_TRADES> trades_;
    alignas(CACHE_LINE_SIZE) std::atomic<int> open_count_{0};

    EngineStats stats_;
    PerformanceStats performance_;
    SeqLocked<TradeLogSnapshot> trade_log_{};
    static inline std::atomic<std::uint64_t> trade_id_counter_{1};

    // -------------------------------------------------------------------------

    void handle_signal(const Signal& sig) noexcept {
        const PortfolioSnapshot port = portfolio_.load();

        // Get market data
        SymbolState* ss = state_.get(sig.symbol.view());
        if (!ss) return;

        const Ticker     ticker = ss->ticker.load();
        const OrderBook  book   = ss->book.load();
        const double     sym_exp = ss->get_position_usd();
        const int        open_trade_idx = find_open_trade(sig.symbol.view());

        if (open_trade_idx >= 0) {
            const Trade& open_trade = trades_[open_trade_idx].trade;
            if (open_trade.side != sig.side)
            {
                const double exit_price = sig.side == Side::SELL
                    ? (ticker.bid > 0.0 ? ticker.bid : ticker.last)
                    : (ticker.ask > 0.0 ? ticker.ask : ticker.last);
                if (exit_price > 0.0) {
                    close_trade(static_cast<std::size_t>(open_trade_idx), exit_price, "signal_exit");
                }
            }
            return;
        }

        if (!cfg_.allow_short_entries
            && !shadow_shorting_enabled()
            && sig.side == Side::SELL) {
            return;
        }

        // Fee gate
        const FeeGateResult fg = fee_gate_.evaluate(sig, &ticker, &book);
        if (!fg.is_viable) {
            stats_.signals_blocked_fee.fetch_add(1, std::memory_order_relaxed);
            if (std::strcmp(fg.rejection_reason, "spread too wide") == 0) {
                stats_.blocked_fee_spread.fetch_add(1, std::memory_order_relaxed);
            } else {
                stats_.blocked_fee_edge.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }

        // Signal rate limit check
        const std::int64_t last_sig = ss->last_signal_ns.load(std::memory_order_acquire);

        // Risk gate
        const RiskDecision rd = risk_.evaluate(sig, port, sym_exp, last_sig);
        if (!rd.allowed) {
            stats_.signals_blocked_risk.fetch_add(1, std::memory_order_relaxed);
            if (std::strcmp(rd.reason, "signal rate limit") == 0) {
                stats_.blocked_risk_rate_limit.fetch_add(1, std::memory_order_relaxed);
            } else if (std::strcmp(rd.reason, "size too small") == 0) {
                stats_.blocked_risk_size.fetch_add(1, std::memory_order_relaxed);
            } else {
                stats_.blocked_risk_exposure.fetch_add(1, std::memory_order_relaxed);
            }
            if (risk_.is_halted()) {
                PortfolioSnapshot halted = port;
                halted.is_halted = true;
                portfolio_.store(halted);
            }
            return;
        }

        // Find free trade slot
        const int slot_idx = find_free_slot();
        if (slot_idx < 0) return;   // all slots full

        // Build order
        const double price = cfg_.exec.use_post_only
            ? ((sig.side == Side::BUY) ? ticker.bid : ticker.ask)
            : ((sig.side == Side::BUY) ? ticker.ask : ticker.bid);
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
        if (!pr.ok || pr.result.filled_qty <= 0.0) {
            stats_.blocked_gateway.fetch_add(1, std::memory_order_relaxed);
            return;
        }

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
        t.opened_epoch_ms = epoch_ms();
        t.entry_latency_us = pr.result.latency_us;
        t.entry_order_status = pr.result.status;
        t.closed_epoch_ms = 0;
        t.exit_latency_us = 0;
        t.exit_order_status = OrdStatus::PENDING;
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
        if (t.side == Side::BUY) {
            np.available_usd -= t.entry_price * t.qty + t.entry_fee;
        } else {
            np.available_usd += t.entry_price * t.qty - t.entry_fee;
        }
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
        t.closed_epoch_ms = epoch_ms();
        t.exit_latency_us = pr.ok ? pr.result.latency_us : 0;
        t.exit_order_status = pr.ok ? pr.result.status : OrdStatus::REJECTED;
        t.state       = TradeState::CLOSED;

        // Update stats
        const double pnl_bps = t.pnl_bps();
        const auto pnl_bps_int = static_cast<std::int64_t>(pnl_bps * 1000.0);
        stats_.total_pnl_bps_x1000.fetch_add(pnl_bps_int, std::memory_order_relaxed);

        // Update portfolio
        const PortfolioSnapshot old_port = portfolio_.load();
        PortfolioSnapshot np = old_port;
        const double notional_entry = t.entry_price * t.qty;
        const double notional_exit  = t.exit_price * t.qty;
        np.total_exposure_usd = std::max(0.0, np.total_exposure_usd - notional_entry);
        if (t.side == Side::BUY) {
            np.available_usd += notional_exit - t.exit_fee;
        } else {
            np.available_usd -= notional_exit + t.exit_fee;
        }
        np.total_pnl_usd     += t.pnl_usd();
        np.daily_pnl_usd     += t.pnl_usd();
        np.equity_usd        += t.pnl_usd();
        if (np.equity_usd > np.peak_equity_usd)
            np.peak_equity_usd = np.equity_usd;
        np.open_trade_count = std::max(0, np.open_trade_count - 1);
        np.is_halted = risk_.is_halted();
        portfolio_.store(np);

        append_closed_trade(t, reason);
        trade_journal_.record_closed_trade(t, np, reason, gateway_.is_shadow());
        performance_.closed_trades.fetch_add(1, std::memory_order_relaxed);
        performance_.total_hold_ms.fetch_add(
            static_cast<std::int64_t>(t.duration_sec() * 1000.0),
            std::memory_order_relaxed);
        performance_.last_close_epoch_ms.store(t.closed_epoch_ms, std::memory_order_relaxed);
        if (t.pnl_usd() >= 0.0) {
            performance_.winning_trades.fetch_add(1, std::memory_order_relaxed);
            performance_.gross_profit_usd_x1000.fetch_add(
                static_cast<std::int64_t>(t.pnl_usd() * 1000.0),
                std::memory_order_relaxed);
        } else {
            performance_.losing_trades.fetch_add(1, std::memory_order_relaxed);
            performance_.gross_loss_usd_x1000.fetch_add(
                static_cast<std::int64_t>(std::abs(t.pnl_usd()) * 1000.0),
                std::memory_order_relaxed);
        }

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

    void append_closed_trade(const Trade& trade, const char* reason) noexcept {
        TradeLogSnapshot snap = trade_log_.load();
        const std::size_t limit = std::min(snap.count, CLOSED_TRADE_LOG_CAP - 1);
        for (std::size_t i = limit; i > 0; --i) {
            snap.entries[i] = snap.entries[i - 1];
        }

        ClosedTradeRecord rec{};
        rec.symbol          = trade.symbol;
        rec.signal_type     = trade.signal.type;
        rec.pnl_bps         = trade.pnl_bps();
        rec.entry_price     = trade.entry_price;
        rec.exit_price      = trade.exit_price;
        rec.closed_epoch_ms = trade.closed_epoch_ms > 0 ? trade.closed_epoch_ms : epoch_ms();
        rec.hold_ms         = static_cast<std::int64_t>((trade.closed_ns - trade.opened_ns) / 1'000'000);

        const char* log_reason =
            std::strcmp(reason, "stop_hit") == 0    ? "SL"  :
            std::strcmp(reason, "signal_exit") == 0 ? "SIG" :
            std::strcmp(reason, "tp_hit") == 0      ? "TP"  :
                                                       reason;
        std::strncpy(rec.reason, log_reason, sizeof(rec.reason) - 1);

        snap.entries[0] = rec;
        snap.count = std::min<std::size_t>(snap.count + 1, CLOSED_TRADE_LOG_CAP);
        trade_log_.store(snap);

        if (std::strcmp(reason, "stop_hit") == 0) {
            stats_.trades_closed_stop.fetch_add(1, std::memory_order_relaxed);
        } else if (std::strcmp(reason, "signal_exit") == 0) {
            stats_.trades_closed_signal.fetch_add(1, std::memory_order_relaxed);
        } else {
            stats_.trades_closed_tp.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool shadow_shorting_enabled() const noexcept {
        return cfg_.is_shadow_mode && cfg_.shadow.allow_synthetic_shorts;
    }
};

} // namespace bot
