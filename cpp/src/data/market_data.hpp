#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// src/data/market_data.hpp
//
// IMarketData — the single interface everything above the data layer calls.
//
// Provider hierarchy
// ──────────────────
//
//   IMarketData  (pure interface)
//     ├── IBKRProvider       ← primary; TWS EClient/EWrapper, C++ socket API
//     ├── SyntheticProvider  ← CI / unit tests; zero external dependencies
//     └── (future: DatabentoProvider for historical research)
//
//   CachedProvider wraps any IMarketData with transparent filesystem caching.
//
// Paper vs live trading
// ─────────────────────
// IBKRProvider::Config::paper = true  → connects to TWS port 7497  (paper)
//                             = false → connects to TWS port 7496  (live)
//
// Factory
// ───────
// make_provider()        — auto-selects based on env + paper flag
// make_ibkr_provider()   — explicit IBKR construction
// make_synthetic_provider() — explicit synthetic (tests/CI)
//
// IBKR integration notes
// ──────────────────────
// TWS ships a free C++ API (EClient / EWrapper).  Add the TWS API source
// directory to CMakeLists.txt and #include "EClient.h" / "EWrapper.h".
//
// Download: https://interactivebrokers.github.io/
// Repo:     https://github.com/InteractiveBrokers/tws-api
//
// IBKRProvider uses a synchronous wrapper pattern:
//   1. Call EClient::reqXxx()
//   2. Block on std::condition_variable
//   3. EWrapper callback fires, sets data, notifies CV
//   4. Caller unblocks with result
//
// This is adequate for EOD/snapshot use (no high-frequency path here).
// For live streaming, swap the CV for a lock-free queue fed to a callback.
//
// Type system
// ───────────
// Everything is volarb::data::models.  The old vol:: namespace types are gone.
// ─────────────────────────────────────────────────────────────────────────────

#include "models/option_quote.hpp"
#include "models/vol_snapshot.hpp"
// #include "ibkr_provider.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace volarb::data {

using models::OptionQuote;
using models::OptionContract;
using models::OptionRight;
using models::UnderlyingTrade;


// ─────────────────────────────────────────────────────────────────────────────
// Date helpers (internal; keep lightweight, no <chrono> year_month_day
// to avoid C++20 calendar dependency in this file)
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

