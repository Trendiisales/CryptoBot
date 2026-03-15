#pragma once
// =============================================================================
// gateway.hpp — CRTP order gateway: Live and Shadow implementations
//
// GatewayBase<Derived> defines the interface via CRTP.
// LiveGateway — signs and dispatches real REST orders (SPOT only).
// ShadowGateway — simulates fills from live market data, zero network calls.
//
// The gateway selected at startup via config shadow_mode = true/false.
// The trade manager calls gateway.place(req) — same call site for both.
// Zero virtual dispatch. The concrete type is known at compile time.
//
// HTTP: Uses POSIX sockets + HMAC-SHA256 signing.
// Dependencies: OpenSSL (libssl, libcrypto) for HMAC.
// =============================================================================

#include "core/common.hpp"
#include "core/models.hpp"
#include "core/config_parser.hpp"
#include "core/market_state.hpp"

#include <atomic>
#include <array>
#include <cstring>
#include <cmath>
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>

// ---------- OpenSSL HMAC (compile-time selected) ----------------------------
#ifdef BOT_USE_OPENSSL
#  include <openssl/hmac.h>
#  include <openssl/sha.h>
#endif

// ---------- Simple blocking HTTP via POSIX (Linux/macOS) --------------------
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <netinet/tcp.h>

namespace bot {

// ---------------------------------------------------------------------------
// Result of an order placement
// ---------------------------------------------------------------------------
struct PlaceResult {
    bool        ok{false};
    OrderResult result{};
    const char* error{""};
};

// ---------------------------------------------------------------------------
// Latency tracker — atomic running stats, no mutex
// ---------------------------------------------------------------------------
struct LatencyStats {
    alignas(CACHE_LINE_SIZE) std::atomic<std::uint64_t> total_us{0};
    alignas(CACHE_LINE_SIZE) std::atomic<std::uint64_t> count{0};
    alignas(CACHE_LINE_SIZE) std::atomic<std::uint64_t> max_us{0};

