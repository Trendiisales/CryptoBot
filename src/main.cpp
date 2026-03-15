// =============================================================================
// main.cpp — CryptoBot C++ entry point
//
// Thread model:
//   main thread     : startup, config, thread launch, signal handling
//   feed_thread     : WebSocket receive loop → parse → MarketState update
//                     → push candle-close events into candle_queue_
//   signal_thread   : drain candle_queue_ → SignalComposer → sig_queue_
//   exec_thread     : drain sig_queue_ → FeeGate → Risk → Gateway
//                     → monitor open trades for stops
//   metrics_thread  : heartbeat every 30s → print stats
//
// All inter-thread communication via SpscRingBuffer (no mutex in hot path).
// Portfolio state via AtomicPortfolio (seqlock).
// Kill switch via RiskManager::is_halted() (atomic<bool>).
//
// Build:
//   g++ -std=c++20 -O3 -march=native -Wall -Wextra
//       -DBOT_USE_OPENSSL
//       -Iinclude
//       src/main.cpp
//       -lssl -lcrypto -lpthread
//       -o cryptobot
//
// Run:
//   ./cryptobot config/config.ini
// =============================================================================

#include "core/common.hpp"
#include "core/models.hpp"
#include "core/config_parser.hpp"
#include "core/market_state.hpp"
#include "core/spsc_ring_buffer.hpp"
#include "core/fee_gate.hpp"
#include "strategies/strategies.hpp"
#include "risk/risk_manager.hpp"
#include "execution/gateway.hpp"
#include "execution/trade_engine.hpp"

#include <atomic>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <fcntl.h>
// POSIX HTTP server for GUI
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <unistd.h>
#include <netinet/tcp.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

// ---------------------------------------------------------------------------
// Global shutdown flag — set by SIGINT/SIGTERM
// ---------------------------------------------------------------------------
static std::atomic<bool> g_shutdown{false};

extern "C" void handle_signal(int /*sig*/) {
    g_shutdown.store(true, std::memory_order_release);
}

static bool shutdown_requested() noexcept {
    return g_shutdown.load(std::memory_order_acquire);
}

template<typename Rep, typename Period>
static bool sleep_or_shutdown(
    std::chrono::duration<Rep, Period> duration,
    std::chrono::milliseconds quantum = std::chrono::milliseconds(100)) noexcept
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration);

    while (!shutdown_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        std::this_thread::sleep_for(std::min(remaining, quantum));
    }

    return true;
}

static void set_socket_timeout(int fd, std::chrono::milliseconds timeout) noexcept {
    struct timeval tv{};
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeout.count() / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static bool wait_fd_ready(
    int fd,
    bool want_write,
    std::chrono::milliseconds timeout) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (!shutdown_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto slice = std::min(remaining, std::chrono::milliseconds(200));

        fd_set readfds;
        fd_set writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        FD_SET(fd, want_write ? &writefds : &readfds);

        struct timeval tv{};
        tv.tv_sec = static_cast<decltype(tv.tv_sec)>(slice.count() / 1000);
        tv.tv_usec = static_cast<decltype(tv.tv_usec)>((slice.count() % 1000) * 1000);

        const int ready = ::select(
            fd + 1,
            want_write ? nullptr : &readfds,
            want_write ? &writefds : nullptr,
            nullptr,
            &tv);
        if (ready > 0) return true;
        if (ready == 0) continue;
        if (errno == EINTR) continue;
        return false;
    }

    return false;
}

static bool connect_with_timeout(
    int fd,
    const struct sockaddr* addr,
    socklen_t addrlen,
    std::chrono::milliseconds timeout) noexcept
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return false;

    int rc = ::connect(fd, addr, addrlen);
    bool connected = (rc == 0);

    if (!connected) {
        if (errno != EINPROGRESS) {
            ::fcntl(fd, F_SETFL, flags);
            return false;
        }

        connected = wait_fd_ready(fd, true, timeout);
        if (connected) {
            int so_error = 0;
            socklen_t so_error_len = sizeof(so_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != 0 || so_error != 0) {
                connected = false;
            }
        }
    }

    ::fcntl(fd, F_SETFL, flags);
    return connected;
}