// "YYYY-MM-DD" string from a Unix-ms timestamp (UTC)
inline std::string ms_to_date_str(int64_t ts_ms) {
    std::time_t t = static_cast<std::time_t>(ts_ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[11];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return {buf};
}

// Unix-ms for midnight UTC of a "YYYY-MM-DD" string
inline int64_t date_str_to_ms(const std::string& s) {
    std::tm tm{};
    tm.tm_year = std::stoi(s.substr(0, 4)) - 1900;
    tm.tm_mon  = std::stoi(s.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(s.substr(8, 2));
    return static_cast<int64_t>(
#if defined(_WIN32)
        _mkgmtime(&tm)
#else
        timegm(&tm)
#endif
    ) * 1000LL;
}

// Enumerate weekdays between two "YYYY-MM-DD" strings (inclusive)
inline std::vector<std::string> trading_days(const std::string& start,
                                              const std::string& end) {
    using namespace std::chrono;
    auto to_tp = [](const std::string& s) {
        std::tm tm{};
        tm.tm_year = std::stoi(s.substr(0, 4)) - 1900;
        tm.tm_mon  = std::stoi(s.substr(5, 2)) - 1;
        tm.tm_mday = std::stoi(s.substr(8, 2));
        return system_clock::from_time_t(
#if defined(_WIN32)
            _mkgmtime(&tm)
#else
            timegm(&tm)
#endif
        );
    };

    std::vector<std::string> days;
    auto cur  = floor<std::chrono::days>(to_tp(start));
    const auto last = floor<std::chrono::days>(to_tp(end));

    while (cur <= last) {
        weekday wd{cur};
        if (wd != Saturday && wd != Sunday)
            days.push_back(ms_to_date_str(
                duration_cast<milliseconds>(cur.time_since_epoch()).count()));
        cur += std::chrono::days{1};
    }
    return days;
}

} // namespace detail


// ─────────────────────────────────────────────────────────────────────────────
// IMarketData  —  pure interface
// ─────────────────────────────────────────────────────────────────────────────

class IMarketData {
public:
    virtual ~IMarketData() = default;

    // EOD option chain for one underlying on one date.
    // date: "YYYY-MM-DD"
    // underlying_price: spot at close; pass 0.0 to let the provider resolve it.
    virtual std::vector<OptionQuote> option_chain(
        const std::string& underlying,
        const std::string& date,
        double             underlying_price = 0.0
    ) = 0;

    // Latest trade for the underlying (used to resolve spot if not supplied).
    virtual UnderlyingTrade underlying_trade(
        const std::string& symbol
    ) = 0;

    // Iterate chains day-by-day over a date range.
    // Default: calls option_chain() sequentially.
    // Override in providers that support a bulk endpoint.
    virtual void option_chain_range(
        const std::string& underlying,
        const std::string& start,
        const std::string& end,
        const std::function<void(const std::string& date,
                                 std::vector<OptionQuote>)>& callback
    );

protected:
    static std::vector<std::string> trading_days(const std::string& s,
                                                  const std::string& e) {
        return detail::trading_days(s, e);
    }
};

inline void IMarketData::option_chain_range(
    const std::string& underlying,
    const std::string& start,
    const std::string& end,
    const std::function<void(const std::string&, std::vector<OptionQuote>)>& cb
) {
    for (const auto& d : trading_days(start, end)) {
        auto chain = option_chain(underlying, d);
        if (!chain.empty()) cb(d, std::move(chain));
    }
}


// #include "EClient.h"
// #include "EWrapper.h"
// #include "Contract.h"

// class IBKRProvider final : public IMarketData {
// public:

//     struct Config {
//         std::string host      = "127.0.0.1";
//         bool        paper     = true;   // true → port 7497 (paper trading)
//                                         // false → port 7496 (live trading)
//         int         client_id = 1;

//         int port() const noexcept { return paper ? 7497 : 7496; }
//     };

//     explicit IBKRProvider(Config cfg) : cfg_(std::move(cfg)) {
//         // TODO (Phase 2): instantiate EClient, call eClient_->eConnect(...)
//         // Example skeleton:
//         //
//         //   reader_ = std::make_unique<EReader>(&client_, &signal_);
//         //   client_.eConnect(cfg_.host.c_str(), cfg_.port(), cfg_.client_id);
//         //   reader_->start();
//         //   reader_thread_ = std::thread([this] {
//         //       while (client_.isConnected()) {
//         //           signal_.waitForSignal();
//         //           reader_->processMsgs();
//         //       }
//         //   });
//         //
//         // For now we log a warning so callers know they're hitting the stub.
//         std::cerr << "[IBKRProvider] WARNING: stub implementation — "
//                      "TWS EClient not yet connected.\n"
//                   << "  mode:      " << (cfg_.paper ? "PAPER" : "LIVE") << "\n"
//                   << "  endpoint:  " << cfg_.host << ":" << cfg_.port() << "\n";
//     }

//     // ── IMarketData ──────────────────────────────────────────────────────

//     std::vector<OptionQuote> option_chain(
//         const std::string& underlying,
//         const std::string& date,
//         double             underlying_price
//     ) override {
//         // TODO (Phase 2):
//         //   1. Build a Contract for the underlying
//         //   2. Call reqContractDetails or reqSecDefOptParams for strikes/expiries
//         //   3. For each (strike, right, expiry) call reqMktData / reqHistoricalData
//         //   4. Block on cv_ until all callbacks fire
//         //   5. Assemble OptionQuote vector and return
//         throw_not_implemented("option_chain");
//     }

//     UnderlyingTrade underlying_trade(const std::string& symbol) override {
//         // TODO (Phase 2): reqMktData, block on cv_, return latest trade tick
//         throw_not_implemented("underlying_trade");
//     }

//     // ── Connection state ─────────────────────────────────────────────────

//     [[nodiscard]] bool is_paper() const noexcept { return cfg_.paper; }

// private:
//     Config cfg_;

//     // Synchronisation primitives for the request/response pattern
//     std::mutex              mtx_;
//     std::condition_variable cv_;

//     [[noreturn]] void throw_not_implemented(const char* method) const {
//         throw std::runtime_error(
//             std::string("IBKRProvider::") + method +
//             " not yet implemented. "
//             "Connect TWS EClient (see market_data.hpp BUILD REQUIREMENT)."
//         );
//     }
// };


// ─────────────────────────────────────────────────────────────────────────────
// SyntheticProvider  —  generated data; no API key, no network
// ─────────────────────────────────────────────────────────────────────────────
// Used for:
//   • Unit tests (all of analytics and pricing)
//   • CI (no secrets or TWS instance needed)
//   • Early exploration before connecting to IBKR

class SyntheticProvider final : public IMarketData {
public:

    struct Config {
        double   spot    = 500.0;
        double   atm_iv  = 0.20;
        double   r       = 0.05;
        double   q       = 0.02;
        double   skew    = -0.10;  // IV slope per unit log-moneyness
        unsigned seed    = 42;
    };

    SyntheticProvider() : cfg_(Config{}) {}
    explicit SyntheticProvider(Config cfg) : cfg_(std::move(cfg)) {}

    std::vector<OptionQuote> option_chain(
        const std::string& underlying,
        const std::string& date,
        double             underlying_price
    ) override {
        const double spot = (underlying_price > 0) ? underlying_price : cfg_.spot;
        return generate_chain(underlying, date, spot);
    }

    UnderlyingTrade underlying_trade(const std::string& symbol) override {
        UnderlyingTrade t;
        t.symbol   = symbol;
        t.price    = cfg_.spot;
        t.size     = 1000;
        t.ts_event = now_ms();
        return t;
    }

private:
    Config cfg_;

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    // Generate a plausible-looking chain around `spot` for `date`.
    // Strikes: ±20% in 2.5% increments; two front expiries synthesised.
    std::vector<OptionQuote> generate_chain(
        const std::string& underlying,
        const std::string& date,
        double             spot
    ) const {
        std::vector<OptionQuote> quotes;

        const int64_t ts = now_ms();

        // Two synthetic expiries: ~30 and ~60 days out
        const std::vector<int> dtes = { 30, 60 };

        for (int dte : dtes) {
            const std::string expiry = advance_date(date, dte);
            const double T = static_cast<double>(dte) / 365.0;

            // Strikes: spot × {0.80, 0.825, ..., 1.20}
            for (int i = -8; i <= 8; ++i) {
                const double K = spot * (1.0 + i * 0.025);

                for (auto right : { OptionRight::Call, OptionRight::Put }) {
                    const double lm  = std::log(K / spot);
                    const double iv  = std::max(0.05,
                        cfg_.atm_iv + cfg_.skew * lm);
                    const double mid = bs_approx_price(spot, K, T, iv,
                                                       cfg_.r, cfg_.q, right);
                    const double half_spread = std::max(0.05, mid * 0.015);

                    OptionContract c;
                    c.underlying  = underlying;
                    c.expiration  = expiry;
                    c.right       = right;
                    c.strike      = K;
                    c.raw_symbol  = make_occ_symbol(underlying, expiry, right, K);

                    OptionQuote q;
                    q.contract    = c;
                    q.bid_price   = std::max(0.01, mid - half_spread);
                    q.ask_price   = mid + half_spread;
                    q.mid_price   = mid;
                    q.spread      = q.ask_price - q.bid_price;
                    q.spread_bps  = (mid > 0)
                                  ? (q.spread / mid) * 10000.0 : 0.0;
                    q.bid_size    = 10;
                    q.ask_size    = 10;
                    q.ts_event    = ts;
                    q.ts_recv     = ts;

                    quotes.push_back(q);
                }
            }
        }
        return quotes;
    }

    // Minimal closed-form approximation (no external BS call here to avoid
    // circular dependency — surface_builder calls the real pricer)
    static double bs_approx_price(double S, double K, double T, double iv,
                                   double r, double q, OptionRight right) {
        const double sqrt_T = std::sqrt(T);
        const double d1 = (std::log(S / K) + (r - q + 0.5*iv*iv)*T) / (iv*sqrt_T);
        const double d2 = d1 - iv*sqrt_T;
        auto N = [](double x){ return 0.5*std::erfc(-x*M_SQRT1_2); };
        if (right == OptionRight::Call)
            return S*std::exp(-q*T)*N(d1) - K*std::exp(-r*T)*N(d2);
        else
            return K*std::exp(-r*T)*N(-d2) - S*std::exp(-q*T)*N(-d1);
    }

    // Advance "YYYY-MM-DD" by n calendar days
    static std::string advance_date(const std::string& s, int days) {
        std::tm tm{};
        tm.tm_year = std::stoi(s.substr(0, 4)) - 1900;
        tm.tm_mon  = std::stoi(s.substr(5, 2)) - 1;
        tm.tm_mday = std::stoi(s.substr(8, 2)) + days;
#if defined(_WIN32)
        _mkgmtime(&tm);
#else
        timegm(&tm);
#endif
        // Normalise by round-tripping through mktime
        std::time_t t = timegm(&tm);
        gmtime_r(&t, &tm);
        char buf[11];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        return {buf};
    }

    // OCC-style symbol: SPY + YYMMDD + C/P + 8-digit strike (×1000)
    static std::string make_occ_symbol(const std::string& root,
                                        const std::string& expiry,
                                        OptionRight right,
                                        double strike) {
        char buf[32];
        const char cp = (right == OptionRight::Call) ? 'C' : 'P';
        const int  k  = static_cast<int>(std::round(strike * 1000.0));
        // expiry "YYYY-MM-DD" → "YYMMDD"
        std::snprintf(buf, sizeof(buf), "%s%s%s%c%08d",
                      root.c_str(),
                      expiry.substr(2, 2).c_str(),
                      expiry.substr(5, 2).c_str(),
                      cp, k);
        return {buf};
    }
};


// ─────────────────────────────────────────────────────────────────────────────
// CachedProvider  —  transparent filesystem cache for any IMarketData
// ─────────────────────────────────────────────────────────────────────────────
// Layout:
//   <cache_dir>/options/<UNDERLYING>/<DATE>.csv
//
// CSV is human-readable on purpose (debuggable with any spreadsheet).
// For bulk multi-year backtests switch the serialiser to Parquet or a
// binary format; at this project scale CSV is fine.

class CachedProvider final : public IMarketData {
public:

    CachedProvider(
        std::unique_ptr<IMarketData> inner,
        std::string                  cache_dir = "data/cache",
        int                          ttl_hours = 24
    )
        : inner_(std::move(inner))
        , cache_dir_(std::move(cache_dir))
        , ttl_s_(static_cast<std::time_t>(ttl_hours) * 3600)
    {
        fs::create_directories(cache_dir_ + "/options");
    }

    // ── IMarketData ──────────────────────────────────────────────────────

    std::vector<OptionQuote> option_chain(
        const std::string& underlying,
        const std::string& date,
        double             underlying_price
    ) override {
        const std::string path = opt_path(underlying, date);
        if (is_fresh(path)) {
            auto q = load_csv(path);
            if (!q.empty()) return q;
        }
        auto q = inner_->option_chain(underlying, date, underlying_price);
        save_csv(path, q);
        return q;
    }

    UnderlyingTrade underlying_trade(const std::string& symbol) override {
        // Pass-through: spot prices are not cached (they're live)
        return inner_->underlying_trade(symbol);
    }

private:
    std::unique_ptr<IMarketData> inner_;
    std::string                  cache_dir_;
    std::time_t                  ttl_s_;

    std::string opt_path(const std::string& und, const std::string& date) const {
        return cache_dir_ + "/options/" + und + "/" + date + ".csv";
    }

    bool is_fresh(const std::string& path) const {
        std::error_code ec;
        auto ftime = fs::last_write_time(path, ec);
        if (ec) return false;
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - fs::file_time_type::clock::now()
            + std::chrono::system_clock::now()
        );
        std::time_t mtime = std::chrono::system_clock::to_time_t(sctp);
        return (std::time(nullptr) - mtime) < ttl_s_;
    }

    // ── CSV serialisation ────────────────────────────────────────────────

    static void save_csv(const std::string& path,
                          const std::vector<OptionQuote>& quotes) {
        if (quotes.empty()) return;
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream f(path);
        f << "raw_symbol,underlying,expiration,right,strike,"
             "bid_price,ask_price,mid_price,spread,spread_bps,"
             "bid_size,ask_size,ts_event,ts_recv\n";
        f << std::fixed;
        for (const auto& q : quotes) {
            f << q.contract.raw_symbol  << ','
              << q.contract.underlying  << ','
              << q.contract.expiration  << ','
              << right_char(q.contract.right) << ','
              << q.contract.strike      << ','
              << q.bid_price            << ','
              << q.ask_price            << ','
              << q.mid_price            << ','
              << q.spread               << ','
              << q.spread_bps           << ','
              << q.bid_size             << ','
              << q.ask_size             << ','
              << q.ts_event             << ','
              << q.ts_recv              << '\n';
        }
    }

    static std::vector<OptionQuote> load_csv(const std::string& path) {
        std::vector<OptionQuote> quotes;
        std::ifstream f(path);
        if (!f) return quotes;
        std::string line;
        std::getline(f, line); // header
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            std::string tok;
            OptionQuote q;
            auto get = [&](auto& dst) {
                std::getline(ss, tok, ',');
                if constexpr (std::is_same_v<std::decay_t<decltype(dst)>, std::string>)
                    dst = tok;
                else if constexpr (std::is_same_v<std::decay_t<decltype(dst)>, double>)
                    dst = std::stod(tok);
                else if constexpr (std::is_same_v<std::decay_t<decltype(dst)>, int64_t>)
                    dst = std::stoll(tok);
                else if constexpr (std::is_same_v<std::decay_t<decltype(dst)>, uint32_t>)
                    dst = static_cast<uint32_t>(std::stoul(tok));
            };
            get(q.contract.raw_symbol);
            get(q.contract.underlying);
            get(q.contract.expiration);
            std::getline(ss, tok, ',');
            q.contract.right = parse_right(tok.empty() ? '?' : tok[0]);
            get(q.contract.strike);
            get(q.bid_price);
            get(q.ask_price);
            get(q.mid_price);
            get(q.spread);
            get(q.spread_bps);
            get(q.bid_size);
            get(q.ask_size);
            get(q.ts_event);
            get(q.ts_recv);
            quotes.push_back(q);
        }
        return quotes;
    }

    static char right_char(OptionRight r) noexcept {
        return r == OptionRight::Call ? 'C' : 'P';
    }

    static OptionRight parse_right(char c) noexcept {
        if (c == 'C' || c == 'c') return OptionRight::Call;
        if (c == 'P' || c == 'p') return OptionRight::Put;
        return OptionRight::Unknown;
    }
};