    void record(std::int64_t us) noexcept {
        const auto v = static_cast<std::uint64_t>(us);
        total_us.fetch_add(v, std::memory_order_relaxed);
        count.fetch_add(1,  std::memory_order_relaxed);
        std::uint64_t cur = max_us.load(std::memory_order_relaxed);
        while (v > cur &&
               !max_us.compare_exchange_weak(cur, v,
                   std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }

    [[nodiscard]] double avg_us() const noexcept {
        const std::uint64_t n = count.load(std::memory_order_relaxed);
        return n > 0 ? static_cast<double>(total_us.load(std::memory_order_relaxed)) / n : 0.0;
    }
    [[nodiscard]] std::uint64_t peak_us() const noexcept {
        return max_us.load(std::memory_order_relaxed);
    }
};

// ---------------------------------------------------------------------------
// Local GatewayBase with latency tracking (standalone, no CRTP from common.hpp)
// ---------------------------------------------------------------------------
template<typename Derived>
class GatewayBase {
public:
    [[nodiscard]] PlaceResult place(const OrderRequest& req) noexcept {
        return static_cast<Derived*>(this)->place_impl(req);
    }
    bool cancel(std::uint64_t exchange_id, const Symbol& sym) noexcept {
        return static_cast<Derived*>(this)->cancel_impl(exchange_id, sym);
    }
    [[nodiscard]] bool is_shadow() const noexcept {
        return static_cast<const Derived*>(this)->is_shadow_impl();
    }
    [[nodiscard]] const LatencyStats& latency() const noexcept { return latency_; }

protected:
    LatencyStats latency_{};
    static inline std::atomic<std::uint64_t> client_id_counter_{1};
};

// ---------------------------------------------------------------------------
// ShadowGateway — paper trading, no real orders
// Simulates fills from live bid/ask. Optionally simulates partial fills.
// ---------------------------------------------------------------------------
class ShadowGateway : public GatewayBase<ShadowGateway> {
public:
    ShadowGateway(
        const ShadowSettings& cfg,
        const FeeSettings&    fee_cfg,
        MarketState&          state)
        : cfg_(cfg)
        , fee_cfg_(fee_cfg)
        , state_(state)
        , rng_(std::random_device{}())
    {
        virtual_balance_.set(cfg.initial_balance_usd);
    }

    [[nodiscard]] bool is_shadow_impl() const noexcept { return true; }

    [[nodiscard]] PlaceResult place_impl(const OrderRequest& req) noexcept {
        const std::int64_t t0 = now_us();

        // Simulate fill latency
        // (In production shadow testing, you'd use a real sleep or spinwait)
        // Here we just record the simulated latency in the result
        const std::int64_t sim_lat_us = cfg_.simulated_fill_latency_us;

        const SymbolState* sym = state_.get(req.symbol.view());
        if (!sym) {
            return {false, {}, "unknown symbol"};
        }

        const Ticker ticker = sym->ticker.load();
        if (ticker.bid <= 0.0) {
            return {false, {}, "no ticker data"};
        }

        const bool is_market    = req.type == OrdType::MARKET;
        const bool is_post_only = req.post_only || req.type == OrdType::LIMIT_MAKER;

        // Determine fill price. Paper mode stays internally consistent with the
        // order semantics we submit from the engine.
        double fill_price = 0.0;
        if (is_market) {
            fill_price = req.side == Side::BUY ? ticker.ask : ticker.bid;
        } else if (is_post_only) {
            if (req.side == Side::BUY && req.price >= ticker.ask) {
                return {false, {}, "post-only order would cross"};
            }
            if (req.side == Side::SELL && req.price <= ticker.bid) {
                return {false, {}, "post-only order would cross"};
            }
            fill_price = req.price;
        } else if (req.side == Side::BUY) {
            fill_price = std::min(req.price, ticker.ask);
        } else {
            fill_price = std::max(req.price, ticker.bid);
        }

        if (fill_price <= 0.0) {
            return {false, {}, "invalid fill price"};
        }

        // Simulate partial fill
        double fill_qty = req.qty;
        if (cfg_.simulate_partial_fills) {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            if (dist(rng_) < cfg_.partial_fill_probability) {
                std::uniform_real_distribution<double> pct(0.3, 0.9);
                fill_qty = req.qty * pct(rng_);
            }
        }

        const double notional  = fill_price * fill_qty;
        const double fee_bps = is_post_only ? fee_cfg_.maker_bps : fee_cfg_.taker_bps;
        const double fee_paid = notional * (fee_bps / 10'000.0);

        // Update virtual balance
        if (req.side == Side::BUY) {
            virtual_balance_.fetch_sub_usd(notional + fee_paid);
        } else {
            virtual_balance_.fetch_add_usd(notional - fee_paid);
        }

        fills_total_.fetch_add(1, std::memory_order_relaxed);

        OrderResult r{};
        r.symbol      = req.symbol;
        r.exchange_id = next_id_.fetch_add(1, std::memory_order_relaxed);
        r.client_id   = req.client_id;
        r.status      = (fill_qty >= req.qty * 0.99)
                        ? OrdStatus::FILLED
                        : OrdStatus::PARTIALLY_FILLED;
        r.filled_qty  = fill_qty;
        r.avg_price   = fill_price;
        r.fee_paid    = fee_paid;
        r.latency_us  = sim_lat_us;
        r.is_shadow   = true;

        const std::int64_t actual_us = now_us() - t0 + sim_lat_us;
        latency_.record(actual_us);

        return {true, r};
    }

    bool cancel_impl(std::uint64_t /*id*/, const Symbol& /*sym*/) noexcept {
        // Shadow: always succeeds
        return true;
    }

    [[nodiscard]] double virtual_balance() const noexcept {
        return virtual_balance_.usd();
    }

    [[nodiscard]] std::uint64_t fills_total() const noexcept {
        return fills_total_.load(std::memory_order_relaxed);
    }

private:
    const ShadowSettings& cfg_;
    const FeeSettings&    fee_cfg_;
    const MarketState&    state_;

    // Thread-safe double balance via int64 bitcast
    struct AtomicDouble {
        std::atomic<std::int64_t> bits{0};
        void set(double v) noexcept {
            std::int64_t b; std::memcpy(&b, &v, 8);
            bits.store(b, std::memory_order_release);
        }
        double usd() const noexcept {
            const std::int64_t b = bits.load(std::memory_order_acquire);
            double v; std::memcpy(&v, &b, 8); return v;
        }
        void fetch_add_usd(double delta) noexcept {
            double cur = usd();
            for (;;) {
                double nxt = cur + delta;
                std::int64_t eb, nb;
                std::memcpy(&eb, &cur, 8);
                std::memcpy(&nb, &nxt, 8);
                if (bits.compare_exchange_weak(eb, nb,
                    std::memory_order_relaxed, std::memory_order_relaxed)) break;
                std::memcpy(&cur, &eb, 8);
            }
        }
        void fetch_sub_usd(double delta) noexcept { fetch_add_usd(-delta); }
    } virtual_balance_;

    mutable std::mt19937 rng_;
    std::atomic<std::uint64_t> next_id_{100'000'000};
    alignas(CACHE_LINE_SIZE) std::atomic<std::uint64_t> fills_total_{0};
};

// ---------------------------------------------------------------------------
// LiveGateway — intentionally disabled.
//
// This build is for shadow-mode testing on live market data. Live order routing
// is blocked until a production-grade exchange client is added.
// ---------------------------------------------------------------------------
class LiveGateway : public GatewayBase<LiveGateway> {
public:
    explicit LiveGateway(const Settings& cfg) : cfg_(cfg) {}

    [[nodiscard]] bool is_shadow_impl() const noexcept { return false; }

    [[nodiscard]] PlaceResult place_impl(const OrderRequest& /*req*/) noexcept {
        orders_failed_.fetch_add(1, std::memory_order_relaxed);
        return {false, {}, "live trading disabled; use shadow_mode=true"};
    }

    bool cancel_impl(std::uint64_t /*exchange_id*/, const Symbol& /*sym*/) noexcept {
        return false;
    }

    [[nodiscard]] std::uint64_t orders_placed() const noexcept {
        return orders_placed_.load(std::memory_order_relaxed);
    }

private:
    const Settings& cfg_;
    alignas(CACHE_LINE_SIZE) std::atomic<std::uint64_t> orders_placed_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<std::uint64_t> orders_failed_{0};
};

} // namespace bot