// ---------------------------------------------------------------------------
// Minimal JSON field extractor (no heap, no external lib)
// ---------------------------------------------------------------------------
namespace parse {

static double dbl_field(const char* json, const char* key) noexcept {
    char needle[64];
    std::snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char* pos = std::strstr(json, needle);
    if (!pos) {
        // Try without quotes (number field)
        std::snprintf(needle, sizeof(needle), "\"%s\":", key);
        pos = std::strstr(json, needle);
        if (!pos) return 0.0;
        return std::strtod(pos + std::strlen(needle), nullptr);
    }
    return std::strtod(pos + std::strlen(needle), nullptr);
}

} // namespace parse

// ---------------------------------------------------------------------------
// CandleClose event — pushed by feed thread into candle queue
// ---------------------------------------------------------------------------
struct alignas(bot::CACHE_LINE_SIZE) CandleCloseEvent {
    bot::Symbol symbol{};
    int         sym_idx{-1};
    bot::Candle candle{};
};

// ---------------------------------------------------------------------------
// REST polling market-data feed for shadow mode.
// Uses Binance HTTPS public endpoints to populate ticker, depth, and closed
// candles without requiring a full WebSocket stack.
// ---------------------------------------------------------------------------
class MarketDataFeed {
public:
    using CandleQueue = bot::SpscRingBuffer<CandleCloseEvent, 1024>;

    MarketDataFeed(
        bot::MarketState&    state,
        CandleQueue&         candle_queue,
        const bot::Settings& cfg)
        : state_(state)
        , candle_queue_(candle_queue)
        , cfg_(cfg)
        , rest_base_(split_rest_base(cfg.rest_base))
    {
        OPENSSL_init_ssl(0, nullptr);
        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (ssl_ctx_) {
            SSL_CTX_set_default_verify_paths(ssl_ctx_);
        }
    }

    ~MarketDataFeed() {
        if (ssl_ctx_) {
            SSL_CTX_free(ssl_ctx_);
        }
    }

    void run() noexcept {
        printf("[FEED] Starting REST market data feed (SHADOW mode)\n");
        printf("[FEED] REST endpoint: %s\n", cfg_.rest_base.c_str());

        if (!ssl_ctx_ || !rest_base_.valid) {
            fprintf(stderr, "[FEED] HTTPS initialisation failed\n");
            g_shutdown.store(true, std::memory_order_release);
            return;
        }

        bootstrap_candles();
        refresh_tickers();
        refresh_books();

        std::uint64_t loop = 0;
        while (!g_shutdown.load(std::memory_order_acquire)) {
            refresh_tickers();
            if ((loop % 2u) == 0u) refresh_books();
            if ((loop % 5u) == 0u) poll_closed_candles();
            ++loop;
            if (sleep_or_shutdown(std::chrono::seconds(1))) break;
        }

        printf("[FEED] Feed thread exiting\n");
    }

private:
    struct RestBase {
        std::string host;
        std::string prefix;
        bool valid{false};
    };

    bot::MarketState&    state_;
    CandleQueue&         candle_queue_;
    const bot::Settings& cfg_;
    RestBase             rest_base_{};
    SSL_CTX*             ssl_ctx_{nullptr};
    std::array<std::int64_t, bot::MarketState::MAX_SYMBOLS> last_closed_candle_ms_{};

    static RestBase split_rest_base(const std::string& url) {
        RestBase base;
        const auto scheme_pos = url.find("://");
        if (scheme_pos == std::string::npos) return base;

        const std::size_t host_start = scheme_pos + 3;
        const auto slash_pos = url.find('/', host_start);
        base.host = url.substr(host_start, slash_pos == std::string::npos
            ? std::string::npos
            : slash_pos - host_start);
        base.prefix = slash_pos == std::string::npos ? "" : url.substr(slash_pos);
        base.valid = !base.host.empty();
        return base;
    }

    static void skip_ws(const char*& p) noexcept {
        while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
    }

    static bool consume_char(const char*& p, char expected) noexcept {
        skip_ws(p);
        if (*p != expected) return false;
        ++p;
        return true;
    }

    static bool parse_int64_token(const char*& p, std::int64_t& out) noexcept {
        skip_ws(p);
        errno = 0;
        char* end = nullptr;
        const long long value = std::strtoll(p, &end, 10);
        if (end == p || errno == ERANGE) return false;
        out = static_cast<std::int64_t>(value);
        p = end;
        return true;
    }

