#pragma once
// =============================================================================
// trade_journal.hpp — Persistent CSV trade journal for evaluation
//
// One row is written per closed trade and flushed immediately so results remain
// available across process restarts. The journal is intentionally append-only.
// =============================================================================

#include "core/models.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace bot {

class TradeJournal {
public:
    explicit TradeJournal(std::string path)
        : path_(std::move(path))
    {
        open();
    }

    TradeJournal(const TradeJournal&) = delete;
    TradeJournal& operator=(const TradeJournal&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return ready_; }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    void record_closed_trade(
        const Trade& trade,
        const PortfolioSnapshot& portfolio_after_close,
        const char* exit_reason,
        bool is_shadow) noexcept
    {
        if (!ready_) return;

        const double entry_notional = trade.entry_price * trade.qty;
        const double exit_notional  = trade.exit_price * trade.qty;
        const char* mode = is_shadow ? "shadow" : "live";
        const char* reason = exit_reason ? exit_reason : "";

        out_
            << trade.id << ','
            << csv_escape(mode) << ','
            << csv_escape(trade.symbol.view()) << ','
            << csv_escape(side_str(trade.side)) << ','
            << csv_escape(signal_type_str(trade.signal.type)) << ','
            << trade.signal.confidence << ','
            << trade.signal.expected_edge_bps << ','
            << static_cast<unsigned>(trade.signal.n_aligned) << ','
            << static_cast<unsigned>(trade.signal.sub_signals) << ','
            << csv_escape(format_iso8601_utc(trade.opened_epoch_ms)) << ','
            << trade.opened_epoch_ms << ','
            << csv_escape(format_iso8601_utc(trade.closed_epoch_ms)) << ','
            << trade.closed_epoch_ms << ','
            << static_cast<std::int64_t>(trade.duration_sec() * 1000.0) << ','
            << csv_escape(ord_status_str(trade.entry_order_status)) << ','
            << csv_escape(ord_status_str(trade.exit_order_status)) << ','
            << trade.entry_latency_us << ','
            << trade.exit_latency_us << ','
            << trade.entry_price << ','
            << trade.exit_price << ','
            << trade.qty << ','
            << entry_notional << ','
            << exit_notional << ','
            << trade.gross_pnl_usd() << ','
            << trade.entry_fee << ','
            << trade.exit_fee << ','
            << trade.pnl_usd() << ','
            << trade.pnl_bps() << ','
            << trade.stop_price << ','
            << trade.take_profit_price << ','
            << trade.trailing_stop << ','
            << csv_escape(reason) << ','
            << portfolio_after_close.equity_usd << ','
            << portfolio_after_close.available_usd << ','
            << portfolio_after_close.total_pnl_usd << ','
            << portfolio_after_close.open_trade_count
            << '\n';
        out_.flush();

        if (!out_) {
            std::fprintf(stderr, "[JOURNAL] Failed writing trade row to %s\n", path_.c_str());
            ready_ = false;
        }
    }

private:
    std::string  path_;
    std::ofstream out_;
    bool         ready_{false};

    void open() noexcept {
        if (path_.empty()) return;

        namespace fs = std::filesystem;
        const fs::path log_path(path_);

        std::error_code ec;
        if (log_path.has_parent_path()) {
            fs::create_directories(log_path.parent_path(), ec);
        }

        ec.clear();
        const bool exists = fs::exists(log_path, ec);
        ec.clear();
        const bool need_header = !exists || fs::file_size(log_path, ec) == 0;

        out_.open(path_, std::ios::out | std::ios::app);
        if (!out_.is_open()) {
            std::fprintf(stderr, "[JOURNAL] Failed opening %s\n", path_.c_str());
            return;
        }

        ready_ = true;
        if (need_header) {
            out_
                << "trade_id,mode,symbol,side,signal_type,signal_confidence,"
                << "expected_edge_bps,aligned_signals,sub_signals_mask,"
                << "opened_at_utc,opened_epoch_ms,closed_at_utc,closed_epoch_ms,hold_ms,"
                << "entry_order_status,exit_order_status,entry_latency_us,exit_latency_us,"
                << "entry_price,exit_price,qty,entry_notional_usd,exit_notional_usd,"
                << "gross_pnl_usd,entry_fee_usd,exit_fee_usd,net_pnl_usd,pnl_bps,"
                << "stop_price,take_profit_price,trailing_stop,exit_reason,"
                << "portfolio_equity_usd,portfolio_available_usd,portfolio_total_pnl_usd,"
                << "portfolio_open_trade_count\n";
            out_.flush();
            if (!out_) {
                std::fprintf(stderr, "[JOURNAL] Failed writing CSV header to %s\n", path_.c_str());
                ready_ = false;
            }
        }
    }

    static std::string csv_escape(std::string_view value) {
        std::string out;
        out.reserve(value.size() + 2);
        out.push_back('"');
        for (char ch : value) {
            if (ch == '"') out.push_back('"');
            out.push_back(ch);
        }
        out.push_back('"');
        return out;
    }

    static std::string format_iso8601_utc(std::int64_t epoch_ms) {
        if (epoch_ms <= 0) return {};

        const std::time_t secs = static_cast<std::time_t>(epoch_ms / 1000);
        const int millis = static_cast<int>(epoch_ms % 1000);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &secs);
#else
        gmtime_r(&secs, &tm);
#endif
        char buf[32];
        std::snprintf(
            buf,
            sizeof(buf),
            "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
            tm.tm_year + 1900,
            tm.tm_mon + 1,
            tm.tm_mday,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec,
            millis);
        return std::string(buf);
    }
};

} // namespace bot
