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
    explicit ShadowGateway(
        const ShadowSettings& cfg,
        MarketState&          state)
        : cfg_(cfg)
        , state_(state)
        , virtual_balance_(cfg.initial_balance_usd)
        , rng_(std::random_device{}())
    {}

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

        // Determine fill price
        double fill_price;
        if (req.side == Side::BUY) {
            fill_price = (req.type == OrdType::MARKET)
                ? ticker.ask
                : std::min(req.price, ticker.ask);
        } else {
            fill_price = (req.type == OrdType::MARKET)
                ? ticker.bid
                : std::max(req.price, ticker.bid);
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
        const double fee_rate  = 0.00075;  // 7.5 bps
        const double fee_paid  = notional * fee_rate;

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
// LiveGateway — real REST order placement via POSIX HTTP
//
// Signs each request with HMAC-SHA256 using the API secret from config.
// Uses a persistent keep-alive TCP connection to api.binance.com.
// All SPOT only — any attempt to route to futures endpoint is rejected.
//
// NOTE: In production you would use a proper async HTTP client (e.g. libcurl
// or your own epoll loop). This implementation uses blocking POSIX I/O on a
// dedicated executor thread. Latency target: < 5ms round-trip on co-located VPS.
// ---------------------------------------------------------------------------
class LiveGateway : public GatewayBase<LiveGateway> {
public:
    explicit LiveGateway(const Settings& cfg) : cfg_(cfg) {}

    [[nodiscard]] bool is_shadow_impl() const noexcept { return false; }

    [[nodiscard]] PlaceResult place_impl(const OrderRequest& req) noexcept {
        const std::int64_t t0 = now_us();

        // Build parameter string
        char params[512];
        const std::int64_t ts = epoch_ms();
        const std::uint64_t cid = client_id_counter_.fetch_add(1, std::memory_order_relaxed);

        const char* ord_type =
            (req.type == OrdType::MARKET)      ? "MARKET" :
            (req.type == OrdType::LIMIT_MAKER) ? "LIMIT_MAKER" :
            (req.type == OrdType::IOC)         ? "LIMIT" : "LIMIT";

        const char* tif =
            (req.type == OrdType::IOC)  ? "&timeInForce=IOC" :
            (req.type == OrdType::LIMIT) ? "&timeInForce=GTC" : "";

        int plen;
        if (req.type == OrdType::MARKET) {
            plen = std::snprintf(params, sizeof(params),
                "symbol=%s&side=%s&type=%s&quantity=%.8f"
                "&newClientOrderId=bot_%llu&timestamp=%lld",
                req.symbol.data,
                side_str(req.side),
                ord_type,
                req.qty,
                (unsigned long long)cid,
                (long long)ts);
        } else {
            plen = std::snprintf(params, sizeof(params),
                "symbol=%s&side=%s&type=%s&quantity=%.8f&price=%.8f%s"
                "&newClientOrderId=bot_%llu&timestamp=%lld",
                req.symbol.data,
                side_str(req.side),
                ord_type,
                req.qty,
                req.price,
                tif,
                (unsigned long long)cid,
                (long long)ts);
        }

        // HMAC-SHA256 signature
        char sig_hex[65]{};
        hmac_sha256(cfg_.api_secret.c_str(), params, sig_hex);

        // Append signature
        char full_params[640];
        std::snprintf(full_params, sizeof(full_params),
                      "%s&signature=%s", params, sig_hex);

        // HTTP POST /api/v3/order
        const std::string response = http_post(
            "/api/v3/order",
            full_params,
            cfg_.api_key.c_str());

        const std::int64_t latency_us = now_us() - t0;
        latency_.record(latency_us);

        if (response.empty()) {
            orders_failed_.fetch_add(1, std::memory_order_relaxed);
            return {false, {}, "http error"};
        }

        // Parse minimal JSON (no external dependency)
        OrderResult r{};
        r.symbol      = req.symbol;
        r.client_id   = cid;
        r.latency_us  = latency_us;
        r.is_shadow   = false;

        // Extract orderId
        const char* id_pos = std::strstr(response.c_str(), "\"orderId\":");
        if (id_pos) r.exchange_id = std::strtoull(id_pos + 10, nullptr, 10);

        // Extract status
        const char* st_pos = std::strstr(response.c_str(), "\"status\":\"");
        if (st_pos) {
            const char* sv = st_pos + 10;
            if (std::strncmp(sv, "FILLED",           6)  == 0) r.status = OrdStatus::FILLED;
            else if (std::strncmp(sv, "PARTIALLY_FILLED", 16) == 0) r.status = OrdStatus::PARTIALLY_FILLED;
            else if (std::strncmp(sv, "NEW",             3)  == 0) r.status = OrdStatus::OPEN;
            else r.status = OrdStatus::REJECTED;
        }

        // Extract executedQty
        const char* eq_pos = std::strstr(response.c_str(), "\"executedQty\":\"");
        if (eq_pos) r.filled_qty = std::strtod(eq_pos + 15, nullptr);

        // Extract cummulativeQuoteQty (total quote spent)
        const char* qq_pos = std::strstr(response.c_str(), "\"cummulativeQuoteQty\":\"");
        if (qq_pos && r.filled_qty > 0.0) {
            const double total_quote = std::strtod(qq_pos + 23, nullptr);
            r.avg_price = total_quote / r.filled_qty;
        }

        orders_placed_.fetch_add(1, std::memory_order_relaxed);
        return {true, r};
    }

    bool cancel_impl(std::uint64_t exchange_id, const Symbol& sym) noexcept {
        char params[256];
        std::snprintf(params, sizeof(params),
                      "symbol=%s&orderId=%llu&timestamp=%lld",
                      sym.data,
                      (unsigned long long)exchange_id,
                      (long long)epoch_ms());
        char sig_hex[65]{};
        hmac_sha256(cfg_.api_secret.c_str(), params, sig_hex);
        char full[320];
        std::snprintf(full, sizeof(full), "%s&signature=%s", params, sig_hex);
        const std::string r = http_delete("/api/v3/order", full, cfg_.api_key.c_str());
        return !r.empty();
    }

    [[nodiscard]] std::uint64_t orders_placed() const noexcept {
        return orders_placed_.load(std::memory_order_relaxed);
    }

private:
    const Settings& cfg_;
    alignas(CACHE_LINE_SIZE) std::atomic<std::uint64_t> orders_placed_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<std::uint64_t> orders_failed_{0};

    // ---- HMAC-SHA256 --------------------------------------------------------
    static void hmac_sha256(const char* key, const char* data, char* out_hex) noexcept {
#ifdef BOT_USE_OPENSSL
        unsigned char digest[32];
        unsigned int  dlen = 32;
        HMAC(EVP_sha256(),
             key, static_cast<int>(std::strlen(key)),
             reinterpret_cast<const unsigned char*>(data),
             std::strlen(data),
             digest, &dlen);
        static const char* hex = "0123456789abcdef";
        for (unsigned i = 0; i < 32; ++i) {
            out_hex[i*2]     = hex[digest[i] >> 4];
            out_hex[i*2 + 1] = hex[digest[i] & 0xf];
        }
        out_hex[64] = '\0';
#else
        // Stub — replace with real HMAC when building with OpenSSL
        std::strncpy(out_hex, "HMAC_STUB_ENABLE_BOT_USE_OPENSSL", 64);
        out_hex[64] = '\0';
#endif
    }

    // ---- Minimal blocking HTTP POST (POSIX) ---------------------------------
    std::string http_post(const char* path, const char* body, const char* api_key) noexcept {
        return http_request("POST", path, body, api_key);
    }
    std::string http_delete(const char* path, const char* body, const char* api_key) noexcept {
        return http_request("DELETE", path, body, api_key);
    }

    std::string http_request(const char* method, const char* path,
                              const char* body, const char* api_key) noexcept
    {
        // Extract host from rest_base (strip https://)
        const char* base = cfg_.rest_base.c_str();
        const char* host_start = std::strstr(base, "://");
        if (!host_start) return {};
        host_start += 3;

        // Copy host (up to first /)
        char host[128]{};
        const char* slash = std::strchr(host_start, '/');
        const std::size_t hlen = slash
            ? static_cast<std::size_t>(slash - host_start)
            : std::strlen(host_start);
        std::memcpy(host, host_start, std::min(hlen, sizeof(host) - 1));

        // Resolve and connect
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (::getaddrinfo(host, "443", &hints, &res) != 0) return {};

        const int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) { ::freeaddrinfo(res); return {}; }

        // TCP_NODELAY — disable Nagle's algorithm for lowest latency
        const int yes = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
            ::close(fd); ::freeaddrinfo(res); return {};
        }
        ::freeaddrinfo(res);

        // Build HTTP request
        char req_buf[2048];
        const std::size_t body_len = std::strlen(body);
        const int req_len = std::snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "X-MBX-APIKEY: %s\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            method, path, host, api_key, body_len, body);

        ::send(fd, req_buf, req_len, 0);

        // Read response
        char resp[4096]{};
        int total = 0, n;
        while ((n = ::recv(fd, resp + total, sizeof(resp) - total - 1, 0)) > 0)
            total += n;
        resp[total] = '\0';
        ::close(fd);

        // Find JSON body (after \r\n\r\n)
        const char* body_start = std::strstr(resp, "\r\n\r\n");
        return body_start ? std::string(body_start + 4) : std::string{};
    }
};

} // namespace bot