    static bool parse_quoted_double_token(const char*& p, double& out) noexcept {
        skip_ws(p);
        if (*p != '"') return false;
        ++p;
        errno = 0;
        char* end = nullptr;
        const double value = std::strtod(p, &end);
        if (end == p || *end != '"' || errno == ERANGE) return false;
        out = value;
        p = end + 1;
        return true;
    }

    static void skip_to_item_end(const char*& p) noexcept {
        while (*p && *p != ']') ++p;
        if (*p == ']') ++p;
    }

    std::string https_get(const std::string& path_query, int* status_out = nullptr) {
        if (status_out) *status_out = 0;
        if (!ssl_ctx_ || !rest_base_.valid || shutdown_requested()) return {};

        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (::getaddrinfo(rest_base_.host.c_str(), "443", &hints, &res) != 0) {
            return {};
        }

        int fd = -1;
        SSL* ssl = nullptr;
        for (struct addrinfo* it = res; it != nullptr; it = it->ai_next) {
            fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
            if (fd < 0) continue;

            const int yes = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            set_socket_timeout(fd, std::chrono::milliseconds(1000));
            if (!connect_with_timeout(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen),
                                      std::chrono::milliseconds(1500))) {
                ::close(fd);
                fd = -1;
                continue;
            }

            ssl = SSL_new(ssl_ctx_);
            if (!ssl) {
                ::close(fd);
                fd = -1;
                continue;
            }

            SSL_set_tlsext_host_name(ssl, rest_base_.host.c_str());
            SSL_set_fd(ssl, fd);
            if (SSL_connect(ssl) == 1) break;

            SSL_free(ssl);
            ssl = nullptr;
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(res);

        if (fd < 0 || !ssl) {
            return {};
        }

        const std::string full_path = rest_base_.prefix + path_query;
        const std::string request =
            "GET " + full_path + " HTTP/1.1\r\n"
            "Host: " + rest_base_.host + "\r\n"
            "User-Agent: CryptoBot/1.0\r\n"
            "Accept: application/json\r\n"
            "Connection: close\r\n\r\n";

        std::size_t written = 0;
        while (written < request.size()) {
            if (shutdown_requested()) {
                SSL_free(ssl);
                ::close(fd);
                return {};
            }
            const int n = SSL_write(
                ssl,
                request.data() + written,
                static_cast<int>(request.size() - written));
            if (n <= 0) {
                SSL_free(ssl);
                ::close(fd);
                return {};
            }
            written += static_cast<std::size_t>(n);
        }

        std::string response;
        char buf[4096];
        for (;;) {
            if (shutdown_requested()) {
                response.clear();
                break;
            }
            const int n = SSL_read(ssl, buf, sizeof(buf));
            if (n > 0) {
                response.append(buf, static_cast<std::size_t>(n));
                continue;
            }

            const int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_ZERO_RETURN) break;
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
            break;
        }

        SSL_shutdown(ssl);
        SSL_free(ssl);
        ::close(fd);

        const auto line_end = response.find("\r\n");
        if (line_end != std::string::npos) {
            const auto status_start = response.find(' ');
            if (status_start != std::string::npos && status_start + 4 <= line_end) {
                if (status_out) {
                    *status_out = std::atoi(response.substr(status_start + 1, 3).c_str());
                }
            }
        }

