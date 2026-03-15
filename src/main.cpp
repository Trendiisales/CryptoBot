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
//   g++ -std=c++20 -O3 -march=native -Wall -Wextra \
//       -DBOT_USE_OPENSSL \
//       -Iinclude \
//       src/main.cpp \
//       -lssl -lcrypto -lpthread \
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
#include <thread>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
// POSIX HTTP server for GUI
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <netinet/tcp.h>

// ---------------------------------------------------------------------------
// Global shutdown flag — set by SIGINT/SIGTERM
// ---------------------------------------------------------------------------
static std::atomic<bool> g_shutdown{false};

extern "C" void handle_signal(int /*sig*/) {
    g_shutdown.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Minimal JSON field extractor (no heap, no external lib)
// ---------------------------------------------------------------------------
namespace parse {

// Extract the value of a JSON string field "key":"<VALUE>"
// Returns pointer to start of value and writes len
static const char* str_field(const char* json, const char* key,
                               std::size_t* len) noexcept {
    char needle[64];
    std::snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char* pos = std::strstr(json, needle);
    if (!pos) { *len = 0; return nullptr; }
    const char* start = pos + std::strlen(needle);
    const char* end   = std::strchr(start, '"');
    if (!end)   { *len = 0; return nullptr; }
    *len = static_cast<std::size_t>(end - start);
    return start;
}

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
// Stub WebSocket feed (uses std::this_thread::sleep as placeholder)
// In production: replace with real WebSocket client (e.g. libwebsockets,
// uWebSockets, or a custom epoll+SSL loop).
// ---------------------------------------------------------------------------
class WebSocketFeed {
public:
    using CandleQueue = bot::SpscRingBuffer<CandleCloseEvent, 1024>;

    WebSocketFeed(
        bot::MarketState&   state,
        CandleQueue&        candle_queue,
        const bot::Settings& cfg)
        : state_(state)
        , candle_queue_(candle_queue)
        , cfg_(cfg)
    {}

    // Feed thread entry point
    void run() noexcept {
        printf("[FEED] Starting market data feed (%s mode)\n",
               cfg_.is_shadow_mode ? "SHADOW" : "LIVE");
        printf("[FEED] WS endpoint: %s\n", cfg_.ws_spot.c_str());
        printf("[FEED] NOTE: Replace this stub with a real WebSocket client\n");
        printf("[FEED]       e.g. uWebSockets, libwebsockets, or Boost.Beast\n");

        // --- Stub: simulate candle closes at 1-minute intervals ---
        // In production this loop would be replaced by the WS receive loop
        // which calls on_message() for each incoming JSON frame.
        std::int64_t tick = 0;
        while (!g_shutdown.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Simulate a candle close for each registered symbol
            for (std::size_t i = 0; i < state_.n_symbols(); ++i) {
                bot::SymbolState* ss = state_.get(static_cast<int>(i));
                if (!ss) continue;

                // Simulate ticker update
                const double fake_price = 50000.0 + static_cast<double>(tick % 200);
                bot::Ticker t{};
                t.symbol    = ss->symbol;
                t.bid       = fake_price - 0.5;
                t.ask       = fake_price + 0.5;
                t.last      = fake_price;
                t.volume_24h = 1000.0;
                t.ts_ns     = bot::now_ns();
                ss->ticker.store(t);

                // Simulate order book
                bot::OrderBook book{};
                book.symbol = ss->symbol;
                book.ts_ns  = bot::now_ns();
                book.n_bids = 5;
                book.n_asks = 5;
                for (int j = 0; j < 5; ++j) {
                    book.bids[j] = {fake_price - (j + 1) * 0.5, 1.0 + j * 0.5};
                    book.asks[j] = {fake_price + (j + 1) * 0.5, 1.0 + j * 0.5};
                }
                ss->book.store(book);

                // Simulate closed candle every 60 ticks
                if (tick > 0 && tick % 60 == 0) {
                    bot::Candle c{};
                    c.symbol       = ss->symbol;
                    c.open         = fake_price - 5;
                    c.high         = fake_price + 10;
                    c.low          = fake_price - 10;
                    c.close        = fake_price;
                    c.volume       = 100.0 + (tick % 50);
                    c.open_time_ms = bot::epoch_ms() - 60000;
                    c.close_time_ms= bot::epoch_ms();
                    c.is_closed    = true;
                    ss->candles.push(c);

                    CandleCloseEvent ev{};
                    ev.symbol  = ss->symbol;
                    ev.sym_idx = static_cast<int>(i);
                    ev.candle  = c;
                    candle_queue_.push(ev);
                }
            }
            ++tick;
        }
        printf("[FEED] Feed thread exiting\n");
    }

private:
    bot::MarketState&    state_;
    CandleQueue&         candle_queue_;
    const bot::Settings& cfg_;
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
    {
        // Capture stats via lambda-compatible pointer
        get_stats_ = [&engine]() -> std::string {
            std::ostringstream ss;
            const auto& st = engine.stats();
            ss << '"' << "signals_generated" << '"' << ':' << st.signals_generated.load() << ','
               << '"' << "signals_blocked_fee" << '"' << ':' << st.signals_blocked_fee.load() << ','
               << '"' << "signals_blocked_risk" << '"' << ':' << st.signals_blocked_risk.load() << ','
               << '"' << "trades_entered" << '"' << ':' << st.trades_entered.load() << ','
               << '"' << "open_trades" << '"' << ':' << engine.open_trade_count() << ','
               << '"' << "gateway_avg_lat_us" << '"' << ':' << engine.gateway_latency_avg_us();
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
    std::function<std::string()> get_stats_;

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
        const auto start_ns = bot::now_ns();

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
        j << '"' << "total_trades" << '"' << ':' << snap.open_trade_count << ',';

        // Uptime
        j << '"' << "uptime_hours" << '"' << ':' << 0.0 << ',';

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

            j << '"' << ss->symbol.view() << '"' << ":{";
            j << '"' << "price" << '"' << ':' << ticker.last << ',';
            j << '"' << "bid" << '"' << ':' << ticker.bid << ',';
            j << '"' << "ask" << '"' << ':' << ticker.ask << ',';
            j << '"' << "spread_bps" << '"' << ':' << ticker.spread_bps() << ',';
            j << '"' << "ofi" << '"' << ':' << book.order_flow_imbalance() << ',';
            j << '"' << "position_usd" << '"' << ':' << pos_usd;
            j << "}";
        }
        j << "},";

        // Empty trade log (backend can populate in future)
        j << '"' << "trade_log" << '"' << ":[]";
        j << '}';
        return j.str();
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
    WebSocketFeed::CandleQueue candle_queue;

    // 5. Gateway selection (compile-time CRTP dispatch via if constexpr + variant approach)
    //    We use a type-erased wrapper via a lambda to avoid template propagation into main.
    //    In a real system, you'd template the whole engine on GatewayT at compile time.

    if (cfg.is_shadow_mode) {
        // ---- SHADOW path ----
        bot::ShadowGateway gateway(cfg.shadow, state);
        bot::TradeEngine<bot::ShadowGateway> engine(
            state, gateway, composer, fee_gate, risk_mgr, portfolio, cfg);

        printf("[INIT] Shadow balance: $%.2f\n", gateway.virtual_balance());

        // Signal handler
        std::signal(SIGINT,  handle_signal);
        std::signal(SIGTERM, handle_signal);

        // Feed thread
        WebSocketFeed feed(state, candle_queue, cfg);
        std::thread feed_thread([&] { feed.run(); });

        // Signal processing thread
        std::thread signal_thread([&] {
            printf("[SIGNAL] Signal thread started\n");
            while (!g_shutdown.load(std::memory_order_acquire)) {
                candle_queue.drain([&](const CandleCloseEvent& ev) {
                    engine.on_closed_candle(ev.sym_idx);
                });
                engine.process_signals();
                engine.monitor_open_trades();
                // Brief yield — in production use a futex/eventfd for notification
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            printf("[SIGNAL] Signal thread exiting\n");
        });

        // Metrics/heartbeat thread
        std::thread metrics_thread([&] {
            int tick = 0;
            while (!g_shutdown.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(
                    std::chrono::seconds(cfg.is_shadow_mode ? 10 : 30));
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
        });

        printf("[MAIN] Running. Press Ctrl+C to stop.\n\n");

        // GUI server thread
        const int gui_port = cfg.metrics_port > 0 ? cfg.metrics_port : GuiServer::DEFAULT_PORT;
        GuiServer gui_server(gui_port, state, portfolio, engine, true);
        std::thread gui_thread([&] { gui_server.run(); });

        feed_thread.join();
        signal_thread.join();
        metrics_thread.join();
        gui_thread.join();

    } else {
        // ---- LIVE path ----
        printf("[INIT] *** LIVE TRADING MODE — REAL MONEY ***\n");
        printf("[INIT] API key: %s...\n",
               cfg.api_key.size() > 8 ? cfg.api_key.substr(0, 8).c_str() : "???");

        bot::LiveGateway gateway(cfg);

        bot::TradeEngine<bot::LiveGateway> engine(
            state, gateway, composer, fee_gate, risk_mgr, portfolio, cfg);

        std::signal(SIGINT,  handle_signal);
        std::signal(SIGTERM, handle_signal);

        WebSocketFeed feed(state, candle_queue, cfg);
        std::thread feed_thread([&] { feed.run(); });

        std::thread exec_thread([&] {
            printf("[EXEC] Execution thread started\n");
            while (!g_shutdown.load(std::memory_order_acquire)) {
                candle_queue.drain([&](const CandleCloseEvent& ev) {
                    engine.on_closed_candle(ev.sym_idx);
                });
                engine.process_signals();
                engine.monitor_open_trades();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            printf("[EXEC] Execution thread exiting\n");
        });

        std::thread metrics_thread([&] {
            int tick = 0;
            while (!g_shutdown.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::seconds(30));
                ++tick;
                const bot::PortfolioSnapshot snap = portfolio.load();
                printf("\n[HB %03d] equity=$%.2f | open=%d | pnl=$%.2f | DD=%.2f%%\n",
                       tick, snap.equity_usd, snap.open_trade_count,
                       snap.total_pnl_usd, snap.drawdown_pct());
                engine.print_stats();
                fflush(stdout);
            }
        });

        printf("[MAIN] LIVE mode running. Press Ctrl+C to stop.\n\n");

        // GUI server thread
        const int gui_port = cfg.metrics_port > 0 ? cfg.metrics_port : GuiServer::DEFAULT_PORT;
        GuiServer gui_server_live(gui_port, state, portfolio, engine, false);
        std::thread gui_thread_live([&] { gui_server_live.run(); });

        feed_thread.join();
        exec_thread.join();
        metrics_thread.join();
        gui_thread_live.join();
    }

    printf("\n[MAIN] Clean shutdown complete.\n");
    return 0;
}
