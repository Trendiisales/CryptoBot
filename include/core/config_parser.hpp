#pragma once
// =============================================================================
// config_parser.hpp — Lightweight INI file parser
//
// Reads config.ini into a flat key=value map organised by [section].
// Access via strongly-typed getters with compile-time defaults.
// Thread-safe after construction (read-only access in production).
// =============================================================================

#include <string>
#include <string_view>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <charconv>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace bot {

class Config {
public:
    explicit Config(const std::string& path) {
        load(path);
    }

    // String accessor
    [[nodiscard]] std::string get(std::string_view section,
                                   std::string_view key,
                                   std::string_view def = "") const {
        const std::string k = make_key(section, key);
        auto it = map_.find(k);
        return it != map_.end() ? it->second : std::string(def);
    }

    // Typed accessors
    [[nodiscard]] double get_double(std::string_view section,
                                     std::string_view key,
                                     double def = 0.0) const {
        const auto s = get(section, key);
        if (s.empty()) return def;
        try { return std::stod(s); }
        catch (...) { return def; }
    }

    [[nodiscard]] int get_int(std::string_view section,
                               std::string_view key,
                               int def = 0) const {
        const auto s = get(section, key);
        if (s.empty()) return def;
        try { return std::stoi(s); }
        catch (...) { return def; }
    }

    [[nodiscard]] std::uint16_t get_uint16(std::string_view section,
                                            std::string_view key,
                                            std::uint16_t def = 0) const {
        return static_cast<std::uint16_t>(get_int(section, key, def));
    }

    [[nodiscard]] bool get_bool(std::string_view section,
                                 std::string_view key,
                                 bool def = false) const {
        const auto s = get(section, key);
        if (s.empty()) return def;
        const auto lower = to_lower(s);
        return lower == "true" || lower == "1" || lower == "yes";
    }

    [[nodiscard]] std::vector<std::string> get_list(std::string_view section,
                                                     std::string_view key) const {
        const auto s = get(section, key);
        std::vector<std::string> result;
        std::istringstream ss(s);
        std::string token;
        while (std::getline(ss, token, ',')) {
            token = trim(token);
            if (!token.empty()) result.push_back(token);
        }
        return result;
    }

    // Convenience: check shadow mode
    [[nodiscard]] bool is_shadow() const {
        return get_bool("bot", "shadow_mode", true);
    }

    [[nodiscard]] bool is_testnet() const {
        return get_bool("bot", "testnet", false);
    }

    [[nodiscard]] std::string rest_base() const {
        return is_testnet()
            ? get("exchange", "rest_base_testnet")
            : get("exchange", "rest_base_live");
    }

    [[nodiscard]] std::string ws_spot() const {
        return is_testnet()
            ? get("exchange", "ws_spot_testnet")
            : get("exchange", "ws_spot_live");
    }

    [[nodiscard]] std::string ws_perp() const {
        return get("exchange", "ws_perp_live");   // perp always live (signals only)
    }

    void dump() const {
        for (const auto& [k, v] : map_) {
            // Mask API secret
            if (k.find("secret") != std::string::npos) {
                printf("  %s = ****\n", k.c_str());
            } else {
                printf("  %s = %s\n", k.c_str(), v.c_str());
            }
        }
    }

private:
    std::unordered_map<std::string, std::string> map_;

    void load(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) {
            throw std::runtime_error("Cannot open config file: " + path);
        }

        std::string section;
        std::string line;
        while (std::getline(f, line)) {
            line = trim(line);

            // Skip empty lines and comments
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;

            // Section header [section]
            if (line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
                section = trim(section);
                continue;
            }

            // Key = value
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key   = trim(line.substr(0, eq));
            std::string value = trim(line.substr(eq + 1));

            // Strip inline comments
            const auto sc = value.find(';');
            if (sc != std::string::npos) value = trim(value.substr(0, sc));

            if (!key.empty() && !section.empty()) {
                map_[make_key(section, key)] = value;
            }
        }
    }

    static std::string make_key(std::string_view section, std::string_view key) {
        std::string k(section);
        k += '.';
        k += key;
        return to_lower(k);
    }

    static std::string trim(std::string s) {
        const auto ws = " \t\r\n";
        s.erase(0, s.find_first_not_of(ws));
        s.erase(s.find_last_not_of(ws) + 1);
        return s;
    }

    static std::string to_lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }
};

// ---------------------------------------------------------------------------
// Strongly-typed settings structs populated from Config at startup
// Pass these by const ref to all subsystems (no repeated map lookups)
// ---------------------------------------------------------------------------
struct FeeSettings {
    double maker_bps;
    double taker_bps;
    double min_net_edge_bps;
    double slippage_buffer_bps;
    double max_spread_bps;

    [[nodiscard]] double break_even_bps()    const { return maker_bps + taker_bps + slippage_buffer_bps * 2; }
    [[nodiscard]] double min_gross_edge_bps() const { return break_even_bps() + min_net_edge_bps; }

    static FeeSettings from_config(const Config& c) {
        return {
            c.get_double("fees", "maker_bps",           7.5),
            c.get_double("fees", "taker_bps",           7.5),
            c.get_double("fees", "min_net_edge_bps",    8.0),
            c.get_double("fees", "slippage_buffer_bps", 2.0),
            c.get_double("fees", "max_spread_bps",      6.0),
        };
    }
};