        const auto body_start = response.find("\r\n\r\n");
        return body_start == std::string::npos ? std::string{} : response.substr(body_start + 4);
    }

    static std::vector<bot::Candle> parse_klines(
        const std::string& json,
        const bot::Symbol& symbol)
    {
        std::vector<bot::Candle> candles;
        const char* p = json.c_str();
        const std::int64_t now_ms = bot::epoch_ms();

        skip_ws(p);
        if (*p != '[') return candles;
        ++p;

        while (*p) {
            skip_ws(p);
            if (*p == ']') break;
            if (*p == ',') { ++p; continue; }
            if (*p != '[') break;
            ++p;

            bot::Candle c{};
            c.symbol = symbol;
            if (!parse_int64_token(p, c.open_time_ms)) break;
            if (!consume_char(p, ',')) break;
            if (!parse_quoted_double_token(p, c.open)) break;
            if (!consume_char(p, ',')) break;
            if (!parse_quoted_double_token(p, c.high)) break;
            if (!consume_char(p, ',')) break;
            if (!parse_quoted_double_token(p, c.low)) break;
            if (!consume_char(p, ',')) break;
            if (!parse_quoted_double_token(p, c.close)) break;
            if (!consume_char(p, ',')) break;
            if (!parse_quoted_double_token(p, c.volume)) break;
            if (!consume_char(p, ',')) break;
            if (!parse_int64_token(p, c.close_time_ms)) break;

            c.is_closed = c.close_time_ms <= now_ms;
            candles.push_back(c);
            skip_to_item_end(p);
        }

        return candles;
    }

    static std::uint32_t parse_levels(
        const char* json,
        const char* key,
        std::array<bot::BookLevel, bot::OrderBook::MAX_LEVELS>& levels) noexcept
    {
        char needle[32];
        std::snprintf(needle, sizeof(needle), "\"%s\":[", key);
        const char* p = std::strstr(json, needle);
        if (!p) return 0;
        p += std::strlen(needle);

        std::uint32_t count = 0;
        while (*p && count < levels.size()) {
            skip_ws(p);
            if (*p == ']') break;
            if (*p == ',') { ++p; continue; }
            if (*p != '[') break;
            ++p;

            double price = 0.0;
            double qty = 0.0;
            if (!parse_quoted_double_token(p, price)) break;
            if (!consume_char(p, ',')) break;
            if (!parse_quoted_double_token(p, qty)) break;

            levels[count++] = {price, qty};
            skip_to_item_end(p);
        }

        return count;
    }

    void bootstrap_candles() {
        const int limit = std::max(30, std::min(cfg_.strategy.candle_history, 256));
        printf("[FEED] Bootstrapping %d historical candles per symbol\n", limit);

        for (std::size_t i = 0; i < state_.n_symbols(); ++i) {
            bot::SymbolState* ss = state_.get(static_cast<int>(i));
            if (!ss) continue;

            int status = 0;
            const std::string body = https_get(
                "/v3/klines?symbol=" + std::string(ss->symbol.view())
                + "&interval=1m&limit=" + std::to_string(limit),
                &status);
            if (status != 200 || body.empty()) {
                fprintf(stderr, "[FEED] Failed to bootstrap candles for %s\n", ss->symbol.data);
                continue;
            }

            const auto candles = parse_klines(body, ss->symbol);
            std::size_t loaded = 0;
            for (const auto& candle : candles) {
                if (!candle.is_closed) continue;
                ss->candles.push(candle);
                last_closed_candle_ms_[i] = std::max(last_closed_candle_ms_[i], candle.close_time_ms);
                ++loaded;
            }

            printf("[FEED] %s historical candles loaded: %zu\n", ss->symbol.data, loaded);
        }
    }

    void refresh_tickers() {
        for (std::size_t i = 0; i < state_.n_symbols(); ++i) {
            bot::SymbolState* ss = state_.get(static_cast<int>(i));
            if (!ss) continue;

            int status = 0;
            const std::string body = https_get(
                "/v3/ticker/bookTicker?symbol=" + std::string(ss->symbol.view()),
                &status);
            if (status != 200 || body.empty()) continue;

            bot::Ticker ticker{};
            ticker.symbol = ss->symbol;
            ticker.bid = parse::dbl_field(body.c_str(), "bidPrice");
            ticker.ask = parse::dbl_field(body.c_str(), "askPrice");
            ticker.last = (ticker.bid > 0.0 && ticker.ask > 0.0)
                ? (ticker.bid + ticker.ask) * 0.5
                : 0.0;
            ticker.ts_ns = bot::now_ns();
            ss->ticker.store(ticker);
        }
    }

    void refresh_books() {
        for (std::size_t i = 0; i < state_.n_symbols(); ++i) {
            bot::SymbolState* ss = state_.get(static_cast<int>(i));
            if (!ss) continue;

            int status = 0;
            const std::string body = https_get(
                "/v3/depth?symbol=" + std::string(ss->symbol.view()) + "&limit=20",
                &status);
            if (status != 200 || body.empty()) continue;

            bot::OrderBook book{};
            book.symbol = ss->symbol;
            book.ts_ns = bot::now_ns();
            book.n_bids = parse_levels(body.c_str(), "bids", book.bids);
            book.n_asks = parse_levels(body.c_str(), "asks", book.asks);
            if (book.n_bids == 0 || book.n_asks == 0) continue;
            ss->book.store(book);
        }
    }

    void poll_closed_candles() {
        for (std::size_t i = 0; i < state_.n_symbols(); ++i) {
            bot::SymbolState* ss = state_.get(static_cast<int>(i));
            if (!ss) continue;

            int status = 0;
            const std::string body = https_get(
                "/v3/klines?symbol=" + std::string(ss->symbol.view()) + "&interval=1m&limit=3",
                &status);
            if (status != 200 || body.empty()) continue;

            const auto candles = parse_klines(body, ss->symbol);
            for (const auto& candle : candles) {
                if (!candle.is_closed) continue;
                if (candle.close_time_ms <= last_closed_candle_ms_[i]) continue;

                last_closed_candle_ms_[i] = candle.close_time_ms;
                ss->candles.push(candle);

                CandleCloseEvent ev{};
                ev.symbol = ss->symbol;
                ev.sym_idx = static_cast<int>(i);
                ev.candle = candle;
                candle_queue_.push(ev);
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// GuiServer — minimal HTTP server serving the GUI and /api/state JSON
// Binds to 0.0.0.0:GUI_PORT (default 9091). Single-threaded accept loop.
// All state is read-only (MarketState + AtomicPortfolio) — no locking needed.
// ---------------------------------------------------------------------------
class GuiServer {
public:
    static constexpr int DEFAULT_PORT = 9091;

    template<typename EngineT>
    GuiServer(
        int                    port,
        bot::MarketState&      state,
        bot::AtomicPortfolio&  portfolio,
        EngineT&               engine,
        bool                   is_shadow)
        : port_(port)
        , state_(state)
        , portfolio_(portfolio)
        , is_shadow_(is_shadow)
        , start_epoch_ms_(bot::epoch_ms())
    {
        // Capture stats via lambda-compatible pointer.
        get_stats_ = [&engine]() -> std::string {
            std::ostringstream ss;
            const auto& st = engine.stats();
            ss << '"' << "signals_generated" << '"' << ':' << st.signals_generated.load() << ','
               << '"' << "signals_blocked_fee" << '"' << ':' << st.signals_blocked_fee.load() << ','
               << '"' << "signals_blocked_risk" << '"' << ':' << st.signals_blocked_risk.load() << ','
               << '"' << "trades_entered" << '"' << ':' << st.trades_entered.load() << ','
               << '"' << "total_trades" << '"' << ':' << engine.total_trade_count() << ','
               << '"' << "open_trades" << '"' << ':' << engine.open_trade_count() << ','
               << '"' << "fee_gate_pass_rate" << '"' << ':' << engine.fee_gate_pass_rate_ratio() << ','
               << '"' << "gateway_avg_lat_us" << '"' << ':' << engine.gateway_latency_avg_us();
            return ss.str();
        };

        get_trade_log_ = [&engine]() -> std::string {
            std::ostringstream ss;
            const auto log = engine.closed_trade_log();
            ss << '[';
            for (std::size_t i = 0; i < log.count; ++i) {
                const auto& rec = log.entries[i];
                if (i > 0) ss << ',';
                ss << '{'
                   << '"' << "t" << '"' << ':' << '"' << format_time_hms(rec.closed_epoch_ms) << '"' << ','
                   << '"' << "s" << '"' << ':' << '"' << rec.symbol.view() << '"' << ','
                   << '"' << "e" << '"' << ':' << '"' << bot::signal_type_str(rec.signal_type) << '"' << ','
                   << '"' << "p" << '"' << ':' << rec.pnl_bps << ','
                   << '"' << "en" << '"' << ':' << rec.entry_price << ','
                   << '"' << "ex" << '"' << ':' << rec.exit_price << ','
                   << '"' << "hold" << '"' << ':' << rec.hold_ms << ','
                   << '"' << "reason" << '"' << ':' << '"' << rec.reason << '"'
                   << '}';
            }
            ss << ']';
            return ss.str();
        };
    }

    void run() noexcept {
        int srv = ::socket(AF_INET, SOCK_STREAM, 0);
        if (srv < 0) { fprintf(stderr, "[GUI] socket() failed\n"); return; }
        const int yes = 1;
        ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        struct sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(static_cast<uint16_t>(port_));

        if (::bind(srv, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            fprintf(stderr, "[GUI] bind() failed on port %d\n", port_);
            ::close(srv); return;
        }
        ::listen(srv, 8);
        printf("[GUI] Serving on http://0.0.0.0:%d\n", port_);

        while (!g_shutdown.load(std::memory_order_acquire)) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(srv, &readfds);
            struct timeval tv{};
            tv.tv_usec = 200000;

            const int ready = ::select(srv + 1, &readfds, nullptr, nullptr, &tv);
            if (ready <= 0) continue;

            struct sockaddr_in client_addr{};
            socklen_t clen = sizeof(client_addr);
            const int fd = ::accept(srv, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
            if (fd < 0) continue;
            handle_client(fd);
        }
        ::close(srv);
    }

private:
    int                    port_;
    bot::MarketState&      state_;
    bot::AtomicPortfolio&  portfolio_;
    bool                   is_shadow_;
    std::int64_t           start_epoch_ms_{0};
    std::function<std::string()> get_stats_;
    std::function<std::string()> get_trade_log_;

    void handle_client(int fd) noexcept {
        char req[2048]{};
        const int n = ::recv(fd, req, sizeof(req) - 1, 0);
        if (n <= 0) { ::close(fd); return; }
        req[n] = '\0';

        // Parse request line
        const bool is_get       = std::strncmp(req, "GET ", 4) == 0;
        const char* path_start  = req + 4;
        const char* path_end    = std::strchr(path_start, ' ');
        std::string path(path_start, path_end ? static_cast<std::size_t>(path_end - path_start) : 0);

        if (!is_get) { send_response(fd, 405, "text/plain", "Method Not Allowed"); ::close(fd); return; }

        if (path == "/api/state") {
            const std::string body = build_state_json();
            send_response(fd, 200, "application/json", body);
        } else if (path == "/" || path == "/index.html") {
            serve_file(fd, "gui/index.html", "text/html");
        } else if (path == "/app.js") {
            serve_file(fd, "gui/app.js", "application/javascript");
        } else if (path == "/style.css") {
            serve_file(fd, "gui/style.css", "text/css");
        } else if (path == "/favicon.svg") {
            serve_file(fd, "gui/favicon.svg", "image/svg+xml");
        } else {
            send_response(fd, 404, "text/plain", "Not Found");
        }
        ::close(fd);
    }

    std::string build_state_json() const noexcept {
        const bot::PortfolioSnapshot snap = portfolio_.load();

        std::ostringstream j;
        j << '{';

        // Top-level portfolio
        j << '"' << "portfolio" << '"' << ":{";
        j << '"' << "equity_usd" << '"' << ':' << snap.equity_usd << ',';
        j << '"' << "available_usd" << '"' << ':' << snap.available_usd << ',';
        j << '"' << "total_pnl_usd" << '"' << ':' << snap.total_pnl_usd << ',';
        j << '"' << "daily_pnl_usd" << '"' << ':' << snap.daily_pnl_usd << ',';
        j << '"' << "total_exposure_usd" << '"' << ':' << snap.total_exposure_usd << ',';
        j << '"' << "open_trade_count" << '"' << ':' << snap.open_trade_count << ',';
        j << '"' << "drawdown_pct" << '"' << ':' << snap.drawdown_pct() << ',';
        j << '"' << "is_halted" << '"' << ':' << (snap.is_halted ? "true" : "false");
        j << "},";

        // Engine stats
        j << get_stats_() << ',';

        j << '"' << "is_shadow" << '"' << ':' << (is_shadow_ ? "true" : "false") << ',';

        // Uptime
        j << '"' << "uptime_hours" << '"' << ':'
          << static_cast<double>(bot::epoch_ms() - start_epoch_ms_) / 3'600'000.0
          << ',';

        // Per-symbol data
        j << '"' << "symbols" << '"' << ":{";
        bool first_sym = true;
        for (std::size_t i = 0; i < state_.n_symbols(); ++i) {
            const bot::SymbolState* ss = state_.get(static_cast<int>(i));
            if (!ss) continue;
            if (!first_sym) j << ',';
            first_sym = false;

            const bot::Ticker    ticker  = ss->ticker.load();
            const bot::OrderBook book    = ss->book.load();
            const double         pos_usd = ss->get_position_usd();
            const auto           last_signal_type = static_cast<bot::SignalType>(
                ss->last_signal_type.load(std::memory_order_acquire));

            j << '"' << ss->symbol.view() << '"' << ":{";
            j << '"' << "price" << '"' << ':' << ticker.last << ',';
            j << '"' << "bid" << '"' << ':' << ticker.bid << ',';
            j << '"' << "ask" << '"' << ':' << ticker.ask << ',';
            j << '"' << "spread_bps" << '"' << ':' << ticker.spread_bps() << ',';
            j << '"' << "ofi" << '"' << ':' << book.order_flow_imbalance() << ',';
            j << '"' << "position_usd" << '"' << ':' << pos_usd << ',';
            j << '"' << "last_signal_type" << '"' << ':' << '"'
              << bot::signal_type_str(last_signal_type) << '"' << ',';
            j << '"' << "engines" << '"' << ":{";
            append_engine_json(j, "momentum", last_signal_type == bot::SignalType::MOMENTUM);
            j << ',';
            append_engine_json(j, "mean_reversion", last_signal_type == bot::SignalType::MEAN_REVERSION);
            j << ',';
            append_engine_json(j, "order_flow", last_signal_type == bot::SignalType::ORDER_FLOW);
            j << ',';
            append_engine_json(j, "perp_basis", last_signal_type == bot::SignalType::PERP_BASIS);
            j << ',';
            append_engine_json(j, "composite", last_signal_type == bot::SignalType::COMPOSITE);
            j << "}";
            j << "}";
        }
        j << "},";

        j << '"' << "trade_log" << '"' << ':' << get_trade_log_();
        j << '}';
        return j.str();
    }

    static std::string format_time_hms(std::int64_t epoch_ms) {
        const std::time_t secs = static_cast<std::time_t>(epoch_ms / 1000);
        std::tm tm{};
        gmtime_r(&secs, &tm);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
        return std::string(buf);
    }

    static void append_engine_json(
        std::ostringstream& out,
        const char* name,
        bool active)
    {
        out << '"' << name << '"' << ":{"
            << '"' << "active" << '"' << ':' << (active ? "true" : "false") << ','
            << '"' << "total_pnl_bp" << '"' << ':' << 0.0 << ','
            << '"' << "trades" << '"' << ':' << 0
            << '}';
    }

    void serve_file(int fd, const char* filepath, const char* mime) noexcept {
        std::ifstream f(filepath, std::ios::binary);
        if (!f.is_open()) { send_response(fd, 404, "text/plain", "File not found"); return; }
        const std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        send_response(fd, 200, mime, body);
    }

    void send_response(int fd, int code, const char* mime, const std::string& body) noexcept {
        const char* status = code == 200 ? "200 OK" : code == 404 ? "404 Not Found" : "405 Method Not Allowed";
        char header[512];
        const int hlen = std::snprintf(header, sizeof(header),
            "HTTP/1.1 %s\r\n"
            "Content-Type: %s; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "\r\n",
            status, mime, body.size());
        ::send(fd, header, hlen, 0);
        ::send(fd, body.c_str(), body.size(), 0);
    }
};

int main(int argc, char* argv[]) {
    const char* config_path = (argc > 1) ? argv[1] : "config/config.ini";

    printf("=============================================================\n");
    printf("  CryptoBot C++ — Ultra-Low Latency Spot Trading Engine\n");
    printf("  Config: %s\n", config_path);
    printf("=============================================================\n");

    // 1. Load configuration
    bot::Config  raw_cfg(config_path);
    bot::Settings cfg = bot::Settings::from_config(raw_cfg);

    printf("[INIT] Mode          : %s\n", cfg.is_shadow_mode ? "SHADOW (paper)" : "*** LIVE ***");
    printf("[INIT] Testnet       : %s\n", cfg.is_testnet     ? "yes" : "no");
    printf("[INIT] Min edge      : %.1f bps\n", cfg.fee.min_net_edge_bps);
    printf("[INIT] Max pos USD   : $%.0f\n",    cfg.risk.max_position_usd);
    printf("[INIT] Daily DD halt : %.1f%%\n",   cfg.risk.daily_dd_limit_pct);

    if (!cfg.is_shadow_mode) {
        fprintf(stderr,
                "[INIT] Live trading is disabled in this build. Set shadow_mode = true for paper trading.\n");
        return 2;
    }

    // 2. Build market state
    bot::MarketState state;
    const auto spot_pairs = raw_cfg.get_list("pairs", "spot");
    for (const auto& sym : spot_pairs) {
        const int idx = state.register_symbol(sym);
        printf("[INIT] Registered: %s (slot %d)\n", sym.c_str(), idx);
    }

    // 3. Shared subsystems
    bot::AtomicPortfolio portfolio;
    {
        bot::PortfolioSnapshot init_snap{};
        if (cfg.is_shadow_mode) {
            init_snap.equity_usd    = cfg.shadow.initial_balance_usd;
            init_snap.available_usd = cfg.shadow.initial_balance_usd;
            init_snap.peak_equity_usd = cfg.shadow.initial_balance_usd;
        }
        portfolio.store(init_snap);
    }

    bot::FeeGate       fee_gate(cfg.fee);
    bot::RiskManager   risk_mgr(cfg.risk);
    bot::SignalComposer composer(cfg);

    // 4. Shared ring buffer — feed → signal thread
    MarketDataFeed::CandleQueue candle_queue;

    bot::ShadowGateway gateway(cfg.shadow, cfg.fee, state);
    bot::TradeJournal trade_journal(cfg.trade_log_file);
    bot::TradeEngine<bot::ShadowGateway> engine(
        state, gateway, composer, fee_gate, risk_mgr, portfolio, trade_journal, cfg);

    printf("[INIT] Shadow balance: $%.2f\n", gateway.virtual_balance());
    printf("[INIT] Trade journal : %s%s\n",
           cfg.trade_log_file.c_str(),
           trade_journal.enabled() ? "" : " (unavailable)");

    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    MarketDataFeed feed(state, candle_queue, cfg);
    std::thread feed_thread([&] { feed.run(); });

    std::thread signal_thread([&] {
        printf("[SIGNAL] Signal thread started\n");
        while (!g_shutdown.load(std::memory_order_acquire)) {
            candle_queue.drain([&](const CandleCloseEvent& ev) {
                engine.on_closed_candle(ev.sym_idx);
            });
            engine.process_signals();
            engine.monitor_open_trades();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        printf("[SIGNAL] Signal thread exiting\n");
    });

    std::thread metrics_thread([&] {
        int tick = 0;
        while (!g_shutdown.load(std::memory_order_acquire)) {
            if (sleep_or_shutdown(std::chrono::seconds(10))) break;
            ++tick;
            const bot::PortfolioSnapshot snap = portfolio.load();
            printf("\n[HB %03d] equity=$%.2f | open=%d | pnl=$%.2f | DD=%.2f%%\n",
                   tick,
                   snap.equity_usd,
                   snap.open_trade_count,
                   snap.total_pnl_usd,
                   snap.drawdown_pct());
            engine.print_stats();
            printf("  Fee gate: pass=%.1f%% | shadow balance=$%.2f\n",
                   fee_gate.pass_rate(),
                   gateway.virtual_balance());
            fflush(stdout);
        }
        printf("[METRICS] Metrics thread exiting\n");
    });

    printf("[MAIN] Running in SHADOW mode. Press Ctrl+C to stop.\n\n");

    const int gui_port = cfg.metrics_port > 0 ? cfg.metrics_port : GuiServer::DEFAULT_PORT;
    GuiServer gui_server(gui_port, state, portfolio, engine, true);
    std::thread gui_thread([&] { gui_server.run(); });

    feed_thread.join();
    signal_thread.join();
    metrics_thread.join();
    gui_thread.join();

    printf("\n[MAIN] Clean shutdown complete.\n");
    return 0;
}