// ─────────────────────────────────────────────────────────────────────────────
// Factory functions
// ─────────────────────────────────────────────────────────────────────────────
//
// Typical usage in main.cpp:
//
//   // Research / CI (no TWS required):
//   auto provider = volarb::data::make_synthetic_provider();
//
//   // Paper trading (TWS running on localhost:7497):
//   auto provider = volarb::data::make_ibkr_provider(/*paper=*/true);
//
//   // Live trading:
//   auto provider = volarb::data::make_ibkr_provider(/*paper=*/false);

// inline std::unique_ptr<IMarketData> make_ibkr_provider(
//     bool               paper      = true,
//     const std::string& cache_dir  = "data/cache",
//     int                ttl_hours  = 1
// ) {
//     IBKRProvider::Config cfg;
//     cfg.paper = paper;

//     std::cout << "[market_data] IBKRProvider — "
//               << (paper ? "PAPER" : "LIVE")
//               << " trading, "
//               << cfg.host << ":" << cfg.port() << "\n";

//     auto inner = std::make_unique<IBKRProvider>(std::move(cfg));
//     return std::make_unique<CachedProvider>(std::move(inner), cache_dir, ttl_hours);
// }

inline std::unique_ptr<IMarketData> make_synthetic_provider(
    SyntheticProvider::Config cfg       = {},
    const std::string&        cache_dir = "data/cache",
    int                       ttl_hours = 24
) {
    std::cout << "[market_data] SyntheticProvider — no external data required\n";
    auto inner = std::make_unique<SyntheticProvider>(std::move(cfg));
    return std::make_unique<CachedProvider>(std::move(inner), cache_dir, ttl_hours);
}

// Auto-select: IBKR paper if TWS_PAPER env var set, else synthetic
// inline std::unique_ptr<IMarketData> make_provider(
//     const std::string& cache_dir = "data/cache",
//     int                ttl_hours = 24
// ) {
//     const char* mode = std::getenv("VOL_PROVIDER");
//     if (mode) {
//         const std::string m(mode);
//         if (m == "ibkr_paper")
//             return make_ibkr_provider(true,  cache_dir, 1);
//         if (m == "ibkr_live")
//             return make_ibkr_provider(false, cache_dir, 0);
//     }
//     // Default: synthetic (safe for CI, tests, early dev)
//     return make_synthetic_provider({}, cache_dir, ttl_hours);
// }

} // namespace volarb::data

#include "ibkr_provider.hpp"