struct RiskSettings {
    double position_size_pct;
    double max_kelly_fraction;
    double max_position_usd;
    double max_total_usd;
    double daily_dd_limit_pct;
    double total_dd_limit_pct;
    double stop_loss_bps;
    double trailing_activate_bps;
    double trailing_distance_bps;
    int    max_open_trades;
    int    min_signal_interval_sec;

    static RiskSettings from_config(const Config& c) {
        return {
            c.get_double("risk", "position_size_pct",      5.0),
            c.get_double("risk", "max_kelly_fraction",     0.25),
            c.get_double("risk", "max_position_usd",       10000.0),
            c.get_double("risk", "max_total_usd",          40000.0),
            c.get_double("risk", "daily_dd_limit_pct",     2.0),
            c.get_double("risk", "total_dd_limit_pct",     8.0),
            c.get_double("risk", "stop_loss_bps",          30.0),
            c.get_double("risk", "trailing_activate_bps",  20.0),
            c.get_double("risk", "trailing_distance_bps",  15.0),
            c.get_int("risk",    "max_open_trades",        4),
            c.get_int("risk",    "min_signal_interval_sec",60),
        };
    }
};

struct StrategySettings {
    int    ema_fast, ema_slow, ema_trend;
    int    rsi_period;
    double rsi_oversold, rsi_overbought;
    int    bb_period;
    double bb_std, bb_entry_zscore;
    double vwap_entry_bps;
    double ofi_threshold;
    double funding_long_bias_bps;
    double funding_short_bias_bps;
    double volume_confirm_ratio;
    int    candle_history;

    static StrategySettings from_config(const Config& c) {
        return {
            c.get_int("strategy",    "ema_fast",              9),
            c.get_int("strategy",    "ema_slow",              21),
            c.get_int("strategy",    "ema_trend",             55),
            c.get_int("strategy",    "rsi_period",            14),
            c.get_double("strategy", "rsi_oversold",          35.0),
            c.get_double("strategy", "rsi_overbought",        65.0),
            c.get_int("strategy",    "bb_period",             20),
            c.get_double("strategy", "bb_std",                2.0),
            c.get_double("strategy", "bb_entry_zscore",       1.8),
            c.get_double("strategy", "vwap_entry_bps",        20.0),
            c.get_double("strategy", "ofi_threshold",         0.65),
            c.get_double("strategy", "funding_long_bias_bps", 3.0),
            c.get_double("strategy", "funding_short_bias_bps",-3.0),
            c.get_double("strategy", "volume_confirm_ratio",  1.3),
            c.get_int("strategy",    "candle_history",        200),
        };
    }
};

struct ExecSettings {
    bool   use_post_only;
    int    post_only_timeout_ms;
    int    max_requeue;
    double twap_min_usd;
    int    twap_slices;
    int    twap_interval_ms;

    static ExecSettings from_config(const Config& c) {
        return {
            c.get_bool("execution",   "use_post_only",          false),
            c.get_int("execution",    "post_only_timeout_ms",   30000),
            c.get_int("execution",    "max_requeue",            5),
            c.get_double("execution", "twap_min_usd",           5000.0),
            c.get_int("execution",    "twap_slices",            5),
            c.get_int("execution",    "twap_interval_ms",       10000),
        };
    }
};

struct ShadowSettings {
    double initial_balance_usd;
    int    simulated_fill_latency_us;
    bool   simulate_partial_fills;
    double partial_fill_probability;

    static ShadowSettings from_config(const Config& c) {
        return {
            c.get_double("shadow", "initial_balance_usd",          100000.0),
            c.get_int("shadow",    "simulated_fill_latency_us",    3500),
            c.get_bool("shadow",   "simulate_partial_fills",       true),
            c.get_double("shadow", "partial_fill_probability",     0.15),
        };
    }
};

// Master settings object — pass this everywhere
struct Settings {
    FeeSettings      fee;
    RiskSettings     risk;
    StrategySettings strategy;
    ExecSettings     exec;
    ShadowSettings   shadow;
    bool             is_shadow_mode{true};
    bool             is_testnet{false};
    bool             allow_short_entries{false};
    std::string      api_key;
    std::string      api_secret;
    std::string      rest_base;
    std::string      ws_spot;
    std::string      ws_perp;
    std::string      log_level;
    std::string      trade_log_file;
    int              metrics_port{9090};

    static Settings from_config(const Config& c) {
        Settings s;
        s.fee         = FeeSettings::from_config(c);
        s.risk        = RiskSettings::from_config(c);
        s.strategy    = StrategySettings::from_config(c);
        s.exec        = ExecSettings::from_config(c);
        s.shadow      = ShadowSettings::from_config(c);
        s.is_shadow_mode = c.is_shadow();
        s.is_testnet     = c.is_testnet();
        s.allow_short_entries = c.get_bool("bot", "allow_short_entries", false);
        s.api_key        = c.get("exchange", "api_key");
        s.api_secret     = c.get("exchange", "api_secret");
        s.rest_base      = c.rest_base();
        s.ws_spot        = c.ws_spot();
        s.ws_perp        = c.ws_perp();
        s.log_level      = c.get("bot", "log_level", "INFO");
        s.trade_log_file = c.get("bot", "trade_log_file", "logs/trades.csv");
        s.metrics_port   = c.get_int("bot", "metrics_port", 9090);
        return s;
    }
};

} // namespace bot
